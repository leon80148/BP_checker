# BP_checker Clinic-Readiness 95+ Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Convert the current HBP-9030 bridge MVP into a reproducible, privacy-preserving, measurement-safe clinic-local gateway that earns at least 95/100 on the repository scorecard.

**Architecture:** Keep the single-loop Arduino application and existing transport/parser/storage boundaries, but make each boundary explicit and host-testable. Parse official format 5 into structured measurement plus transient identity, use queue/stream ownership for USB callbacks, reconstruct persistence from self-validating slots, centralize route access policy, and expose record revision/freshness instead of treating stored data as live.

**Tech Stack:** Arduino ESP32 core 3.3.7, ArduinoJson 7.4.2, ESP-IDF USB Host/FreeRTOS, C++17 host harness, Bash quality gate, GitHub Actions.

---

## Mandatory TDD Protocol

For every production behavior change:

1. Add one focused test.
2. Run the narrow test/full host script and capture the expected failure in `docs/reviews/TDD_EVIDENCE.md`.
3. Implement the smallest complete behavior.
4. Run the focused test and full owning suite; capture the passing result.
5. Refactor only while green.
6. Commit the task before its spec and code-quality reviews.

No production implementation may precede its RED evidence. Firmware-only code that cannot run in the host harness must first expose a pure state reducer/policy with host tests, then receive a warning-clean firmware build and independent code review.

### Task 1: Freeze SDD, Scorecard, and Baseline

**Files:**
- Create: `docs/specs/2026-07-11-clinic-readiness-sdd.md`
- Create: `docs/reviews/PROJECT_SCORECARD.md`
- Create: `docs/reviews/2026-07-11-iteration-0.md`
- Create: `docs/reviews/EVIDENCE_MANIFEST.md`
- Create: `docs/reviews/TDD_EVIDENCE.md`

**Step 1: Verify the baseline evidence**

Run:

```bash
bash scripts/run_host_tests.sh
bash scripts/check_ui_markup.sh
arduino-cli compile -b esp32:esp32:esp32s3 --board-options USBMode=default .
```

Expected: 175 host checks pass, UI check passes, direct firmware build fails on sketch/directory mismatch.

**Step 2: Record requirements and strict scoring**

Document protocol, privacy, concurrency, persistence, access-control, freshness, build, and verification invariants. Score only current evidence and apply the raw-identity hard cap.

**Step 3: Independent reviews**

Have one firmware reviewer, one security reviewer, and one product/scoring reviewer inspect the documents. Resolve every critical or important gap before proceeding.

**Step 4: Commit**

```bash
git add docs/specs docs/reviews docs/plans/2026-07-11-clinic-readiness-95.md
git commit -m "docs: define clinic-readiness specification and scorecard"
```

### Task 2: Make Build and Quality Gates Reproducible

**Files:**
- Rename: `bp_checker.ino` -> `BP_checker.ino`
- Create: `sketch.yaml`
- Create: `VERSION`
- Create: `scripts/run_quality_gate.sh`
- Create: `scripts/generate_sbom.sh`
- Create: `.github/workflows/quality.yml`
- Modify: `scripts/run_host_tests.sh`
- Modify: `README.md`
- Modify: `AGENTS.md` only if it is tracked in this branch

**Step 1: RED, prove clean checkout cannot build**

Run the documented `arduino-cli compile ... .` command and record the missing-main-file failure.

**Step 2: GREEN, rename the sketch and pin the toolchain**

Use a case-safe `git mv` through a temporary tracked name. Add an Arduino CLI profile pinning ESP32 core 3.3.7 and ArduinoJson 7.4.2. Add `VERSION` and expose it as a compile definition or header. Generate a machine-readable SBOM containing the CLI, platform, libraries, compiler, source SHA, and artifact hash.

**Step 3: Add the quality gate**

`scripts/run_quality_gate.sh` must run host tests, UI/static privacy checks, and `arduino-cli compile --warnings all`. It must fail on project warnings or any subcommand failure and print firmware size.

**Step 4: Add CI parity**

