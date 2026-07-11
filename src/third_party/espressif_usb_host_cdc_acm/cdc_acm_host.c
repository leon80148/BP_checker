/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/queue.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "soc/soc_caps.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "cdc_host_descriptor_parsing.h"
#include "cdc_host_common.h"
#include "cdc_host_acm_compliant.h"
#include "cdc_notification_parser.h"

static const char *TAG = "cdc_acm";

// Control transfer constants
#define CDC_ACM_CTRL_TRANSFER_SIZE (64)   // All standard CTRL requests and responses fit in this size

#if CONFIG_IDF_TARGET_LINUX
#define CDC_ACM_CTRL_TIMEOUT_MS    (1000) // To shorten transfer timeout errors on the linux target
#else
#define CDC_ACM_CTRL_TIMEOUT_MS    (5000) // Every CDC device should be able to respond to CTRL transfer in 5 seconds
#endif // CONFIG_IDF_TARGET_LINUX

// CDC-ACM spinlock
static portMUX_TYPE cdc_acm_lock = portMUX_INITIALIZER_UNLOCKED;
#define CDC_ACM_ENTER_CRITICAL()   portENTER_CRITICAL(&cdc_acm_lock)
#define CDC_ACM_EXIT_CRITICAL()    portEXIT_CRITICAL(&cdc_acm_lock)

// This mutex is intentionally never deleted. It serializes observation and
// destruction of p_cdc_acm_obj across driver instances, so a waiter can never
// block on storage freed by a concurrent uninstall.
static StaticSemaphore_t cdc_acm_lifetime_mutex_storage;
static SemaphoreHandle_t cdc_acm_lifetime_mutex = NULL;

static SemaphoreHandle_t cdc_acm_get_lifetime_mutex(void)
{
    CDC_ACM_ENTER_CRITICAL();
    if (cdc_acm_lifetime_mutex == NULL) {
        cdc_acm_lifetime_mutex =
            xSemaphoreCreateMutexStatic(&cdc_acm_lifetime_mutex_storage);
    }
    SemaphoreHandle_t mutex = cdc_acm_lifetime_mutex;
    CDC_ACM_EXIT_CRITICAL();
    return mutex;
}

static bool cdc_acm_lifetime_lock(void)
{
    SemaphoreHandle_t mutex = cdc_acm_get_lifetime_mutex();
    return mutex != NULL && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
}

static void cdc_acm_lifetime_unlock(void)
{
    assert(cdc_acm_lifetime_mutex != NULL);
    xSemaphoreGive(cdc_acm_lifetime_mutex);
}

// CDC-ACM events
#define CDC_ACM_TEARDOWN          BIT0
#define CDC_ACM_TEARDOWN_COMPLETE BIT1
#define CDC_ACM_CLOSE_QUEUE_DEPTH 4
#define CDC_ACM_CLOSE_DRAIN_ATTEMPTS 100
#define CDC_ACM_UNINSTALL_WAIT_MS 1000
#define CDC_ACM_CLOSE_QUEUE_WAIT_MS 100
#define CDC_ACM_CLOSE_WAIT_MS 1500
#define CDC_ACM_TRANSFER_DRAIN_WAIT_MS 1000

typedef struct {
    cdc_acm_dev_hdl_t handle;
    SemaphoreHandle_t completion;
    esp_err_t result;
    size_t ref_count;
} cdc_acm_close_request_t;

// CDC-ACM driver object
typedef struct {
    usb_host_client_handle_t cdc_acm_client_hdl;        /*!< USB Host handle reused for all CDC-ACM devices in the system */
    SemaphoreHandle_t open_close_mutex;
    EventGroupHandle_t event_group;
    QueueHandle_t close_request_queue;
    TaskHandle_t driver_task_h;
    bool uninstalling;
    bool task_quiescent;
    esp_err_t teardown_result;
    uint32_t dispatch_generation;
    cdc_acm_new_dev_callback_t new_dev_cb;
    SLIST_HEAD(list_dev, cdc_dev_s) cdc_devices_list;   /*!< List of open pseudo devices */
} cdc_acm_obj_t;

static cdc_acm_obj_t *p_cdc_acm_obj = NULL;
static esp_err_t cdc_acm_host_close_direct(cdc_acm_dev_hdl_t cdc_hdl);

static void cdc_acm_close_request_release(cdc_acm_close_request_t *request)
{
    bool destroy = false;
    CDC_ACM_ENTER_CRITICAL();
    assert(request->ref_count > 0);
    request->ref_count--;
    destroy = request->ref_count == 0;
    CDC_ACM_EXIT_CRITICAL();

    if (destroy) {
        vSemaphoreDelete(request->completion);
        free(request);
    }
}

static void cdc_acm_close_request_complete(cdc_acm_close_request_t *request,
                                           esp_err_t result)
{
    CDC_ACM_ENTER_CRITICAL();
    request->result = result;
    CDC_ACM_EXIT_CRITICAL();

    // The request-owned semaphore cannot consume an unrelated task
    // notification. The driver's reference keeps both objects alive until
    // xSemaphoreGive() has completely returned, even if the caller timed out.
    xSemaphoreGive(request->completion);
    cdc_acm_close_request_release(request);
}

static esp_err_t cdc_acm_schedule_internal_close(cdc_dev_t *cdc_dev,
                                                 bool poll_was_submitted)
{
    CDC_ACM_ENTER_CRITICAL();
    cdc_dev->closing = true;
    cdc_dev->close_prepared = !poll_was_submitted;
    cdc_dev->start_cleanup_retained = true;
    SLIST_INSERT_HEAD(&p_cdc_acm_obj->cdc_devices_list, cdc_dev, list_entry);
    CDC_ACM_EXIT_CRITICAL();
    (void)usb_host_client_unblock(p_cdc_acm_obj->cdc_acm_client_hdl);
    return ESP_OK;
}

static bool cdc_acm_has_retained_cleanup(cdc_acm_obj_t *cdc_acm_obj)
{
    bool pending = false;
    CDC_ACM_ENTER_CRITICAL();
    cdc_dev_t *cdc_dev = NULL;
    SLIST_FOREACH(cdc_dev, &cdc_acm_obj->cdc_devices_list, list_entry) {
        if (cdc_dev->start_cleanup_retained) {
            pending = true;
            break;
        }
    }
    CDC_ACM_EXIT_CRITICAL();
    return pending;
}

static void cdc_acm_retry_retained_cleanup(cdc_acm_obj_t *cdc_acm_obj)
{
    cdc_dev_t *retained = NULL;
    CDC_ACM_ENTER_CRITICAL();
    SLIST_FOREACH(retained, &cdc_acm_obj->cdc_devices_list, list_entry) {
        if (retained->start_cleanup_retained) break;
    }
    CDC_ACM_EXIT_CRITICAL();

    if (retained != NULL) {
        // close_direct is bounded. If a callback is still late, the device
        // remains list-owned and the finite-time client loop retries it.
        (void)cdc_acm_host_close_direct((cdc_acm_dev_hdl_t)retained);
    }
}

/**
 * @brief Default CDC-ACM driver configuration
 *
 * This configuration is used when user passes NULL to config pointer during device open.
 */
static const cdc_acm_host_driver_config_t cdc_acm_driver_config_default = {
    .driver_task_stack_size = 4096,
    .driver_task_priority = 10,
    .xCoreID = 0,
    .new_dev_cb = NULL,
};

/**
 * @brief Notification received callback
 *
 * Notification (interrupt) IN transfer is submitted at the end of this function to ensure periodic poll of IN endpoint.
 *
 * @param[in] transfer Transfer that triggered the callback
 */
static void notif_xfer_cb(usb_transfer_t *transfer);

/**
 * @brief Data received callback
 *
 * Data (bulk) IN transfer is submitted at the end of this function to ensure continuous poll of IN endpoint.
 *
 * @param[in] transfer Transfer that triggered the callback
 */
static void in_xfer_cb(usb_transfer_t *transfer);

/**
 * @brief Data send callback
 *
 * Reused for bulk OUT and CTRL transfers
 *
 * @param[in] transfer Transfer that triggered the callback
 */
static void out_xfer_cb(usb_transfer_t *transfer);

/**
 * @brief USB Host Client event callback
 *
 * Handling of USB device connection/disconnection to/from root HUB.
 *
 * @param[in] event_msg Event message type
 * @param[in] arg Caller's argument (not used in this driver)
 */
static void usb_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg);

/**
 * @brief Reset IN transfer
 *
 * In in_xfer_cb() we can modify IN transfer parameters, this function resets the transfer to its defaults
 *
 * @param[in] cdc_dev Pointer to CDC device
 */
static void cdc_acm_reset_in_transfer(cdc_dev_t *cdc_dev)
{
    assert(cdc_dev->data.in_xfer);
    usb_transfer_t *transfer = cdc_dev->data.in_xfer;
    uint8_t **ptr = (uint8_t **)(&(transfer->data_buffer));
    *ptr = cdc_dev->data.in_data_buffer_base;
    transfer->num_bytes = transfer->data_buffer_size;
    // This is a hotfix for IDF changes, where 'transfer->data_buffer_size' does not contain actual buffer length,
    // but *allocated* buffer length, which can be larger if CONFIG_HEAP_POISONING_COMPREHENSIVE is enabled
    transfer->num_bytes -= transfer->data_buffer_size % cdc_dev->data.in_mps;
}

/**
 * @brief CDC-ACM driver handling task
 *
 * USB host client registration and deregistration is handled here.
 *
 * @param[in] arg User's argument. Handle of a task that started this task.
 */
static void cdc_acm_client_task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    cdc_acm_obj_t *cdc_acm_obj = p_cdc_acm_obj; // Make local copy of the driver's handle
    assert(cdc_acm_obj->cdc_acm_client_hdl);

    // Start handling client's events
    while (1) {
        TickType_t event_wait = cdc_acm_has_retained_cleanup(cdc_acm_obj)
                                    ? pdMS_TO_TICKS(10)
                                    : portMAX_DELAY;
        usb_host_client_handle_events(cdc_acm_obj->cdc_acm_client_hdl,
                                      event_wait);
        cdc_acm_close_request_t *close_request = NULL;
        while (xQueueReceive(cdc_acm_obj->close_request_queue, &close_request, 0) == pdPASS) {
            if (close_request != NULL) {
                esp_err_t close_result =
                    cdc_acm_host_close_direct(close_request->handle);
                cdc_acm_close_request_complete(close_request, close_result);
            }
        }
        cdc_acm_retry_retained_cleanup(cdc_acm_obj);
        EventBits_t events = xEventGroupGetBits(cdc_acm_obj->event_group);
        if ((events & CDC_ACM_TEARDOWN) == 0) continue;

        ESP_LOGD(TAG, "Deregistering client");
        esp_err_t deregister_result = usb_host_client_deregister(
            cdc_acm_obj->cdc_acm_client_hdl);
        CDC_ACM_ENTER_CRITICAL();
        cdc_acm_obj->teardown_result = deregister_result;
        CDC_ACM_EXIT_CRITICAL();

        EventGroupHandle_t completion_event = cdc_acm_obj->event_group;
        if (deregister_result != ESP_OK) {
            // The client is still registered. Publish this failed attempt,
            // then keep the task alive so a later uninstall can retry safely.
            xEventGroupClearBits(completion_event, CDC_ACM_TEARDOWN);
            xEventGroupSetBits(completion_event, CDC_ACM_TEARDOWN_COMPLETE);
            continue;
        }

        xEventGroupSetBits(completion_event, CDC_ACM_TEARDOWN_COMPLETE);
        CDC_ACM_ENTER_CRITICAL();
        // Set only after xEventGroupSetBits() has fully returned. A waiter that
        // wakes earlier observes false and retries instead of deleting the
        // event group beneath its setter. This is the task's final object access.
        cdc_acm_obj->task_quiescent = true;
        CDC_ACM_EXIT_CRITICAL();
        vTaskSuspend(NULL);
    }
}

/**
 * @brief Cancel transfer and reset endpoint
 *
 * This function will cancel ongoing transfer a reset its endpoint to ready state.
 *
 * @param[in] dev_hdl USB device handle
 * @param[in] transfer Transfer to be cancelled
 * @return esp_err_t
 */
static bool cdc_acm_status_is_gone(esp_err_t status)
{
    return status == ESP_ERR_NOT_FOUND || status == ESP_ERR_INVALID_STATE;
}

static bool cdc_acm_device_is_gone(cdc_dev_t *cdc_dev)
{
    CDC_ACM_ENTER_CRITICAL();
    bool device_gone = cdc_dev->device_gone;
    CDC_ACM_EXIT_CRITICAL();
    return device_gone;
}

