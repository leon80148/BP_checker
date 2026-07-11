#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SOURCE="$ROOT/src/transports/UsbCdcTransport.cpp"
DRIVER="$ROOT/src/third_party/espressif_usb_host_cdc_acm/cdc_acm_host.c"
DRIVER_COMMON="$ROOT/src/third_party/espressif_usb_host_cdc_acm/cdc_host_common.h"
DRIVER_OPS="$ROOT/src/third_party/espressif_usb_host_cdc_acm/cdc_host_ops.c"
DRIVER_COMPLIANT="$ROOT/src/third_party/espressif_usb_host_cdc_acm/cdc_host_acm_compliant.c"
NOTIFICATION_PARSER="$ROOT/src/third_party/espressif_usb_host_cdc_acm/cdc_notification_parser.h"
CONCURRENCY="$ROOT/lib/transports/UsbCdcConcurrency.h"

begin_count=$(grep -Fc 'CALLBACK_POD_BEGIN' "$SOURCE" || true)
end_count=$(grep -Fc 'CALLBACK_POD_END' "$SOURCE" || true)
if [[ "$begin_count" -ne 5 || "$begin_count" != "$end_count" ]]; then
  echo "expected exactly five balanced CALLBACK_POD blocks" >&2
  exit 1
fi

blocks=$(sed -n '/CALLBACK_POD_BEGIN/,/CALLBACK_POD_END/p' "$SOURCE")
for forbidden in 'currentDetail' 'currentState' 'String' 'cdc_acm_host_close' \
  'delay(' 'portMAX_DELAY' 'xSemaphore' 'malloc(' 'calloc(' 'new ' 'delete '; do
  if grep -Fq -- "$forbidden" <<<"$blocks"; then
    echo "USB callback violates POD-only contract: $forbidden" >&2
    exit 1
  fi
done

compact_blocks=$(tr '\n' ' ' <<<"$blocks")
grep -Eq 'xStreamBufferSend\([^;]*,[[:space:]]*0\)' <<<"$compact_blocks"
grep -Eq 'xQueueSendToBack\([^;]*,[[:space:]]*0\)' <<<"$compact_blocks"
grep -Fq 'acquireCallback' <<<"$blocks"
grep -Fq 'nextRxEvent' "$SOURCE"
grep -Fq 'UsbCdcOrderedEvent' "$SOURCE"
grep -Fq 'STREAM_RESET' "$SOURCE"
grep -Fq 'UsbCdcSynchronizedState' "$SOURCE"
grep -Fq 'UsbCdcOrderedChannel' "$CONCURRENCY"
grep -Fq 'UsbCdcSessionGate' "$CONCURRENCY"
grep -Fq 'UsbCdcScopedLock' "$CONCURRENCY"
grep -Fq 'USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS' "$SOURCE"
grep -Fq 'USB_HOST_LIB_EVENT_FLAGS_ALL_FREE' "$SOURCE"
grep -Fq 'xSemaphoreTake(impl->daemonExit, portMAX_DELAY)' "$SOURCE"
grep -Fq 'loss_events=' "$SOURCE"
grep -Fq 'dropped_bytes=' "$SOURCE"
grep -Fq 'retireFailedOpenCallbackContext' "$SOURCE"
grep -Fq 'reapRetiredCallbackContexts' "$SOURCE"
if [[ $(grep -Fc 'reapRetiredCallbackContexts(impl)' "$SOURCE") -lt 2 ]]; then
  echo "retired failed-open callback contexts must be reaped in poll and before open" >&2
  exit 1
fi
grep -Fq 'recordRejectedDrop' "$CONCURRENCY"
grep -Fq 'beginOverflowLoss' "$CONCURRENCY"
grep -Fq 'beginTerminalLoss' "$CONCURRENCY"
grep -Fq 'diagnosticsSnapshot' "$CONCURRENCY"
grep -Fq 'pendingTerminalFact' "$CONCURRENCY"
grep -Fq 'acknowledgeTerminalFact' "$CONCURRENCY"
grep -Fq 'replayPendingTerminalFact' "$SOURCE"
if grep -Eq 'std::atomic<uint32_t>[[:space:]]+(producerEpoch|lifetimeDroppedBytes|lifetimeLossEpisodes|lifetimeOverflowEpisodes)' "$SOURCE"; then
  echo "USB loss diagnostics must share one synchronized snapshot" >&2
  exit 1
