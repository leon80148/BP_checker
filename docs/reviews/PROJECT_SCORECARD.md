# BP_checker Clinic-Readiness Scorecard

## Release Decision

This rubric measures current release evidence, not implementation intent. `Clinic-ready` requires:

- raw score at least **95/100**
- category floors: Measurement 19/20, Security 18/18, Reliability 14/15, Persistence 9/10, UX 11/12, Tests 10/10, Build 7/7, Operations 7/8
- no open P0 or P1 and every mandatory control passing

Every control below is worth exactly one point. An awarded control must have a row in `EVIDENCE_MANIFEST.md` containing its ID, commit SHA, artifact/test, exact command, toolchain/hardware version, date, result, and reviewer. Missing, stale, unsupported code intent, or non-reproducible claims earn zero.

### Hard Caps

- Any P0: effective score at most 49 and release blocked.
- Any P1: effective score at most 89 and release blocked.
- Supported target cannot compile, link, flash, or boot: at most 49.
- Canonical clean build fails, or artifact is not traceable to SHA: at most 79.
- Any required automated suite fails or is flaky: at most 69.
- Unauthenticated access to subject identity or identifiable raw frames: at most 69.
- No current HBP-9030 device-in-loop acceptance, or no soak after transport/persistence changes: at most 89.
- A production behavior change without observed RED evidence earns zero TDD-discipline credit.

Mandatory controls are A02, A07-A18, B03-B18, C04-C13, D01-D09, E03, E06-E09, F09-F10, and G01-G02. They cannot be offset by unrelated points.

## A. Measurement and Protocol Integrity (20)

| ID | One-point control |
|---|---|
| A01 | HBP-9030 USB CDC is the named product target. |
| A02 | Production accepts only the supported format-5 contract. |
| A03 | CRLF frames survive arbitrary fragmentation. |
| A04 | Multiple complete frames in one burst remain ordered. |
| A05 | Partial lines never complete or resynchronize on idle time. |
| A06 | An overlong/overflowed frame is rejected without persistence. |
| A07 | Overflow/discontinuity recovery uses only a trusted boundary. |
| A08 | The 53-byte payload has exact field/comma offsets. |
| A09 | Field count and CRLF termination are exact. |
| A10 | Fixed-width numeric fields accept ASCII digits only. |
| A11 | NUL, signs, whitespace, prefixes, and trailing bytes are rejected. |
| A12 | Calendar-valid device time is authoritative. |
| A13 | The 20-byte ID is transient and absent from `BPData`. |
| A14 | Monitor error frames have distinct non-persistable semantics. |
| A15 | Movement is retained as quality metadata. |
| A16 | Official SYS/DIA/pulse boundaries are inclusive and tested. |
| A17 | Space-filled error vitals never become a measurement. |
| A18 | Unknown/CUSTOM/unverified models are production-disabled. |
| A19 | VID/PID, CDC interface binding, and record provenance are explicit. |
| A20 | Real-device normal/error/movement/boundary corpus passes. |

## B. Privacy and Security (18)

| ID | One-point control |
|---|---|
| B01 | Threat model defines actors, interfaces, trust, and residual risk. |
| B02 | Data-flow inventory covers every identity/measurement sink. |
| B03 | Per-device AP secret uses at least 128 bits of valid entropy. |
| B04 | Bootstrap/admin/staff secrets are independent, 128-bit, and fail closed. |
| B05 | First claim is physical/AP-bound, one-time, and crash-consistent. |
| B06 | All health/history/API/export/diagnostic reads require authorization. |
| B07 | Configuration/reset/update mutations require admin authorization. |
| B08 | Staff read/export and admin mutation roles are enforced separately. |
| B09 | Fresh Host validation covers every route and 404. |
| B10 | Methods and same-origin CSRF protect all mutations. |
| B11 | Parser-layer request/header/body bounds run before allocation/auth. |
| B12 | Provisioning/recovery AP closes and stays closed after transition. |
| B13 | Recovery AP requires physical presence and expires automatically. |
| B14 | Subject ID is absent from HTML/API/CSV/NVS and RAM records. |
| B15 | Production logs/hex diagnostics cannot expose raw identity. |
| B16 | Credential, at-rest, and HTTP/WPA/VLAN policies are explicit. |
| B17 | Firmware update authorization and signature checks fail closed. |
| B18 | Pinned-dependency and vulnerability-response process is executable. |

Stock Arduino MD5 Digest earns no credit. Basic authentication is limited to the documented de-identified, WPA2, isolated-clinic-network boundary. Deployment assumptions cannot earn B11; stock `WebServer` parses unbounded bodies before middleware. Identifiable data requires HTTPS or a trusted TLS gateway.

## C. Reliability and Concurrency (15)