static esp_err_t cdc_acm_reset_transfer_endpoint(cdc_dev_t *cdc_dev,
                                                 usb_transfer_t *transfer)
{
    assert(cdc_dev);
    assert(cdc_dev->dev_hdl);
    assert(transfer);

    esp_err_t ret = usb_host_endpoint_halt(
        cdc_dev->dev_hdl, transfer->bEndpointAddress);
    if (ret != ESP_OK) {
        if (cdc_acm_device_is_gone(cdc_dev) && cdc_acm_status_is_gone(ret)) {
            return ESP_OK;
        }
        return ret;
    }
    ret = usb_host_endpoint_flush(cdc_dev->dev_hdl,
                                  transfer->bEndpointAddress);
    if (ret != ESP_OK) {
        if (cdc_acm_device_is_gone(cdc_dev) && cdc_acm_status_is_gone(ret)) {
            return ESP_OK;
        }
        return ret;
    }
    usb_host_endpoint_clear(cdc_dev->dev_hdl, transfer->bEndpointAddress);
    return ESP_OK;
}

esp_err_t cdc_acm_acquire_device_operation(cdc_acm_dev_hdl_t cdc_hdl,
                                           cdc_dev_t **cdc_dev_ret)
{
    if (cdc_hdl == NULL || cdc_dev_ret == NULL) return ESP_ERR_INVALID_ARG;

    CDC_ACM_ENTER_CRITICAL();
    if (p_cdc_acm_obj == NULL || p_cdc_acm_obj->uninstalling) {
        CDC_ACM_EXIT_CRITICAL();
        return ESP_ERR_INVALID_STATE;
    }
    cdc_dev_t *cdc_dev = NULL;
    SLIST_FOREACH(cdc_dev, &p_cdc_acm_obj->cdc_devices_list, list_entry) {
        if (cdc_dev == (cdc_dev_t *)cdc_hdl) break;
    }
    if (cdc_dev == NULL || cdc_dev->closing || cdc_dev->device_gone) {
        CDC_ACM_EXIT_CRITICAL();
        return ESP_ERR_NOT_FOUND;
    }
    cdc_dev->operation_refs++;
    *cdc_dev_ret = cdc_dev;
    CDC_ACM_EXIT_CRITICAL();
    return ESP_OK;
}

void cdc_acm_release_device_operation(cdc_dev_t *cdc_dev)
{
    assert(cdc_dev != NULL);
    CDC_ACM_ENTER_CRITICAL();
    assert(cdc_dev->operation_refs > 0);
    cdc_dev->operation_refs--;
    CDC_ACM_EXIT_CRITICAL();
}

static esp_err_t cdc_acm_wait_device_operations(cdc_acm_obj_t *cdc_acm_obj,
                                                cdc_dev_t *cdc_dev)
{
    for (size_t attempt = 0; attempt < CDC_ACM_CLOSE_DRAIN_ATTEMPTS; ++attempt) {
        CDC_ACM_ENTER_CRITICAL();
        size_t operation_refs = cdc_dev->operation_refs;
        CDC_ACM_EXIT_CRITICAL();
        if (operation_refs == 0) return ESP_OK;

        esp_err_t ret = usb_host_client_handle_events(
            cdc_acm_obj->cdc_acm_client_hdl, pdMS_TO_TICKS(10));
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) return ret;
        vTaskDelay(1);
    }
    return ESP_ERR_NOT_FINISHED;
}

static void cdc_acm_set_control_state(cdc_dev_t *cdc_dev, bool in_flight,
                                      bool poisoned)
{
    CDC_ACM_ENTER_CRITICAL();
    cdc_dev->ctrl_in_flight = in_flight;
    cdc_dev->ctrl_poisoned = poisoned;
    CDC_ACM_EXIT_CRITICAL();
}

static esp_err_t cdc_acm_admit_control_operation(cdc_acm_dev_hdl_t cdc_hdl,
                                                 cdc_dev_t **cdc_dev_ret)
{
    CDC_ACM_ENTER_CRITICAL();
    if (p_cdc_acm_obj == NULL) {
        CDC_ACM_EXIT_CRITICAL();
        return ESP_ERR_INVALID_STATE;
    }
    cdc_dev_t *cdc_dev = NULL;
    SLIST_FOREACH(cdc_dev, &p_cdc_acm_obj->cdc_devices_list, list_entry) {
        if (cdc_dev == (cdc_dev_t *)cdc_hdl) break;
    }
    if (cdc_dev == NULL || cdc_dev->closing || cdc_dev->device_gone) {
        CDC_ACM_EXIT_CRITICAL();
        return ESP_ERR_NOT_FOUND;
    }
    cdc_dev->ctrl_operation_refs++;
    *cdc_dev_ret = cdc_dev;
    CDC_ACM_EXIT_CRITICAL();
    return ESP_OK;
}

static void cdc_acm_get_control_state(cdc_dev_t *cdc_dev,
                                      size_t *operation_refs,
                                      bool *in_flight, bool *poisoned)
{
    CDC_ACM_ENTER_CRITICAL();
    *operation_refs = cdc_dev->ctrl_operation_refs;
    *in_flight = cdc_dev->ctrl_in_flight;
    *poisoned = cdc_dev->ctrl_poisoned;
    CDC_ACM_EXIT_CRITICAL();
}

static void cdc_acm_end_control_operation(cdc_dev_t *cdc_dev)
{
    CDC_ACM_ENTER_CRITICAL();
    assert(cdc_dev->ctrl_operation_refs > 0);
    cdc_dev->ctrl_operation_refs--;
    CDC_ACM_EXIT_CRITICAL();
}

/**
 * Submit the shared EP0 transfer and do not release it for reuse until its
 * completion callback has been consumed. The caller owns ctrl_mux.
 */
static esp_err_t cdc_acm_submit_control_and_wait(cdc_dev_t *cdc_dev)
{
    SemaphoreHandle_t completion =
        (SemaphoreHandle_t)cdc_dev->ctrl_transfer->context;

    CDC_ACM_ENTER_CRITICAL();
    if (cdc_dev->closing || cdc_dev->ctrl_poisoned ||
            cdc_dev->ctrl_operation_refs == 0 || cdc_dev->ctrl_in_flight) {
        CDC_ACM_EXIT_CRITICAL();
        return ESP_ERR_INVALID_STATE;
    }
    cdc_dev->ctrl_in_flight = true;
    CDC_ACM_EXIT_CRITICAL();

    // A clean transfer has no outstanding completion. Drain defensively before
    // reusing the binary semaphore and shared transfer buffer.
    (void)xSemaphoreTake(completion, 0);
    esp_err_t ret = usb_host_transfer_submit_control(
        p_cdc_acm_obj->cdc_acm_client_hdl, cdc_dev->ctrl_transfer);
    if (ret != ESP_OK) {
        cdc_acm_set_control_state(cdc_dev, false, false);
        return ret;
    }

    if (xSemaphoreTake(completion,
                       pdMS_TO_TICKS(CDC_ACM_CTRL_TIMEOUT_MS)) == pdTRUE) {
        cdc_acm_set_control_state(cdc_dev, false, false);
        return ESP_OK;
    }

    // A timeout does not imply that the callback is finished. Cancel/reset EP0
    // and perform a second bounded drain while ctrl_mux still excludes reuse.
    // Some host implementations reject halt/flush for EP0; in that case we
    // still wait for natural completion and poison the transfer if it is late.
    esp_err_t cancel_result = cdc_acm_reset_transfer_endpoint(
        cdc_dev, cdc_dev->ctrl_transfer);
    if (xSemaphoreTake(completion,
                       pdMS_TO_TICKS(CDC_ACM_TRANSFER_DRAIN_WAIT_MS)) == pdTRUE) {
        cdc_acm_set_control_state(cdc_dev, false, false);
        return ESP_ERR_TIMEOUT;
    }

    cdc_acm_set_control_state(cdc_dev, true, true);
    ESP_LOGE(TAG, "EP0 callback did not drain after timeout (reset=%s)",
             esp_err_to_name(cancel_result));
    return ESP_ERR_NOT_FINISHED;
}

/**
 * Called only from the CDC client task. An ordinary in-flight transfer still
 * has a waiting caller, so the caller consumes its completion. A poisoned
 * transfer has no waiter and is consumed here. Failure retains cdc_dev.
 */
static esp_err_t cdc_acm_drain_control_for_close(cdc_acm_obj_t *cdc_acm_obj,
                                                 cdc_dev_t *cdc_dev)
{
    bool reset_attempted = false;
    SemaphoreHandle_t completion =
        (SemaphoreHandle_t)cdc_dev->ctrl_transfer->context;

    for (size_t attempt = 0; attempt < CDC_ACM_CLOSE_DRAIN_ATTEMPTS; ++attempt) {
        size_t operation_refs = 0;
        bool in_flight = false;
        bool poisoned = false;
        cdc_acm_get_control_state(cdc_dev, &operation_refs, &in_flight,
                                  &poisoned);
        if (operation_refs == 0 && !in_flight) {
            return poisoned ? ESP_ERR_INVALID_STATE : ESP_OK;
        }
        if (operation_refs == 0 && in_flight && !poisoned) {
            return ESP_ERR_NOT_FINISHED;
        }

        if (operation_refs == 0 && poisoned) {
            if (xSemaphoreTake(completion, 0) == pdTRUE) {
                cdc_acm_set_control_state(cdc_dev, false, false);
                return ESP_OK;
            }
            if (!reset_attempted) {
                (void)cdc_acm_reset_transfer_endpoint(cdc_dev,
                                                       cdc_dev->ctrl_transfer);
                reset_attempted = true;
            }
        }

        esp_err_t ret = usb_host_client_handle_events(
            cdc_acm_obj->cdc_acm_client_hdl, pdMS_TO_TICKS(10));
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) return ret;

        if (operation_refs == 0 && poisoned &&
                xSemaphoreTake(completion, 0) == pdTRUE) {
            cdc_acm_set_control_state(cdc_dev, false, false);
            return ESP_OK;
        }

        // Let an ordinary API caller consume its own completion and clear the
        // in-flight flag before this task considers freeing the transfer.
        vTaskDelay(1);
    }
    return ESP_ERR_TIMEOUT;
}

static void cdc_acm_set_out_transfer_state(cdc_dev_t *cdc_dev, bool in_flight,
                                           bool poisoned)
{
    CDC_ACM_ENTER_CRITICAL();
    cdc_dev->data.out_in_flight = in_flight;
    cdc_dev->data.out_poisoned = poisoned;
    CDC_ACM_EXIT_CRITICAL();
}

static esp_err_t cdc_acm_admit_out_operation(cdc_acm_dev_hdl_t cdc_hdl,
                                             cdc_dev_t **cdc_dev_ret)
{
    CDC_ACM_ENTER_CRITICAL();
    if (p_cdc_acm_obj == NULL) {
        CDC_ACM_EXIT_CRITICAL();
        return ESP_ERR_INVALID_STATE;
    }
    cdc_dev_t *cdc_dev = NULL;
    SLIST_FOREACH(cdc_dev, &p_cdc_acm_obj->cdc_devices_list, list_entry) {
        if (cdc_dev == (cdc_dev_t *)cdc_hdl) break;
    }
    if (cdc_dev == NULL || cdc_dev->closing || cdc_dev->device_gone) {
        CDC_ACM_EXIT_CRITICAL();
        return ESP_ERR_NOT_FOUND;
    }
    cdc_dev->data.out_operation_refs++;
    *cdc_dev_ret = cdc_dev;
    CDC_ACM_EXIT_CRITICAL();
    return ESP_OK;
}

static void cdc_acm_end_out_operation(cdc_dev_t *cdc_dev)
{
    CDC_ACM_ENTER_CRITICAL();
    assert(cdc_dev->data.out_operation_refs > 0);
    cdc_dev->data.out_operation_refs--;
    CDC_ACM_EXIT_CRITICAL();
}

static void cdc_acm_get_out_state(cdc_dev_t *cdc_dev, size_t *operation_refs,
                                  bool *in_flight, bool *poisoned)
{
    CDC_ACM_ENTER_CRITICAL();
    *operation_refs = cdc_dev->data.out_operation_refs;
    *in_flight = cdc_dev->data.out_in_flight;
    *poisoned = cdc_dev->data.out_poisoned;
    CDC_ACM_EXIT_CRITICAL();
}