fi
data_callback_body=$(sed -n '/^static bool cdcDataCallback(/,/^}/p' "$SOURCE")
lease_failure=$(sed -n '/if (!lease)/,/^  }/p' <<<"$data_callback_body")
grep -Fq 'recordRejectedDrop' <<<"$lease_failure"
event_callback_body=$(sed -n '/^static void cdcEventCallback(/,/^}/p' "$SOURCE")
grep -Fq 'acknowledgeTerminalFact' <<<"$event_callback_body"
drain_body=$(sed -n '/^static void drainLifecycleQueue(/,/^}/p' "$SOURCE")
grep -Fq 'replayPendingTerminalFact' <<<"$drain_body"

starting_line=$(grep -n 'UsbCdcDaemonPhase::STARTING' "$SOURCE" | head -1 | cut -d: -f1)
create_line=$(grep -n 'xTaskCreatePinnedToCore(' "$SOURCE" | tail -1 | cut -d: -f1)
if [[ -z "$starting_line" || -z "$create_line" || "$starting_line" -ge "$create_line" ]]; then
  echo "USB daemon must enter STARTING before task creation" >&2
  exit 1
fi

grep -Fq 'cdc_acm_close_request_t' "$DRIVER"
grep -Fq 'close_request_queue' "$DRIVER"
grep -Fq 'CDC_ACM_UNINSTALL_WAIT_MS' "$DRIVER"
grep -Fq 'uninstalling' "$DRIVER"
grep -Fq 'cdc_acm_wait_interface_release' "$DRIVER"
grep -Fq 'close_prepared' "$DRIVER"
grep -Fq '!cdc_dev->close_prepared' "$DRIVER"
grep -Fq 'data_interface_claimed' "$DRIVER"
grep -Fq 'notif_interface_claimed' "$DRIVER"
grep -Fq 'usb_host_client_unblock' "$DRIVER"
grep -Fq 'portMAX_DELAY' "$DRIVER"
grep -Fq 'StaticSemaphore_t cdc_acm_lifetime_mutex_storage' "$DRIVER"
grep -Fq 'cdc_acm_lifetime_lock' "$DRIVER"
grep -Fq 'cdc_acm_lifetime_unlock' "$DRIVER"
if grep -Fq 'vSemaphoreDelete(cdc_acm_lifetime_mutex' "$DRIVER"; then
  echo "driver lifetime mutex must outlive every driver instance" >&2
  exit 1
fi
for api in cdc_acm_host_install cdc_acm_host_open cdc_acm_host_close \
  cdc_acm_host_uninstall cdc_acm_host_register_new_dev_callback; do
  body=$(sed -n "/esp_err_t $api(/,/^}/p" "$DRIVER")
  grep -Fq 'cdc_acm_lifetime_lock' <<<"$body"
  grep -Fq 'cdc_acm_lifetime_unlock' <<<"$body"
done

close_request=$(sed -n '/typedef struct {/,/} cdc_acm_close_request_t;/p' "$DRIVER")
close_api=$(sed -n '/esp_err_t cdc_acm_host_close(cdc_acm_dev_hdl_t cdc_hdl)/,/^}/p' "$DRIVER")
grep -Fq 'SemaphoreHandle_t completion;' <<<"$close_request"
grep -Fq 'size_t ref_count;' <<<"$close_request"
grep -Fq 'cdc_acm_close_request_release' "$DRIVER"
grep -Fq 'xSemaphoreCreateBinary()' <<<"$close_api"
grep -Fq 'xSemaphoreTake(request->completion' <<<"$close_api"
if grep -Eq 'TaskHandle_t[[:space:]]+waiter|xTaskNotifyGive\(waiter\)' <<<"$close_request $close_api"; then
  echo "close completion must not consume the caller task notification slot" >&2
  exit 1
fi