The GitHub workflow installs the pinned CLI/core/library and runs the same quality gate. Do not duplicate test commands in YAML. Upload the versioned firmware, build metadata, and SBOM as SHA-traceable CI artifacts.

**Step 5: Verify and commit**

Expected: direct worktree build passes with no project warnings and all existing tests remain green.

```bash
git add -A
git commit -m "build: make firmware builds reproducible"
```

### Task 3: Implement Exact HBP-9030 Format-5 Parsing

**Files:**
- Create: `lib/BPProtocol.h`
- Create: `test/host/test_hbp9030_protocol.cpp`
- Modify: `lib/BP_Parser.h`
- Modify: `test/host/test_bp_parser.cpp`
- Modify: `scripts/run_host_tests.sh` only if link/include support is required

**Step 1: RED, add official golden fixtures**

Add focused tests for:

- exact 53-byte valid 11-field frame (`2026,07,11,09,05,12345678901234567890,0,120,080,072,0`)
- valid leap day `2024-02-29` and an exactly 20-space blank/padded transient ID
- wrong field counts (10 and 12)
- short/long ID, misplaced comma, embedded NUL, numeric prefix (`120junk`), sign, and whitespace where digits are required
- invalid month/day/hour/minute and non-leap February 29
- nonzero monitor error and blank error-frame vitals
- nonzero monitor error with otherwise numeric in-range vitals still classified `DEVICE_ERROR` and never persisted
- official min/max values and one-below/one-above boundaries
- movement count retention
- transient subject ID returned separately from persistable measurement
- formats 1-4 rejected with an actionable `unsupported_format` reason

Run `bash scripts/run_host_tests.sh`; expected new checks fail against the permissive parser.

**Step 2: GREEN, add a structured parse result**

Introduce stable `MALFORMED`, `DEVICE_ERROR`, and `OUT_OF_RANGE` results plus motion quality, strict fixed-offset unsigned parsing, calendar validation, and `BPParseResult { BPData measurement; String transientSubjectId; BPParseError error; }`. Keep `BPData` free of subject identity. The parser receives 53 bytes without CRLF and rejects delimiters; DataProcessor alone removes a complete CRLF boundary.

**Step 3: Preserve experimental parsers deliberately**

Production model selection accepts only `OMRON-HBP9030`; `CUSTOM` and existing unverified binary parsers are development-only until each has an official grammar, resynchronization contract, real-device corpus, and HIL acceptance. Unknown model names must not silently fall back.

**Step 4: Verify, refactor, commit**

Run the focused test binary, full host suite, and firmware build.

```bash
git add lib/BPProtocol.h lib/BP_Parser.h test/host
git commit -m "feat(protocol): validate HBP-9030 format 5"
```

### Task 4: Make Frame Processing Nonblocking and Loss-Aware

**Files:**
- Modify: `lib/DataProcessor.h`
- Modify: `lib/transports/MonitorTransport.h`
- Modify: `test/host/test_data_processor.cpp`
- Modify: `test/host/Arduino.h`

**Step 1: RED, encode the known failures**

Add tests proving:

- a valid HBP-9030 frame never calls the system clock and fake `millis()` is unchanged
- HBP-9030 device timestamp wins even when system/NTP time differs
- invalid device time is rejected rather than repaired with system/NTP time
- two fixed-length binary frames in one burst create two records
- trailing bytes cannot be ignored as a valid first binary frame
- one fixed frame delivered as 4+6 bytes waits then emits once
- one complete fixed frame plus half a second emits one and retains the half
- embedded `0x0A` in a fixed frame remains data
- garbage prefix resynchronizes only at a verified header/checksum boundary
- a half HBP-9030 line remains pending after five seconds and completes only at CRLF
- LF-only and CR-only HBP-9030 lines are rejected
- overlong/missing-LF input is discarded and the next complete line succeeds
- model switch and disconnect clear partial frame state
- transport loss clears a partial frame and discards through the next protocol-trusted boundary (CRLF for format 5; verified header/checksum for supported binary)
- the first clean frame after loss is accepted
- production diagnostics redact the subject ID
- invalid frames expose a stable sanitized reason without persisting

Run the host suite and record each expected failure.

**Step 2: GREEN, implement protocol-driven framing**

