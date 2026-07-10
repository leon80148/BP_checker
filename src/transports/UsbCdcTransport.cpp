#include "../../lib/transports/UsbCdcTransport.h"
#include "../../lib/transports/UsbCdcState.h"

#include <atomic>
#include <limits.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
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
constexpr UBaseType_t kOrderedQueueDepth = 12;
constexpr uint32_t kOpenTimeoutMs = 20;

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

struct UsbCdcTransport::Impl {
  struct CallbackContext {
    Impl* owner = nullptr;
    uint32_t session = 0;
  };

  struct FallbackControl {
    bool pending = false;
    bool resumeProducer = false;
    UsbCdcOrderedEvent event;
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

  StaticQueue_t orderedQueueState = {};
  alignas(4) uint8_t orderedQueueStorage[
    kOrderedQueueDepth * sizeof(UsbCdcOrderedEvent)] = {};
  QueueHandle_t orderedQueue = nullptr;

  UsbCdcOrderedEvent orderedHead;
  bool orderedHeadValid = false;
  FallbackControl fallback;
  portMUX_TYPE fallbackMux = portMUX_INITIALIZER_UNLOCKED;

  std::atomic<uint32_t> producerSession{0};
  std::atomic<uint32_t> producerEpoch{0};
  std::atomic<uint32_t> acceptedByteSequence{0};
  std::atomic<uint32_t> lifetimeDroppedBytes{0};
  std::atomic<uint32_t> lifetimeLossEpisodes{0};
  std::atomic<uint32_t> lifetimeOverflowEpisodes{0};
  std::atomic<uint32_t> lifecycleQueueFailures{0};
  std::atomic<bool> producerEnabled{false};
  std::atomic<bool> producerQuarantined{false};
  std::atomic<bool> overflowActive{false};
  std::atomic<bool> overflowMarkerPublished{false};
  std::atomic<bool> terminalLossActive{false};
  std::atomic<bool> shutdownRequested{false};
  std::atomic<bool> daemonStopped{true};

  CallbackContext callbackContext;
  bool beginCalled = false;
  bool daemonTaskStarted = false;
  uint32_t activeSession = 0;
  uint32_t lastMillis32 = 0;
  uint64_t millisHigh = 0;
  bool millisInitialized = false;

#if SOC_USB_OTG_SUPPORTED
  TaskHandle_t daemonTaskHandle = nullptr;
  cdc_acm_dev_hdl_t cdcHandle = nullptr;
#endif

  Impl() {
    callbackContext.owner = this;
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
static std::atomic<UsbCdcTransport*> gUsbCdcTransport{nullptr};

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
    impl->producerQuarantined.store(true, std::memory_order_release);
    impl->producerEnabled.store(false, std::memory_order_release);
  }
  return false;
}
// CALLBACK_POD_END lifecycle queue producer

static void publishFallback(UsbCdcTransport::Impl* impl,
                            const UsbCdcOrderedEvent& event,
                            bool resumeProducer) {
  portENTER_CRITICAL(&impl->fallbackMux);
  if (!impl->fallback.pending) {
    impl->fallback.pending = true;
    impl->fallback.resumeProducer = resumeProducer;
    impl->fallback.event = event;
  } else {
    if (event.type == UsbCdcOrderedType::STREAM_RESET) {
      impl->fallback.event.type = UsbCdcOrderedType::STREAM_RESET;
    }
    impl->fallback.event.session = event.session;
    impl->fallback.event.epoch = event.epoch;
    impl->fallback.event.droppedBytes = saturatingAdd(
      impl->fallback.event.droppedBytes, event.droppedBytes);
    impl->fallback.resumeProducer =
      impl->fallback.resumeProducer || resumeProducer;
  }
  portEXIT_CRITICAL(&impl->fallbackMux);
  impl->producerQuarantined.store(true, std::memory_order_release);
  impl->producerEnabled.store(false, std::memory_order_release);
}

// CALLBACK_POD_BEGIN ordered loss producer
static bool enqueueOrdered(UsbCdcTransport::Impl* impl,
                           UsbCdcOrderedType type, uint32_t session,
                           uint32_t epoch, uint32_t droppedBytes,
                           bool resumeProducer = false) {
  UsbCdcOrderedEvent event;
  event.type = type;
  event.session = session;
  event.epoch = epoch;
  event.byteBoundary =
    impl->acceptedByteSequence.load(std::memory_order_acquire);
  event.droppedBytes = droppedBytes;
  if (impl->orderedQueue != nullptr &&
      xQueueSendToBack(impl->orderedQueue, &event, 0) == pdPASS) {
    return true;
  }
  publishFallback(impl, event, resumeProducer);
  return false;
}
// CALLBACK_POD_END ordered loss producer

static uint32_t beginOverflowLoss(UsbCdcTransport::Impl* impl,
                                  bool& newEpisode) {
  bool expected = false;
  newEpisode = impl->overflowActive.compare_exchange_strong(
    expected, true, std::memory_order_acq_rel);
  if (newEpisode) {
    atomicSaturatingIncrement(impl->lifetimeOverflowEpisodes);
    atomicSaturatingIncrement(impl->lifetimeLossEpisodes);
    return atomicSaturatingIncrement(impl->producerEpoch);
  }
  return impl->producerEpoch.load(std::memory_order_acquire);
}

static uint32_t beginTerminalLoss(UsbCdcTransport::Impl* impl) {
  bool expected = false;
  if (impl->terminalLossActive.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel)) {
    atomicSaturatingIncrement(impl->lifetimeLossEpisodes);
    return atomicSaturatingIncrement(impl->producerEpoch);
  }
  return impl->producerEpoch.load(std::memory_order_acquire);
}

