# Hardware Guide

## Primary Product Definition

This repository primarily supports:

- `ESP32-S3`
- `USB OTG/Host`
- Blood pressure monitor connected over `USB CDC`

The intended field topology is:

1. Board power/programming USB to computer or power
2. Board OTG/Host USB to blood pressure monitor

No GPIO data wiring is part of the primary path.

## Current Firmware Migration Status

The repository has been re-scoped around the OTG Host product definition, but the firmware is still in migration:

- Transport selection has been separated from parser and UI logic
- `UART fallback` remains available for working deployments
- `USB OTG Host` is the target primary transport, but the CDC host runtime is not yet complete in the Arduino sketch

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
