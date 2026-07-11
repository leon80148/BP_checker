#include "../../lib/transports/UsbCdcTransport.h"
#include "../../lib/transports/UsbCdcConcurrency.h"
#include "../../lib/transports/UsbCdcState.h"

#include <atomic>
#include <limits.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include <soc/soc_caps.h>

#if SOC_USB_OTG_SUPPORTED
#include <esp_err.h>
#include <esp_intr_alloc.h>
#include <usb/usb_host.h>
#include "../third_party/espressif_usb_host_cdc_acm/usb/cdc_acm_host.h"
#endif

namespace {
constexpr size_t kRxUsableBytes = 1024;
constexpr size_t kRxStorageBytes = kRxUsableBytes + 1;
constexpr UBaseType_t kLifecycleQueueDepth = 16;
constexpr size_t kOrderedChannelDepth = 12;
constexpr size_t kCallbackContextCount = 2;
constexpr uint32_t kOpenTimeoutMs = 20;
constexpr size_t kTeardownEventAttempts = 50;
constexpr size_t kCdcUninstallAttempts = 5;
constexpr size_t kDestructorCloseAttempts = 3;
constexpr size_t kCallbackQuiesceAttempts = 128;

uint32_t saturatingAdd(uint32_t left, uint32_t right) {
  if (UINT32_MAX - left < right) return UINT32_MAX;
  return left + right;
}

uint32_t atomicSaturatingAdd(std::atomic<uint32_t>& target, uint32_t amount) {
  uint32_t current = target.load(std::memory_order_relaxed);
  while (true) {
    uint32_t next = saturatingAdd(current, amount);
    if (target.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
      return next;
    }
  }
}

uint32_t atomicSaturatingIncrement(std::atomic<uint32_t>& target) {
  return atomicSaturatingAdd(target, 1);
}
}  // namespace

class UsbCdcPortMutex {
public:
  void lock() { portENTER_CRITICAL(&_mux); }
  void unlock() { portEXIT_CRITICAL(&_mux); }

private:
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

struct UsbCdcTransport::Impl {
  struct CallbackContext {
    Impl* owner = nullptr;
    size_t slot = 0;
    uint32_t session = 0;
  };

  UsbCdcLifecycle lifecycle;
  UsbCdcOrderedCursor cursor;
  MonitorTransportState currentState = TRANSPORT_STATE_STARTING;
  String currentDetail = "USB CDC host not initialized";

  StaticStreamBuffer_t rxStreamState = {};
  alignas(4) uint8_t rxStreamStorage[kRxStorageBytes] = {};
  StreamBufferHandle_t rxStream = nullptr;

  StaticQueue_t lifecycleQueueState = {};
  alignas(4) uint8_t lifecycleQueueStorage[
    kLifecycleQueueDepth * sizeof(UsbCdcControlEvent)] = {};
  QueueHandle_t lifecycleQueue = nullptr;

  UsbCdcSynchronizedState<UsbCdcPortMutex, kOrderedChannelDepth,
                          kCallbackContextCount> shared;
  CallbackContext callbackContexts[kCallbackContextCount];
  int activeCallbackSlot = -1;

  std::atomic<uint32_t> acceptedByteSequence{0};
  std::atomic<uint32_t> lifecycleQueueFailures{0};
  std::atomic<bool> overflowMarkerPublished{false};
  std::atomic<bool> shutdownRequested{false};
  std::atomic<bool> teardownSafe{true};
  std::atomic<UsbCdcDaemonPhase> daemonPhase{UsbCdcDaemonPhase::STOPPED};

  StaticSemaphore_t daemonExitState = {};
  SemaphoreHandle_t daemonExit = nullptr;
  bool beginCalled = false;
  bool daemonTaskStarted = false;
  uint32_t activeSession = 0;
  uint32_t lastMillis32 = 0;
  uint64_t millisHigh = 0;
  bool millisInitialized = false;
  bool terminalBoundaryPending = false;
  UsbCdcTerminalBoundaryTracker terminalBoundary;
  UsbCdcControlEvent pendingTerminalEvent;
  UsbCdcOrderedType pendingTerminalType = UsbCdcOrderedType::STREAM_RESET;
  UsbCdcDiagnosticsSnapshot currentDiagnostics;
  uint32_t currentReconnectCount = 0;

#if SOC_USB_OTG_SUPPORTED
  cdc_acm_dev_hdl_t cdcHandle = nullptr;
#endif

  Impl() {
    for (size_t i = 0; i < kCallbackContextCount; ++i) {
      callbackContexts[i].owner = this;
      callbackContexts[i].slot = i;
    }
    cursor.beginSession(0, 0, 0);
  }