static bool peekFallback(UsbCdcTransport::Impl* impl,
                         UsbCdcTransport::Impl::FallbackControl& out) {
  portENTER_CRITICAL(&impl->fallbackMux);
  bool pending = impl->fallback.pending;
  if (pending) out = impl->fallback;
  portEXIT_CRITICAL(&impl->fallbackMux);
  return pending;
}

static void consumeFallback(UsbCdcTransport::Impl* impl) {
  portENTER_CRITICAL(&impl->fallbackMux);
  impl->fallback.pending = false;
  impl->fallback.resumeProducer = false;
  impl->fallback.event = UsbCdcOrderedEvent{};
  portEXIT_CRITICAL(&impl->fallbackMux);
}

// CALLBACK_POD_BEGIN CDC data callback
static bool cdcDataCallback(const uint8_t* data, size_t dataLen, void* userArg) {
  auto* context = static_cast<UsbCdcTransport::Impl::CallbackContext*>(userArg);
  if (context == nullptr || context->owner == nullptr || data == nullptr ||
      dataLen == 0) {
    return true;
  }
  auto* impl = context->owner;
  if (!impl->producerEnabled.load(std::memory_order_acquire) ||
      impl->producerQuarantined.load(std::memory_order_acquire)) {
    if (!impl->producerQuarantined.load(std::memory_order_acquire)) {
      beginTerminalLoss(impl);
    }
    atomicSaturatingAdd(impl->lifetimeDroppedBytes,
                        dataLen > UINT32_MAX ? UINT32_MAX
                                             : static_cast<uint32_t>(dataLen));
    return true;
  }

  size_t sent = xStreamBufferSend(impl->rxStream, data, dataLen, 0);
  impl->acceptedByteSequence.fetch_add(static_cast<uint32_t>(sent),
                                       std::memory_order_release);
  if (sent < dataLen) {
    size_t lostSize = dataLen - sent;
    uint32_t lost = lostSize > UINT32_MAX ? UINT32_MAX
                                          : static_cast<uint32_t>(lostSize);
    atomicSaturatingAdd(impl->lifetimeDroppedBytes, lost);
    bool newEpisode = false;
    uint32_t epoch = beginOverflowLoss(impl, newEpisode);
    if (newEpisode) {
      bool published = enqueueOrdered(impl, UsbCdcOrderedType::DISCONTINUITY,
                                      context->session, epoch, lost, true);
      impl->overflowMarkerPublished.store(published ||
        impl->producerQuarantined.load(std::memory_order_acquire),
        std::memory_order_release);
    }
    enqueueLifecycle(impl, UsbCdcControlType::RX_OVERFLOW, 0, lost,
                     context->session, false);
  } else if (impl->overflowActive.load(std::memory_order_acquire) &&
             impl->overflowMarkerPublished.load(std::memory_order_acquire)) {
    impl->overflowActive.store(false, std::memory_order_release);
    impl->overflowMarkerPublished.store(false, std::memory_order_release);
    enqueueLifecycle(impl, UsbCdcControlType::RX_CAPACITY_RECOVERED, 0, 0,
                     context->session, false);
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
  switch (event->type) {
    case CDC_ACM_HOST_ERROR: {
      impl->producerEnabled.store(false, std::memory_order_release);
      uint32_t epoch = beginTerminalLoss(impl);
      enqueueOrdered(impl, UsbCdcOrderedType::DISCONTINUITY, context->session,
                     epoch, 0, false);
      enqueueLifecycle(impl, UsbCdcControlType::TRANSFER_ERROR,
                       event->data.error, 0, context->session, true);
      break;
    }
    case CDC_ACM_HOST_DEVICE_DISCONNECTED: {
      impl->producerEnabled.store(false, std::memory_order_release);
      uint32_t epoch = beginTerminalLoss(impl);
      enqueueOrdered(impl, UsbCdcOrderedType::STREAM_RESET, context->session,
                     epoch, 0, false);
      enqueueLifecycle(impl, UsbCdcControlType::DEVICE_DISCONNECTED, 0, 0,
                       context->session, true);
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
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delayMs));
  return !impl->shutdownRequested.load(std::memory_order_acquire);
}

static void usbHostDaemonTask(void* arg) {
  auto* self = static_cast<UsbCdcTransport*>(arg);
  auto* impl = self->impl;
  impl->daemonStopped.store(false, std::memory_order_release);
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
        UsbCdcTransport* transport =
          gUsbCdcTransport.load(std::memory_order_acquire);
        if (transport != nullptr && transport->impl != nullptr) {
          enqueueLifecycle(transport->impl,
                           UsbCdcControlType::DEVICE_ATTACHED, 0, 0, 0, true);
        }
        // CALLBACK_POD_END new-device callback
      },
    };

    result = cdc_acm_host_install(&driverConfig);
    if (result != ESP_OK) {
      enqueueLifecycle(impl, UsbCdcControlType::DRIVER_INSTALL_FAILED, result);
      usb_host_uninstall();
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

    cdc_acm_host_uninstall();
    usb_host_device_free_all();
    usb_host_uninstall();
    break;
  }

  impl->daemonStopped.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}
