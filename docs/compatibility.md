# Compatibility Matrix

Only rows backed by current retained evidence are supported.

| Component | Supported baseline | Current status |
|---|---|---|
| MCU / build target | ESP32-S3, `esp32:esp32:esp32s3` | Software gate passes; exact production board HIL pending |
| Arduino toolchain | Arduino CLI 1.4.1, Arduino-ESP32 3.3.7 | Pinned and build-verified |
| JSON library | ArduinoJson 7.4.2 | Pinned and SBOM-recorded |
| Monitor | Omron HBP-9030, USB2 Type-B, function item 32 / format 5 | Parser/protocol tests pass; current device acceptance pending |
| Transport | ESP32-S3 native USB OTG host | Host stress/TSan pass; board/monitor soak pending |
| Browser | Clinic kiosk browser/version recorded by browser evidence | No dynamic browser evidence retained yet |
| Network | Per-device WPA2 AP for commissioning/recovery; isolated clinic VLAN for HTTP | Deployment control; not a firmware confidentiality guarantee |

Do not substitute another monitor format, ESP32 variant, USB topology, browser,
or network boundary without a new acceptance record. Identifiable patient data
remains prohibited unless HTTPS or a trusted TLS gateway becomes mandatory.
