# BP_checker Clinic-Readiness Software Design Specification

**Status:** Reviewed implementation baseline

**Date:** 2026-07-11

**Primary product:** Local-first clinic measurement gateway for OMRON HBP-9030 over USB CDC

## 1. Product Boundary

BP_checker transfers a completed blood-pressure measurement from one supported monitor to an authenticated clinic-local Web interface. It is not a diagnostic engine, an electronic medical record, or a cloud service. The production path is ESP32-S3 + USB OTG Host + HBP-9030 USB output format 5. `CUSTOM` is available only in an explicit development build and cannot be enabled in a production artifact.

The device may retain a bounded measurement history, but it must not persist the HBP-9030 subject ID or an unredacted raw frame. Patient-to-record reconciliation belongs in a trusted clinic system, not in this ESP32 firmware.

## 2. Authoritative Protocol Contract

HBP-9030 factory format 5 is a CRLF-terminated CSV frame with exactly 11 fields:

1. year (`YYYY`)
2. month (`MM`)
3. day (`DD`)
4. hour (`HH`)
5. minute (`MM`)
6. subject ID (exactly 20 wire bytes; semantically up to 20 characters plus padding)
7. measurement error number
8. systolic pressure
9. diastolic pressure
10. pulse
11. movement count

The payload is exactly 53 bytes before CRLF (55 bytes on the wire). Its comma offsets and widths are fixed: `YYYY(4),MM(2),DD(2),HH(2),mm(2),ID(20),error(1),SYS(3),DIA(3),PR(3),movement(1)`. The 20-byte ID is opaque printable ASCII excluding delimiters and is transient even when blank/padded. A zero-error measurement requires ASCII digits in every numeric field; a nonzero-error frame requires exactly three spaces in each vital field. No sign, trim, prefix conversion, embedded NUL, or trailing byte is accepted.

Production acceptance requires a calendar-valid device time, zero error, and inclusive official measurement ranges: systolic 60-260 mmHg, diastolic 30-215 mmHg, and pulse 40-180 bpm. A nonzero error with three space-filled vital fields is a well-formed `DEVICE_ERROR`, not a malformed measurement; it is never added to history. Movement is retained as a quality warning. Parsing distinguishes `MALFORMED`, `DEVICE_ERROR`, `OUT_OF_RANGE`, and valid-with-motion.

Source: OMRON HBP-9030/HBP-9031C instruction manual, USB output format 5, page 20 (`2873643-0F`).

## 3. Data Model

`BPData` is the persistable measurement and contains only:

- validated HBP-9030 device timestamp and source (`device`; legacy migration may retain `legacy_system` or `legacy_unsynced`)
- systolic, diastolic, pulse, and movement count
- monotonic record sequence and opaque session sequence
- protocol validity and quality state
- sanitized diagnostic data that cannot contain the subject ID

Parser-only identity is represented separately from `BPData`, exists only long enough to advance an opaque session sequence when the transient identity changes, and is then cleared. It is never serialized, exported, rendered, or logged. Authorized staff hand an opaque record sequence to the trusted downstream workflow for patient association; the ESP32 never accepts or stores a patient identifier.

## 4. System Invariants

### Measurement integrity

- M1: Only exact, supported protocol frames may become valid records.
- M2: Numeric prefixes such as `120junk`, missing fields, trailing fields, invalid dates, nonzero error, and out-of-range vitals are rejected.
- M3: HBP-9030 device time is mandatory and authoritative. Invalid device time rejects the production record; system/NTP time never repairs it. `unsynced` is a legacy/diagnostic state and cannot be persisted for a new clinical record.
- M4: Framing is an explicit protocol contract: HBP-9030 uses `LINE_CRLF`; a supported binary protocol must define `FIXED_LENGTH(n)` plus a verified resynchronization header/checksum. Two fixed frames in one burst emit twice. Illustrative binary models without that contract are unsupported in production.
- M5: Transport overflow or discontinuity invalidates the in-progress frame through the next trusted boundary.
- M6: The subject ID cannot appear in persisted slots, API/HTML/CSV responses, or production Serial output.
- M7: A partial line never becomes a frame because of idle time. Overlong/missing-LF input is discarded, and model change/disconnect clears partial framing state.