Expose an explicit `LINE_CRLF` or `FIXED_LENGTH(n)` framing contract from each supported parser. Complete fixed frames immediately; line frames never idle-flush partial input. Binary support requires a verified resynchronization boundary. Add a monotonic `dataLossCount()`/frame epoch transport contract and track generation changes.

**Step 3: Remove blocking time and raw logging**

Use parsed device time and remove system-clock lookup from the production acquisition path. New production records never persist `system` or `unsynced` timestamps; those values are legacy-migration/diagnostic states only. Remove full-frame Serial output from production; render only sanitized structured diagnostics. Guard optional byte dumps behind an explicit development-only compile flag and never dump a format whose identity boundary is unknown.

**Step 4: Verify and commit**

```bash
git add lib/DataProcessor.h lib/transports/MonitorTransport.h test/host
git commit -m "fix(data): make frame processing loss-aware and nonblocking"
```

### Task 5: Make USB Transport Task-Safe and Recoverable

**Files:**
- Create: `lib/transports/UsbCdcState.h`
- Create: `test/host/test_usb_cdc_state.cpp`
- Modify: `src/transports/UsbCdcTransport.cpp`
- Modify: `lib/transports/UsbCdcTransport.h`

**Step 1: RED, test the pure lifecycle reducer**

Cover attach, open success, config failure, transfer error, disconnect, install failure, retry backoff, overflow generation, and recovery. Assert callback events never directly transition ownership-sensitive handles.

Also cover disconnect between open/configure, error followed by disconnect, data-queue saturation with a control event, exact dropped-byte/episode counters, two independent overflow episodes, and install-fails-once retry. Define disconnect/error ordering across data and control queues using an in-band epoch or atomic owner-side flush: no queued/partial byte crosses reconnect, duplicate error/disconnect closes once, and the first post-reconnect clean frame is accepted. Run a host concurrency stress harness under ThreadSanitizer where the platform supports it.

**Step 2: GREEN, move cross-task communication to FreeRTOS primitives**

Use a single-writer/single-reader stream buffer for bytes and an independently bounded queue of POD USB control events. Callbacks enqueue only; they do not allocate/mutate `String` or close handles. `poll()` owns `cdcHandle`, state strings, close/reopen, backoff, and persistent diagnostic counters. Status and detail are returned as one consistent snapshot.

**Step 3: Propagate loss safely**

If stream send is short, enqueue an ordered overflow marker or advance a frame epoch visible with the affected bytes. The main owner discards through the next trusted boundary, then accepts the next clean frame. Do not use an unordered boolean overflow flag.

**Step 4: Verify and commit**

Run reducer tests, full host suite, and warning-clean firmware build. Independently review callback/main ownership line by line.

```bash
git add lib/transports src/transports test/host/test_usb_cdc_state.cpp
git commit -m "fix(usb): serialize CDC transport state ownership"
```

### Task 6: Introduce Crash-Consistent v3 Persistence

**Files:**
- Modify: `lib/BPProtocol.h`
- Modify: `lib/BPRecordManager.h`
- Modify: `lib/DataProcessor.h`
- Modify: `lib/WebHandler.h`
- Modify: `test/host/Preferences.h`
- Modify: `test/host/test_record_manager.cpp`
- Modify: `test/host/test_data_processor.cpp`
- Modify: `scripts/check_ui_markup.sh`

**Step 1: RED, add write-fault injection**

Extend only the test shim with a controllable failure after N Preferences writes. For every write boundary, simulate reboot and assert:

- no fabricated valid record
- newest surviving sequence is returned first
- either old or new valid record set is visible
- checksum-corrupt/malformed slot is ignored
- missing metadata does not lose a self-validating slot

Also cover v2 and legacy migration, wraparound, clear failure, and raw/identity absence.

The Preferences fake must inject failure both before application and after application/report-failure. Migration must be idempotent at every cut point. Clear must prove generation/tombstone semantics: cuts before retain all history; cuts after expose zero even when garbage collection never runs.

Assert `addRecord()` reports persistence failure. A before-apply failure leaves runtime history unchanged and surfaces storage failure instead of durable success. An applied-then-reported failure triggers deterministic reload/reconciliation and may expose only the complete pre-write or post-write history.