| ID | One-point control |
|---|---|
| C01 | Frame assembly has no intentional wait or idle completion. |
| C02 | Wi-Fi scanning is asynchronous to measurement work. |
| C03 | Clock/auth/request paths meet documented loop latency budgets. |
| C04 | USB callbacks enqueue POD only; the main task owns handles/state. |
| C05 | Byte/control queues and snapshots are race-free by construction. |
| C06 | `String`, CDC handle, open/configure/close lifetimes are single-owner. |
| C07 | Executable concurrency stress reports no race. |
| C08 | Measurement frames and USB byte queues have tested hard bounds. |
| C09 | Ordered overflow/disconnect epochs prevent corrupt frame acceptance. |
| C10 | Drop/overflow/reconnect counters prevent silent loss. |
| C11 | Transport health/detail is explicit and operator-visible. |
| C12 | CDC open failure retries with bounded backoff. |
| C13 | Install/configure/disconnect/power/network/NTP fault matrix passes. |
| C14 | 24-hour soak records heap, stack, throughput, loss, and reconnects. |
| C15 | Soak shows no watchdog reset, leak trend, or stack exhaustion. |

## D. Persistence and Recovery (10)

| ID | One-point control |
|---|---|
| D01 | Each slot is atomically visible or absent after every cut point. |
| D02 | Startup derives order/count from self-validating slots. |
| D03 | CRC/version rejects truncated or corrupted records. |
| D04 | v2 migration preserves all supported values and order. |
| D05 | Migration is idempotent at every failure cut. |
| D06 | Retention, wrap, and overwrite behavior are explicit. |
| D07 | `uint64_t` sequence and trusted timestamp source survive reboot. |
| D08 | Clean clear removes current and legacy application history. |
| D09 | Generation/tombstone clear is atomic and failed writes are not claimed durable. |
| D10 | CSV order/escaping and bounded-retention wear rationale are verified. |

## E. UX and Clinical Safety (12)

| ID | One-point control |
|---|---|
| E01 | Staff measurement/read/export surface is explicit. |
| E02 | Admin-only operations and opaque downstream record handoff are explicit. |
| E03 | UI makes no normal/abnormal diagnosis. |
| E04 | Any reference range is named, versioned, and clinician-owned. |
| E05 | Retest/escalation copy avoids automatic cross-patient averaging. |
| E06 | Monitor/storage/transport errors give a concrete operator action. |
| E07 | Movement and quality state use text as well as color. |
| E08 | Current, stale, historical, invalid, and disconnected cannot be confused. |
| E09 | One record revision refreshes every measurement surface consistently. |
| E10 | Language, labels, focus, status, and tables pass static accessibility checks. |
| E11 | Supported kiosk/browser passes dynamic responsive/accessibility checks. |
| E12 | Destructive actions require safe confirmation and recovery guidance. |

The device never stores patient identity. Authorized staff hand an opaque record sequence to the trusted EMR/workflow that performs patient association; the ESP32 is not a patient-facing or EMR surface.

## F. Tests and Verification Discipline (10)

| ID | One-point control |
|---|---|
| F01 | Deterministic host suite runs from a clean checkout. |
| F02 | Parser/data/persistence/security policy core has boundary coverage. |
| F03 | Official real-device format-5 corpus is versioned. |
| F04 | Corpus covers normal, error, movement, ranges, split, and burst cases. |
| F05 | Transport framing/loss failure injection is executable. |
| F06 | Concurrency/race test is executable on every relevant change. |
| F07 | Persistence power-cut matrix and fuzz/corruption tests pass. |
| F08 | Dynamic route-security and browser accessibility tests pass. |
| F09 | Current target hardware/HBP-9030 acceptance passes. |
| F10 | Current transport/persistence soak passes. |

## G. Build and Release (7)

| ID | One-point control |
|---|---|
| G01 | Case-sensitive clean checkout builds with the canonical command. |
| G02 | ESP32-S3 target compiles/links with warning and size evidence. |
| G03 | CLI/core/libraries are pinned and an SBOM is generated. |
| G04 | CI runs exactly the local quality gate. |
| G05 | Versioned artifact is reproducible and traceable to commit SHA. |
| G06 | Signed update plus boot validation/rollback is implemented and tested. |
| G07 | Release checklist records all gates, versions, artifacts, and approvals. |

## H. Documentation and Operations (8)

| ID | One-point control |
|---|---|
| H01 | Local de-identified gateway scope and exclusions are explicit. |
| H02 | Reference ESP32-S3 board, OTG topology, and power budget are explicit. |
| H03 | USB2 Type-B and function item 32/format 5 setup are explicit. |
| H04 | WPA2/VLAN/HTTP residuals and credential onboarding are deployable. |
| H05 | Operator runbook covers normal work, errors, and recovery. |
| H06 | Retention, time, export, decommission, and privacy operations are explicit. |
| H07 | Tested board/monitor/browser compatibility matrix is current. |
| H08 | Maintenance, vulnerability response, update, rollback, and EOL are explicit. |

## Review Procedure

1. Run `bash scripts/run_quality_gate.sh` and retain unabridged output.
2. Evaluate hard caps and mandatory controls before scoring.
3. Rebuild `EVIDENCE_MANIFEST.md`; award only one-point IDs with current evidence.
4. Record deductions with severity, owner file, and the next executable test.
5. Fix the highest-risk deduction through RED-GREEN-REFACTOR and repeat.
6. A reviewer other than the implementer performs the final score audit.