#endif  // SOC_USB_OTG_SUPPORTED

UsbCdcTransport::UsbCdcTransport() : impl(new Impl()) {}

UsbCdcTransport::~UsbCdcTransport() {
  if (impl == nullptr) return;
  impl->producerEnabled.store(false, std::memory_order_release);
  impl->producerQuarantined.store(true, std::memory_order_release);
#if SOC_USB_OTG_SUPPORTED
  UsbCdcTransport* expected = this;
  gUsbCdcTransport.compare_exchange_strong(expected, nullptr,
                                            std::memory_order_acq_rel);
  if (impl->cdcHandle != nullptr) {
    cdc_acm_dev_hdl_t handle = impl->cdcHandle;
    impl->cdcHandle = nullptr;
    cdc_acm_host_close(handle);
  }
  impl->shutdownRequested.store(true, std::memory_order_release);
  if (impl->daemonTaskHandle != nullptr) xTaskNotifyGive(impl->daemonTaskHandle);
  for (int i = 0; i < 100 &&
                  !impl->daemonStopped.load(std::memory_order_acquire); ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!impl->daemonStopped.load(std::memory_order_acquire)) {
    // The task still references Impl. Leak safely rather than create a UAF.
    impl = nullptr;
    return;
  }
#endif
  memset(impl->rxStreamStorage, 0, sizeof(impl->rxStreamStorage));
  memset(impl->orderedQueueStorage, 0, sizeof(impl->orderedQueueStorage));
  delete impl;
  impl = nullptr;
}