grep -Fq 'ctrl_in_flight' "$ROOT/src/third_party/espressif_usb_host_cdc_acm/cdc_host_common.h"
grep -Fq 'ctrl_poisoned' "$ROOT/src/third_party/espressif_usb_host_cdc_acm/cdc_host_common.h"
grep -Fq 'ctrl_operation_refs' "$ROOT/src/third_party/espressif_usb_host_cdc_acm/cdc_host_common.h"
grep -Fq 'cdc_acm_admit_control_operation' "$DRIVER"
grep -Fq 'cdc_acm_submit_control_and_wait' "$DRIVER"
grep -Fq 'cdc_acm_drain_control_for_close' "$DRIVER"
custom_request=$(sed -n '/esp_err_t cdc_acm_host_send_custom_request(/,/^}/p' "$DRIVER")
grep -Fq 'if (!in_transfer && wLength > 0)' <<<"$custom_request"
grep -Fq 'if (in_transfer && wLength > 0)' <<<"$custom_request"
grep -Fq 'out_in_flight' "$ROOT/src/third_party/espressif_usb_host_cdc_acm/cdc_host_common.h"
grep -Fq 'out_poisoned' "$ROOT/src/third_party/espressif_usb_host_cdc_acm/cdc_host_common.h"
grep -Fq 'out_operation_refs' "$ROOT/src/third_party/espressif_usb_host_cdc_acm/cdc_host_common.h"
grep -Fq 'cdc_acm_admit_out_operation' "$DRIVER"
grep -Fq 'cdc_acm_drain_out_for_close' "$DRIVER"
grep -Fq 'bool device_gone;' "$DRIVER_COMMON"
grep -Fq 'cdc_acm_status_is_gone' "$DRIVER"
grep -Fq 'cdc_acm_drain_periodic_for_close' "$DRIVER"
grep -Fq 'cdc_acm_dispatch_device_gone' "$DRIVER"
grep -Fq 'cdc_acm_snapshot_open_candidate' "$DRIVER"
grep -Fq 'cdc_acm_dispatch_suspend_resume' "$DRIVER"
resume_body=$(sed -n '/static esp_err_t cdc_acm_resume(/,/^}/p' "$DRIVER")
grep -Fq 'cdc_acm_report_poll_error' <<<"$resume_body"
resume_dispatch=$(sed -n '/static void cdc_acm_dispatch_suspend_resume(/,/^}/p' "$DRIVER")
grep -Fq 'resume_result == ESP_OK' <<<"$resume_dispatch"
usb_event_body=$(sed -n '/^static void usb_event_cb(const usb_host_client_event_msg_t \*event_msg, void \*arg)$/,/^}/p' "$DRIVER" | tail -n +2)
if grep -Fq 'SLIST_FOREACH_SAFE' <<<"$usb_event_body"; then
  echo "USB host events must pin list entries before callback dispatch" >&2
  exit 1
fi
reset_body=$(sed -n '/static esp_err_t cdc_acm_reset_transfer_endpoint(/,/^}/p' "$DRIVER")
release_body=$(sed -n '/static esp_err_t cdc_acm_wait_interface_release(/,/^}/p' "$DRIVER")
grep -Fq 'cdc_acm_status_is_gone' <<<"$reset_body"
grep -Fq 'cdc_acm_status_is_gone' <<<"$release_body"
control_drain=$(sed -n '/static esp_err_t cdc_acm_drain_control_for_close(/,/^}/p' "$DRIVER")
out_drain=$(sed -n '/static esp_err_t cdc_acm_drain_out_for_close(/,/^}/p' "$DRIVER")
if grep -Fq 'cdc_acm_device_is_gone' <<<"$control_drain$out_drain"; then
  echo "DEV_GONE cannot synthesize control/OUT callback completion" >&2
  exit 1
fi
if grep -Fq 'cdc_acm_finalize_gone_poll_state' "$DRIVER"; then
  echo "DEV_GONE cannot synthesize periodic callback completion" >&2
  exit 1
fi

find_body=$(sed -n '/static esp_err_t cdc_acm_find_and_open_usb_device(/,/^}/p' "$DRIVER")
grep -Fq 'cdc_acm_snapshot_open_candidate' <<<"$find_body"
open_body=$(sed -n '/esp_err_t cdc_acm_host_open(/,/^}/p' "$DRIVER")
if grep -Fq 'ESP_ERROR_CHECK(' <<<"$find_body$open_body"; then
  echo "device removal during open must unwind through returned errors" >&2
  exit 1
fi