### Runtime reliability

- R1: Measurement processing, authentication failure, and time lookup never delay the Arduino loop intentionally.
- R2: USB callbacks do not allocate or mutate Arduino `String`, close handles, or modify main-loop-owned state.
- R3: Cross-task data uses FreeRTOS queue/stream primitives or an equivalently proven synchronization boundary.
- R4: USB install/open/configuration failures expose a stable diagnostic and retry with bounded backoff.
- R5: Data-loss and reconnect counters survive transient status changes for operator diagnosis.
- R6: Overflow is ordered with received data through an in-band marker or frame epoch. A full data queue cannot suppress disconnect/error control events, and duplicate error/disconnect closes a handle at most once.

### Persistence

- P1: Each v3 slot is self-validating with sequence and checksum.
- P2: Startup reconstructs order from valid slots rather than trusting separately written count/index metadata.
- P3: Power loss after any single Preferences operation yields either the previous record set or the new record set, never a fabricated valid record.
- P4: Clear/reset commits an atomic generation/tombstone before garbage collection. A cut before it retains all records; a cut after it exposes none.
- P5: v2/legacy migration is idempotent at every cut point, preserves chronological order, and never introduces raw/identity data.
- P6: Clear/reset reports failure and removes both application and ESP Wi-Fi credentials when requested.
- P7: `addRecord()` reports storage failure. A write rejected before application leaves runtime history unchanged and the UI cannot claim durability; an applied-then-reported failure is reconciled deterministically from storage before success/failure is presented.

### Security and privacy

- S1: Every device has independent AP PSK, one-time bootstrap token, administrator access secret, and staff read/export secret. Each generated secret uses at least 128 bits of CSPRNG entropy, is never derived from MAC/chip ID or a repository default, and fails closed on entropy or NVS failure. Entropy initialization follows ESP-IDF RNG requirements.
- S2: First administrator setup requires the one-time token and provisioning AP interface, or an explicit physical-presence state. Claim state, administrator/staff secrets, and token-consumed state commit as one checksummed security bundle (or an equivalent recovery journal). Boot deterministically completes or rolls back every cut point, leaving only the safe unclaimed or complete claimed state.
- S3: Staff authorization permits de-identified health/history/API/export reads only. Administrator authorization permits those reads plus configuration, reset, credential rotation, and update. Empty/corrupt/uninitialized credentials deny all access.
- S4: Every route rejects a fresh request Host value that is not the AP IP, active STA IP, or device mDNS name. Policy must not use Arduino `WebServer::hostHeader()` unless the parser is fixed, because core 3.3.7 can retain the prior request's Host when a later request omits it.
- S5: State-changing requests also enforce same-origin CSRF checks.
- S6: Authentication failures are constant-work/nonblocking and rate-limited by bounded per-IP and global RAM buckets without `delay()` or persistent lockout. Expensive operations such as Wi-Fi scans have a separate cooldown.
- S7: Sensitive responses send `Cache-Control: no-store`; raw diagnostics default to sanitized output.
- S8: The WPA2-PSK provisioning AP is disabled after authenticated STA provisioning. A transient STA loss never reopens it. Recovery requires physical action, is time-bounded, and closes automatically.
- S9: Built-in Arduino-ESP32 MD5 Digest is not an accepted security boundary. The current HTTP product is permitted only after direct identity removal and inside the per-device WPA/isolated-clinic-network threat model. Any future identifiable-patient payload requires HTTPS or a trusted TLS gateway.
- S10: `WiFi.persistent(false)` is called before the first `WiFi.mode()`, `softAP()`, or `begin()`. Legacy SDK credentials are explicitly migrated/erased once. Reset uses a persisted `wipe_pending` state so application and driver credentials are removed before networking even across power cuts.
- S11: Every route belongs to a compile/test-checked default-deny registry, including 404 handling. The order is request bounds, fresh Host, state/interface policy, rate limit, authentication, mutation CSRF, then handler. Only the minimal unprovisioned bootstrap route is public.
- S12: The stock Arduino `WebServer` request parser is not release-acceptable because it reads unbounded request/header strings and a complete POST body before middleware. A fork/replacement must enforce request-line, header-count/bytes, strict `Content-Length`, route-specific body size, and body timeout before allocation. Duplicate/malformed Host is rejected at this layer.
- S13: Credential deletion is recoverable. Decommission reset also removes admin/bootstrap secrets, history, and sensitive RAM, and rotates recovery credentials. Logical NVS deletion is not claimed as forensic erase without flash encryption/secure boot or partition erasure.
- S14: Signed update metadata binds firmware hash, version, target, and anti-rollback sequence. Only the embedded trust anchor authorizes an update; boot validation rolls back an image that does not confirm healthy startup. The highest accepted sequence uses eFuse secure version or crash-consistent protected monotonic storage and cannot decrease during credential, factory, or decommission reset.