bool UsbCdcTransport::begin() {
  impl->beginCalled = true;
  impl->rxStream = xStreamBufferCreateStatic(
    sizeof(impl->rxStreamStorage), 1, impl->rxStreamStorage,
    &impl->rxStreamState);
  impl->lifecycleQueue = xQueueCreateStatic(
    kLifecycleQueueDepth, sizeof(UsbCdcControlEvent),
    impl->lifecycleQueueStorage, &impl->lifecycleQueueState);
  impl->orderedQueue = xQueueCreateStatic(
    kOrderedQueueDepth, sizeof(UsbCdcOrderedEvent),
    impl->orderedQueueStorage, &impl->orderedQueueState);
  if (impl->rxStream == nullptr || impl->lifecycleQueue == nullptr ||
      impl->orderedQueue == nullptr) {
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
  gUsbCdcTransport.store(this, std::memory_order_release);
  BaseType_t created = xTaskCreatePinnedToCore(
    usbHostDaemonTask, "usb_host_daemon", 6144, this, 20,
    &impl->daemonTaskHandle, 0);
  if (created != pdPASS) {
    impl->currentState = TRANSPORT_STATE_ERROR;
    impl->currentDetail = "Failed to create USB host daemon task";
    gUsbCdcTransport.store(nullptr, std::memory_order_release);
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

static void drainLifecycleQueue(UsbCdcTransport::Impl* impl, uint64_t nowMs) {
  UsbCdcControlEvent event;
  while (xQueueReceive(impl->lifecycleQueue, &event, 0) == pdPASS) {
    applyLifecycleEvent(impl, event, nowMs);
  }
  uint32_t failures = impl->lifecycleQueueFailures.exchange(
    0, std::memory_order_acq_rel);
  if (failures > 0) {
    UsbCdcControlEvent failure;
    failure.type = UsbCdcControlType::CONTROL_QUEUE_OVERFLOW;
    failure.code = static_cast<int32_t>(failures);
    failure.session = impl->activeSession;
    applyLifecycleEvent(impl, failure, nowMs);
    uint32_t epoch = beginTerminalLoss(impl);
    enqueueOrdered(impl, UsbCdcOrderedType::STREAM_RESET,
                   impl->activeSession, epoch, 0, false);
  }
}

static void closeOwnedHandle(UsbCdcTransport::Impl* impl, uint64_t nowMs) {
  if (!impl->lifecycle.takeCloseRequest()) return;
  cdc_acm_dev_hdl_t handle = impl->cdcHandle;
  impl->cdcHandle = nullptr;
  if (handle != nullptr) cdc_acm_host_close(handle);
  UsbCdcControlEvent closed;
  closed.type = UsbCdcControlType::HANDLE_CLOSED;
  closed.session = impl->activeSession;
  impl->lifecycle.apply(closed, nowMs);
}

static uint32_t nextSession(uint32_t current) {
  if (current == UINT32_MAX) return current;
  return current + 1;
}

static void attemptOpenAndConfigure(UsbCdcTransport* self, uint64_t nowMs) {
  auto* impl = self->impl;
  if (!impl->lifecycle.shouldAttemptOpen(nowMs)) return;

  impl->activeSession = nextSession(impl->activeSession);
  impl->producerSession.store(impl->activeSession, std::memory_order_release);
  impl->callbackContext.session = impl->activeSession;
  impl->producerEnabled.store(false, std::memory_order_release);

  UsbCdcControlEvent started;
  started.type = UsbCdcControlType::OPEN_STARTED;
  started.session = impl->activeSession;
  impl->lifecycle.apply(started, nowMs);
  impl->currentDetail = "Probing CDC device";

  cdc_acm_host_device_config_t config = {
    .connection_timeout_ms = kOpenTimeoutMs,
    .out_buffer_size = 64,
    .in_buffer_size = 64,
    .event_cb = cdcEventCallback,
    .data_cb = cdcDataCallback,
    .user_arg = &impl->callbackContext,
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
  if (impl->cdcHandle == nullptr ||
      impl->lifecycle.phase() != UsbCdcPhase::CONFIGURING) {
    return;
  }

  cdc_acm_line_coding_t lineCoding = {
    .dwDTERate = 9600,
    .bCharFormat = 0,
    .bParityType = 0,
    .bDataBits = 8,
  };
  esp_err_t lineResult = cdc_acm_host_line_coding_set(candidate, &lineCoding);
  drainLifecycleQueue(impl, nowMs);
  closeOwnedHandle(impl, nowMs);
  if (impl->cdcHandle == nullptr) return;

  esp_err_t controlResult =
    cdc_acm_host_set_control_line_state(candidate, true, true);
  drainLifecycleQueue(impl, nowMs);
  closeOwnedHandle(impl, nowMs);
  if (impl->cdcHandle == nullptr) return;

  if (lineResult != ESP_OK || controlResult != ESP_OK) {
    uint32_t epoch = beginTerminalLoss(impl);
    enqueueOrdered(impl, UsbCdcOrderedType::STREAM_RESET,
                   impl->activeSession, epoch, 0, false);
    UsbCdcControlEvent failed;
    failed.type = UsbCdcControlType::CONFIG_FAILED;
    failed.code = lineResult != ESP_OK ? lineResult : controlResult;
    failed.session = impl->activeSession;
    impl->lifecycle.apply(failed, nowMs);
    impl->currentDetail = "CDC configuration failed; retry scheduled";
    closeOwnedHandle(impl, nowMs);
    return;
  }

  impl->terminalLossActive.store(false, std::memory_order_release);
  impl->overflowActive.store(false, std::memory_order_release);
  impl->producerQuarantined.store(false, std::memory_order_release);
  uint32_t epoch = impl->producerEpoch.load(std::memory_order_acquire);
  bool startPublished = enqueueOrdered(
    impl, UsbCdcOrderedType::STREAM_RESET, impl->activeSession, epoch, 0, true);
  impl->producerEnabled.store(startPublished, std::memory_order_release);

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
#endif
}

int UsbCdcTransport::available() {
  if (impl == nullptr || impl->rxStream == nullptr) return 0;
  size_t count = xStreamBufferBytesAvailable(impl->rxStream);
  if (impl->orderedQueue != nullptr) count += uxQueueMessagesWaiting(impl->orderedQueue);
  Impl::FallbackControl fallback;
#if SOC_USB_OTG_SUPPORTED
  if (peekFallback(impl, fallback)) count++;
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
  if (impl == nullptr || impl->rxStream == nullptr || impl->orderedQueue == nullptr) {
    return false;
  }

  for (UBaseType_t attempt = 0; attempt < kOrderedQueueDepth + 2; ++attempt) {
    if (!impl->orderedHeadValid &&
        xQueueReceive(impl->orderedQueue, &impl->orderedHead, 0) == pdPASS) {
      impl->orderedHeadValid = true;
    }

    if (impl->orderedHeadValid) {
      if (impl->cursor.controlStale(impl->orderedHead)) {
        impl->orderedHead = UsbCdcOrderedEvent{};
        impl->orderedHeadValid = false;
        continue;
      }
      if (impl->cursor.controlDue(impl->orderedHead)) {
        impl->cursor.applyControl(impl->orderedHead);
        output.type = impl->orderedHead.type == UsbCdcOrderedType::STREAM_RESET
          ? MonitorRxEventType::STREAM_RESET
          : MonitorRxEventType::DISCONTINUITY;
        output.byte = 0;
        output.epoch = impl->orderedHead.epoch;
        impl->orderedHead = UsbCdcOrderedEvent{};
        impl->orderedHeadValid = false;
        return true;
      }
    } else {
      Impl::FallbackControl fallback;
      if (peekFallback(impl, fallback) &&
          impl->cursor.controlDue(fallback.event)) {
        impl->cursor.applyControl(fallback.event);
        output.type = fallback.event.type == UsbCdcOrderedType::STREAM_RESET
          ? MonitorRxEventType::STREAM_RESET
          : MonitorRxEventType::DISCONTINUITY;
        output.byte = 0;
        output.epoch = fallback.event.epoch;
        consumeFallback(impl);
        impl->producerQuarantined.store(false, std::memory_order_release);
        if (fallback.resumeProducer && impl->lifecycle.connected()) {
          impl->producerEnabled.store(true, std::memory_order_release);
        }
        return true;
      }
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
  return impl == nullptr ? String("USB transport unavailable")
                         : impl->currentDetail;
}

uint32_t UsbCdcTransport::dataLossCount() const {
  return impl == nullptr ? 0
    : impl->lifetimeLossEpisodes.load(std::memory_order_acquire);
}

uint32_t UsbCdcTransport::reconnectCount() const {
  return impl == nullptr ? 0 : impl->lifecycle.reconnectCount();
}

uint32_t UsbCdcTransport::droppedByteCount() const {
  return impl == nullptr ? 0
    : impl->lifetimeDroppedBytes.load(std::memory_order_acquire);
}

uint32_t UsbCdcTransport::overflowEpisodeCount() const {
  return impl == nullptr ? 0
    : impl->lifetimeOverflowEpisodes.load(std::memory_order_acquire);
}