Assert the production acquisition path renders and logs acceptance only after `addRecord()` confirms durability. A failed or ambiguous write must expose a storage-specific operator action. History clear must likewise return a non-success response when the tombstone or cleanup reports failure; it may not erase the in-memory diagnostic first and then claim success.

**Step 2: GREEN, store self-validating slots**

`BPData` gains the opaque `uint64_t` record and session sequences required by the SDD. A single self-validating `v3_state` value atomically binds schema version, active generation, sequence floor, and CRC32. Each v3 slot is one separately atomic binary value containing version, generation, record/session sequences, every structured measurement field, and CRC32; encoding is byte-defined rather than native-struct/padding dependent. Startup validates the state and every slot, filters by committed generation, rejects malformed/duplicate sequences, sorts by record sequence, compacts holes in memory, and derives the next sequence without trusting count/index metadata.

Clear first advances the generation in `v3_state` while retaining the sequence floor, then garbage-collects old v3/v2/legacy keys without ever deleting the active tombstone. Migration stages all v3 slots while legacy metadata remains authoritative and writes `v3_state` as the final activation operation. A present but corrupt v3 state fails closed instead of falling back to stale history.

**Step 3: Verify and commit**

```bash
git add lib/BPProtocol.h lib/BPRecordManager.h lib/DataProcessor.h lib/WebHandler.h \
  test/host/Preferences.h test/host/test_record_manager.cpp \
  test/host/test_data_processor.cpp scripts/check_ui_markup.sh
git commit -m "feat(storage): add crash-consistent record slots"
```

### Task 7: Close Provisioning, Authentication, and Privacy Boundaries

**Files:**
- Create: `lib/DeviceSecurity.h`
- Create: `lib/BoundedHttpRequest.h`
- Create: `lib/WebAccessPolicy.h`
- Create: `test/host/test_device_security.cpp`
- Create: `test/host/test_bounded_http_request.cpp`
- Create: `test/host/test_web_access_policy.cpp`
- Modify: `lib/WebSecurity.h`
- Modify: `lib/WebHandler.h`
- Modify: `lib/WiFiManager.h`
- Modify: `BP_checker.ino`
- Modify: `test/host/test_web_security.cpp`

**Step 1: RED, write security contract tests**

Prove malicious and legitimate controls for:

- four independent 128-bit AP/bootstrap/admin/staff secrets from injected entropy; zero/failing entropy and NVS write failure fail closed
- first administrator claim denied without correct bootstrap token/interface; correct token succeeds once; every persistence cut is fail closed
- every registered route, including 404, classified as provisioning-public or authenticated; an unclassified route fails the gate
- staff may read/export but cannot configure/reset/update; admin may perform both classes
- foreign/missing Host rejected for GET and POST
- a valid-Host request followed by a Host-less request on the same server is rejected
- CSRF still required for state changes
- oversized/negative/overflow/duplicate `Content-Length`, duplicate Host, long request/header, slow/interrupted body, and unknown POST are rejected before unbounded allocation
- auth failure rate limiter returns immediately, remains bounded after 100 sources, and resets after cooldown
- invalid model value rejected by allowlist
- sensitive responses request `no-store`
- reset erases custom and driver credentials in the Wi-Fi state contract

**Step 2: GREEN, centralize device security state**

Generate/store independent per-device AP, one-time bootstrap, administrator, and staff credentials from 128-bit CSPRNG entropy. Do not derive them from MAC/chip ID; enable a documented valid entropy source first, and fail closed on entropy/NVS failure. Require the token on the provisioning AP or a physical-presence state to perform an atomic first claim. Store claim state, administrator/staff credentials, and token-consumed state in one checksummed atomic bundle, or use a journal whose boot recovery deterministically completes/rolls back every injected cut point. Protect all routes with one default-deny registry. Staff may read/export de-identified records; only admin may configure, reset, rotate credentials, or update. Collect and validate the per-request `Host` header value; do not rely on the stale core `_hostHeader` field.

