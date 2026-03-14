# OTG Host Version Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Convert the repository from a mixed OTG/UART definition into an OTG-first `ESP32-S3` project with a documented UART fallback path.

**Architecture:** First clean up the product definition and documentation so the repository has one primary transport story. Then refactor firmware so transport handling is separated from parser/UI/storage responsibilities, enabling an OTG CDC primary backend and an explicitly non-primary UART fallback backend.

**Tech Stack:** Arduino for ESP32-S3, `WebServer`, `Preferences`, existing parser/storage/UI code, future ESP32-S3 USB Host/CDC support, Markdown docs.

---

### Task 1: Freeze the product definition in documentation

**Files:**
- Modify: `README.md`
- Create: `docs/hardware.md`
- Create: `docs/fallback-uart.md`
- Modify: `docs/CHANGELOG.md`

**Step 1: Write the failing doc checklist**

Create a checklist in `docs/hardware.md` draft notes that asserts:

- README primary flow is OTG-first
- Supported target family is ESP32-S3
- Fallback UART is separated from the primary flow

**Step 2: Verify the repo fails the checklist**

Run:

```powershell
Get-Content README.md
```

Expected: README still mixes OTG messaging with UART implementation details.

**Step 3: Write minimal documentation updates**

- Rewrite README overview, wiring, and compatibility around OTG-first behavior
- Add `docs/hardware.md` with support matrix and wiring model
- Add `docs/fallback-uart.md` with explicit non-primary UART fallback notes
- Update changelog with the documentation cleanup entry

**Step 4: Verify the documentation passes review**

Run:

```powershell
Get-Content README.md
Get-Content docs/hardware.md
Get-Content docs/fallback-uart.md
```

Expected: OTG is primary, UART is clearly fallback-only.

**Step 5: Commit**

```bash
git add README.md docs/hardware.md docs/fallback-uart.md docs/CHANGELOG.md
git commit -m "docs: define OTG-first project scope"
```

### Task 2: Isolate transport concerns in firmware

**Files:**
- Modify: `bp_checker.ino`
- Modify: `lib/DataProcessor.h`
- Create: `lib/transports/MonitorTransport.h`
- Create: `lib/transports/UartTransport.h`
- Create: `lib/transports/UsbCdcTransport.h`

**Step 1: Write the failing compile-oriented design check**

Document the intended responsibilities:

- Parser code must not directly depend on fixed `Serial1` wiring
- Transport selection must not be embedded in `bp_checker.ino`

**Step 2: Verify current code fails the design check**

Run:

```powershell
Get-Content bp_checker.ino
Get-Content lib/DataProcessor.h
```

Expected: fixed UART pins and `Serial1` calls are hard-coded.

**Step 3: Write minimal transport abstractions**

- Add a transport interface for `begin`, `available`, `read`, and state reporting
- Move current UART behavior into `UartTransport`
- Stub `UsbCdcTransport` with explicit not-yet-complete state handling if OTG implementation is not ready yet
- Update `DataProcessor` to consume a transport abstraction instead of `Serial1`

**Step 4: Verify compile behavior**

Run:

```bash
arduino-cli compile -b esp32:esp32:esp32s3 --board-options USBMode=default /path/to/BP_checker
```

Expected: build succeeds with the transport abstraction in place.

**Step 5: Commit**

```bash
git add bp_checker.ino lib/DataProcessor.h lib/transports
git commit -m "refactor: isolate monitor transport layer"
```

### Task 3: Implement OTG-first runtime behavior

**Files:**
- Modify: `bp_checker.ino`
- Modify: `lib/transports/UsbCdcTransport.h`
- Modify: `lib/WebHandler.h`
- Modify: `lib/WiFiManager.h`

**Step 1: Write the failing behavior checklist**

Checklist:

- Device can boot without a monitor attached
- Serial logs show OTG wait/attach/error states
- UI remains reachable while transport is disconnected

**Step 2: Verify current firmware fails the checklist**

Run a build and inspect runtime logs on hardware.

Expected: current implementation only understands UART activity.

**Step 3: Write minimal OTG-first behavior**

- Initialize OTG transport first
- Report attach/disconnect/error states in logs
- Keep web server active regardless of monitor state
- Add a simple transport-status surface for future UI display

**Step 4: Verify on hardware**

Run:

```bash
arduino-cli compile --upload -b esp32:esp32:esp32s3 --board-options USBMode=default -p <PORT> /path/to/BP_checker
arduino-cli monitor -p <PORT> -c baudrate=115200
```

Expected: firmware boots, OTG state is visible, and UI remains accessible without live monitor data.

**Step 5: Commit**

```bash
git add bp_checker.ino lib/transports/UsbCdcTransport.h lib/WebHandler.h lib/WiFiManager.h
git commit -m "feat: add OTG-first monitor transport flow"
```

### Task 4: Preserve explicit UART fallback without mixing product messaging

**Files:**
- Modify: `README.md`
- Modify: `docs/fallback-uart.md`
- Modify: `bp_checker.ino`
- Modify: any transport-selection config file introduced earlier

**Step 1: Write the failing fallback checklist**

Checklist:

- UART fallback is opt-in only
- UART fallback docs do not appear in the README main quickstart path
- Firmware logs identify fallback mode explicitly

**Step 2: Verify current repo fails the checklist**

Inspect docs and code.

Expected: UART and OTG concepts are currently mixed.

**Step 3: Write minimal fallback controls**

- Add explicit compile-time or config-time selection for fallback UART mode
- Update docs so UART fallback is isolated from the main quickstart
- Add startup logs that clearly state when fallback mode is active

**Step 4: Verify**

Run:

```bash
arduino-cli compile -b esp32:esp32:esp32s3 --board-options USBMode=default /path/to/BP_checker
```

Expected: default build remains OTG-first, fallback is only enabled intentionally.

**Step 5: Commit**

```bash
git add README.md docs/fallback-uart.md bp_checker.ino
git commit -m "docs: isolate UART fallback flow"
```

### Task 5: Final validation and handoff

**Files:**
- Modify: `README.md` if validation reveals gaps
- Modify: `docs/CHANGELOG.md`

**Step 1: Run validation**

Run:

```bash
arduino-cli compile -b esp32:esp32:esp32s3 --board-options USBMode=default /path/to/BP_checker
```

Then perform hardware smoke checks:

- Boot with no monitor attached
- Boot with monitor attached to OTG
- Confirm UI is reachable
- Confirm serial logs show transport state

**Step 2: Fix any validation gaps minimally**

Only touch the files that correspond to the verified gap.

**Step 3: Update changelog**

Add the OTG-first transport milestone notes.

**Step 4: Commit**

```bash
git add README.md docs/CHANGELOG.md
git commit -m "chore: finalize OTG-first validation notes"
```