static esp_err_t cdc_acm_submit_out_and_wait(cdc_dev_t *cdc_dev,
                                             TickType_t wait_ticks)
{
    SemaphoreHandle_t completion =
        (SemaphoreHandle_t)cdc_dev->data.out_xfer->context;

    CDC_ACM_ENTER_CRITICAL();
    if (cdc_dev->closing || cdc_dev->data.out_operation_refs == 0 ||
            cdc_dev->data.out_in_flight || cdc_dev->data.out_poisoned) {
        CDC_ACM_EXIT_CRITICAL();
        return ESP_ERR_INVALID_STATE;
    }
    cdc_dev->data.out_in_flight = true;
    CDC_ACM_EXIT_CRITICAL();

    (void)xSemaphoreTake(completion, 0);
    esp_err_t ret = usb_host_transfer_submit(cdc_dev->data.out_xfer);
    if (ret != ESP_OK) {
        cdc_acm_set_out_transfer_state(cdc_dev, false, false);
        return ret;
    }

    if (xSemaphoreTake(completion, wait_ticks) == pdTRUE) {
        cdc_acm_set_out_transfer_state(cdc_dev, false, false);
        return ESP_OK;
    }

    esp_err_t cancel_result = cdc_acm_reset_transfer_endpoint(
        cdc_dev, cdc_dev->data.out_xfer);
    if (xSemaphoreTake(completion,
                       pdMS_TO_TICKS(CDC_ACM_TRANSFER_DRAIN_WAIT_MS)) == pdTRUE) {
        cdc_acm_set_out_transfer_state(cdc_dev, false, false);
        return ESP_ERR_TIMEOUT;
    }

    cdc_acm_set_out_transfer_state(cdc_dev, true, true);
    ESP_LOGE(TAG, "bulk OUT callback did not drain after timeout (reset=%s)",
             esp_err_to_name(cancel_result));
    return ESP_ERR_NOT_FINISHED;
}

static esp_err_t cdc_acm_drain_out_for_close(cdc_acm_obj_t *cdc_acm_obj,
                                             cdc_dev_t *cdc_dev)
{
    if (cdc_dev->data.out_xfer == NULL) return ESP_OK;

    bool reset_attempted = false;
    SemaphoreHandle_t completion =
        (SemaphoreHandle_t)cdc_dev->data.out_xfer->context;
    for (size_t attempt = 0; attempt < CDC_ACM_CLOSE_DRAIN_ATTEMPTS; ++attempt) {
        size_t operation_refs = 0;
        bool in_flight = false;
        bool poisoned = false;
        cdc_acm_get_out_state(cdc_dev, &operation_refs, &in_flight,
                              &poisoned);
        if (operation_refs == 0 && !in_flight) {
            return poisoned ? ESP_ERR_INVALID_STATE : ESP_OK;
        }
        if (operation_refs == 0 && in_flight && !poisoned) {
            return ESP_ERR_NOT_FINISHED;
        }

        if (operation_refs == 0 && in_flight && poisoned) {
            if (xSemaphoreTake(completion, 0) == pdTRUE) {
                cdc_acm_set_out_transfer_state(cdc_dev, false, false);
                return ESP_OK;
            }
            if (!reset_attempted) {
                (void)cdc_acm_reset_transfer_endpoint(cdc_dev,
                                                       cdc_dev->data.out_xfer);
                reset_attempted = true;
            }
        }

        esp_err_t ret = usb_host_client_handle_events(
            cdc_acm_obj->cdc_acm_client_hdl, pdMS_TO_TICKS(10));
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) return ret;

        if (operation_refs == 0 && in_flight && poisoned &&
                xSemaphoreTake(completion, 0) == pdTRUE) {
            cdc_acm_set_out_transfer_state(cdc_dev, false, false);
            return ESP_OK;
        }
        vTaskDelay(1);
    }
    return ESP_ERR_TIMEOUT;
}

static void cdc_acm_set_data_poll_in_flight(cdc_dev_t *cdc_dev, bool in_flight)
{
    CDC_ACM_ENTER_CRITICAL();
    cdc_dev->data.data_poll_in_flight = in_flight;
    CDC_ACM_EXIT_CRITICAL();
}

static void cdc_acm_set_notif_poll_in_flight(cdc_dev_t *cdc_dev, bool in_flight)
{
    CDC_ACM_ENTER_CRITICAL();
    cdc_dev->notif.notif_poll_in_flight = in_flight;
    CDC_ACM_EXIT_CRITICAL();
}

static void cdc_acm_data_callback_enter(cdc_dev_t *cdc_dev)
{
    CDC_ACM_ENTER_CRITICAL();
    cdc_dev->data.data_poll_in_flight = false;
    cdc_dev->data.data_callback_active++;
    CDC_ACM_EXIT_CRITICAL();
}

static void cdc_acm_data_callback_exit(cdc_dev_t *cdc_dev)
{
    CDC_ACM_ENTER_CRITICAL();
    assert(cdc_dev->data.data_callback_active > 0);
    cdc_dev->data.data_callback_active--;
    CDC_ACM_EXIT_CRITICAL();
}

static void cdc_acm_notif_callback_enter(cdc_dev_t *cdc_dev)
{
    CDC_ACM_ENTER_CRITICAL();
    cdc_dev->notif.notif_poll_in_flight = false;
    cdc_dev->notif.notif_callback_active++;
    CDC_ACM_EXIT_CRITICAL();
}

static void cdc_acm_notif_callback_exit(cdc_dev_t *cdc_dev)
{
    CDC_ACM_ENTER_CRITICAL();
    assert(cdc_dev->notif.notif_callback_active > 0);
    cdc_dev->notif.notif_callback_active--;
    CDC_ACM_EXIT_CRITICAL();
}

static void cdc_acm_snapshot_data_callbacks(
    cdc_dev_t *cdc_dev, cdc_acm_data_callback_t *data_cb,
    cdc_acm_host_dev_callback_t *event_cb, void **cb_arg)
{
    CDC_ACM_ENTER_CRITICAL();
    *data_cb = cdc_dev->data.in_cb;
    *event_cb = cdc_dev->notif.cb;
    *cb_arg = cdc_dev->cb_arg;
    CDC_ACM_EXIT_CRITICAL();
}

static void cdc_acm_snapshot_event_callback(
    cdc_dev_t *cdc_dev, cdc_acm_host_dev_callback_t *event_cb, void **cb_arg)
{
    CDC_ACM_ENTER_CRITICAL();
    *event_cb = cdc_dev->notif.cb;
    *cb_arg = cdc_dev->cb_arg;
    CDC_ACM_EXIT_CRITICAL();
}

static void cdc_acm_disable_user_callbacks(cdc_dev_t *cdc_dev)
{
    CDC_ACM_ENTER_CRITICAL();
    cdc_dev->data.in_cb = NULL;
    cdc_dev->notif.cb = NULL;
    cdc_dev->cb_arg = NULL;
    CDC_ACM_EXIT_CRITICAL();
}

static esp_err_t cdc_acm_enable_user_callbacks(
    cdc_dev_t *cdc_dev, cdc_acm_host_dev_callback_t event_cb,
    cdc_acm_data_callback_t data_cb, void *cb_arg)
{
    CDC_ACM_ENTER_CRITICAL();
    esp_err_t ret = cdc_dev->poll_error;
    if (ret == ESP_OK) {
        // Publishing the list entry and callbacks in one critical section
        // ensures every admitted callback observes a close-trackable device.
        SLIST_INSERT_HEAD(&p_cdc_acm_obj->cdc_devices_list, cdc_dev, list_entry);
        cdc_dev->cb_arg = cb_arg;
        cdc_dev->data.in_cb = data_cb;
        cdc_dev->notif.cb = event_cb;
    }
    CDC_ACM_EXIT_CRITICAL();
    return ret;
}

static void cdc_acm_report_poll_error(cdc_dev_t *cdc_dev, esp_err_t error)
{
    cdc_acm_host_dev_callback_t event_cb = NULL;
    void *cb_arg = NULL;
    CDC_ACM_ENTER_CRITICAL();
    if (cdc_dev->closing || cdc_dev->close_prepared || cdc_dev->device_gone) {
        CDC_ACM_EXIT_CRITICAL();
        return;
    }
    if (cdc_dev->poll_error == ESP_OK) cdc_dev->poll_error = error;
    event_cb = cdc_dev->notif.cb;
    cb_arg = cdc_dev->cb_arg;
    CDC_ACM_EXIT_CRITICAL();

    if (event_cb != NULL) {
        const cdc_acm_host_dev_event_data_t error_event = {
            .type = CDC_ACM_HOST_ERROR,
            .data.error = (int)error,
        };
        event_cb(&error_event, cb_arg);
    }
}

static esp_err_t cdc_acm_submit_data_poll(cdc_dev_t *cdc_dev)
{
    CDC_ACM_ENTER_CRITICAL();
    if (cdc_dev->closing || cdc_dev->close_prepared || cdc_dev->device_gone ||
            cdc_dev->data.data_poll_in_flight) {
        CDC_ACM_EXIT_CRITICAL();
        return ESP_ERR_INVALID_STATE;
    }
    cdc_dev->data.data_poll_in_flight = true;
    CDC_ACM_EXIT_CRITICAL();

    esp_err_t ret = usb_host_transfer_submit(cdc_dev->data.in_xfer);
    if (ret != ESP_OK) cdc_acm_set_data_poll_in_flight(cdc_dev, false);
    return ret;
}

static esp_err_t cdc_acm_submit_notif_poll(cdc_dev_t *cdc_dev)
{
    CDC_ACM_ENTER_CRITICAL();
    if (cdc_dev->closing || cdc_dev->close_prepared || cdc_dev->device_gone ||
            cdc_dev->notif.notif_poll_in_flight) {
        CDC_ACM_EXIT_CRITICAL();
        return ESP_ERR_INVALID_STATE;
    }
    cdc_dev->notif.notif_poll_in_flight = true;
    CDC_ACM_EXIT_CRITICAL();

    esp_err_t ret = usb_host_transfer_submit(cdc_dev->notif.xfer);
    if (ret != ESP_OK) cdc_acm_set_notif_poll_in_flight(cdc_dev, false);
    return ret;
}

static void cdc_acm_get_poll_state(cdc_dev_t *cdc_dev, bool *data_in_flight,
                                   bool *notif_in_flight,
                                   size_t *data_callback_active,
                                   size_t *notif_callback_active)
{
    CDC_ACM_ENTER_CRITICAL();
    *data_in_flight = cdc_dev->data.data_poll_in_flight;
    *notif_in_flight = cdc_dev->notif.notif_poll_in_flight;
    *data_callback_active = cdc_dev->data.data_callback_active;
    *notif_callback_active = cdc_dev->notif.notif_callback_active;
    CDC_ACM_EXIT_CRITICAL();
}

static esp_err_t cdc_acm_drain_periodic_for_close(
    cdc_acm_obj_t *cdc_acm_obj, cdc_dev_t *cdc_dev)
{
    for (size_t attempt = 0; attempt < CDC_ACM_CLOSE_DRAIN_ATTEMPTS; ++attempt) {
        bool data_in_flight = false;
        bool notif_in_flight = false;
        size_t data_callback_active = 0;
        size_t notif_callback_active = 0;
        cdc_acm_get_poll_state(cdc_dev, &data_in_flight, &notif_in_flight,
                               &data_callback_active,
                               &notif_callback_active);
        if (!data_in_flight && !notif_in_flight &&
                data_callback_active == 0 && notif_callback_active == 0) {
            return ESP_OK;
        }

        // Transfer storage stays list-owned until the USB host has delivered
        // every real callback, including NO_DEVICE/CANCELED completions.
        esp_err_t ret = usb_host_client_handle_events(
            cdc_acm_obj->cdc_acm_client_hdl, pdMS_TO_TICKS(10));
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) return ret;
    }
    return ESP_ERR_NOT_FINISHED;
}

