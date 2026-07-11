/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <sys/queue.h>                  // For singly linked list

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"            // For mutexes and semaphores

#include "usb/usb_host.h"               // For USB device handle and transfers
#include "usb/cdc_acm_host_interface.h" // For CDC interface function table
#include "usb/usb_types_cdc.h"          // For protocol and serial state

// CDC-ACM check macros
#define CDC_ACM_CHECK(cond, ret_val) ({                             \
    if (!(cond)) {                                                  \
        return (ret_val);                                           \
    }                                                               \
})

#define CDC_ACM_CHECK_FROM_CRIT(cond, ret_val) ({                   \
    if (!(cond)) {                                                  \
        CDC_ACM_EXIT_CRITICAL();                                    \
        return ret_val;                                             \
    }                                                               \
})

typedef struct cdc_dev_s cdc_dev_t;
struct cdc_dev_s {
    cdc_acm_intf_t intf_func;             // CDC interface function table
    usb_device_handle_t dev_hdl;          // USB device handle
    void *cb_arg;                         // Common argument for user's callbacks (data IN and Notification)
    size_t operation_refs;                // Generic API callers still using immutable device state
    struct {
        usb_transfer_t *out_xfer;         // OUT data transfer
        usb_transfer_t *in_xfer;          // IN data transfer
        cdc_acm_data_callback_t in_cb;    // User's callback for async (non-blocking) data IN
        uint16_t in_mps;                  // IN endpoint Maximum Packet Size
        uint8_t *in_data_buffer_base;     // Pointer to IN data buffer in usb_transfer_t
        const usb_intf_desc_t *intf_desc; // Pointer to data interface descriptor
        SemaphoreHandle_t out_mux;        // OUT mutex
        bool data_poll_in_flight;         // Periodic bulk IN callback pending
        size_t data_callback_active;      // Bulk IN callbacks still using cdc_dev
        size_t out_operation_refs;        // Callers that can still access OUT state
        bool out_in_flight;               // Bulk OUT submitted but callback not drained
        bool out_poisoned;                // Timed-out OUT transfer must not be reused
    } data;

    struct {
        usb_transfer_t *xfer;             // IN notification transfer
        const usb_intf_desc_t *intf_desc; // Pointer to notification interface descriptor, can be NULL if there is no notification channel in the device
        cdc_acm_host_dev_callback_t cb;   // User's callback for device events
        bool notif_poll_in_flight;        // Periodic interrupt IN callback pending
        size_t notif_callback_active;     // Interrupt callbacks still using cdc_dev
    } notif;                              // Structure with Notif pipe data

    usb_transfer_t *ctrl_transfer;        // CTRL (endpoint 0) transfer
    SemaphoreHandle_t ctrl_mux;           // CTRL mutex
    size_t ctrl_operation_refs;           // Callers that can still access EP0 state
    bool ctrl_in_flight;                  // EP0 transfer submitted but callback not drained
    bool ctrl_poisoned;                   // Timed-out EP0 transfer must not be reused
    cdc_acm_uart_state_t serial_state;    // Serial State
    cdc_comm_protocol_t comm_protocol;
    cdc_data_protocol_t data_protocol;
    bool close_prepared;                    // IN transfers canceled; callbacks disabled
    bool closing;                           // Reject new operations while close drains
    bool device_gone;                       // Host reported physical removal
    uint32_t dispatch_generation;           // Last host event snapshot generation
    bool start_cleanup_retained;             // Open failed with a callback still pending
    esp_err_t poll_error;                    // First periodic resubmit failure
    bool data_interface_released;
    bool notif_interface_released;
    int cdc_func_desc_cnt;                // Number of CDC Functional descriptors in following array
    const usb_standard_desc_t *(*cdc_func_desc)[]; // Pointer to array of pointers to const usb_standard_desc_t
#ifdef CDC_HOST_REMOTE_WAKE_SUPPORTED
    bool remote_wakeup_enabled;           // Remote wakeup currently enabled/disabled on the device
#endif // CDC_HOST_REMOTE_WAKE_SUPPORTED
    SLIST_ENTRY(cdc_dev_s) list_entry;
};

// Internal lifetime admission used by wrapper and descriptor APIs before
// their first cdc_dev_t dereference. Close rejects new callers, then waits for
// every acquired reference before freeing the device.
esp_err_t cdc_acm_acquire_device_operation(cdc_acm_dev_hdl_t cdc_hdl,
                                           cdc_dev_t **cdc_dev_ret);
void cdc_acm_release_device_operation(cdc_dev_t *cdc_dev);
