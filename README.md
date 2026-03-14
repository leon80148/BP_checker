# BP_checker (ESP32-S3 USB OTG Host Blood Pressure Bridge)

Support target: `ESP32-S3` boards with a usable `USB OTG/Host` path.

## Overview

`BP_checker` runs on an `ESP32-S3` and bridges blood pressure monitor data to Wi-Fi:

- Reads monitor data through `USB OTG Host`
- Assumes the monitor exposes a supported `USB CDC` serial-like interface
- Publishes a local Wi-Fi web UI for monitoring and history
- Stores the latest 20 measurements
- Preserves raw payload visibility for debugging

This repository is `OTG-first` at the product-definition level.

Current firmware status:

- Transport architecture is being migrated toward `USB OTG Host`
- The repository keeps `UART fallback` available while the CDC host data path is being completed
- The active transport is shown in serial logs and the web UI

If your board does not support OTG Host, or your monitor integration only works through direct serial wiring, see the fallback guide:

- [`docs/fallback-uart.md`](docs/fallback-uart.md)

## Primary Support Matrix

Primary support applies only when all of the following are true:

- MCU family is `ESP32-S3`
- The board exposes a usable `USB OTG/Host` connection
- The OTG side can power the attached monitor or uses a validated powered topology
- The blood pressure monitor enumerates as a supported `USB CDC` device

Boards outside that scope are not part of the main supported path.

Additional hardware guidance:

- [`docs/hardware.md`](docs/hardware.md)

## Hardware Topology

Target primary wiring:

1. Connect the board programming/power USB port to the computer or stable power source
2. Connect the blood pressure monitor USB cable to the board `OTG/Host` port

Primary OTG flow does not require GPIO data wiring.

## First Boot

1. Power on the board
2. Connect to `ESP32_BP_checker`
3. Open `http://192.168.4.1`
4. Configure the site Wi-Fi if needed

After configuration, access the device by either:

- `http://bp_checker.local`
- The device LAN IP shown in serial logs

## Web UI

### `/`

- Latest SYS/DIA/PUL values
- Recent measurements
- Raw data view

### `/history`

- Latest 20 saved records
- Raw payload drill-down
- History clear action

### `/config`

- Wi-Fi scanning and selection
- Manual SSID entry

### `/bp_model`

- `OMRON-HBP9030`
- `CUSTOM`

## Development

### UI Markup Check

```bash
bash scripts/check_ui_markup.sh
```

### Compile

```bash
arduino-cli compile -b esp32:esp32:esp32s3 --board-options USBMode=default /path/to/BP_checker
```

If the Arduino sketch folder name does not match the sketch file name, rename the main sketch to `BP_checker.ino` inside a `BP_checker` directory before compiling.

### Upload

```bash
arduino-cli compile --upload -b esp32:esp32:esp32s3 --board-options USBMode=default,UploadSpeed=115200 -p <PORT> /path/to/BP_checker
```

## Troubleshooting

### The UI is reachable but there is no monitor data

- Confirm the board is an `ESP32-S3` with usable OTG Host hardware
- Confirm the monitor is connected to the OTG/Host port, not just the programming port
- Confirm the monitor exposes a compatible `USB CDC` interface
- Check serial logs for attach, unsupported-device, disconnect, or no-data states
- If the firmware reports `UART fallback`, use the board-specific serial wiring path instead of OTG

### `bp_checker.local` does not open

- Use the LAN IP directly
- Confirm the client network supports mDNS

### My board does not support OTG Host

Use the fallback path instead:

- [`docs/fallback-uart.md`](docs/fallback-uart.md)

## 3D Printed Case Files

Development enclosure files are stored in `docs/3d_print_case/`:

- `docs/3d_print_case/2.FCStd` - FreeCAD source model
- `docs/3d_print_case/bp_checker_case.3mf` - 3MF print file