static esp_err_t cdc_acm_cancel_started_transfers(cdc_dev_t *cdc_dev)
{
    cdc_acm_disable_user_callbacks(cdc_dev);
    CDC_ACM_ENTER_CRITICAL();
    cdc_dev->close_prepared = true;
    bool data_in_flight = cdc_dev->data.data_poll_in_flight;
    bool notif_in_flight = cdc_dev->notif.notif_poll_in_flight;
    size_t data_callback_active = cdc_dev->data.data_callback_active;
    size_t notif_callback_active = cdc_dev->notif.notif_callback_active;
    CDC_ACM_EXIT_CRITICAL();

    if (data_in_flight) {
        (void)cdc_acm_reset_transfer_endpoint(cdc_dev,
                                               cdc_dev->data.in_xfer);
    }
    if (notif_in_flight) {
        (void)cdc_acm_reset_transfer_endpoint(cdc_dev,
                                               cdc_dev->notif.xfer);
    }

    // cdc_acm_start() runs in the opener task. The dedicated CDC task owns
    // usb_host_client_handle_events(), so wait for that task to retire the
    // canceled callbacks instead of pumping the client concurrently here.
    for (size_t attempt = 0; attempt < CDC_ACM_CLOSE_DRAIN_ATTEMPTS; ++attempt) {
        cdc_acm_get_poll_state(cdc_dev, &data_in_flight, &notif_in_flight,
                               &data_callback_active,
                               &notif_callback_active);
        if (!data_in_flight && !notif_in_flight &&
                data_callback_active == 0 && notif_callback_active == 0) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_ERR_NOT_FINISHED;
}

/**
 * @brief Start CDC device
 *
 * After this call, USB host peripheral will continuously poll IN endpoints.
 *
 * @param cdc_dev
 * @param[in] event_cb  Device event callback
 * @param[in] in_cb     Data received callback
 * @param[in] user_arg  Optional user's argument, that will be passed to the callbacks
 * @return esp_err_t
 */
static esp_err_t cdc_acm_start(cdc_dev_t *cdc_dev, cdc_acm_host_dev_callback_t event_cb, cdc_acm_data_callback_t in_cb, void *user_arg)
{
    esp_err_t ret = ESP_OK;
    bool data_interface_claimed = false;
    bool notif_interface_claimed = false;
    bool data_poll_submitted = false;
    bool notif_poll_submitted = false;
    assert(cdc_dev);

    // Claim every interface before submitting any transfer. This keeps every
    // ordinary open-error path free of in-flight callbacks and makes it safe
    // for cdc_acm_host_open() to release user_arg after this function returns.
    ESP_GOTO_ON_ERROR(
        usb_host_interface_claim(
            p_cdc_acm_obj->cdc_acm_client_hdl,
            cdc_dev->dev_hdl,
            cdc_dev->data.intf_desc->bInterfaceNumber,
            cdc_dev->data.intf_desc->bAlternateSetting),
        err, TAG, "Could not claim interface");
    data_interface_claimed = true;

    if (cdc_dev->notif.xfer &&
            cdc_dev->notif.intf_desc != cdc_dev->data.intf_desc) {
        ESP_GOTO_ON_ERROR(
            usb_host_interface_claim(
                p_cdc_acm_obj->cdc_acm_client_hdl,
                cdc_dev->dev_hdl,
                cdc_dev->notif.intf_desc->bInterfaceNumber,
                cdc_dev->notif.intf_desc->bAlternateSetting),
            err, TAG, "Could not claim notification interface");
        notif_interface_claimed = true;
    }

    if (cdc_dev->data.in_xfer) {
        ESP_LOGD(TAG, "Submitting poll for BULK IN transfer");
        ret = cdc_acm_submit_data_poll(cdc_dev);
        if (ret != ESP_OK) goto err;
        data_poll_submitted = true;
    }
    if (cdc_dev->notif.xfer) {
        ESP_LOGD(TAG, "Submitting poll for INTR IN transfer");
        ret = cdc_acm_submit_notif_poll(cdc_dev);
        if (ret != ESP_OK) goto err;
        notif_poll_submitted = true;
    }

    // Only publish user_arg after every initial poll is live. A failed open
    // therefore returns with callbacks atomically disabled; a concurrent
    // resubmit failure either prevents this commit or sees the published
    // event callback and reports a terminal error.
    ret = cdc_acm_enable_user_callbacks(cdc_dev, event_cb, in_cb, user_arg);
    if (ret != ESP_OK) goto err;
    return ret;

err:
    {
        esp_err_t start_error = ret;
        esp_err_t cleanup_result = cdc_acm_cancel_started_transfers(cdc_dev);
        if (!data_interface_claimed) {
            cdc_dev->data_interface_released = true;
        }
        if (cdc_dev->notif.intf_desc != cdc_dev->data.intf_desc &&
                !notif_interface_claimed) {
            cdc_dev->notif_interface_released = true;
        }

        // Once an interface was claimed, only a successful interface_release
        // in the CDC task proves the USB host has fully unwound callbacks.
        // Transfer list ownership even if our active counters already reached
        // zero; the opener must never free such a transfer directly.
        if (data_poll_submitted || notif_poll_submitted ||
                data_interface_claimed || notif_interface_claimed) {
            bool poll_was_submitted =
                data_poll_submitted || notif_poll_submitted;
            (void)cdc_acm_schedule_internal_close(cdc_dev,
                                                   poll_was_submitted);
            return cleanup_result == ESP_OK ? start_error : cleanup_result;
        }
        ret = cleanup_result == ESP_OK ? start_error : cleanup_result;
    }
    return ret;
}

static void cdc_acm_transfers_free(cdc_dev_t *cdc_dev);
/**
 * @brief Helper function that releases resources claimed by CDC device
 *
 * Close underlying USB device, free device driver memory
 *
 * @note All interfaces claimed by this device must be release before calling this function
 * @param cdc_dev CDC device handle to be removed
 */
static void cdc_acm_device_remove(cdc_dev_t *cdc_dev)
{
    assert(cdc_dev);
    if (cdc_dev->intf_func.del) {
        cdc_dev->intf_func.del(cdc_dev);
    }
    cdc_acm_transfers_free(cdc_dev);
    free(cdc_dev->cdc_func_desc);
    // We don't check the error code of usb_host_device_close, as the close might fail, if someone else is still using the device (not all interfaces are released)
    usb_host_device_close(p_cdc_acm_obj->cdc_acm_client_hdl, cdc_dev->dev_hdl); // Gracefully continue on error
    free(cdc_dev);
}

/**
 * @brief Open USB device with requested VID/PID
 *
 * This function has two regular return paths:
 * 1. USB device with matching VID/PID is already opened by this driver: allocate new CDC device on top of the already opened USB device.
 * 2. USB device with matching VID/PID is NOT opened by this driver yet: poll USB connected devices until it is found.
 *
 * @note This function will block for timeout_ms, if the device is not enumerated at the moment of calling this function.
 * @param[in] vid Vendor ID
 * @param[in] pid Product ID
 * @param[in] timeout_ms Connection timeout [ms]
 * @param[out] dev CDC-ACM device
 * @return esp_err_t
 */
static bool cdc_acm_snapshot_open_candidate(
    cdc_dev_t *cdc_dev, usb_device_handle_t *dev_hdl)
{
    CDC_ACM_ENTER_CRITICAL();
    bool available = !cdc_dev->closing && !cdc_dev->device_gone;
    *dev_hdl = available ? cdc_dev->dev_hdl : NULL;
    CDC_ACM_EXIT_CRITICAL();
    return available;
}

static esp_err_t cdc_acm_find_and_open_usb_device(uint16_t vid, uint16_t pid, int timeout_ms, cdc_dev_t **dev)
{
    assert(p_cdc_acm_obj);
    assert(dev);

    *dev = calloc(1, sizeof(cdc_dev_t));
    if (*dev == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // First, check list of already opened CDC devices
    ESP_LOGD(TAG, "Checking list of opened USB devices");
    cdc_dev_t *cdc_dev;
    SLIST_FOREACH(cdc_dev, &p_cdc_acm_obj->cdc_devices_list, list_entry) {
        usb_device_handle_t candidate_handle = NULL;
        if (!cdc_acm_snapshot_open_candidate(cdc_dev, &candidate_handle)) {
            continue;
        }
        const usb_device_desc_t *device_desc = NULL;
        if (usb_host_get_device_descriptor(candidate_handle,
                                           &device_desc) != ESP_OK) {
            continue;
        }
        if ((vid == device_desc->idVendor || vid == CDC_HOST_ANY_VID) &&
                (pid == device_desc->idProduct || pid == CDC_HOST_ANY_PID)) {
            // Return path 1:
            (*dev)->dev_hdl = candidate_handle;
            return ESP_OK;
        }
    }

    // Second, poll connected devices until new device is connected or timeout
    TickType_t timeout_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    TimeOut_t connection_timeout;
    vTaskSetTimeOutState(&connection_timeout);

    do {
        ESP_LOGD(TAG, "Checking list of connected USB devices");
        uint8_t dev_addr_list[10];
        int num_of_devices;
        esp_err_t list_result = usb_host_device_addr_list_fill(
            sizeof(dev_addr_list), dev_addr_list, &num_of_devices);
        if (list_result != ESP_OK) {
            free(*dev);
            *dev = NULL;
            return list_result;
        }

        // Go through device address list and find the one we are looking for
        for (int i = 0; i < num_of_devices; i++) {
            usb_device_handle_t current_device;
            // Open USB device
            if (usb_host_device_open(p_cdc_acm_obj->cdc_acm_client_hdl, dev_addr_list[i], &current_device) == ESP_OK) {
                assert(current_device);
                const usb_device_desc_t *device_desc = NULL;
                if (usb_host_get_device_descriptor(current_device,
                                                    &device_desc) != ESP_OK) {
                    usb_host_device_close(
                        p_cdc_acm_obj->cdc_acm_client_hdl, current_device);
                    continue;
                }
                if ((device_desc->bDeviceClass != USB_CLASS_HUB) &&
                        (vid == device_desc->idVendor || vid == CDC_HOST_ANY_VID) &&
                        (pid == device_desc->idProduct || pid == CDC_HOST_ANY_PID)) {
                    // Return path 2:
                    (*dev)->dev_hdl = current_device;
                    return ESP_OK;
                }
                usb_host_device_close(p_cdc_acm_obj->cdc_acm_client_hdl, current_device);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    } while (xTaskCheckForTimeOut(&connection_timeout, &timeout_ticks) == pdFALSE);

    // Timeout was reached, clean-up
    free(*dev);
    *dev = NULL;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t cdc_acm_host_install(const cdc_acm_host_driver_config_t *driver_config)
{
    if (!cdc_acm_lifetime_lock()) return ESP_ERR_NO_MEM;
    if (p_cdc_acm_obj != NULL) {
        cdc_acm_lifetime_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    // Check driver configuration, use default if NULL is passed
    if (driver_config == NULL) {
        driver_config = &cdc_acm_driver_config_default;
    }

    // Allocate all we need for this driver
    esp_err_t ret;
    cdc_acm_obj_t *cdc_acm_obj = heap_caps_calloc(1, sizeof(cdc_acm_obj_t), MALLOC_CAP_DEFAULT);
    EventGroupHandle_t event_group = xEventGroupCreate();
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    QueueHandle_t close_request_queue = xQueueCreate(
        CDC_ACM_CLOSE_QUEUE_DEPTH, sizeof(cdc_acm_close_request_t *));
    TaskHandle_t driver_task_h = NULL;
    xTaskCreatePinnedToCore(
        cdc_acm_client_task, "USB-CDC", driver_config->driver_task_stack_size, NULL,
        driver_config->driver_task_priority, &driver_task_h, driver_config->xCoreID);

    if (cdc_acm_obj == NULL || driver_task_h == NULL || event_group == NULL ||
            mutex == NULL || close_request_queue == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto err;
    }

    // Register USB Host client
    usb_host_client_handle_t usb_client = NULL;
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 3,
        .async.client_event_callback = usb_event_cb,
        .async.callback_arg = NULL
    };
    ESP_GOTO_ON_ERROR(usb_host_client_register(&client_config, &usb_client), err, TAG, "Failed to register USB host client");

    // Initialize CDC-ACM driver structure
    SLIST_INIT(&(cdc_acm_obj->cdc_devices_list));
    cdc_acm_obj->event_group = event_group;
    cdc_acm_obj->open_close_mutex = mutex;
    cdc_acm_obj->close_request_queue = close_request_queue;
    cdc_acm_obj->driver_task_h = driver_task_h;
    cdc_acm_obj->cdc_acm_client_hdl = usb_client;
    cdc_acm_obj->new_dev_cb = driver_config->new_dev_cb;

    // Between 1st call of this function and following section, another task might try to install this driver:
    // Make sure that there is only one instance of this driver in the system
    CDC_ACM_ENTER_CRITICAL();
    if (p_cdc_acm_obj) {
        // Already created
        ret = ESP_ERR_INVALID_STATE;
        CDC_ACM_EXIT_CRITICAL();
        goto client_err;
    } else {
        p_cdc_acm_obj = cdc_acm_obj;
    }
    CDC_ACM_EXIT_CRITICAL();

    // Everything OK: Start CDC-Driver task and return
    xTaskNotifyGive(driver_task_h);
    cdc_acm_lifetime_unlock();
    return ESP_OK;

client_err:
    usb_host_client_deregister(usb_client);
err: // Clean-up
    free(cdc_acm_obj);
    if (event_group) {
        vEventGroupDelete(event_group);
    }
    if (driver_task_h) {
        vTaskDelete(driver_task_h);
    }
    if (mutex) {
        vSemaphoreDelete(mutex);
    }
    if (close_request_queue) {
        vQueueDelete(close_request_queue);
    }
    cdc_acm_lifetime_unlock();
    return ret;
}

esp_err_t cdc_acm_host_uninstall()
{
    esp_err_t ret = ESP_OK;
    if (!cdc_acm_lifetime_lock()) return ESP_ERR_NO_MEM;

    CDC_ACM_ENTER_CRITICAL();
    if (p_cdc_acm_obj == NULL) {
        CDC_ACM_EXIT_CRITICAL();
        cdc_acm_lifetime_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    cdc_acm_obj_t *cdc_acm_obj = p_cdc_acm_obj; // Save Driver's handle to temporary handle
    CDC_ACM_EXIT_CRITICAL();

    xSemaphoreTake(cdc_acm_obj->open_close_mutex, portMAX_DELAY); // Wait for all open/close calls to finish

    CDC_ACM_ENTER_CRITICAL();
    if (!cdc_acm_obj->uninstalling &&
            !SLIST_EMPTY(&cdc_acm_obj->cdc_devices_list)) {
        ret = ESP_ERR_INVALID_STATE;
        CDC_ACM_EXIT_CRITICAL();
        goto unblock;
    }
    bool start_teardown = !cdc_acm_obj->uninstalling;
    cdc_acm_obj->uninstalling = true;
    CDC_ACM_EXIT_CRITICAL();

    // Keep p_cdc_acm_obj alive until the client task has actually joined. A
    // timed-out caller may safely retry without losing the object pointer.
    if (start_teardown) {
        xEventGroupClearBits(cdc_acm_obj->event_group,
                             CDC_ACM_TEARDOWN_COMPLETE);
        CDC_ACM_ENTER_CRITICAL();
        cdc_acm_obj->teardown_result = ESP_ERR_NOT_FINISHED;
        cdc_acm_obj->task_quiescent = false;
        CDC_ACM_EXIT_CRITICAL();
        xEventGroupSetBits(cdc_acm_obj->event_group, CDC_ACM_TEARDOWN);
        usb_host_client_unblock(cdc_acm_obj->cdc_acm_client_hdl);
    }
    EventBits_t bits = xEventGroupWaitBits(
        cdc_acm_obj->event_group, CDC_ACM_TEARDOWN_COMPLETE, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(CDC_ACM_UNINSTALL_WAIT_MS));
    if ((bits & CDC_ACM_TEARDOWN_COMPLETE) == 0) {
        ret = ESP_ERR_NOT_FINISHED;
        goto unblock;
    }

    CDC_ACM_ENTER_CRITICAL();
    esp_err_t teardown_result = cdc_acm_obj->teardown_result;
    bool task_quiescent = cdc_acm_obj->task_quiescent;
    CDC_ACM_EXIT_CRITICAL();
    if (teardown_result != ESP_OK) {
        // Deregistration failed and the client task is still alive. Roll back
        // admission and clear this attempt's completion before retrying.
        xEventGroupClearBits(cdc_acm_obj->event_group,
                             CDC_ACM_TEARDOWN_COMPLETE);
        CDC_ACM_ENTER_CRITICAL();
        cdc_acm_obj->uninstalling = false;
        CDC_ACM_EXIT_CRITICAL();
        ret = teardown_result;
        goto unblock;
    }
    if (!task_quiescent) {
        ret = ESP_ERR_NOT_FINISHED;
        goto unblock;
    }
    vTaskDelete(cdc_acm_obj->driver_task_h);
    cdc_acm_obj->driver_task_h = NULL;

    CDC_ACM_ENTER_CRITICAL();
    if (p_cdc_acm_obj == cdc_acm_obj) p_cdc_acm_obj = NULL;
    CDC_ACM_EXIT_CRITICAL();

    // Free remaining resources and return
    vEventGroupDelete(cdc_acm_obj->event_group);
    vQueueDelete(cdc_acm_obj->close_request_queue);
    xSemaphoreGive(cdc_acm_obj->open_close_mutex);
    vSemaphoreDelete(cdc_acm_obj->open_close_mutex);
    free(cdc_acm_obj);
    cdc_acm_lifetime_unlock();
    return ESP_OK;

unblock:
    xSemaphoreGive(cdc_acm_obj->open_close_mutex);
    cdc_acm_lifetime_unlock();
    return ret;
}

esp_err_t cdc_acm_host_register_new_dev_callback(cdc_acm_new_dev_callback_t new_dev_cb)
{
    if (!cdc_acm_lifetime_lock()) return ESP_ERR_NO_MEM;
    CDC_ACM_ENTER_CRITICAL();
    if (p_cdc_acm_obj == NULL || p_cdc_acm_obj->uninstalling) {
        CDC_ACM_EXIT_CRITICAL();
        cdc_acm_lifetime_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    p_cdc_acm_obj->new_dev_cb = new_dev_cb;
    CDC_ACM_EXIT_CRITICAL();
    cdc_acm_lifetime_unlock();
    return ESP_OK;
}

/**
 * @brief Free USB transfers used by this device
 *
 * @note There can be no transfers in flight, at the moment of calling this function.
 * @param[in] cdc_dev Pointer to CDC device
 */
static void cdc_acm_transfers_free(cdc_dev_t *cdc_dev)
{
    assert(cdc_dev);
    assert(cdc_dev->data.data_poll_in_flight == false);
    assert(cdc_dev->data.data_callback_active == 0);
    assert(cdc_dev->notif.notif_poll_in_flight == false);
    assert(cdc_dev->notif.notif_callback_active == 0);
    assert(cdc_dev->operation_refs == 0);
    assert(cdc_dev->data.out_operation_refs == 0);
    assert(cdc_dev->data.out_in_flight == false);
    assert(cdc_dev->ctrl_operation_refs == 0);
    assert(cdc_dev->ctrl_in_flight == false);
    if (cdc_dev->notif.xfer != NULL) {
        usb_host_transfer_free(cdc_dev->notif.xfer);
    }
    if (cdc_dev->data.in_xfer != NULL) {
        cdc_acm_reset_in_transfer(cdc_dev);
        usb_host_transfer_free(cdc_dev->data.in_xfer);
    }
    if (cdc_dev->data.out_xfer != NULL) {
        if (cdc_dev->data.out_xfer->context != NULL) {
            vSemaphoreDelete((SemaphoreHandle_t)cdc_dev->data.out_xfer->context);
        }
        if (cdc_dev->data.out_mux != NULL) {
            vSemaphoreDelete(cdc_dev->data.out_mux);
        }
        usb_host_transfer_free(cdc_dev->data.out_xfer);
    }
    if (cdc_dev->ctrl_transfer != NULL) {
        if (cdc_dev->ctrl_transfer->context != NULL) {
            vSemaphoreDelete((SemaphoreHandle_t)cdc_dev->ctrl_transfer->context);
        }
        if (cdc_dev->ctrl_mux != NULL) {
            vSemaphoreDelete(cdc_dev->ctrl_mux);
        }
        usb_host_transfer_free(cdc_dev->ctrl_transfer);
    }
}

/**
 * @brief Allocate CDC transfers
 *
 * @param[in] cdc_dev       Pointer to CDC device
 * @param[in] notif_ep_desc Pointer to notification EP descriptor
 * @param[in] in_ep_desc-   Pointer to data IN EP descriptor
 * @param[in] in_buf_len    Length of data IN buffer
 * @param[in] out_ep_desc   Pointer to data OUT EP descriptor
 * @param[in] out_buf_len   Length of data OUT buffer
 * @return
 *     - ESP_OK:            Success
 *     - ESP_ERR_NO_MEM:    Not enough memory for transfers and semaphores allocation
 *     - ESP_ERR_NOT_FOUND: IN or OUT endpoints were not found in the selected interface
 */
static esp_err_t cdc_acm_transfers_allocate(cdc_dev_t *cdc_dev, const usb_ep_desc_t *notif_ep_desc, const usb_ep_desc_t *in_ep_desc, size_t in_buf_len, const usb_ep_desc_t *out_ep_desc, size_t out_buf_len)
{
    assert(in_ep_desc);
    assert(out_ep_desc);
    esp_err_t ret;

    // 1. Setup notification transfer if it is supported
    if (notif_ep_desc) {
        ESP_GOTO_ON_ERROR(
            usb_host_transfer_alloc(USB_EP_DESC_GET_MPS(notif_ep_desc), 0, &cdc_dev->notif.xfer),
            err, TAG,);
        cdc_dev->notif.xfer->device_handle = cdc_dev->dev_hdl;
        cdc_dev->notif.xfer->bEndpointAddress = notif_ep_desc->bEndpointAddress;
        cdc_dev->notif.xfer->callback = notif_xfer_cb;
        cdc_dev->notif.xfer->context = cdc_dev;
        cdc_dev->notif.xfer->num_bytes = USB_EP_DESC_GET_MPS(notif_ep_desc);
    }

    // 2. Setup control transfer
    ESP_GOTO_ON_ERROR(
        usb_host_transfer_alloc(CDC_ACM_CTRL_TRANSFER_SIZE, 0, &cdc_dev->ctrl_transfer),
        err, TAG,);
    cdc_dev->ctrl_transfer->timeout_ms = 1000;
    cdc_dev->ctrl_transfer->bEndpointAddress = 0;
    cdc_dev->ctrl_transfer->device_handle = cdc_dev->dev_hdl;
    cdc_dev->ctrl_transfer->callback = out_xfer_cb;
    cdc_dev->ctrl_transfer->context = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(cdc_dev->ctrl_transfer->context, ESP_ERR_NO_MEM, err, TAG,);
    cdc_dev->ctrl_mux = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(cdc_dev->ctrl_mux, ESP_ERR_NO_MEM, err, TAG,);

    // 3. Setup IN data transfer (if it is required (in_buf_len > 0))
    if (in_buf_len != 0) {
        ESP_GOTO_ON_ERROR(
            usb_host_transfer_alloc(in_buf_len, 0, &cdc_dev->data.in_xfer),
            err, TAG,
        );
        assert(cdc_dev->data.in_xfer);
        cdc_dev->data.in_xfer->callback = in_xfer_cb;
        cdc_dev->data.in_xfer->num_bytes = in_buf_len;
        cdc_dev->data.in_xfer->bEndpointAddress = in_ep_desc->bEndpointAddress;
        cdc_dev->data.in_xfer->device_handle = cdc_dev->dev_hdl;
        cdc_dev->data.in_xfer->context = cdc_dev;
        cdc_dev->data.in_mps = USB_EP_DESC_GET_MPS(in_ep_desc);
        cdc_dev->data.in_data_buffer_base = cdc_dev->data.in_xfer->data_buffer;
    }

    // 4. Setup OUT bulk transfer (if it is required (out_buf_len > 0))
    if (out_buf_len != 0) {
        ESP_GOTO_ON_ERROR(
            usb_host_transfer_alloc(out_buf_len, 0, &cdc_dev->data.out_xfer),
            err, TAG,
        );
        assert(cdc_dev->data.out_xfer);
        cdc_dev->data.out_xfer->device_handle = cdc_dev->dev_hdl;
        cdc_dev->data.out_xfer->context = xSemaphoreCreateBinary();
        ESP_GOTO_ON_FALSE(cdc_dev->data.out_xfer->context, ESP_ERR_NO_MEM, err, TAG,);
        cdc_dev->data.out_mux = xSemaphoreCreateMutex();
        ESP_GOTO_ON_FALSE(cdc_dev->data.out_mux, ESP_ERR_NO_MEM, err, TAG,);
        cdc_dev->data.out_xfer->bEndpointAddress = out_ep_desc->bEndpointAddress;
        cdc_dev->data.out_xfer->callback = out_xfer_cb;
    }
    return ESP_OK;

err:
    cdc_acm_transfers_free(cdc_dev);
    return ret;
}

esp_err_t cdc_acm_host_open(uint16_t vid, uint16_t pid, uint8_t interface_idx, const cdc_acm_host_device_config_t *dev_config, cdc_acm_dev_hdl_t *cdc_hdl_ret)
{
    esp_err_t ret;
    CDC_ACM_CHECK(dev_config, ESP_ERR_INVALID_ARG);
    CDC_ACM_CHECK(cdc_hdl_ret, ESP_ERR_INVALID_ARG);
    *cdc_hdl_ret = NULL;
    if (!cdc_acm_lifetime_lock()) return ESP_ERR_NO_MEM;

    CDC_ACM_ENTER_CRITICAL();
    cdc_acm_obj_t *cdc_acm_obj = p_cdc_acm_obj;
    bool driver_available = cdc_acm_obj != NULL && !cdc_acm_obj->uninstalling;
    CDC_ACM_EXIT_CRITICAL();
    if (!driver_available) {
        cdc_acm_lifetime_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(cdc_acm_obj->open_close_mutex, portMAX_DELAY);
    if (cdc_acm_obj->uninstalling) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    // Find underlying USB device
    cdc_dev_t *cdc_dev;
    ret =  cdc_acm_find_and_open_usb_device(vid, pid, dev_config->connection_timeout_ms, &cdc_dev);
    if (ESP_OK != ret) {
        goto exit;
    }

    // Get Device and Configuration descriptors
    const usb_config_desc_t *config_desc = NULL;
    const usb_device_desc_t *device_desc = NULL;
    ret = usb_host_get_device_descriptor(cdc_dev->dev_hdl, &device_desc);
    if (ret != ESP_OK) goto err;
    ret = usb_host_get_active_config_descriptor(cdc_dev->dev_hdl,
                                                 &config_desc);
    if (ret != ESP_OK) goto err;

    // Parse the required interface descriptor
    cdc_parsed_info_t cdc_info;
    ESP_GOTO_ON_ERROR(
        cdc_parse_interface_descriptor(device_desc, config_desc, interface_idx, &cdc_info),
        err, TAG, "Could not open required interface as CDC");

    // Save all members of cdc_dev
    cdc_dev->data.intf_desc = cdc_info.data_intf;
    cdc_dev->data_protocol = (cdc_data_protocol_t)cdc_dev->data.intf_desc->bInterfaceProtocol;
    cdc_dev->notif.intf_desc = cdc_info.notif_intf;
    if (cdc_info.notif_intf) {
        cdc_dev->comm_protocol = (cdc_comm_protocol_t)cdc_dev->notif.intf_desc->bInterfaceProtocol;
    }
    cdc_dev->cdc_func_desc = cdc_info.func;
    cdc_dev->cdc_func_desc_cnt = cdc_info.func_cnt;

    // For CDC compliant devices, this driver provides default implementation of CDC-ACM specific functions.
    if (cdc_dev->cdc_func_desc_cnt > 1) {
        cdc_dev->intf_func.line_coding_get = acm_compliant_line_coding_get;
        cdc_dev->intf_func.line_coding_set = acm_compliant_line_coding_set;
        cdc_dev->intf_func.set_control_line_state = acm_compliant_set_control_line_state;
        cdc_dev->intf_func.send_break = acm_compliant_send_break;
    }

    // The following line is here for backward compatibility with v1.0.*
    // where fixed size of IN buffer (equal to IN Maximum Packet Size) was used
    const size_t in_buf_size = (dev_config->data_cb && (dev_config->in_buffer_size == 0)) ? USB_EP_DESC_GET_MPS(cdc_info.in_ep) : dev_config->in_buffer_size;

    // Allocate USB transfers, claim CDC interfaces and return CDC-ACM handle
    ESP_GOTO_ON_ERROR(
        cdc_acm_transfers_allocate(cdc_dev, cdc_info.notif_ep, cdc_info.in_ep, in_buf_size, cdc_info.out_ep, dev_config->out_buffer_size),
        err, TAG,);
    ESP_GOTO_ON_ERROR(cdc_acm_start(cdc_dev, dev_config->event_cb, dev_config->data_cb, dev_config->user_arg), err, TAG,);
    *cdc_hdl_ret = (cdc_acm_dev_hdl_t)cdc_dev;
    xSemaphoreGive(cdc_acm_obj->open_close_mutex);
    cdc_acm_lifetime_unlock();
    return ESP_OK;

err:
    if (!cdc_dev->start_cleanup_retained) {
        cdc_acm_device_remove(cdc_dev);
    } else {
        ESP_LOGE(TAG, "retaining failed-open CDC device until late callback retires");
    }
exit:
    xSemaphoreGive(cdc_acm_obj->open_close_mutex);
    *cdc_hdl_ret = NULL;
    cdc_acm_lifetime_unlock();
    return ret;
}

static esp_err_t cdc_acm_wait_interface_release(cdc_acm_obj_t *cdc_acm_obj,
                                                 cdc_dev_t *cdc_dev,
                                                 uint8_t interface_number)
{
    for (size_t attempt = 0; attempt < CDC_ACM_CLOSE_DRAIN_ATTEMPTS; ++attempt) {
        esp_err_t ret = usb_host_interface_release(
            cdc_acm_obj->cdc_acm_client_hdl, cdc_dev->dev_hdl,
            interface_number);
        if (ret == ESP_OK) return ESP_OK;
        if (cdc_acm_device_is_gone(cdc_dev) &&
                cdc_acm_status_is_gone(ret)) {
            return ESP_OK;
        }
        if (ret != ESP_ERR_INVALID_STATE) return ret;

        // endpoint_flush() schedules CANCELED completions. Pumping the client
        // here lets those callbacks retire their in-flight URBs before retrying
        // interface release; callbacks were already nulled above.
        ret = usb_host_client_handle_events(cdc_acm_obj->cdc_acm_client_hdl,
                                             pdMS_TO_TICKS(10));
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) return ret;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t cdc_acm_host_close_direct(cdc_acm_dev_hdl_t cdc_hdl)
{
    CDC_ACM_CHECK(p_cdc_acm_obj, ESP_ERR_INVALID_STATE);
    CDC_ACM_CHECK(cdc_hdl, ESP_ERR_INVALID_ARG);

    xSemaphoreTake(p_cdc_acm_obj->open_close_mutex, portMAX_DELAY);

    // Make sure that the device is in the devices list (that it is not already closed)
    cdc_dev_t *cdc_dev;
    bool device_found = false;
    CDC_ACM_ENTER_CRITICAL();
    SLIST_FOREACH(cdc_dev, &p_cdc_acm_obj->cdc_devices_list, list_entry) {
        if (cdc_dev == (cdc_dev_t *)cdc_hdl) {
            device_found = true;
            break;
        }
    }

    // Device was not found in the cdc_devices_list; it was already closed, return OK
    if (!device_found) {
        CDC_ACM_EXIT_CRITICAL();
        xSemaphoreGive(p_cdc_acm_obj->open_close_mutex);
        return ESP_OK;
    }

    // No user callbacks from this point.
    cdc_dev->notif.cb = NULL;
    cdc_dev->data.in_cb = NULL;
    cdc_dev->cb_arg = NULL;
    cdc_dev->closing = true;
    CDC_ACM_EXIT_CRITICAL();

    esp_err_t ret = ESP_OK;
    ret = cdc_acm_drain_control_for_close(p_cdc_acm_obj, cdc_dev);
    if (ret != ESP_OK) goto unblock;
    ret = cdc_acm_drain_out_for_close(p_cdc_acm_obj, cdc_dev);
    if (ret != ESP_OK) goto unblock;
    // Wrapper references can encompass a nested control operation. Drain the
    // transfer first so this driver task can deliver its completion, then wait
    // for the outer wrapper to release its generic lifetime reference.
    ret = cdc_acm_wait_device_operations(p_cdc_acm_obj, cdc_dev);
    if (ret != ESP_OK) goto unblock;

    if (!cdc_dev->close_prepared) {
        // Cancel periodic IN transfers first. Their CANCELED completions are
        // delivered only while the CDC client pumps its event loop.
        bool data_poll_in_flight = false;
        bool notif_poll_in_flight = false;
        size_t data_callback_active = 0;
        size_t notif_callback_active = 0;
        cdc_acm_get_poll_state(cdc_dev, &data_poll_in_flight,
                               &notif_poll_in_flight,
                               &data_callback_active,
                               &notif_callback_active);
        if (data_callback_active != 0 || notif_callback_active != 0) {
            ret = ESP_ERR_NOT_FINISHED;
            goto unblock;
        }
        if (data_poll_in_flight) {
            ret = cdc_acm_reset_transfer_endpoint(cdc_dev,
                                                   cdc_dev->data.in_xfer);
            if (ret != ESP_OK) goto unblock;
        }
        if (notif_poll_in_flight) {
            ret = cdc_acm_reset_transfer_endpoint(cdc_dev,
                                                   cdc_dev->notif.xfer);
            if (ret != ESP_OK) goto unblock;
        }
        cdc_dev->close_prepared = true;
    }

    ret = cdc_acm_drain_periodic_for_close(p_cdc_acm_obj, cdc_dev);
    if (ret != ESP_OK) goto unblock;

    if (!cdc_dev->data_interface_released) {
        ret = cdc_acm_wait_interface_release(
            p_cdc_acm_obj, cdc_dev,
            cdc_dev->data.intf_desc->bInterfaceNumber);
        if (ret != ESP_OK) goto unblock;
        cdc_dev->data_interface_released = true;
    }
    if ((cdc_dev->notif.intf_desc != NULL) &&
            (cdc_dev->notif.intf_desc != cdc_dev->data.intf_desc) &&
            !cdc_dev->notif_interface_released) {
        ret = cdc_acm_wait_interface_release(
            p_cdc_acm_obj, cdc_dev,
            cdc_dev->notif.intf_desc->bInterfaceNumber);
        if (ret != ESP_OK) goto unblock;
        cdc_dev->notif_interface_released = true;
    }

    CDC_ACM_ENTER_CRITICAL();
    SLIST_REMOVE(&p_cdc_acm_obj->cdc_devices_list, cdc_dev, cdc_dev_s, list_entry);
    CDC_ACM_EXIT_CRITICAL();

    cdc_acm_device_remove(cdc_dev);
    xSemaphoreGive(p_cdc_acm_obj->open_close_mutex);
    return ESP_OK;

unblock:
    xSemaphoreGive(p_cdc_acm_obj->open_close_mutex);
    return ret;
}

esp_err_t cdc_acm_host_close(cdc_acm_dev_hdl_t cdc_hdl)
{
    CDC_ACM_CHECK(cdc_hdl, ESP_ERR_INVALID_ARG);
    if (!cdc_acm_lifetime_lock()) return ESP_ERR_NO_MEM;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    cdc_acm_close_request_t *request = NULL;

    CDC_ACM_ENTER_CRITICAL();
    cdc_acm_obj_t *cdc_acm_obj = p_cdc_acm_obj;
    bool closing_driver = cdc_acm_obj && cdc_acm_obj->uninstalling;
    CDC_ACM_EXIT_CRITICAL();
    if (cdc_acm_obj == NULL || closing_driver) goto release_lifetime;

    // USB transfer and client callbacks execute in driver_task_h. Dispatching
    // close there ensures no callback stack can still dereference cdc_dev when
    // cdc_acm_device_remove() frees it.
    // A user callback runs on driver_task_h. Closing synchronously there would
    // free cdc_dev before the transfer callback returns to its caller.
    if (xTaskGetCurrentTaskHandle() == cdc_acm_obj->driver_task_h) {
        goto release_lifetime;
    }

    request = calloc(1, sizeof(*request));
    if (request == NULL) {
        result = ESP_ERR_NO_MEM;
        goto release_lifetime;
    }
    request->completion = xSemaphoreCreateBinary();
    if (request->completion == NULL) {
        free(request);
        result = ESP_ERR_NO_MEM;
        goto release_lifetime;
    }
    request->handle = cdc_hdl;
    request->result = ESP_FAIL;
    // One reference belongs to this caller and one transfers to the driver
    // task when the queue send succeeds.
    request->ref_count = 2;
    if (xQueueSend(cdc_acm_obj->close_request_queue, &request,
                   pdMS_TO_TICKS(CDC_ACM_CLOSE_QUEUE_WAIT_MS)) != pdPASS) {
        cdc_acm_close_request_release(request);
        cdc_acm_close_request_release(request);
        result = ESP_ERR_TIMEOUT;
        goto release_lifetime;
    }
    (void)usb_host_client_unblock(cdc_acm_obj->cdc_acm_client_hdl);
    if (xSemaphoreTake(request->completion,
                       pdMS_TO_TICKS(CDC_ACM_CLOSE_WAIT_MS)) != pdTRUE) {
        cdc_acm_close_request_release(request);
        result = ESP_ERR_TIMEOUT;
        goto release_lifetime;
    }
    result = request->result;
    cdc_acm_close_request_release(request);

release_lifetime:
    cdc_acm_lifetime_unlock();
    return result;
}

void cdc_acm_host_desc_print(cdc_acm_dev_hdl_t cdc_hdl)
{
    assert(cdc_hdl);
    cdc_dev_t *cdc_dev = NULL;
    if (cdc_acm_acquire_device_operation(cdc_hdl, &cdc_dev) != ESP_OK) return;

    const usb_device_desc_t *device_desc = NULL;
    const usb_config_desc_t *config_desc = NULL;
    if (usb_host_get_device_descriptor(cdc_dev->dev_hdl, &device_desc) == ESP_OK) {
        usb_print_device_descriptor(device_desc);
    }
    if (usb_host_get_active_config_descriptor(cdc_dev->dev_hdl,
                                               &config_desc) == ESP_OK) {
        usb_print_config_descriptor(config_desc, cdc_print_desc);
    }
    cdc_acm_release_device_operation(cdc_dev);
}

/**
 * @brief Check finished transfer status
 *
 * Return to on transfer completed OK.
 * Cancel the transfer and issue user's callback in case of an error.
 *
 * @param[in] transfer Transfer to be checked
 * @return true Transfer completed
 * @return false Transfer NOT completed
 */
static void cdc_acm_latch_transfer_failure(
    cdc_dev_t *cdc_dev, usb_transfer_status_t status)
{
    if (status == USB_TRANSFER_STATUS_CANCELED) return;
    bool disconnected = status == USB_TRANSFER_STATUS_NO_DEVICE;
    cdc_acm_host_dev_callback_t event_cb = NULL;
    void *cb_arg = NULL;
    CDC_ACM_ENTER_CRITICAL();
    if (cdc_dev->closing || cdc_dev->close_prepared) {
        if (disconnected) cdc_dev->device_gone = true;
        CDC_ACM_EXIT_CRITICAL();
        return;
    }
    if (disconnected) cdc_dev->device_gone = true;
    if (cdc_dev->poll_error == ESP_OK) {
        cdc_dev->poll_error = disconnected ? ESP_ERR_NOT_FOUND
                                           : ESP_ERR_INVALID_RESPONSE;
    }
    // This snapshot shares the publication critical section. If failure wins,
    // enable_user_callbacks observes poll_error and refuses READY; if enable
    // wins, this callback snapshot is non-null and emits the terminal event.
    event_cb = cdc_dev->notif.cb;
    cb_arg = cdc_dev->cb_arg;
    CDC_ACM_EXIT_CRITICAL();

    if (event_cb == NULL) return;
    cdc_acm_host_dev_event_data_t failure_event;
    if (disconnected) {
        failure_event.type = CDC_ACM_HOST_DEVICE_DISCONNECTED;
        failure_event.data.cdc_hdl = (cdc_acm_dev_hdl_t)cdc_dev;
    } else {
        failure_event.type = CDC_ACM_HOST_ERROR;
        failure_event.data.error = (int)status;
    }
    event_cb(&failure_event, cb_arg);
}

static bool cdc_acm_is_transfer_completed(usb_transfer_t *transfer)
{
    bool completed = false;

    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        completed = true;
    } else {
        cdc_acm_latch_transfer_failure(
            (cdc_dev_t *)transfer->context, transfer->status);
    }
    return completed;
}

static void in_xfer_cb(usb_transfer_t *transfer)
{
    ESP_LOGD(TAG, "in xfer cb");
    cdc_dev_t *cdc_dev = (cdc_dev_t *)transfer->context;
    cdc_acm_data_callback_enter(cdc_dev);
    cdc_acm_data_callback_t data_cb = NULL;
    cdc_acm_host_dev_callback_t event_cb = NULL;
    void *cb_arg = NULL;
    cdc_acm_snapshot_data_callbacks(cdc_dev, &data_cb, &event_cb, &cb_arg);

    if (!cdc_acm_is_transfer_completed(transfer)) {
        goto callback_exit;
    }

    if (data_cb != NULL) {
        const bool data_processed = data_cb(
            transfer->data_buffer, transfer->actual_num_bytes, cb_arg);

        // Information for developers:
        // In order to save RAM and CPU time, the application can indicate that the received data was not processed and that the application expects more data.
        // In this case, the next received data must be appended to the existing buffer.
        // Since the data_buffer in usb_transfer_t is a constant pointer, we must cast away to const qualifier.
        if (!data_processed) {
#if !SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
            // In case the received data was not processed, the next RX data must be appended to current buffer
            uint8_t **ptr = (uint8_t **)(&(transfer->data_buffer));
            *ptr += transfer->actual_num_bytes;

            // Calculate remaining space in the buffer. Attention: pointer arithmetic!
            size_t space_left = transfer->data_buffer_size - (transfer->data_buffer - cdc_dev->data.in_data_buffer_base);
            uint16_t mps = cdc_dev->data.in_mps;
            transfer->num_bytes = (space_left / mps) * mps; // Round down to MPS for next transfer

            if (transfer->num_bytes == 0) {
                // The IN buffer cannot accept more data, inform the user and reset the buffer
                ESP_LOGW(TAG, "IN buffer overflow");
                cdc_dev->serial_state.bOverRun = true;
                if (event_cb != NULL) {
                    const cdc_acm_host_dev_event_data_t serial_state_event = {
                        .type = CDC_ACM_HOST_SERIAL_STATE,
                        .data.serial_state = cdc_dev->serial_state
                    };
                    event_cb(&serial_state_event, cb_arg);
                }

                cdc_acm_reset_in_transfer(cdc_dev);
                cdc_dev->serial_state.bOverRun = false;
            }
#else
            // For targets that must sync internal memory through L1CACHE, we cannot change the data_buffer
            // because it would lead to unaligned cache sync, which is not allowed
            ESP_LOGW(TAG, "RX buffer append is not yet supported on ESP32-P4!");
#endif
        } else {
            cdc_acm_reset_in_transfer(cdc_dev);
        }
    }

    if (!cdc_dev->closing && !cdc_dev->close_prepared) {
        ESP_LOGD(TAG, "Submitting poll for BULK IN transfer");
        esp_err_t ret = cdc_acm_submit_data_poll(cdc_dev);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Could not resubmit BULK IN transfer: %s",
                     esp_err_to_name(ret));
            cdc_acm_report_poll_error(cdc_dev, ret);
        }
    }

callback_exit:
    cdc_acm_data_callback_exit(cdc_dev);
}

static void notif_xfer_cb(usb_transfer_t *transfer)
{
    ESP_LOGD(TAG, "notif xfer cb");
    cdc_dev_t *cdc_dev = (cdc_dev_t *)transfer->context;
    cdc_acm_notif_callback_enter(cdc_dev);
    cdc_acm_host_dev_callback_t event_cb = NULL;
    void *cb_arg = NULL;
    cdc_acm_snapshot_event_callback(cdc_dev, &event_cb, &cb_arg);

    if (cdc_acm_is_transfer_completed(transfer)) {
        CdcAcmNotificationView notif;
        bool valid_notification = cdc_acm_notification_parse(
            transfer->data_buffer, transfer->actual_num_bytes, &notif);
        if (valid_notification &&
                (cdc_dev->notif.intf_desc == NULL ||
                 notif.interfaceIndex !=
                    cdc_dev->notif.intf_desc->bInterfaceNumber)) {
            valid_notification = false;
        }
        if (!valid_notification) {
            ESP_LOGE(TAG, "Malformed CDC notification (%zu bytes)",
                     transfer->actual_num_bytes);
            cdc_acm_report_poll_error(cdc_dev, ESP_ERR_INVALID_SIZE);
            goto callback_exit;
        }
        switch (notif.notificationCode) {
        case USB_CDC_NOTIF_NETWORK_CONNECTION: {
            if (event_cb != NULL) {
                const cdc_acm_host_dev_event_data_t net_conn_event = {
                    .type = CDC_ACM_HOST_NETWORK_CONNECTION,
                    .data.network_connected = (bool)notif.value
                };
                event_cb(&net_conn_event, cb_arg);
            }
            break;
        }
        case USB_CDC_NOTIF_SERIAL_STATE: {
            cdc_dev->serial_state.val = notif.serialState;
            if (event_cb != NULL) {
                const cdc_acm_host_dev_event_data_t serial_state_event = {
                    .type = CDC_ACM_HOST_SERIAL_STATE,
                    .data.serial_state = cdc_dev->serial_state
                };
                event_cb(&serial_state_event, cb_arg);
            }
            break;
        }
        case USB_CDC_NOTIF_RESPONSE_AVAILABLE: // Encapsulated commands not implemented - fallthrough
        default:
            ESP_LOGW(TAG,
                     "Unsupported notification type 0x%02X (%zu-byte transfer, %u-byte payload)",
                     notif.notificationCode, transfer->actual_num_bytes,
                     (unsigned)notif.payloadLength);
            break;
        }

        // Start polling for new data again
        if (!cdc_dev->closing && !cdc_dev->close_prepared) {
            ESP_LOGD(TAG, "Submitting poll for INTR IN transfer");
            esp_err_t ret = cdc_acm_submit_notif_poll(cdc_dev);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Could not resubmit INTR IN transfer: %s",
                         esp_err_to_name(ret));
                cdc_acm_report_poll_error(cdc_dev, ret);
            }
        }
    }
callback_exit:
    cdc_acm_notif_callback_exit(cdc_dev);
}

static void out_xfer_cb(usb_transfer_t *transfer)
{
    ESP_LOGD(TAG, "out/ctrl xfer cb");
    assert(transfer->context);
    xSemaphoreGive((SemaphoreHandle_t)transfer->context);
}

/**
 * @brief Resume CDC device
 *
 * Submit poll for BULK IN and INTR IN transfers
 *
 * @param cdc_dev
 * @return esp_err_t
 */
#ifdef CDC_HOST_SUSPEND_RESUME_API_SUPPORTED
static esp_err_t cdc_acm_resume(cdc_dev_t *cdc_dev)
{
    assert(cdc_dev);

    if (cdc_dev->data.in_xfer) {
        ESP_LOGD(TAG, "Submitting poll for BULK IN transfer");
        esp_err_t ret = cdc_acm_submit_data_poll(cdc_dev);
        if (ret != ESP_OK) {
            cdc_acm_report_poll_error(cdc_dev, ret);
            return ret;
        }
    }

    if (cdc_dev->notif.xfer) {
        ESP_LOGD(TAG, "Submitting poll for INTR IN transfer");
        esp_err_t ret = cdc_acm_submit_notif_poll(cdc_dev);
        if (ret != ESP_OK) {
            cdc_acm_report_poll_error(cdc_dev, ret);
            return ret;
        }
    }
    return ESP_OK;
}

static void cdc_acm_dispatch_suspend_resume(usb_device_handle_t dev_hdl,
                                            bool resumed)
{
    CDC_ACM_ENTER_CRITICAL();
    p_cdc_acm_obj->dispatch_generation++;
    if (p_cdc_acm_obj->dispatch_generation == 0) {
        p_cdc_acm_obj->dispatch_generation = 1;
    }
    uint32_t generation = p_cdc_acm_obj->dispatch_generation;
    CDC_ACM_EXIT_CRITICAL();

    while (true) {
        cdc_dev_t *target = NULL;
        cdc_acm_host_dev_callback_t event_cb = NULL;
        void *cb_arg = NULL;

        CDC_ACM_ENTER_CRITICAL();
        cdc_dev_t *candidate = NULL;
        SLIST_FOREACH(candidate, &p_cdc_acm_obj->cdc_devices_list, list_entry) {
            if (candidate->dev_hdl != dev_hdl ||
                    candidate->dispatch_generation == generation) {
                continue;
            }
            candidate->dispatch_generation = generation;
            if (candidate->closing || candidate->device_gone) continue;
            candidate->operation_refs++;
            target = candidate;
            event_cb = candidate->notif.cb;
            cb_arg = candidate->cb_arg;
            break;
        }
        CDC_ACM_EXIT_CRITICAL();

        if (target == NULL) return;
        esp_err_t resume_result = resumed ? cdc_acm_resume(target) : ESP_OK;
        if (event_cb != NULL && resume_result == ESP_OK) {
            const cdc_acm_host_dev_event_data_t event = {
                .type = resumed ? CDC_ACM_HOST_DEVICE_RESUMED
                                : CDC_ACM_HOST_DEVICE_SUSPENDED,
                .data.cdc_hdl = (cdc_acm_dev_hdl_t)target,
            };
            event_cb(&event, cb_arg);
        }
        cdc_acm_release_device_operation(target);
    }
}
#endif // CDC_HOST_SUSPEND_RESUME_API_SUPPORTED

static void cdc_acm_dispatch_device_gone(usb_device_handle_t dev_hdl)
{
    while (true) {
        cdc_dev_t *target = NULL;
        cdc_acm_host_dev_callback_t event_cb = NULL;
        void *cb_arg = NULL;

        CDC_ACM_ENTER_CRITICAL();
        SLIST_FOREACH(target, &p_cdc_acm_obj->cdc_devices_list, list_entry) {
            if (target->dev_hdl == dev_hdl && !target->device_gone) {
                target->device_gone = true;
                target->operation_refs++;
                event_cb = target->notif.cb;
                cb_arg = target->cb_arg;
                break;
            }
        }
        CDC_ACM_EXIT_CRITICAL();

        if (target == NULL) return;
        if (event_cb != NULL) {
            const cdc_acm_host_dev_event_data_t disconn_event = {
                .type = CDC_ACM_HOST_DEVICE_DISCONNECTED,
                .data.cdc_hdl = (cdc_acm_dev_hdl_t)target,
            };
            event_cb(&disconn_event, cb_arg);
        }
        cdc_acm_release_device_operation(target);
    }
}

static void usb_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV: {
        // Guard p_cdc_acm_obj->new_dev_cb from concurrent access
        ESP_LOGD(TAG, "New device connected");
        CDC_ACM_ENTER_CRITICAL();
        cdc_acm_new_dev_callback_t _new_dev_cb = p_cdc_acm_obj->new_dev_cb;
        CDC_ACM_EXIT_CRITICAL();

        if (_new_dev_cb) {
            usb_device_handle_t new_dev;
            if (usb_host_device_open(p_cdc_acm_obj->cdc_acm_client_hdl, event_msg->new_dev.address, &new_dev) != ESP_OK) {
                break;
            }
            assert(new_dev);
            _new_dev_cb(new_dev);
            usb_host_device_close(p_cdc_acm_obj->cdc_acm_client_hdl, new_dev);
        }
        break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_GONE: {
        ESP_LOGD(TAG, "Device suddenly disconnected");
        cdc_acm_dispatch_device_gone(event_msg->dev_gone.dev_hdl);
        break;
    }
#ifdef CDC_HOST_SUSPEND_RESUME_API_SUPPORTED
    case USB_HOST_CLIENT_EVENT_DEV_SUSPENDED: {
        ESP_LOGD(TAG, "Device suspended");
        // The host library already halted and flushed every endpoint.
        cdc_acm_dispatch_suspend_resume(
            event_msg->dev_suspend_resume.dev_hdl, false);
        break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_RESUMED: {
        ESP_LOGD(TAG, "Device resumed");
        cdc_acm_dispatch_suspend_resume(
            event_msg->dev_suspend_resume.dev_hdl, true);
        break;
    }
#endif // CDC_HOST_SUSPEND_RESUME_API_SUPPORTED
    default:
        ESP_LOGE(TAG, "Unrecognized USB Host client event");
        assert(false);
        break;
    }
}

esp_err_t cdc_acm_host_data_tx_blocking(cdc_acm_dev_hdl_t cdc_hdl, const uint8_t *data, size_t data_len, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    CDC_ACM_CHECK(cdc_hdl, ESP_ERR_INVALID_ARG);
    CDC_ACM_CHECK(data && (data_len > 0), ESP_ERR_INVALID_ARG);
    cdc_dev_t *cdc_dev = NULL;
    ret = cdc_acm_admit_out_operation(cdc_hdl, &cdc_dev);
    if (ret != ESP_OK) return ret;
    if (cdc_dev->data.out_xfer == NULL) {
        ret = ESP_ERR_NOT_SUPPORTED;
        goto release_operation;
    }

    const size_t buffer_size = cdc_dev->data.out_xfer->data_buffer_size;
    const uint8_t *data_ptr = data;
    size_t remaining = data_len;

    // Record start time for timeout tracking
    TickType_t start_ticks = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    TickType_t elapsed_ticks = 0;
    int remaining_timeout_ticks = timeout_ticks;

    // Take OUT mutex for the entire transfer operation
    BaseType_t taken = xSemaphoreTake(cdc_dev->data.out_mux, remaining_timeout_ticks);
    if (taken != pdTRUE) {
        ret = ESP_ERR_TIMEOUT;
        goto release_operation;
    }

    // Process data in chunks if it's larger than the buffer size
    while (remaining > 0) {
        size_t chunk_size = (remaining > buffer_size) ? buffer_size : remaining;

        ESP_LOGV(TAG, "Submitting BULK OUT transfer chunk: %zu bytes (remaining: %zu)", chunk_size, remaining);
        memcpy(cdc_dev->data.out_xfer->data_buffer, data_ptr, chunk_size);
        cdc_dev->data.out_xfer->num_bytes = chunk_size;
        // Use remaining timeout for this chunk's transfer timeout
        cdc_dev->data.out_xfer->timeout_ms = pdTICKS_TO_MS(remaining_timeout_ticks);
        ret = cdc_acm_submit_out_and_wait(cdc_dev, remaining_timeout_ticks);
        elapsed_ticks = xTaskGetTickCount() - start_ticks;
        remaining_timeout_ticks = timeout_ticks - elapsed_ticks;
        if (ret != ESP_OK || remaining_timeout_ticks < 0) {
            ESP_LOGW(TAG, "TX transfer timeout");
            if (ret == ESP_OK) ret = ESP_ERR_TIMEOUT;
            goto unblock;
        }

        ESP_GOTO_ON_FALSE(cdc_dev->data.out_xfer->status == USB_TRANSFER_STATUS_COMPLETED, ESP_ERR_INVALID_RESPONSE, unblock, TAG, "Bulk OUT transfer error");
        ESP_GOTO_ON_FALSE(cdc_dev->data.out_xfer->actual_num_bytes == chunk_size, ESP_ERR_INVALID_RESPONSE, unblock, TAG, "Incorrect number of bytes transferred");

        remaining -= chunk_size;
        data_ptr += chunk_size;
    }

    ret = ESP_OK;

unblock:
    xSemaphoreGive(cdc_dev->data.out_mux);
release_operation:
    cdc_acm_end_out_operation(cdc_dev);
    return ret;
}

esp_err_t cdc_acm_host_send_custom_request(cdc_acm_dev_hdl_t cdc_hdl, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint16_t wLength, uint8_t *data)
{
    CDC_ACM_CHECK(cdc_hdl, ESP_ERR_INVALID_ARG);
    if (wLength > 0) {
        CDC_ACM_CHECK(data, ESP_ERR_INVALID_ARG);
    }
    cdc_dev_t *cdc_dev = NULL;
    esp_err_t ret = cdc_acm_admit_control_operation(cdc_hdl, &cdc_dev);
    if (ret != ESP_OK) return ret;
    if (cdc_dev->ctrl_transfer->data_buffer_size <
            wLength + sizeof(usb_setup_packet_t)) {
        ret = ESP_ERR_INVALID_SIZE;
        goto release_operation;
    }

    // Take Mutex and fill the CTRL request
    BaseType_t taken = xSemaphoreTake(cdc_dev->ctrl_mux, pdMS_TO_TICKS(CDC_ACM_CTRL_TIMEOUT_MS));
    if (!taken) {
        ret = ESP_ERR_TIMEOUT;
        goto release_operation;
    }
    usb_setup_packet_t *req = (usb_setup_packet_t *)(cdc_dev->ctrl_transfer->data_buffer);
    uint8_t *start_of_data = (uint8_t *)req + sizeof(usb_setup_packet_t);
    req->bmRequestType = bmRequestType;
    req->bRequest = bRequest;
    req->wValue = wValue;
    req->wIndex = wIndex;
    req->wLength = wLength;

    // For IN transfers we must transfer data ownership to CDC driver
    const bool in_transfer = bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN;
    if (!in_transfer && wLength > 0) {
        memcpy(start_of_data, data, wLength);
    }

    cdc_dev->ctrl_transfer->num_bytes = wLength + sizeof(usb_setup_packet_t);
    ret = cdc_acm_submit_control_and_wait(cdc_dev);
    if (ret != ESP_OK) goto unblock;

    ESP_GOTO_ON_FALSE(cdc_dev->ctrl_transfer->status == USB_TRANSFER_STATUS_COMPLETED, ESP_ERR_INVALID_RESPONSE, unblock, TAG, "Control transfer error");
    ESP_GOTO_ON_FALSE(cdc_dev->ctrl_transfer->actual_num_bytes == cdc_dev->ctrl_transfer->num_bytes, ESP_ERR_INVALID_RESPONSE, unblock, TAG, "Incorrect number of bytes transferred");

    // For OUT transfers, we must transfer data ownership to user
    if (in_transfer && wLength > 0) {
        memcpy(data, start_of_data, wLength);
    }
    ret = ESP_OK;

unblock:
    xSemaphoreGive(cdc_dev->ctrl_mux);
release_operation:
    cdc_acm_end_control_operation(cdc_dev);
    return ret;
}

esp_err_t cdc_acm_host_protocols_get(cdc_acm_dev_hdl_t cdc_hdl, cdc_comm_protocol_t *comm, cdc_data_protocol_t *data)
{
    CDC_ACM_CHECK(cdc_hdl, ESP_ERR_INVALID_ARG);
    cdc_dev_t *cdc_dev = NULL;
    esp_err_t ret = cdc_acm_acquire_device_operation(cdc_hdl, &cdc_dev);
    if (ret != ESP_OK) return ret;

    if (comm != NULL) {
        *comm = cdc_dev->comm_protocol;
    }
    if (data != NULL) {
        *data = cdc_dev->data_protocol;
    }
    cdc_acm_release_device_operation(cdc_dev);
    return ret;
}

esp_err_t cdc_acm_host_cdc_desc_get(cdc_acm_dev_hdl_t cdc_hdl, cdc_desc_subtype_t desc_type, const usb_standard_desc_t **desc_out)
{
    CDC_ACM_CHECK(cdc_hdl, ESP_ERR_INVALID_ARG);
    CDC_ACM_CHECK(desc_type < USB_CDC_DESC_SUBTYPE_MAX, ESP_ERR_INVALID_ARG);
    CDC_ACM_CHECK(desc_out, ESP_ERR_INVALID_ARG);
    cdc_dev_t *cdc_dev = NULL;
    esp_err_t ret = cdc_acm_acquire_device_operation(cdc_hdl, &cdc_dev);
    if (ret != ESP_OK) return ret;
    ret = ESP_ERR_NOT_FOUND;
    *desc_out = NULL;

    for (int i = 0; i < cdc_dev->cdc_func_desc_cnt; i++) {
        const cdc_header_desc_t *_desc = (const cdc_header_desc_t *)((*(cdc_dev->cdc_func_desc))[i]);
        if (_desc->bDescriptorSubtype == desc_type) {
            ret = ESP_OK;
            *desc_out = (const usb_standard_desc_t *)_desc;
            break;
        }
    }
    cdc_acm_release_device_operation(cdc_dev);
    return ret;
}

#ifdef CDC_HOST_REMOTE_WAKE_SUPPORTED

esp_err_t cdc_acm_host_enable_remote_wakeup(cdc_acm_dev_hdl_t cdc_hdl, bool enable)
{
    CDC_ACM_CHECK(cdc_hdl, ESP_ERR_INVALID_ARG);
    cdc_dev_t *this_cdc_dev = NULL;
    esp_err_t ret = cdc_acm_admit_control_operation(cdc_hdl, &this_cdc_dev);
    if (ret != ESP_OK) return ret;
    cdc_dev_t *cdc_dev = NULL;
    bool ctrl_mutex_taken = false;

    // Get device's config descriptor
    const usb_config_desc_t *config_desc = NULL;
    ret = usb_host_get_active_config_descriptor(this_cdc_dev->dev_hdl,
                                                 &config_desc);
    if (ret != ESP_OK) goto release_operation;

    // Check if the device reports remote wakeup feature in it's configuration descriptor
    if ((config_desc->bmAttributes & USB_BM_ATTRIBUTES_WAKEUP) == 0) {
        ret = ESP_ERR_NOT_SUPPORTED;
        goto release_operation;
    }

    // Go through all pseudo devices and find out whether current physical USB Device has remote wakeup enabled/disabled
    bool remote_wake_enabled = false;       // Disabled by default (if supported) after reset
    CDC_ACM_ENTER_CRITICAL();
    SLIST_FOREACH(cdc_dev, &p_cdc_acm_obj->cdc_devices_list, list_entry) {
        if ((cdc_dev->dev_hdl == this_cdc_dev->dev_hdl) && cdc_dev->remote_wakeup_enabled) {
            remote_wake_enabled = true;
            break;
        }
    }
    CDC_ACM_EXIT_CRITICAL();

    // Check current remote wakeup status,
    // If user wants to enable it and is already enabled (or vice versa) return early, otherwise proceed to ctrl transfer
    if (remote_wake_enabled == enable) {
        ESP_LOGD(TAG, "Remote wakeup already %s on this device", (enable) ? "enabled" : "disabled");
        ret = ESP_OK;
        goto release_operation;
    }

    // Take Mutex and fill the CTRL request
    BaseType_t taken = xSemaphoreTake(this_cdc_dev->ctrl_mux, pdMS_TO_TICKS(CDC_ACM_CTRL_TIMEOUT_MS));
    if (!taken) {
        ret = ESP_ERR_TIMEOUT;
        goto release_operation;
    }
    ctrl_mutex_taken = true;

    usb_setup_packet_t *req = (usb_setup_packet_t *)(this_cdc_dev->ctrl_transfer->data_buffer);
    if (enable) {
        USB_SETUP_PACKET_INIT_SET_FEATURE(req, DEVICE_REMOTE_WAKEUP);
        ESP_LOGI(TAG, "Enabling remote wakeup on device");
    } else {
        USB_SETUP_PACKET_INIT_CLEAR_FEATURE(req, DEVICE_REMOTE_WAKEUP);
        ESP_LOGI(TAG, "Disabling remote wakeup on device");
    }
    this_cdc_dev->ctrl_transfer->num_bytes = sizeof(usb_setup_packet_t);

    ret = cdc_acm_submit_control_and_wait(this_cdc_dev);
    if (ret != ESP_OK) goto unlock_ctrl_mux;

    ESP_GOTO_ON_FALSE(this_cdc_dev->ctrl_transfer->status == USB_TRANSFER_STATUS_COMPLETED, ESP_ERR_INVALID_RESPONSE, unlock_ctrl_mux, TAG, "Control transfer error");
    ESP_GOTO_ON_FALSE(this_cdc_dev->ctrl_transfer->actual_num_bytes == this_cdc_dev->ctrl_transfer->num_bytes, ESP_ERR_INVALID_RESPONSE, unlock_ctrl_mux, TAG, "Incorrect number of bytes transferred");
    ret = ESP_OK;

    // Remote wakeup status has changed, update all pseudo-devices statuses about remote wakeup
    CDC_ACM_ENTER_CRITICAL();
    SLIST_FOREACH(cdc_dev, &p_cdc_acm_obj->cdc_devices_list, list_entry) {
        if (cdc_dev->dev_hdl == this_cdc_dev->dev_hdl) {
            cdc_dev->remote_wakeup_enabled = enable;
        }
    }
    CDC_ACM_EXIT_CRITICAL();

unlock_ctrl_mux:
    if (ctrl_mutex_taken) xSemaphoreGive(this_cdc_dev->ctrl_mux);
release_operation:
    cdc_acm_end_control_operation(this_cdc_dev);
    return ret;
}
#endif // CDC_HOST_REMOTE_WAKE_SUPPORTED
