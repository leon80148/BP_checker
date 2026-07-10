# Release Evidence Manifest

## Iteration 0 Environment

- Revision: `e63d9a08b2636b713a1fe138006d2f1fa886d8df`
- Date/reviewer: 2026-07-11, independent product/scoring review reconciled by root
- Toolchain: Arduino CLI 1.4.1, `esp32:esp32` 3.3.7, ArduinoJson 7.4.2
- Hardware: none attached; HIL and soak controls receive zero
- Host command: `bash scripts/run_host_tests.sh` — PASS, 175 checks (parser 48, CSV 6, data 34, record 44, security 43)
- UI command: `bash scripts/check_ui_markup.sh` — PASS
- Canonical build: `arduino-cli compile -b esp32:esp32:esp32s3 --board-options USBMode=default .` — FAIL, missing `BP_checker.ino`
- Normalized-name probe: `tmp=$(mktemp -d) && mkdir "$tmp/BP_checker" && rsync -a --exclude .git --exclude .worktrees ./ "$tmp/BP_checker/" && mv "$tmp/BP_checker/bp_checker.ino" "$tmp/BP_checker/BP_checker.ino" && arduino-cli compile -b esp32:esp32:esp32s3 --board-options USBMode=default "$tmp/BP_checker"; rc=$?; rm -rf "$tmp"; exit $rc` — PASS, flash 1,078,388/1,310,720 bytes (82%), globals 48,540/327,680 bytes (14%)

## Awarded One-Point Controls

Every row is one point. IDs absent from this table receive zero.

| ID | SHA | Artifact and exact verification | Result | Reviewer/date |
|---|---|---|---|---|
| A01 | `e63d9a0` | `bash scripts/run_host_tests.sh`; `test/host/test_bp_parser.cpp` HBP-9030 dispatch | PASS; named HBP-9030 path executes | Product / 2026-07-11 |
| A04 | `e63d9a0` | `bash scripts/run_host_tests.sh`; `testTwoFramesInOneBurst` | PASS; two ordered records | Product / 2026-07-11 |
| A06 | `e63d9a0` | `bash scripts/run_host_tests.sh`; `testOverflowDropped` | PASS; overflowed frame is not persisted | Product / 2026-07-11 |
| B10 | `e63d9a0` | `bash scripts/run_host_tests.sh`; `test/host/test_web_security.cpp` plus mutation-route audit | PASS; existing mutations exercise CSRF/Host policy | Security / 2026-07-11 |
| C02 | `e63d9a0` | `rg -n "scanNetworks\\(true\\)|scanComplete\\(\\)|scanDelete\\(\\)" lib/WebHandler.h` | Async scan start/collect/delete path present | Firmware / 2026-07-11 |
| C08 | `e63d9a0` | `bash scripts/run_host_tests.sh`; `DataProcessor::kFrameBufferSize` and overflow test | PASS; bounded frame rejects overflow | Firmware / 2026-07-11 |
| C11 | `e63d9a0` | `bash scripts/run_host_tests.sh && bash scripts/check_ui_markup.sh`; transport status tests/tokens | PASS; transport state/detail is surfaced | Firmware / 2026-07-11 |
| C12 | `e63d9a0` | `rg -n "lastOpenAttemptMs|openInProgress|cdc_acm_host_open" src/transports/UsbCdcTransport.cpp` plus normalized-name target compile | Bounded open-attempt path compiles; install retry remains unproved | Firmware / 2026-07-11 |
| D04 | `e63d9a0` | `bash scripts/run_host_tests.sh`; `testLegacyMigration` | PASS; legacy values/order migrate to v2 | Firmware / 2026-07-11 |
| D06 | `e63d9a0` | `bash scripts/run_host_tests.sh`; `testRingBuffer` and wrap reload | PASS; 20-slot policy behavior is executable | Firmware / 2026-07-11 |
| D08 | `e63d9a0` | `bash scripts/run_host_tests.sh`; `testClear` | PASS; clean clear removes v2/legacy keys | Firmware / 2026-07-11 |
| D10 | `e63d9a0` | `bash scripts/run_host_tests.sh`; `test_csv_export.cpp` and bounded-ring write rationale in `BPRecordManager.h` | PASS; CSV order/escaping and bounded writes covered | Firmware / 2026-07-11 |
| E01 | `e63d9a0` | `bash scripts/check_ui_markup.sh`; dashboard/history/export tokens | PASS; staff-oriented measurement surface exists | Product / 2026-07-11 |
| E12 | `e63d9a0` | `rg -n "confirm\\('.*(重置|清除)" lib/WebHandler.h` | Reset and clear require confirmation | Product / 2026-07-11 |
| F01 | `e63d9a0` | `bash scripts/run_host_tests.sh` | PASS; 175 deterministic host checks | Firmware / 2026-07-11 |
| F02 | `e63d9a0` | `bash scripts/run_host_tests.sh` | PASS; parser/CSV/data/persistence/security-policy modules execute | Firmware / 2026-07-11 |
| F05 | `e63d9a0` | `bash scripts/run_host_tests.sh`; split/burst/stale/overflow fake-transport tests | PASS; framing/loss injection executes | Firmware / 2026-07-11 |
| G02 | `e63d9a0` | Exact normalized-name probe command in the environment section | PASS; flash 82%, globals 14% | Product / 2026-07-11 |
| H05 | `e63d9a0` | `rg -n "故障排除|Troubleshooting|reset|重置" README.md docs` | Basic troubleshooting/recovery guidance present | Product / 2026-07-11 |

## Recalculation

`A 3 + B 1 + C 4 + D 4 + E 2 + F 3 + G 1 + H 1 = 19/100`.

The manifest records baseline credit only. It does not waive the P1 findings or the canonical-build, privacy, HIL, and soak hard caps.