# Every API that dispatches through or reads a cdc_dev_t must pin that device
# before its first raw handle dereference. Close marks the device closing and
# waits for these generic references before freeing immutable interface data.
grep -Fq 'size_t operation_refs;' "$DRIVER_COMMON"
grep -Fq 'cdc_acm_acquire_device_operation' "$DRIVER_COMMON"
grep -Fq 'cdc_acm_release_device_operation' "$DRIVER_COMMON"
grep -Fq 'cdc_acm_wait_device_operations' "$DRIVER"
grep -Fq 'assert(cdc_dev->operation_refs == 0)' "$DRIVER"

for api in cdc_acm_host_line_coding_set cdc_acm_host_line_coding_get \
  cdc_acm_host_set_control_line_state cdc_acm_host_send_break; do
  body=$(sed -n "/esp_err_t $api(/,/^}/p" "$DRIVER_OPS")
  acquire_line=$(grep -n 'cdc_acm_acquire_device_operation' <<<"$body" | head -1 | cut -d: -f1)
  dispatch_line=$(grep -n 'intf_func' <<<"$body" | head -1 | cut -d: -f1)
  if [[ -z "$acquire_line" || -z "$dispatch_line" ||
        "$acquire_line" -ge "$dispatch_line" ]]; then
    echo "$api must pin cdc_dev before interface dispatch" >&2
    exit 1
  fi
  grep -Fq 'cdc_acm_release_device_operation' <<<"$body"
done

compliant_request=$(sed -n '/static esp_err_t send_cdc_request(/,/^}/p' "$DRIVER_COMPLIANT")
acquire_line=$(grep -n 'cdc_acm_acquire_device_operation' <<<"$compliant_request" | head -1 | cut -d: -f1)
descriptor_line=$(grep -n 'notif.intf_desc' <<<"$compliant_request" | head -1 | cut -d: -f1)
if [[ -z "$acquire_line" || -z "$descriptor_line" ||
      "$acquire_line" -ge "$descriptor_line" ]]; then
  echo "CDC compliant requests must pin cdc_dev before descriptor access" >&2
  exit 1
fi
grep -Fq 'cdc_acm_release_device_operation' <<<"$compliant_request"

for api in cdc_acm_host_desc_print cdc_acm_host_protocols_get \
  cdc_acm_host_cdc_desc_get; do
  body=$(sed -n "/$api(/,/^}/p" "$DRIVER")
  grep -Fq 'cdc_acm_acquire_device_operation' <<<"$body"
  grep -Fq 'cdc_acm_release_device_operation' <<<"$body"
done

start_body=$(sed -n '/static esp_err_t cdc_acm_start(/,/^}/p' "$DRIVER")
if grep -Eq 'ESP_ERROR_CHECK\(usb_host_transfer_submit\(' <<<"$start_body"; then
  echo "CDC start transfer submission failures must be returned, not abort" >&2
  exit 1
fi
grep -Fq 'cdc_acm_cancel_started_transfers' <<<"$start_body"
grep -Fq 'data_poll_submitted || notif_poll_submitted' <<<"$start_body"
grep -Fq 'data_interface_claimed || notif_interface_claimed' <<<"$start_body"
grep -Fq 'data_poll_in_flight' "$ROOT/src/third_party/espressif_usb_host_cdc_acm/cdc_host_common.h"
grep -Fq 'notif_poll_in_flight' "$ROOT/src/third_party/espressif_usb_host_cdc_acm/cdc_host_common.h"
grep -Fq 'data_callback_active' "$ROOT/src/third_party/espressif_usb_host_cdc_acm/cdc_host_common.h"
grep -Fq 'notif_callback_active' "$ROOT/src/third_party/espressif_usb_host_cdc_acm/cdc_host_common.h"
grep -Fq 'cdc_acm_submit_data_poll' "$DRIVER"
grep -Fq 'cdc_acm_submit_notif_poll' "$DRIVER"
grep -Fq 'cdc_acm_schedule_internal_close' "$DRIVER"
grep -Fq 'cdc_acm_retry_retained_cleanup' "$DRIVER"
grep -Fq 'cdc_acm_notification_parse' "$NOTIFICATION_PARSER"