Do **not** use Arduino-ESP32 3.3.7 built-in Digest as the security boundary: it is MD5-based and its verifier does not provide the URI, nonce-count, or body binding needed by this threat model. The initial software-only implementation may use built-in Basic authentication only inside the explicitly documented per-device WPA/isolated-clinic-network boundary because the firmware removes subject identity. Record HTTP confidentiality as residual risk. If identifiable patient data is ever added, require HTTPS or a trusted TLS gateway before release.

**Step 3: GREEN, make AP lifecycle explicit**

Provisioning AP uses WPA2-PSK and is active only while unprovisioned or during explicit, physically initiated, time-bounded recovery. Call `WiFi.persistent(false)` before the first Wi-Fi initialization call. After STA connects and provisioning completes, stop the AP, switch to STA mode, and verify the radio mode. Transient STA loss never reopens AP. Perform a one-time legacy SDK credential erase/migration. Reset commits `wipe_pending` first and completes application/SDK erasure before networking on the next boot.

**Step 4: GREEN, bound parsing before middleware**

Fork or replace the stock request parser at the narrowest layer that executes before Arduino `WebServer` body parsing. Enforce a maximum request line, total/count/per-header limits, strict decimal `Content-Length`, route-specific body caps, and a short body deadline before allocating the payload. Reject duplicate/malformed Host. The parser must continue servicing measurement work within its stated latency budget during slow or interrupted requests.

**Step 5: Preserve legitimate behavior**

The owner can provision from AP, use staff read/export access, use admin mutation access, rotate each access secret, and recover physically. No handler uses `delay()` for authentication. Document that Basic credentials and measurements remain visible to a party already on the IP segment; WPA/VLAN is a deployment assumption, not a firmware guarantee. Identifiable patient data remains prohibited until HTTPS or a trusted TLS gateway is required.

**Step 6: Verify and commit**

Run security tests, full host suite, static route/leak checks, and firmware build.

```bash
git add lib BP_checker.ino test/host
git commit -m "feat(security): require authenticated clinic access"
```

### Task 8: Add Freshness, Safe Clinical UI, and Operations State

**Files:**
- Create: `lib/MeasurementPolicy.h`
- Create: `test/host/test_measurement_policy.cpp`
- Modify: `lib/WebHandler.h`
- Modify: `lib/BPRecordManager.h`
- Modify: `scripts/check_ui_markup.sh`
- Modify: `README.md`
- Modify: `docs/hardware.md`
- Create: `docs/deployment.md`
- Create: `docs/security.md`

**Step 1: RED, test classification and freshness policy**

Cover configured review boundaries, urgent boundaries, invalid values, current/stale/historical/disconnected transitions, revision changes after ring wrap, and repeated-measurement guidance. Assert no result string says `正常` or `異常`.

**Step 2: GREEN, expose explicit status**

API fields include revision, sequence, timestamp source, quality/movement, freshness state, firmware version, protocol, data-loss/reconnect counts, and last successful receive age. Persisted boot history is historical, not current.

Expose an opaque record/session sequence for the authenticated staff handoff. Do not provide any patient-ID input. Render admin-only controls separately and prove a staff credential cannot discover or invoke them.

**Step 3: GREEN, refresh the whole measurement surface**

When revision changes, refresh KPI, recent history, and sanitized diagnostic state. Poll failure shows a visible stale/disconnected banner with last successful update; it is never silently swallowed.

**Step 4: GREEN, static accessibility**

Add `lang="zh-Hant"`, label associations, visible focus, status/live semantics, table captions/scopes, mobile overflow wrappers, and text cues in addition to color. Remove diagnostic `normal/abnormal` copy and name the configured reference policy.

**Step 5: Deployment documentation**

Document one reference ESP32-S3 board/topology, OTG power limits, HBP-9030 USB2 Type-B, function item 32 format 5, unique secret onboarding, recovery, retention, privacy, supported/experimental models, and device-in-loop checklist.

**Step 6: Verify and commit**

```bash
git add lib test/host scripts README.md docs
git commit -m "feat(web): surface fresh and actionable measurement state"
```

### Task 9: Add Signed Release, Browser, and Hardware Evidence

