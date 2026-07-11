# Hardware Guide

## Reference Validation Topology

The release-validation reference is the **Espressif ESP32-S3-USB-OTG**
development board. It has a dedicated USB Type-A female host connector, USB
switching, and a current-limited host-power path. This is the reference target,
not a claim that every ESP32-S3 board is electrically equivalent. The official
board guide documents a 5 V, 500 mA maximum at the host port:

- [Espressif ESP32-S3-USB-OTG user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-usb-otg/user_guide.html)

Reference wiring:

1. Power/program/debug the board through its USB-to-UART Micro-USB connector.
2. Provide the host-port 5 V source using the board's documented power path;
   enable exactly one of `DEV_VBUS_EN` or `BOOST_EN`, and enable the 500 mA
   limiter with `IDEV_LIMIT_EN`.
3. Select the Type-A female host data path with the board's `USB_SEL` control.
4. Connect Type-A male to USB 2.0 Type-B male cable from the board host port to
   the HBP-9030 rear **USB2** Type-B connector.

The board-specific GPIO/power sequence and an HBP-9030 device-in-loop run must
be recorded before a release claims this topology as verified. Until then it
is the reproducible validation target, not completed hardware evidence.

## Primary Product Definition

This repository primarily supports:

- `ESP32-S3`
- `USB OTG/Host`
- Blood pressure monitor connected over `USB CDC`

The intended field topology is:

1. Board power/programming USB to computer or power
2. Board OTG/Host USB to blood pressure monitor

No GPIO data wiring is part of the primary path.

Do not power the monitor itself from the ESP32. The HBP-9030 uses its own AC
power; USB VBUS powers only the USB interface. Do not back-feed VBUS, enable
both host sources simultaneously, or assume a generic USB-C cable provides a
host-current switch. If enumeration resets or VBUS droops, use the documented
board power path or a validated, isolated powered topology.

OMRON specifies USB 2.0 and recommends a cable no longer than 2 m with ferrite
cores. Its manual identifies the monitor-side connector as Type-B and the rear
data connector as USB2. The same manual says function item **32** selects USB
output format **1–5**; this firmware accepts only **format 5**:

- [OMRON HBP-9030/HBP-9031C instruction manual](https://store.healthcare.omron.co.jp/support/download/manual/2873643-0F_HBP-9030_9031C_IM_ja_web.pdf)

## Current Firmware Status

The repository now builds with a real `USB OTG Host` CDC transport in the Arduino sketch:

- `USB OTG Host` is the default runtime on supported `ESP32-S3` boards
- `UART fallback` remains available for working deployments that cannot use OTG
- Transport selection is isolated from parser and UI logic through `MonitorTransport`

## Compatibility Status

| Component | Status | Release meaning |
|---|---|---|
| OMRON HBP-9030, USB2 Type-B, item 32 format 5 | Supported protocol | Strict parser and host corpus; still requires current device-in-loop evidence for a release |
| Espressif ESP32-S3-USB-OTG topology above | Reference target | Exact board/power checklist is defined; hardware run is separately recorded |
| UART fallback | Experimental/bench | Available for controlled debugging, not the primary clinic topology |
| HBP-9031C, HBP-1300, custom frames, formats 1–4 | Unsupported/experimental | Production allowlist rejects them until official grammar and device evidence are added |
| Other ESP32-S3 boards | Unverified | Must document OTG routing, VBUS switching/current limit, and pass the full device checklist |

## Primary Support Criteria

A board is in primary support scope only when all of these are true:

- It uses an `ESP32-S3`
- It exposes a real OTG/Host-capable USB path
- That path is usable by firmware as a USB host
- The OTG side can provide stable power or the setup uses a validated powered topology

## Not Primary Support

These cases are outside the main support path:

- Generic `ESP32` boards without native USB host capability
- `ESP32-S3` boards where OTG pins are not routed for host use
- Setups that only work by direct `TTL/UART` wiring
- Monitors that do not enumerate as supported `USB CDC` devices

## Fallback Path

For unsupported or non-OTG hardware, use the fallback serial path:

- [`fallback-uart.md`](fallback-uart.md)

That path is valid for debugging and bench use, but it is not the primary product definition of this repository.

## Device-in-loop Checklist

Record board revision, monitor serial/model, browser/version, firmware SHA,
cable, power source, and timestamp for every run.

- Confirm host-port VBUS is 5 V and remains within the board's 500 mA limit.
- Confirm HBP-9030 rear USB2 Type-B connection and function item 32 = format 5.
- Cold boot in each order: board first, monitor first, and simultaneous power.
- Verify attach, CDC enumeration, first valid format-5 reading, device timestamp,
  movement flag, and opaque record/session sequences.
- Send format 1–4, malformed, device-error, out-of-range, and partial frames;
  confirm none persist and raw identity never appears in UI/log/NVS/CSV.
- Unplug during a frame, reconnect, and confirm exact data-loss/reconnect counts,
  visible disconnected state, clean resynchronization, and no mixed frame.
- Fill beyond 20 records and confirm revision advances while newest-first history
  remains correct; reboot and confirm persisted records display as historical.
- Leave the current record past the configured stale interval and confirm stale
  UI, then take a new measurement and confirm KPI/history/diagnostic refresh.
- Exercise staff/admin roles, keyboard focus, mobile table scrolling, CSV export,
  recovery AP, credential rotation, and a storage-failure path.
- Complete the required continuous transport/persistence soak and retain logs,
  screenshots, power observations, and pass/fail evidence with the release.