### User safety and accessibility

- U1: UI wording never diagnoses `normal`/`abnormal`; it reports configured review ranges and the guideline/version.
- U2: A reading is visibly marked current, stale, historical, invalid, or disconnected. Old data cannot masquerade as a new measurement.
- U3: A new record revision refreshes KPI, recent history, and diagnostic state even after the ring is full.
- U4: The UI recommends a second reading after one minute without automatically combining different unidentified patients.
- U5: Color is not the only status signal; language, labels, focus, tables, and dynamic status meet the static WCAG-oriented checklist.
- U6: Staff and administrator surfaces are visibly distinct. The record handoff exposes only an opaque sequence and never invites patient identity entry.

### Build and operations

- B1: A clean checkout builds directly with its real directory/sketch name.
- B2: ESP32 core and ArduinoJson versions are pinned in project/CI documentation.
- B3: Host tests, UI checks, and a warning-clean firmware build run from one quality-gate command.
- B4: The UI/API expose firmware version, build identifier, transport data-loss count, and supported protocol mode.
- B5: CI emits a commit-traceable firmware artifact, dependency SBOM, signature/manifest, and release checklist. Secure update/rollback is tested on target hardware before release.

## 5. Session and Freshness Rules

- A measurement received during this boot receives a monotonic revision.
- Persisted measurements loaded at boot are historical until a new frame is accepted.
- A current reading becomes stale after a configurable interval. Stale values may remain visible only with an explicit stale label.
- Repeated-measurement guidance is shown after a valid reading. The opaque in-memory session sequence may group repeats but never automatically averages them or leaves the authenticated staff/downstream handoff boundary.

## 6. Error Semantics

Invalid input is classified into stable reasons: wrong field count, bad syntax, invalid timestamp, monitor error, out-of-range value, overflow/discontinuity, and unsupported model/format. The Web UI reports an operator action without exposing raw identity. APIs use deterministic HTTP status and JSON fields; state-changing failures do not reboot unless the requested operation explicitly requires it.

## 7. Verification Contract

Completion requires all of the following current evidence:

- every new behavior has a test that was observed failing before implementation
- `bash scripts/run_host_tests.sh` passes
- `bash scripts/check_ui_markup.sh` passes
- the firmware builds from the real worktree path with `--warnings all` and no project warnings
- protocol probes reject malformed/out-of-range frames and split a two-frame binary burst
- security tests prove both malicious denial and legitimate authenticated access, including valid-Host followed by missing-Host on the same server
- persistence fault-injection tests cover every v3 write boundary
- `bash scripts/run_quality_gate.sh` passes
- dynamic browser route/role/accessibility checks pass on the supported kiosk/browser
- SBOM, signed artifact/manifest, boot confirmation, and rollback evidence are traceable to the reviewed SHA
- current HBP-9030 device-in-loop acceptance and the required transport/persistence soak pass
- the strict scorecard in `docs/reviews/PROJECT_SCORECARD.md` has independently verified evidence for at least 95 points and no hard-cap condition

Hardware-in-loop and browser/assistive-technology checks remain explicitly reported as unverified when the required hardware is unavailable; their unearned points cannot be substituted with static inspection.