**Files:**
- Create: `lib/FirmwareUpdatePolicy.h`
- Create: `test/host/test_firmware_update_policy.cpp`
- Create: `scripts/package_release.sh`
- Create: `scripts/run_browser_checks.sh`
- Create: `scripts/run_hil_acceptance.sh`
- Create: `docs/release-checklist.md`
- Create: `docs/compatibility.md`
- Create: `docs/reviews/hil/README.md`
- Modify: `.github/workflows/quality.yml`
- Modify: `BP_checker.ino`

**Step 1: RED, authorize update metadata**

Add host tests for wrong target, downgrade/replay sequence, malformed manifest, wrong hash, wrong signing key/signature, interrupted write, unconfirmed boot, successful health confirmation, every anti-rollback storage cut, and factory/decommission reset followed by replay. The signed metadata binds artifact SHA-256, semantic version, board target, source SHA, minimum sequence, and size.

**Step 2: GREEN, signed update and rollback**

Embed only the release public trust anchor. Stream a parser-bounded, admin-authorized update to the inactive OTA partition while hashing; verify the signed manifest and artifact before selecting the partition. Store the highest accepted sequence in eFuse secure version or crash-consistent protected monotonic storage that credential/factory/decommission reset cannot reduce. Enable pending-verify boot, confirm only after storage/USB/Web self-checks, and let the bootloader roll back otherwise. Never store a signing private key in firmware or CI logs.

**Step 3: Package a reproducible release**

`scripts/package_release.sh` runs the quality gate, generates SBOM/build metadata, hashes/signs through an injected external signing command, and produces a version/SHA-named bundle. CI creates an unsigned candidate; an authorized release job signs it without duplicating build steps. Complete `docs/release-checklist.md` with approvals and artifact hashes.

**Step 4: Dynamic browser and role verification**

Run the host Web fixture in the supported kiosk browser. Test every route as anonymous/staff/admin, foreign/missing Host, refresh after ring wrap, stale/disconnected banners, keyboard focus, responsive tables, and automated accessibility checks. Retain browser/version/screenshots and machine-readable output.

**Step 5: HBP-9030 HIL and soak**

On the reference board, capture a de-identified official corpus for normal, boundary, error, movement, fragmented, burst, disconnect, and reconnect cases. Verify AP RF shutdown/recovery, old credentials, SDK erase, signed update/failed-update rollback, and exact USB2/function-32 configuration. Then run a 24-hour transport/persistence soak recording heap, stack watermark, watchdog/reset reason, loss/reconnect counters, record order/checksum, and throughput. The script must refuse PASS without connected target identifiers and retained logs.

**Step 6: Commit**

```bash
git add lib test/host scripts docs .github BP_checker.ino
git commit -m "release: verify signed clinic gateway artifacts"
```

### Task 10: Iterate the Strict Score to 95+

**Files:**
- Create per iteration: `docs/reviews/2026-07-11-iteration-N.md`
- Modify: `docs/reviews/EVIDENCE_MANIFEST.md`
- Modify: `docs/reviews/TDD_EVIDENCE.md`
- Modify: `docs/CHANGELOG.md`

**Step 1: Run the full gate**

```bash
bash scripts/run_quality_gate.sh
git diff --check origin/main...HEAD
```

**Step 2: Independent strict score**

A reviewer other than the implementer verifies every atomic scorecard ID against `EVIDENCE_MANIFEST.md` and checks all hard caps. No points for unavailable hardware/browser evidence.

**Step 3: Continue while below 95**

For each deduction, add a failing test or strongest repeatable verification, implement the narrowest complete fix, run the gate, and rescore. Do not lower weights or reinterpret criteria to pass.

**Step 4: Final review**

Run specification compliance, security bypass review, full code-quality review, clean-checkout build, and completion audit. Resolve every critical/important item and rescore.

**Step 5: Commit the passing evidence**

```bash
git add docs/reviews docs/CHANGELOG.md
git commit -m "docs: record clinic-readiness quality score"
```

The task is complete only when the independent current score is at least 95, no hard cap applies, the full gate passes, and every SDD invariant has authoritative evidence or an explicit scored deduction.
