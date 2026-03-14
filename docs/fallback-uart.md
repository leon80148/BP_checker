# UART Fallback Guide

## Purpose

This guide exists for boards or monitor setups that cannot use the primary `USB OTG Host` path.

It is a fallback path only.

The main repository definition remains:

- `ESP32-S3`
- `USB OTG Host`
- Monitor over `USB CDC`

## When to Use UART Fallback

Use this path only when at least one of the following is true:

- The board does not expose a usable OTG/Host connection
- The monitor integration is only available through direct serial wiring
- You are doing parser or transport debugging on a bench setup

## Important Constraints

- UART pin mapping is board-specific
- There is no universal `RX/TX` pair across all `ESP32-S3` boards
- Current repository history includes fixed UART pins, but those must not be treated as universally valid
- UART fallback should be enabled intentionally, not assumed from the primary quickstart

## Recommended Fallback Documentation Practice

When using UART fallback for a specific board, document:

- Exact board model
- Exact `RX`, `TX`, and `GND` wiring
- Signal voltage expectations
- Whether a level shifter or external converter is required

## Current Repository State

The repository keeps UART support as an explicit fallback transport.

- Change `kTransportMode` in `lib/BPConfig.h` to `TRANSPORT_MODE_UART_FALLBACK`
- Set the correct board-specific `RX` and `TX` pins in the same file
- Treat UART as an intentional deployment choice, not the default wiring model