  uint64_t monotonicMillis() {
    uint32_t now = millis();
    if (!millisInitialized) {
      lastMillis32 = now;
      millisInitialized = true;
    } else if (now < lastMillis32) {
      millisHigh += (static_cast<uint64_t>(1) << 32);
    }
    lastMillis32 = now;
    return millisHigh + now;
  }
};

#if SOC_USB_OTG_SUPPORTED
static std::atomic<UsbCdcTransport::Impl*> gUsbCdcImpl{nullptr};

// CALLBACK_POD_BEGIN lifecycle queue producer
static bool enqueueLifecycle(UsbCdcTransport::Impl* impl,
                             UsbCdcControlType type, int32_t code = 0,
                             uint32_t count = 0, uint32_t session = 0,
                             bool critical = true) {
  if (impl == nullptr || impl->lifecycleQueue == nullptr) return false;
  if (!critical && !usbCdcMayEnqueueNormalControl(
        uxQueueSpacesAvailable(impl->lifecycleQueue))) {
    return false;
  }
  UsbCdcControlEvent event;
  event.type = type;
  event.code = code;
  event.count = count;
  event.session = session;
  if (xQueueSendToBack(impl->lifecycleQueue, &event, 0) == pdPASS) return true;
  if (critical) {
    atomicSaturatingIncrement(impl->lifecycleQueueFailures);
    impl->shared.quarantineTerminal(session);
  }
  return false;
}
// CALLBACK_POD_END lifecycle queue producer

// CALLBACK_POD_BEGIN ordered loss producer
static UsbCdcOrderedPublishResult enqueueOrdered(
    UsbCdcTransport::Impl* impl, UsbCdcOrderedType type, uint32_t session,
    uint32_t epoch, uint32_t droppedBytes, bool resumeProducer = false,
    bool quarantineBarrier = false) {
  UsbCdcOrderedEvent event;
  event.type = type;
  event.session = session;
  event.epoch = epoch;
  event.byteBoundary =
    impl->acceptedByteSequence.load(std::memory_order_acquire);
  event.droppedBytes = droppedBytes;
  if (quarantineBarrier) {
    return impl->shared.publishQuarantineBarrier(event);
  }
  return impl->shared.publish(event, resumeProducer);
}
// CALLBACK_POD_END ordered loss producer

static uint32_t beginTerminalLoss(UsbCdcTransport::Impl* impl) {
  return impl->shared.beginTerminalLoss();
}

// CALLBACK_POD_BEGIN CDC data callback
static bool cdcDataCallback(const uint8_t* data, size_t dataLen, void* userArg) {
  auto* context = static_cast<UsbCdcTransport::Impl::CallbackContext*>(userArg);
  if (context == nullptr || context->owner == nullptr || data == nullptr ||
      dataLen == 0) {
    return true;
  }
  auto* impl = context->owner;
  uint32_t rejectedBytes = dataLen > UINT32_MAX
    ? UINT32_MAX : static_cast<uint32_t>(dataLen);
  auto lease = impl->shared.acquireCallback(context->slot, context->session);
  if (!lease) {
    impl->shared.recordRejectedDrop(rejectedBytes);
    return true;
  }
  uint32_t epoch = impl->shared.producerEpoch();
  UsbCdcByteAdmission admission = UsbCdcByteAdmission::REJECTED_INACTIVE;
  auto byteCommit = impl->shared.acquireByteCommitOrRecordDrop(
    context->slot, context->session, epoch, rejectedBytes, admission);
  if (!byteCommit) {
    impl->shared.recordRejectedDrop(rejectedBytes);
    return true;
  }

  size_t sent = xStreamBufferSend(impl->rxStream, data, dataLen, 0);
  impl->acceptedByteSequence.fetch_add(static_cast<uint32_t>(sent),
                                       std::memory_order_release);
  if (sent < dataLen) {
    size_t lostSize = dataLen - sent;
    uint32_t lost = lostSize > UINT32_MAX ? UINT32_MAX
                                          : static_cast<uint32_t>(lostSize);
    epoch = impl->shared.beginOverflowLoss(lost);
    impl->overflowMarkerPublished.store(true, std::memory_order_release);
    enqueueOrdered(impl, UsbCdcOrderedType::DISCONTINUITY,
                   context->session, epoch, lost, true, true);
  }
  return true;  // false makes the vendored driver append into its USB buffer.
}
// CALLBACK_POD_END CDC data callback

// CALLBACK_POD_BEGIN CDC lifecycle callback
static void cdcEventCallback(const cdc_acm_host_dev_event_data_t* event,
                             void* userArg) {
  auto* context = static_cast<UsbCdcTransport::Impl::CallbackContext*>(userArg);
  if (context == nullptr || context->owner == nullptr || event == nullptr) return;
  auto* impl = context->owner;
  bool terminalEvent = event->type == CDC_ACM_HOST_ERROR ||
                       event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED;
  auto lease = terminalEvent
    ? impl->shared.acquireTerminalCallback(context->slot, context->session)
    : impl->shared.acquireCallback(context->slot, context->session);
  if (!lease) return;
  switch (event->type) {
    case CDC_ACM_HOST_ERROR: {
      if (impl->shared.noteTerminalFact(
            context->session, UsbCdcTerminalFact::ERROR) ==
          UsbCdcTerminalUpdate::IGNORED) break;
      if (enqueueLifecycle(impl, UsbCdcControlType::TRANSFER_ERROR,
                           event->data.error, 0, context->session, true)) {
        impl->shared.acknowledgeTerminalFact(
          context->session, UsbCdcTerminalFact::ERROR);
      }
      break;
    }
    case CDC_ACM_HOST_DEVICE_DISCONNECTED: {
      if (impl->shared.noteTerminalFact(
            context->session, UsbCdcTerminalFact::DISCONNECTED) ==
          UsbCdcTerminalUpdate::IGNORED) break;
      if (enqueueLifecycle(impl, UsbCdcControlType::DEVICE_DISCONNECTED, 0, 0,
                           context->session, true)) {
        impl->shared.acknowledgeTerminalFact(
          context->session, UsbCdcTerminalFact::DISCONNECTED);
      }
      break;
    }
    case CDC_ACM_HOST_SERIAL_STATE:
    case CDC_ACM_HOST_NETWORK_CONNECTION:
      enqueueLifecycle(impl, UsbCdcControlType::RX_ACTIVITY, 0, 0,
                       context->session, false);
      break;
#ifdef CDC_HOST_SUSPEND_RESUME_API_SUPPORTED
    case CDC_ACM_HOST_DEVICE_SUSPENDED:
    case CDC_ACM_HOST_DEVICE_RESUMED:
      enqueueLifecycle(impl, UsbCdcControlType::RX_ACTIVITY, 0, 0,
                       context->session, false);
      break;
#endif
    default:
      break;
  }
}
// CALLBACK_POD_END CDC lifecycle callback

static uint32_t daemonRetryDelay(uint8_t attempt) {
  uint8_t shift = attempt > 5 ? 4 : static_cast<uint8_t>(attempt - 1);
  return static_cast<uint32_t>(1000U << shift);
}

static bool daemonWait(UsbCdcTransport::Impl* impl, uint32_t delayMs) {
  uint32_t remaining = delayMs;
  while (remaining > 0 &&
         !impl->shutdownRequested.load(std::memory_order_acquire)) {
    uint32_t slice = remaining > 100 ? 100 : remaining;
    vTaskDelay(pdMS_TO_TICKS(slice));
    remaining -= slice;
  }
  return !impl->shutdownRequested.load(std::memory_order_acquire);
}

static void noteTeardownFlags(UsbCdcTeardownTracker& tracker,
                              uint32_t eventFlags) {
  if ((eventFlags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0) {
    tracker.noteNoClients();
  }
  if ((eventFlags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0) {
    tracker.noteAllFree();
  }
}

static bool pumpHostTeardownEvent(UsbCdcTeardownTracker& tracker) {
  uint32_t eventFlags = 0;
  esp_err_t result = usb_host_lib_handle_events(pdMS_TO_TICKS(100),
                                                 &eventFlags);
  if (result == ESP_OK) noteTeardownFlags(tracker, eventFlags);
  return result == ESP_OK || result == ESP_ERR_TIMEOUT;
}

static bool teardownUsbHost(bool driverInstalled) {
  UsbCdcTeardownTracker tracker(driverInstalled);
  if (driverInstalled) {
    esp_err_t result = ESP_ERR_NOT_FINISHED;
    for (size_t attempt = 0;
         attempt < kCdcUninstallAttempts && result != ESP_OK; ++attempt) {
      result = cdc_acm_host_uninstall();
      if (result != ESP_OK && result != ESP_ERR_NOT_FINISHED) return false;
      if (result == ESP_ERR_NOT_FINISHED) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (result != ESP_OK) return false;
    tracker.noteCdcUninstalled();
    for (size_t attempt = 0;
         attempt < kTeardownEventAttempts && !tracker.mayFreeDevices();
         ++attempt) {
      if (!pumpHostTeardownEvent(tracker)) return false;
    }
    if (!tracker.mayFreeDevices()) return false;
  }

  esp_err_t freeResult = usb_host_device_free_all();
  if (freeResult != ESP_OK && freeResult != ESP_ERR_NOT_FINISHED) return false;
  tracker.noteFreeAllRequested(freeResult == ESP_OK);
  for (size_t attempt = 0;
       attempt < kTeardownEventAttempts && !tracker.mayUninstallHost();
       ++attempt) {
    if (!pumpHostTeardownEvent(tracker)) return false;
  }
  if (!tracker.mayUninstallHost()) return false;

  esp_err_t uninstallResult = usb_host_uninstall();
  if (uninstallResult != ESP_OK) return false;
  tracker.noteHostUninstalled();
  return tracker.complete();
}

static void usbHostDaemonTask(void* arg) {
  auto* impl = static_cast<UsbCdcTransport::Impl*>(arg);
  impl->daemonPhase.store(UsbCdcDaemonPhase::RUNNING,
                          std::memory_order_release);
  uint8_t retryAttempt = 0;

  while (!impl->shutdownRequested.load(std::memory_order_acquire)) {
    usb_host_config_t hostConfig = {};
    hostConfig.skip_phy_setup = false;
    hostConfig.root_port_unpowered = false;
    hostConfig.intr_flags = ESP_INTR_FLAG_LOWMED;

    esp_err_t result = usb_host_install(&hostConfig);
    if (result != ESP_OK) {
      enqueueLifecycle(impl, UsbCdcControlType::HOST_INSTALL_FAILED, result);
      if (retryAttempt < UINT8_MAX) retryAttempt++;
      if (!daemonWait(impl, daemonRetryDelay(retryAttempt))) break;
      continue;
    }
    enqueueLifecycle(impl, UsbCdcControlType::HOST_INSTALL_OK);

    const cdc_acm_host_driver_config_t driverConfig = {
      .driver_task_stack_size = 4096,
      .driver_task_priority = 5,
      .xCoreID = 0,
      .new_dev_cb = [](usb_device_handle_t) {
        // CALLBACK_POD_BEGIN new-device callback
        UsbCdcTransport::Impl* active =
          gUsbCdcImpl.load(std::memory_order_acquire);
        if (active != nullptr) {
          enqueueLifecycle(active, UsbCdcControlType::DEVICE_ATTACHED,
                           0, 0, 0, true);
        }
        // CALLBACK_POD_END new-device callback
      },
    };

    result = cdc_acm_host_install(&driverConfig);
    if (result != ESP_OK) {
      enqueueLifecycle(impl, UsbCdcControlType::DRIVER_INSTALL_FAILED, result);
      if (!teardownUsbHost(false)) {
        impl->teardownSafe.store(false, std::memory_order_release);
        break;
      }
      if (retryAttempt < UINT8_MAX) retryAttempt++;
      if (!daemonWait(impl, daemonRetryDelay(retryAttempt))) break;
      continue;
    }

    retryAttempt = 0;
    enqueueLifecycle(impl, UsbCdcControlType::DRIVER_INSTALL_OK);
    while (!impl->shutdownRequested.load(std::memory_order_acquire)) {
      uint32_t eventFlags = 0;
      result = usb_host_lib_handle_events(pdMS_TO_TICKS(100), &eventFlags);
      if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
        enqueueLifecycle(impl, UsbCdcControlType::CONTROL_QUEUE_OVERFLOW,
                         result, 0, 0, true);
      }
    }

    if (!teardownUsbHost(true)) {
      impl->teardownSafe.store(false, std::memory_order_release);
    }
    break;
  }

  if (impl->daemonExit != nullptr) xSemaphoreGive(impl->daemonExit);
  // This is the daemon's final access to Impl. The destructor waits for both
  // the semaphore and this release-store before freeing the backing storage.
  impl->daemonPhase.store(UsbCdcDaemonPhase::STOPPED,
                          std::memory_order_release);
  vTaskDelete(nullptr);
}
#endif  // SOC_USB_OTG_SUPPORTED

UsbCdcTransport::UsbCdcTransport() : impl(new Impl()) {}

UsbCdcTransport::~UsbCdcTransport() {
  if (impl == nullptr) return;
#if SOC_USB_OTG_SUPPORTED
  impl->shared.quarantineProducer(impl->activeSession);
  Impl* expected = impl;
  gUsbCdcImpl.compare_exchange_strong(expected, nullptr,
                                       std::memory_order_acq_rel);
  size_t closeAttempts = 0;
  while (impl->cdcHandle != nullptr &&
         closeAttempts < kDestructorCloseAttempts) {
    closeAttempts++;
    size_t slot = impl->activeCallbackSlot < 0
      ? kCallbackContextCount
      : static_cast<size_t>(impl->activeCallbackSlot);
    if (slot < kCallbackContextCount) {
      impl->shared.retireContext(slot, impl->activeSession);
    }
    esp_err_t result = cdc_acm_host_close(impl->cdcHandle);
    if (result == ESP_OK) {
      impl->cdcHandle = nullptr;
      if (slot < kCallbackContextCount) {
        UsbCdcCloseCompletion completion = impl->shared.finishClose(
          slot, impl->activeSession, true);
        while (completion == UsbCdcCloseCompletion::CALLBACKS_ACTIVE) {
          vTaskDelay(pdMS_TO_TICKS(1));
          completion = impl->shared.finishClose(
            slot, impl->activeSession, true);
        }
        if (completion == UsbCdcCloseCompletion::RELEASED) {
          impl->callbackContexts[slot].session = 0;
          impl->activeCallbackSlot = -1;
        }
      }
    } else {
      if (slot < kCallbackContextCount) {
        impl->shared.finishClose(slot, impl->activeSession, false);
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  if (impl->cdcHandle != nullptr) {
    impl->teardownSafe.store(false, std::memory_order_release);
  }
  impl->shutdownRequested.store(true, std::memory_order_release);
  UsbCdcDaemonPhase phase = impl->daemonPhase.load(std::memory_order_acquire);
  if (phase != UsbCdcDaemonPhase::STOPPED) {
    impl->daemonPhase.store(UsbCdcDaemonPhase::STOPPING,
                            std::memory_order_release);
    usb_host_lib_unblock();
  }
  if (impl->daemonTaskStarted && impl->daemonExit != nullptr) {
    xSemaphoreTake(impl->daemonExit, portMAX_DELAY);
    while (impl->daemonPhase.load(std::memory_order_acquire) !=
           UsbCdcDaemonPhase::STOPPED) {
      taskYIELD();
    }
  }
  if (!impl->teardownSafe.load(std::memory_order_acquire)) {
    // A driver/host task may still own callback pointers. Retain the complete
    // backing object instead of hanging teardown or creating a use-after-free.
    impl = nullptr;
    return;
  }
#endif
  memset(impl->rxStreamStorage, 0, sizeof(impl->rxStreamStorage));
  impl->shared.clearOrdered();
  delete impl;
  impl = nullptr;
}

bool UsbCdcTransport::begin() {
  if (impl == nullptr || impl->beginCalled) return false;
  impl->beginCalled = true;
  impl->rxStream = xStreamBufferCreateStatic(
    sizeof(impl->rxStreamStorage), 1, impl->rxStreamStorage,
    &impl->rxStreamState);
  impl->lifecycleQueue = xQueueCreateStatic(
    kLifecycleQueueDepth, sizeof(UsbCdcControlEvent),
    impl->lifecycleQueueStorage, &impl->lifecycleQueueState);
  impl->daemonExit = xSemaphoreCreateBinaryStatic(&impl->daemonExitState);
  if (impl->rxStream == nullptr || impl->lifecycleQueue == nullptr ||
      impl->daemonExit == nullptr) {
    impl->currentState = TRANSPORT_STATE_ERROR;
    impl->currentDetail = "USB queue allocation failed";
    return false;
  }

  UsbCdcControlEvent beginEvent;
  beginEvent.type = UsbCdcControlType::BEGIN;
  impl->lifecycle.apply(beginEvent, impl->monotonicMillis());

#if !SOC_USB_OTG_SUPPORTED
  impl->currentState = TRANSPORT_STATE_UNSUPPORTED;
  impl->currentDetail = "SOC_USB_OTG_SUPPORTED is not available on this target";
  return false;
#else
  Impl* expected = nullptr;
  if (!gUsbCdcImpl.compare_exchange_strong(expected, impl,
                                            std::memory_order_acq_rel)) {
    impl->currentState = TRANSPORT_STATE_ERROR;
    impl->currentDetail = "Another USB CDC transport is already active";
    return false;
  }
  impl->daemonPhase.store(UsbCdcDaemonPhase::STARTING,
                          std::memory_order_release);
  BaseType_t created = xTaskCreatePinnedToCore(
    usbHostDaemonTask, "usb_host_daemon", 6144, impl, 20, nullptr, 0);
  if (created != pdPASS) {
    impl->daemonPhase.store(UsbCdcDaemonPhase::STOPPED,
                            std::memory_order_release);
    impl->currentState = TRANSPORT_STATE_ERROR;
    impl->currentDetail = "Failed to create USB host daemon task";
    expected = impl;
    gUsbCdcImpl.compare_exchange_strong(expected, nullptr,
                                         std::memory_order_acq_rel);
    return false;
  }
  impl->daemonTaskStarted = true;
  impl->currentState = TRANSPORT_STATE_STARTING;
  impl->currentDetail = "Starting USB host stack";
  return true;
#endif
}

#if SOC_USB_OTG_SUPPORTED
static bool sessionScoped(UsbCdcControlType type) {
  switch (type) {
    case UsbCdcControlType::RX_ACTIVITY:
    case UsbCdcControlType::RX_OVERFLOW:
    case UsbCdcControlType::RX_CAPACITY_RECOVERED:
    case UsbCdcControlType::TRANSFER_ERROR:
    case UsbCdcControlType::DEVICE_DISCONNECTED:
    case UsbCdcControlType::HANDLE_CLOSE_FAILED:
      return true;
    default:
      return false;
  }
}

static void applyLifecycleEvent(UsbCdcTransport::Impl* impl,
                                const UsbCdcControlEvent& event,
                                uint64_t nowMs) {
  if (sessionScoped(event.type) && event.session != impl->activeSession) return;
  impl->lifecycle.apply(event, nowMs);
  switch (event.type) {
    case UsbCdcControlType::HOST_INSTALL_OK:
      impl->currentDetail = "USB host installed";
      break;
    case UsbCdcControlType::HOST_INSTALL_FAILED:
      impl->currentDetail = "usb_host_install failed; retry scheduled: ";
      impl->currentDetail += event.code;
      break;
    case UsbCdcControlType::DRIVER_INSTALL_OK:
      impl->currentDetail = "USB host ready. Waiting for CDC device.";
      break;
    case UsbCdcControlType::DRIVER_INSTALL_FAILED:
      impl->currentDetail = "cdc_acm_host_install failed; retry scheduled: ";
      impl->currentDetail += event.code;
      break;
    case UsbCdcControlType::DEVICE_ATTACHED:
      impl->currentDetail = "USB device detected. Probing CDC interface.";
      break;
    case UsbCdcControlType::TRANSFER_ERROR:
      impl->currentDetail = "CDC transfer error; reconnect scheduled: ";
      impl->currentDetail += event.code;
      break;
    case UsbCdcControlType::DEVICE_DISCONNECTED:
      impl->currentDetail = "CDC device disconnected";
      break;
    case UsbCdcControlType::HANDLE_CLOSE_FAILED:
      impl->currentDetail = "cdc_acm_host_close failed; ownership retained: ";
      impl->currentDetail += event.code;
      break;
    case UsbCdcControlType::RX_OVERFLOW:
      impl->currentDetail = "CDC RX overflow; frame boundary recovery active";
      break;
    case UsbCdcControlType::CONTROL_QUEUE_OVERFLOW:
      impl->currentDetail = "CDC control queue overflow; fail-closed recovery";
      break;
    default:
      break;
  }
}

static bool terminalOrderedType(UsbCdcControlType type,
                                UsbCdcOrderedType& orderedType) {
  switch (type) {
    case UsbCdcControlType::TRANSFER_ERROR:
      orderedType = UsbCdcOrderedType::DISCONTINUITY;
      return true;
    case UsbCdcControlType::CONFIG_FAILED:
    case UsbCdcControlType::DEVICE_DISCONNECTED:
    case UsbCdcControlType::CONTROL_QUEUE_OVERFLOW:
      orderedType = UsbCdcOrderedType::STREAM_RESET;
      return true;
    default:
      return false;
  }
}

static bool quiesceTerminalCallbacks(UsbCdcTransport::Impl* impl,
                                     uint32_t session) {
  if (impl->activeCallbackSlot < 0) return true;
  size_t slot = static_cast<size_t>(impl->activeCallbackSlot);
  if (!impl->shared.contextMatches(slot, session)) return true;
  impl->shared.retireContext(slot, session);
  for (size_t attempt = 0; attempt < kCallbackQuiesceAttempts; ++attempt) {
    if (impl->shared.callbacksQuiescent(slot, session)) return true;
    taskYIELD();
  }
  return impl->shared.callbacksQuiescent(slot, session);
}

static bool publishTerminalBoundary(UsbCdcTransport::Impl* impl,
                                    uint32_t session,
                                    UsbCdcOrderedType orderedType) {
  impl->shared.quarantineTerminal(session);
  if (!impl->terminalBoundary.needsPublish(orderedType)) return true;
  if (!quiesceTerminalCallbacks(impl, session)) return false;
  uint32_t epoch = beginTerminalLoss(impl);
  enqueueOrdered(impl, orderedType, session, epoch, 0, false);
  impl->terminalBoundary.notePublished(orderedType);
  return true;
}

static bool applyTerminalEventOrDefer(UsbCdcTransport::Impl* impl,
                                      const UsbCdcControlEvent& event,
                                      uint64_t nowMs) {
  UsbCdcControlEvent normalized = event;
  normalized.session = usbCdcEffectiveControlSession(
    normalized.type, normalized.session, impl->activeSession);
  if (sessionScoped(normalized.type) &&
      normalized.session != impl->activeSession) {
    return true;
  }
  UsbCdcOrderedType orderedType;
  if (!terminalOrderedType(normalized.type, orderedType)) {
    applyLifecycleEvent(impl, normalized, nowMs);
    return true;
  }
  if (!publishTerminalBoundary(impl, normalized.session, orderedType)) {
    impl->pendingTerminalEvent = normalized;
    impl->pendingTerminalType = orderedType;
    impl->terminalBoundaryPending = true;
    return false;
  }
  applyLifecycleEvent(impl, normalized, nowMs);
  return true;
}

static bool replayPendingTerminalFact(UsbCdcTransport::Impl* impl,
                                      uint64_t nowMs) {
  UsbCdcTerminalFact fact =
    impl->shared.pendingTerminalFact(impl->activeSession);
  if (fact == UsbCdcTerminalFact::NONE) return true;

  UsbCdcControlEvent replay;
  replay.session = impl->activeSession;
  if (fact == UsbCdcTerminalFact::DISCONNECTED) {
    replay.type = UsbCdcControlType::DEVICE_DISCONNECTED;
  } else {
    replay.type = UsbCdcControlType::TRANSFER_ERROR;
    replay.code = ESP_FAIL;
  }
  if (!applyTerminalEventOrDefer(impl, replay, nowMs)) return false;
  impl->shared.acknowledgeTerminalFact(impl->activeSession, fact);
  return true;
}

static void drainLifecycleQueue(UsbCdcTransport::Impl* impl, uint64_t nowMs) {
  if (impl->terminalBoundaryPending) {
    if (!publishTerminalBoundary(impl, impl->pendingTerminalEvent.session,
                                 impl->pendingTerminalType)) {
      return;
    }
    UsbCdcControlEvent pending = impl->pendingTerminalEvent;
    impl->terminalBoundaryPending = false;
    applyLifecycleEvent(impl, pending, nowMs);
  }

  UsbCdcControlEvent event;
  while (xQueueReceive(impl->lifecycleQueue, &event, 0) == pdPASS) {
    if (!applyTerminalEventOrDefer(impl, event, nowMs)) return;
  }
  uint32_t failures = impl->lifecycleQueueFailures.exchange(
    0, std::memory_order_acq_rel);
  if (failures > 0) {
    UsbCdcControlEvent failure;
    failure.type = UsbCdcControlType::CONTROL_QUEUE_OVERFLOW;
    failure.code = static_cast<int32_t>(failures);
    failure.session = impl->activeSession;
    impl->shared.quarantineTerminal(impl->activeSession);
    if (!applyTerminalEventOrDefer(impl, failure, nowMs)) return;
  }
  (void)replayPendingTerminalFact(impl, nowMs);
}

static void retireFailedOpenCallbackContext(UsbCdcTransport::Impl* impl) {
  if (impl->activeCallbackSlot < 0) return;
  size_t slot = static_cast<size_t>(impl->activeCallbackSlot);
  uint32_t session = impl->callbackContexts[slot].session;
  if (session != 0) {
    impl->shared.retireContext(slot, session);
  }
  // Open failure guarantees the driver has disabled future callback
  // snapshots. Detach this slot from new opens, but keep its immutable
  // session until every already-snapshotted callback lease drains.
  impl->activeCallbackSlot = -1;
}

static void reapRetiredCallbackContexts(UsbCdcTransport::Impl* impl) {
  for (size_t slot = 0; slot < kCallbackContextCount; ++slot) {
    if (impl->activeCallbackSlot == static_cast<int>(slot)) continue;
    uint32_t session = impl->callbackContexts[slot].session;
    if (session != 0 && impl->shared.abandonContext(slot, session)) {
      impl->callbackContexts[slot].session = 0;
    }
  }
}

static bool closeOwnedHandle(UsbCdcTransport::Impl* impl, uint64_t nowMs) {
  if (!impl->lifecycle.takeCloseRequest()) return false;
  cdc_acm_dev_hdl_t handle = impl->cdcHandle;
  size_t slot = impl->activeCallbackSlot < 0
    ? kCallbackContextCount
    : static_cast<size_t>(impl->activeCallbackSlot);
  if (slot < kCallbackContextCount) {
    impl->shared.retireContext(slot, impl->activeSession);
  }
  esp_err_t result = handle == nullptr ? ESP_ERR_INVALID_STATE
                                        : cdc_acm_host_close(handle);
  UsbCdcControlEvent outcome;
  outcome.session = impl->activeSession;
  if (result == ESP_OK) {
    impl->cdcHandle = nullptr;
    UsbCdcCloseCompletion completion = slot < kCallbackContextCount
      ? impl->shared.finishClose(slot, impl->activeSession, true)
      : UsbCdcCloseCompletion::RELEASED;
    if (completion == UsbCdcCloseCompletion::RELEASED) {
      if (slot < kCallbackContextCount) {
        impl->callbackContexts[slot].session = 0;
        impl->activeCallbackSlot = -1;
      }
      outcome.type = UsbCdcControlType::HANDLE_CLOSED;
    } else {
      outcome.type = UsbCdcControlType::HANDLE_CLOSE_FAILED;
      outcome.code = ESP_ERR_INVALID_STATE;
      result = ESP_ERR_INVALID_STATE;
    }
  } else {
    if (slot < kCallbackContextCount) {
      impl->shared.finishClose(slot, impl->activeSession, false);
    }
    outcome.type = UsbCdcControlType::HANDLE_CLOSE_FAILED;
    outcome.code = result;
  }
  applyLifecycleEvent(impl, outcome, nowMs);
  return result == ESP_OK;
}

static bool configurationMayContinue(UsbCdcTransport::Impl* impl,
                                     cdc_acm_dev_hdl_t candidate,
                                     const UsbCdcConfigToken& token) {
  if (impl->activeCallbackSlot < 0) return false;
  size_t slot = static_cast<size_t>(impl->activeCallbackSlot);
  bool handleMatches = candidate != nullptr && impl->cdcHandle == candidate;
  bool contextValid = impl->shared.configurationContextValid(
    slot, impl->activeSession);
  bool tokenValid = impl->shared.configurationTokenValid(token);
  return usbCdcConfigurationMayContinue(
    impl->lifecycle.phase(), impl->lifecycle.closePending(),
    handleMatches, contextValid, tokenValid);
}

static void failConfiguration(UsbCdcTransport::Impl* impl, uint64_t nowMs,
                              esp_err_t error) {
  UsbCdcControlEvent failed;
  failed.type = UsbCdcControlType::CONFIG_FAILED;
  failed.code = error;
  failed.session = impl->activeSession;
  impl->currentDetail = "CDC configuration failed; retry scheduled";
  if (applyTerminalEventOrDefer(impl, failed, nowMs)) {
    closeOwnedHandle(impl, nowMs);
  }
}

static void attemptOpenAndConfigure(UsbCdcTransport* self, uint64_t nowMs) {
  auto* impl = self->impl;
  reapRetiredCallbackContexts(impl);
  if (!impl->lifecycle.shouldAttemptOpen(nowMs)) return;

  bool sessionWrapped = impl->activeSession == UINT32_MAX;
  impl->activeSession = usbCdcNextSession(impl->activeSession);
  if (sessionWrapped) {
    xStreamBufferReset(impl->rxStream);
    impl->shared.clearOrdered();
    impl->acceptedByteSequence.store(0, std::memory_order_release);
    impl->cursor.beginSession(0, 0,
      impl->shared.producerEpoch());
  }
  impl->shared.startSession(impl->activeSession);
  UsbCdcConfigToken configToken = impl->shared.configurationToken();
  impl->terminalBoundaryPending = false;
  impl->terminalBoundary.reset();
  impl->overflowMarkerPublished.store(false, std::memory_order_release);

  UsbCdcControlEvent started;
  started.type = UsbCdcControlType::OPEN_STARTED;
  started.session = impl->activeSession;
  impl->lifecycle.apply(started, nowMs);
  impl->currentDetail = "Probing CDC device";

  int callbackSlot = impl->shared.acquireContext(impl->activeSession);
  if (callbackSlot < 0) {
    UsbCdcControlEvent failed;
    failed.type = UsbCdcControlType::OPEN_FAILED;
    failed.code = ESP_ERR_NO_MEM;
    failed.session = impl->activeSession;
    impl->lifecycle.apply(failed, nowMs);
    impl->currentDetail = "No quiescent CDC callback context available";
    return;
  }
  impl->activeCallbackSlot = callbackSlot;
  UsbCdcTransport::Impl::CallbackContext* callbackContext =
    &impl->callbackContexts[static_cast<size_t>(callbackSlot)];
  callbackContext->session = impl->activeSession;

  cdc_acm_host_device_config_t config = {
    .connection_timeout_ms = kOpenTimeoutMs,
    .out_buffer_size = 64,
    .in_buffer_size = 64,
    .event_cb = cdcEventCallback,
    .data_cb = cdcDataCallback,
    .user_arg = callbackContext,
  };

  cdc_acm_dev_hdl_t candidate = nullptr;
  esp_err_t openResult = ESP_ERR_NOT_FOUND;
  uint8_t openedInterface = 0;
  for (uint8_t interfaceIndex = 0; interfaceIndex < 3; ++interfaceIndex) {
    openResult = cdc_acm_host_open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID,
                                   interfaceIndex, &config, &candidate);
    if (openResult == ESP_OK && candidate != nullptr) {
      openedInterface = interfaceIndex;
      break;
    }
  }

  if (candidate == nullptr || openResult != ESP_OK) {
    retireFailedOpenCallbackContext(impl);
    UsbCdcControlEvent failed;
    failed.type = UsbCdcControlType::OPEN_FAILED;
    failed.code = openResult;
    failed.session = impl->activeSession;
    impl->lifecycle.apply(failed, nowMs);
    impl->currentDetail = "No compatible CDC interface; retry scheduled";
    return;
  }

  impl->cdcHandle = candidate;
  UsbCdcControlEvent opened;
  opened.type = UsbCdcControlType::OPEN_SUCCEEDED;
  opened.session = impl->activeSession;
  impl->lifecycle.apply(opened, nowMs);
  drainLifecycleQueue(impl, nowMs);
  closeOwnedHandle(impl, nowMs);
  if (!configurationMayContinue(impl, candidate, configToken)) return;

  cdc_acm_line_coding_t lineCoding = {
    .dwDTERate = 9600,
    .bCharFormat = 0,
    .bParityType = 0,
    .bDataBits = 8,
  };
  esp_err_t lineResult = cdc_acm_host_line_coding_set(candidate, &lineCoding);
  drainLifecycleQueue(impl, nowMs);
  closeOwnedHandle(impl, nowMs);
  if (!configurationMayContinue(impl, candidate, configToken)) return;
  if (lineResult != ESP_OK) {
    failConfiguration(impl, nowMs, lineResult);
    return;
  }

  esp_err_t controlResult =
    cdc_acm_host_set_control_line_state(candidate, true, true);
  drainLifecycleQueue(impl, nowMs);
  closeOwnedHandle(impl, nowMs);
  if (!configurationMayContinue(impl, candidate, configToken)) return;
  if (controlResult != ESP_OK) {
    failConfiguration(impl, nowMs, controlResult);
    return;
  }

  uint32_t epoch = impl->shared.producerEpoch();
  UsbCdcOrderedPublishResult startResult = enqueueOrdered(
    impl, UsbCdcOrderedType::STREAM_RESET, impl->activeSession, epoch, 0, true);
  if (!configurationMayContinue(impl, candidate, configToken)) {
    drainLifecycleQueue(impl, nowMs);
    closeOwnedHandle(impl, nowMs);
    return;
  }
  bool committed = impl->shared.commitConfiguration(configToken, startResult);
  if (!committed) {
    impl->currentDetail = "CDC configuration superseded by terminal event";
    drainLifecycleQueue(impl, nowMs);
    closeOwnedHandle(impl, nowMs);
    return;
  }

  UsbCdcControlEvent configured;
  configured.type = UsbCdcControlType::CONFIG_SUCCEEDED;
  configured.session = impl->activeSession;
  impl->lifecycle.apply(configured, nowMs);
  impl->currentDetail = "CDC device ready on interface ";
  impl->currentDetail += openedInterface;
}
#endif

void UsbCdcTransport::poll() {
#if !SOC_USB_OTG_SUPPORTED
  return;
#else
  if (!impl->beginCalled) return;
  reapRetiredCallbackContexts(impl);
  uint64_t nowMs = impl->monotonicMillis();
  drainLifecycleQueue(impl, nowMs);
  closeOwnedHandle(impl, nowMs);

  UsbCdcControlEvent tick;
  tick.type = UsbCdcControlType::RETRY_TICK;
  impl->lifecycle.apply(tick, nowMs);
  attemptOpenAndConfigure(this, nowMs);
  drainLifecycleQueue(impl, nowMs);
  closeOwnedHandle(impl, nowMs);

  switch (impl->lifecycle.phase()) {
    case UsbCdcPhase::READY:
      impl->currentState = xStreamBufferBytesAvailable(impl->rxStream) > 0
        ? TRANSPORT_STATE_RECEIVING : TRANSPORT_STATE_READY;
      break;
    case UsbCdcPhase::WAITING_DEVICE:
    case UsbCdcPhase::RETRY_WAIT:
    case UsbCdcPhase::OPENING:
    case UsbCdcPhase::CONFIGURING:
      impl->currentState = TRANSPORT_STATE_WAITING_DEVICE;
      break;
    case UsbCdcPhase::ERROR:
      impl->currentState = TRANSPORT_STATE_ERROR;
      break;
    default:
      impl->currentState = TRANSPORT_STATE_STARTING;
      break;
  }
  impl->currentDiagnostics = impl->shared.diagnosticsSnapshot();
  impl->currentReconnectCount = impl->lifecycle.reconnectCount();
#endif
}

int UsbCdcTransport::available() {
  if (impl == nullptr || impl->rxStream == nullptr) return 0;
  size_t count = xStreamBufferBytesAvailable(impl->rxStream);
#if SOC_USB_OTG_SUPPORTED
  count += impl->shared.pendingCount();
#endif
  return count > static_cast<size_t>(INT_MAX) ? INT_MAX
                                               : static_cast<int>(count);
}

int UsbCdcTransport::read() {
  MonitorRxEvent event;
  if (!nextRxEvent(event) || event.type != MonitorRxEventType::BYTE) return -1;
  return event.byte;
}

bool UsbCdcTransport::nextRxEvent(MonitorRxEvent& output) {
#if !SOC_USB_OTG_SUPPORTED
  (void)output;
  return false;
#else
  if (impl == nullptr || impl->rxStream == nullptr) return false;
  Impl* activeImpl = impl;

  for (size_t attempt = 0; attempt < kOrderedChannelDepth + 2; ++attempt) {
    UsbCdcOrderedDelivery delivery;
    UsbCdcOrderedClaimResult claim =
      activeImpl->shared.claim(activeImpl->cursor, delivery,
        activeImpl->lifecycle.connected(), [activeImpl]() {
          activeImpl->overflowMarkerPublished.store(
            false, std::memory_order_release);
        });

    if (claim == UsbCdcOrderedClaimResult::CLAIMED) {
      if (delivery.event.droppedBytes > 0) {
        UsbCdcControlEvent overflow;
        overflow.type = UsbCdcControlType::RX_OVERFLOW;
        overflow.count = delivery.event.droppedBytes;
        overflow.session = delivery.event.session;
        activeImpl->lifecycle.apply(overflow, 0);
      }
      if (delivery.producerResumed) {
        UsbCdcControlEvent recovered;
        recovered.type = UsbCdcControlType::RX_CAPACITY_RECOVERED;
        recovered.session = delivery.event.session;
        activeImpl->lifecycle.apply(recovered, 0);
      }
      impl->cursor.applyControl(delivery.event);
      output.type = delivery.event.type == UsbCdcOrderedType::STREAM_RESET
        ? MonitorRxEventType::STREAM_RESET
        : MonitorRxEventType::DISCONTINUITY;
      output.byte = 0;
      output.epoch = delivery.event.epoch;
      return true;
    }
    if (claim == UsbCdcOrderedClaimResult::STALE_QUEUE_DISCARDED) continue;
    if (claim == UsbCdcOrderedClaimResult::STALE_FALLBACK_DISCARDED) {
      uint64_t nowMs = impl->monotonicMillis();
      UsbCdcControlEvent failure;
      failure.type = UsbCdcControlType::CONTROL_QUEUE_OVERFLOW;
      failure.code = ESP_ERR_INVALID_STATE;
      failure.session = impl->activeSession;
      if (applyTerminalEventOrDefer(impl, failure, nowMs)) {
        closeOwnedHandle(impl, nowMs);
      }
      continue;
    }

    uint8_t value = 0;
    if (xStreamBufferReceive(impl->rxStream, &value, 1, 0) == 1) {
      output.type = MonitorRxEventType::BYTE;
      output.byte = value;
      output.epoch = impl->cursor.epoch();
      impl->cursor.noteByteDelivered();
      value = 0;
      impl->currentState = TRANSPORT_STATE_RECEIVING;
      return true;
    }
    return false;
  }
  return false;
#endif
}

const char* UsbCdcTransport::name() const { return "USB OTG Host"; }

MonitorTransportState UsbCdcTransport::state() const {
  return impl == nullptr ? TRANSPORT_STATE_ERROR : impl->currentState;
}

String UsbCdcTransport::detail() const {
  if (impl == nullptr) return String("USB transport unavailable");
  String result = impl->currentDetail;
  result += " | loss_events=";
  result += impl->currentDiagnostics.lossEpisodes;
  result += " dropped_bytes=";
  result += impl->currentDiagnostics.droppedBytes;
  result += " overflow_events=";
  result += impl->currentDiagnostics.overflowEpisodes;
  result += " reconnects=";
  result += impl->currentReconnectCount;
  return result;
}

uint32_t UsbCdcTransport::dataLossCount() const {
  return impl == nullptr ? 0 : impl->currentDiagnostics.lossEpisodes;
}

uint32_t UsbCdcTransport::reconnectCount() const {
  return impl == nullptr ? 0 : impl->currentReconnectCount;
}

uint32_t UsbCdcTransport::droppedByteCount() const {
  return impl == nullptr ? 0 : impl->currentDiagnostics.droppedBytes;
}

uint32_t UsbCdcTransport::overflowEpisodeCount() const {
  return impl == nullptr ? 0 : impl->currentDiagnostics.overflowEpisodes;
}
