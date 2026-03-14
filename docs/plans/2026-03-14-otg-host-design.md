# OTG Host Version Design

**Status:** Approved design baseline

**Date:** 2026-03-14

## Problem

The repository currently mixes two incompatible product definitions:

- The documentation describes an `ESP32-S3 + USB OTG Host + CDC` device.
- The firmware implementation reads blood pressure data from `Serial1` over `TTL/UART` on fixed GPIO pins.

This mismatch causes deployment failures when users follow the documented dual-USB wiring flow on a board that is not physically wired for the current UART implementation.

## Goals

- Define a single primary product path for this repository.
- Make the primary path match the intended field wiring model: one USB connection for power/programming and one USB connection for the blood pressure monitor.
- Restrict the primary support target to boards that can realistically support the chosen transport.
- Document a clear fallback path for boards that cannot support the primary path.
- Separate primary-path instructions from fallback instructions so users do not mix them.

## Non-Goals

- Guarantee support for every `ESP32` or `ESP32-S3` board on the market.
- Preserve the current mixed OTG/UART messaging in user-facing docs.
- Build a generic abstraction for arbitrary USB classes beyond the blood pressure monitor use case.
- Keep `TTL/UART` as a first-class co-equal transport in the main user flow.

## Product Definition

The repository will be defined as:

`ESP32-S3 blood pressure USB OTG Host to Wi-Fi monitor`

Primary operating model:

- `ESP32-S3` board
- USB OTG/Host-capable hardware path
- Blood pressure monitor connected through the OTG/Host port
- Expected monitor-side transport: standard `USB CDC` serial-like device
- Web UI and Wi-Fi behavior remain the same product surface

## Support Matrix

### Primary Support

Supported when all of the following are true:

- MCU family is `ESP32-S3`
- The board exposes a usable `USB OTG/Host` connection
- The board can provide stable power for the attached monitor or uses a validated powered topology
- The blood pressure monitor enumerates as a supported `USB CDC` device

### Fallback Support

Allowed but not part of the primary product path:

- `ESP32-S3` boards without usable OTG/Host routing
- Boards where monitor integration only works through `TTL/UART`
- Debug or bench setups that use GPIO serial wiring to validate parser behavior

Fallback constraints:

- Fallback is documented in a dedicated section only
- Fallback is not the default getting-started flow
- Fallback does not redefine the product description of the repository

### Out of Scope

- Non-S3 ESP32 boards as primary targets
- Vendor-specific USB monitor integrations that are not `CDC`, unless explicitly implemented later

## Hardware and Wiring Rules

### Primary OTG Flow

- One USB connection from the board programming/power port to the computer or power source
- One USB connection from the blood pressure monitor to the board OTG/Host port
- No GPIO data wiring required for the primary flow

### Fallback UART Flow

- Used only when OTG is unavailable
- Requires explicit `RX/TX/GND` wiring documentation
- Pin assignments are board-specific and must not be presented as universal

## Software Architecture Direction

The firmware should move from a single hard-coded UART transport to a transport-oriented design:

- `MonitorTransport` concept with a single active backend at runtime
- Primary backend: `UsbCdcTransport`
- Optional fallback backend: `UartTransport`
- Parsing, record storage, Wi-Fi, and web UI remain transport-agnostic

This keeps monitor parsing logic independent from the physical link layer and avoids repeating current transport confusion in future changes.

## Runtime Behavior

Primary OTG behavior:

- Firmware initializes OTG/Host stack
- Firmware waits for USB device attach
- Firmware validates that the device is a supported CDC target
- Once connected, firmware reads byte stream, parses data, stores history, and publishes to the web UI
- UI and serial logs clearly show transport state: waiting, attached, unsupported device, receiving, disconnected

Fallback UART behavior:

- Only enabled by explicit build/config selection
- Logs and docs must clearly label the device as running in fallback mode

## Error Handling

Primary-path error states to define explicitly:

- No OTG-capable board or no host hardware path
- No device attached
- Attached device is not CDC
- CDC device attached but no readable data stream
- Device disconnect during measurement
- Power instability on the OTG side

Required behavior:

- Surface transport status in serial logs
- Keep Wi-Fi UI available even when monitor transport is unavailable
- Do not silently fall back from OTG to UART in the main flow

## Documentation Reorganization

### README

The README should be rewritten around OTG-first messaging:

- Product title and overview describe OTG Host as the primary path
- Hardware section explains the dual-USB topology
- Compatibility section states that only OTG-capable `ESP32-S3` boards are primary targets
- Fallback section documents `TTL/UART` as a separate alternative, not the main path

### Additional Docs

Suggested companion docs:

- `docs/hardware.md` for support matrix and wiring diagrams
- `docs/fallback-uart.md` for legacy/debug transport instructions
- `docs/migration.md` for moving from the current UART codebase to OTG-first implementation

## Acceptance Criteria

- The repository states exactly one primary transport: `USB OTG Host + CDC`
- The repository states exactly one primary target family: `ESP32-S3`
- Fallback `TTL/UART` is documented as non-primary
- README no longer mixes OTG main flow with fixed universal UART pin claims
- Firmware architecture plan separates transport from parser/UI/storage responsibilities

## Recommendation

Adopt `OTG-first with explicit fallback` immediately at the specification level, then implement in two phases:

1. Documentation and architecture cleanup
2. Firmware transport refactor and OTG transport implementation

This yields the best balance of field usability, conceptual clarity, and future maintainability.
