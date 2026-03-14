#include "../../lib/transports/UsbCdcTransport.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <soc/soc_caps.h>

#if SOC_USB_OTG_SUPPORTED
#include <esp_err.h>
#include <esp_intr_alloc.h>
#include <usb/usb_host.h>
#include "../third_party/espressif_usb_host_cdc_acm/usb/cdc_acm_host.h"
#endif

struct UsbCdcTransport::Impl {
  MonitorTransportState currentState = TRANSPORT_STATE_STARTING;
  String currentDetail = "USB CDC host not initialized";
  uint8_t rxBuffer[1024] = {0};
  volatile size_t rxHead = 0;
  volatile size_t rxTail = 0;
  volatile bool hostReady = false;
  volatile bool driverReady = false;
  volatile bool openInProgress = false;
  volatile bool connected = false;
  bool beginCalled = false;
  bool daemonTaskStarted = false;
  unsigned long lastOpenAttemptMs = 0;
#if SOC_USB_OTG_SUPPORTED
  TaskHandle_t daemonTaskHandle = nullptr;
  cdc_acm_dev_hdl_t cdcHandle = nullptr;
#endif
};

#if SOC_USB_OTG_SUPPORTED
static UsbCdcTransport* gUsbCdcTransport = nullptr;

static void usbHostDaemonTask(void* arg) {
  auto* self = static_cast<UsbCdcTransport*>(arg);
  auto* impl = self->impl;

  const usb_host_config_t hostConfig = {
    .skip_phy_setup = false,
    .root_port_unpowered = false,
    .intr_flags = ESP_INTR_FLAG_LOWMED,
  };

  esp_err_t ret = usb_host_install(&hostConfig);
  if (ret != ESP_OK) {
    impl->currentState = TRANSPORT_STATE_ERROR;
    impl->currentDetail = "usb_host_install failed: " + String(static_cast<int>(ret));
    vTaskDelete(nullptr);
    return;
  }

  const cdc_acm_host_driver_config_t driverConfig = {
    .driver_task_stack_size = 4096,
    .driver_task_priority = 5,
    .xCoreID = 0,
    .new_dev_cb = [](usb_device_handle_t) {
      if (gUsbCdcTransport == nullptr) {
        return;
      }
      gUsbCdcTransport->impl->currentState = TRANSPORT_STATE_WAITING_DEVICE;
      gUsbCdcTransport->impl->currentDetail = "USB device detected. Probing CDC interface.";
    },
  };

  ret = cdc_acm_host_install(&driverConfig);
  if (ret != ESP_OK) {
    impl->currentState = TRANSPORT_STATE_ERROR;
    impl->currentDetail = "cdc_acm_host_install failed: " + String(static_cast<int>(ret));
    usb_host_uninstall();
    vTaskDelete(nullptr);
    return;
  }

  impl->hostReady = true;
  impl->driverReady = true;
  impl->currentState = TRANSPORT_STATE_WAITING_DEVICE;
  impl->currentDetail = "USB host ready. Waiting for CDC device.";

  bool hasClients = true;
  bool hasDevices = false;
  while (hasClients) {
    uint32_t eventFlags = 0;
    ret = usb_host_lib_handle_events(pdMS_TO_TICKS(100), &eventFlags);
    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
      impl->currentState = TRANSPORT_STATE_ERROR;
      impl->currentDetail = "usb_host_lib_handle_events failed: " + String(static_cast<int>(ret));
    }

    if (eventFlags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      if (usb_host_device_free_all() == ESP_OK) {
        hasClients = false;
      } else {
        hasDevices = true;
      }
    }
    if (hasDevices && (eventFlags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)) {
      hasClients = false;
    }
  }

  cdc_acm_host_uninstall();
  usb_host_uninstall();
  impl->hostReady = false;
  impl->driverReady = false;
  impl->connected = false;
  impl->currentState = TRANSPORT_STATE_ERROR;
  impl->currentDetail = "USB host daemon stopped";
  vTaskDelete(nullptr);
}

static bool cdcDataCallback(const uint8_t* data, size_t dataLen, void* userArg) {
  auto* self = static_cast<UsbCdcTransport*>(userArg);
  auto* impl = self->impl;
  if (data == nullptr || dataLen == 0) {
    return true;
  }

  for (size_t i = 0; i < dataLen; ++i) {
    size_t nextHead = (impl->rxHead + 1) % sizeof(impl->rxBuffer);
    if (nextHead == impl->rxTail) {
      impl->currentState = TRANSPORT_STATE_ERROR;
      impl->currentDetail = "RX buffer overflow";
      break;
    }
    impl->rxBuffer[impl->rxHead] = data[i];
    impl->rxHead = nextHead;
  }

  impl->currentState = TRANSPORT_STATE_RECEIVING;
  impl->currentDetail = "Receiving CDC data";
  return true;
}