# User callback pointers and their shared argument are one synchronization
# unit. Transfer callbacks may only invoke an atomic local snapshot, while a
# failed start disables future snapshots before cdc_acm_host_open() returns.
grep -Fq 'cdc_acm_snapshot_data_callbacks' "$DRIVER"
grep -Fq 'cdc_acm_snapshot_event_callback' "$DRIVER"
grep -Fq 'cdc_acm_latch_transfer_failure' "$DRIVER"
latch_body=$(sed -n '/static void cdc_acm_latch_transfer_failure(/,/^}/p' "$DRIVER")
grep -Fq 'event_cb = cdc_dev->notif.cb' <<<"$latch_body"
grep -Fq 'cb_arg = cdc_dev->cb_arg' <<<"$latch_body"
grep -Fq 'if (status == USB_TRANSFER_STATUS_CANCELED) return;' <<<"$latch_body"
grep -Fq 'cdc_acm_disable_user_callbacks' "$DRIVER"
grep -Fq 'cdc_acm_enable_user_callbacks' "$DRIVER"

data_callback=$(sed -n '/^static void in_xfer_cb(usb_transfer_t \*transfer)$/,/^}/p' "$DRIVER" | tail -n +2)
notif_callback=$(sed -n '/^static void notif_xfer_cb(usb_transfer_t \*transfer)$/,/^}/p' "$DRIVER" | tail -n +2)
if grep -Fq 'ESP_LOG_BUFFER_HEX' <<<"$notif_callback"; then
  echo "CDC notifications must not log raw device payloads" >&2
  exit 1
fi
grep -Fq 'cdc_acm_notification_parse' <<<"$notif_callback"
grep -Fq 'ESP_ERR_INVALID_SIZE' <<<"$notif_callback"
grep -Fq 'notif.interfaceIndex' <<<"$notif_callback"
grep -Fq 'cdc_acm_report_poll_error' <<<"$notif_callback"
for callback_body in "$data_callback" "$notif_callback"; do
  if grep -Eq 'cdc_dev->(data\.in_cb|notif\.cb)[[:space:]]*\(|cdc_dev->cb_arg' \
      <<<"$callback_body"; then
    echo "transfer callbacks must invoke an atomic callback/argument snapshot" >&2
    exit 1
  fi
done

enable_line=$(grep -n 'cdc_acm_enable_user_callbacks' <<<"$start_body" | tail -1 | cut -d: -f1)
last_submit_line=$(grep -n 'cdc_acm_submit_.*_poll' <<<"$start_body" | tail -1 | cut -d: -f1)
if [[ -z "$enable_line" || -z "$last_submit_line" ||
      "$enable_line" -le "$last_submit_line" ]]; then
  echo "failed-open callback context must not be published before all initial polls submit" >&2
  exit 1
fi
cancel_body=$(sed -n '/static esp_err_t cdc_acm_cancel_started_transfers(/,/^}/p' "$DRIVER")
grep -Fq 'cdc_acm_disable_user_callbacks' <<<"$cancel_body"

grep -Fq 'task_quiescent' "$DRIVER"
grep -Fq 'teardown_result' "$DRIVER"
grep -Fq 'vTaskDelete(cdc_acm_obj->driver_task_h)' "$DRIVER"
client_task=$(sed -n '/static void cdc_acm_client_task(/,/^}/p' "$DRIVER")
if grep -Fq 'vTaskDelete(NULL)' <<<"$client_task"; then
  echo "uninstaller must join/delete the CDC task before freeing its event group" >&2
  exit 1
fi
if grep -Fq 'ESP_ERROR_CHECK(usb_host_client_deregister' <<<"$client_task"; then
  echo "CDC teardown must return deregistration failure without rebooting" >&2
  exit 1
fi
grep -Fq 'usb_host_client_deregister' <<<"$client_task"
grep -Fq 'continue;' <<<"$client_task"
teardown_signal_line=$(grep -n 'xEventGroupSetBits(completion_event, CDC_ACM_TEARDOWN_COMPLETE)' "$DRIVER" | tail -1 | cut -d: -f1)
quiescent_line=$(grep -n 'task_quiescent = true' "$DRIVER" | cut -d: -f1)
if [[ ! "$teardown_signal_line" =~ ^[0-9]+$ ||
      ! "$quiescent_line" =~ ^[0-9]+$ ||
      "$teardown_signal_line" -ge "$quiescent_line" ]]; then
  echo "CDC task may publish quiescence only after teardown signaling returns" >&2
  exit 1
fi

if grep -Eq 'volatile[[:space:]].*(rxHead|rxTail|connected|cdcHandle)' "$SOURCE"; then
  echo "legacy volatile cross-task ownership remains" >&2
  exit 1
fi

echo "USB callback ownership checks passed."