static void cdcEventCallback(const cdc_acm_host_dev_event_data_t* event, void* userArg) {
  auto* self = static_cast<UsbCdcTransport*>(userArg);
  auto* impl = self->impl;
  if (event == nullptr) {
    return;
  }

  switch (event->type) {
    case CDC_ACM_HOST_ERROR:
      impl->currentState = TRANSPORT_STATE_ERROR;
      impl->currentDetail = "CDC error: " + String(event->data.error);
      break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
      impl->connected = false;
      impl->currentState = TRANSPORT_STATE_WAITING_DEVICE;
      impl->currentDetail = "CDC device disconnected";
      if (impl->cdcHandle != nullptr) {
        cdc_acm_host_close(impl->cdcHandle);
        impl->cdcHandle = nullptr;
      }
      break;
    case CDC_ACM_HOST_SERIAL_STATE:
      impl->currentState = TRANSPORT_STATE_READY;
      impl->currentDetail = "CDC serial state updated";
      break;
    case CDC_ACM_HOST_NETWORK_CONNECTION:
      impl->currentState = TRANSPORT_STATE_READY;
      impl->currentDetail = "CDC network connection state updated";
      break;
#ifdef CDC_HOST_SUSPEND_RESUME_API_SUPPORTED
    case CDC_ACM_HOST_DEVICE_SUSPENDED:
      impl->currentState = TRANSPORT_STATE_WAITING_DEVICE;
      impl->currentDetail = "CDC device suspended";
      break;
    case CDC_ACM_HOST_DEVICE_RESUMED:
      impl->currentState = TRANSPORT_STATE_READY;
      impl->currentDetail = "CDC device resumed";
      break;
#endif
    default:
      break;
  }
}
#endif

UsbCdcTransport::UsbCdcTransport() : impl(new Impl()) {}

UsbCdcTransport::~UsbCdcTransport() {
#if SOC_USB_OTG_SUPPORTED
  if (impl != nullptr && impl->cdcHandle != nullptr) {
    cdc_acm_host_close(impl->cdcHandle);
    impl->cdcHandle = nullptr;
  }
#endif
  delete impl;
}

bool UsbCdcTransport::begin() {
  impl->beginCalled = true;

#if !SOC_USB_OTG_SUPPORTED
  impl->currentState = TRANSPORT_STATE_UNSUPPORTED;
  impl->currentDetail = "SOC_USB_OTG_SUPPORTED is not available on this target";
  return false;
#else
  gUsbCdcTransport = this;
  if (!impl->daemonTaskStarted) {
    BaseType_t created = xTaskCreatePinnedToCore(usbHostDaemonTask, "usb_host_daemon", 6144, this, 20, &impl->daemonTaskHandle, 0);
    if (created != pdPASS) {
      impl->currentState = TRANSPORT_STATE_ERROR;
      impl->currentDetail = "Failed to create USB host daemon task";
      return false;
    }
    impl->daemonTaskStarted = true;
  }

  impl->currentState = TRANSPORT_STATE_WAITING_DEVICE;
  impl->currentDetail = "Starting USB host stack";
  return true;
#endif
}

void UsbCdcTransport::poll() {
#if !SOC_USB_OTG_SUPPORTED
  return;
#else
  if (!impl->beginCalled || !impl->hostReady || !impl->driverReady || impl->connected || impl->openInProgress) {
    return;
  }

  if (millis() - impl->lastOpenAttemptMs < 1000) {
    return;
  }

  impl->lastOpenAttemptMs = millis();
  impl->openInProgress = true;
  impl->currentState = TRANSPORT_STATE_WAITING_DEVICE;
  impl->currentDetail = "Probing CDC device";

  cdc_acm_host_device_config_t devConfig = {
    .connection_timeout_ms = 50,
    .out_buffer_size = 64,
    .in_buffer_size = 64,
    .event_cb = cdcEventCallback,
    .data_cb = cdcDataCallback,
    .user_arg = this,
  };

  for (uint8_t interfaceIndex = 0; interfaceIndex < 3 && !impl->connected; ++interfaceIndex) {
    cdc_acm_dev_hdl_t handle = nullptr;
    esp_err_t ret = cdc_acm_host_open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID, interfaceIndex, &devConfig, &handle);
    if (ret == ESP_OK && handle != nullptr) {
      impl->cdcHandle = handle;
      impl->connected = true;
      impl->currentState = TRANSPORT_STATE_READY;
      impl->currentDetail = "CDC device opened on interface " + String(interfaceIndex);

      cdc_acm_line_coding_t lineCoding = {
        .dwDTERate = 9600,
        .bCharFormat = 0,
        .bParityType = 0,
        .bDataBits = 8,
      };
      cdc_acm_host_line_coding_set(handle, &lineCoding);
      cdc_acm_host_set_control_line_state(handle, true, true);
      break;
    }
  }

  if (!impl->connected) {
    impl->currentState = TRANSPORT_STATE_WAITING_DEVICE;
    impl->currentDetail = "No compatible CDC device attached";
  }

  impl->openInProgress = false;
#endif
}

int UsbCdcTransport::available() {
  if (impl->rxHead >= impl->rxTail) {
    return static_cast<int>(impl->rxHead - impl->rxTail);
  }
  return static_cast<int>((sizeof(impl->rxBuffer) - impl->rxTail) + impl->rxHead);
}

int UsbCdcTransport::read() {
  if (impl->rxHead == impl->rxTail) {
    if (impl->connected && impl->currentState == TRANSPORT_STATE_RECEIVING) {
      impl->currentState = TRANSPORT_STATE_READY;
      impl->currentDetail = "CDC device connected";
    }
    return -1;
  }

  uint8_t value = impl->rxBuffer[impl->rxTail];
  impl->rxTail = (impl->rxTail + 1) % sizeof(impl->rxBuffer);
  if (impl->rxHead == impl->rxTail && impl->connected) {
    impl->currentState = TRANSPORT_STATE_READY;
    impl->currentDetail = "CDC device connected";
  }
  return value;
}

const char* UsbCdcTransport::name() const {
  return "USB OTG Host";
}

MonitorTransportState UsbCdcTransport::state() const {
  return impl->currentState;
}

String UsbCdcTransport::detail() const {
  return impl->currentDetail;
}

bool UsbCdcTransport::isFallback() const {
  return false;
}
