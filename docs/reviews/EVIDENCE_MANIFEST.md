# Release Evidence Manifest

## Iteration 1 Environment

- Audited clean-gate revision: `39ca2ea2355f860a992b83a1a39adadcbd09c5c7`
- Date/reviewer: 2026-07-11, independent strict scoring review
- Canonical command: `bash scripts/run_quality_gate.sh` — PASS
- Toolchain: Arduino CLI 1.4.1, Arduino-ESP32 3.3.7, ArduinoJson 7.4.2
- Target build: 1,157,032 bytes flash (88%); 73,540 bytes globals (22%)
- Artifact: 1,157,184 bytes; SHA-256
  `90b95d34b6bf1e42ddac86254ecd63057ec95a6119de25931ee1d4f53c750d39`
- Hardware/browser: none attested. HBP-9030 HIL, supported-browser dynamic
  run, and 24-hour soak receive zero.

Every automated row below uses the canonical command above on the audited
revision. That command runs the named host/static/stress/TSan/build/SBOM
artifact and fails on any stage or project/compiler warning.

| ID | SHA | Artifact/test and exact verification | Result | Reviewer/date |
|---|---|---|---|---|
| A01 | `39ca2ea` | Canonical gate; `test_hbp9030_protocol`, named target config | PASS | Product / 2026-07-11 |
| A02 | `39ca2ea` | Canonical gate; format-5 production allowlist tests | PASS | Product / 2026-07-11 |
| A03 | `39ca2ea` | Canonical gate; protocol framer split-at-every-byte cases | PASS | Firmware / 2026-07-11 |
| A04 | `39ca2ea` | Canonical gate; ordered burst/multi-frame tests | PASS | Firmware / 2026-07-11 |
| A05 | `39ca2ea` | Canonical gate; partial line/no-idle-completion tests | PASS | Firmware / 2026-07-11 |
| A06 | `39ca2ea` | Canonical gate; overflow rejection/no persistence | PASS | Firmware / 2026-07-11 |
| A07 | `39ca2ea` | Canonical gate; trusted discontinuity boundary tests | PASS | Firmware / 2026-07-11 |
| A08 | `39ca2ea` | Canonical gate; exact 53-byte payload offsets | PASS | Firmware / 2026-07-11 |
| A09 | `39ca2ea` | Canonical gate; field count/CRLF vectors | PASS | Firmware / 2026-07-11 |
| A10 | `39ca2ea` | Canonical gate; fixed-width ASCII digit cases | PASS | Firmware / 2026-07-11 |
| A11 | `39ca2ea` | Canonical gate; NUL/sign/space/trailing rejection | PASS | Firmware / 2026-07-11 |
| A12 | `39ca2ea` | Canonical gate; calendar/device-time validation | PASS | Product / 2026-07-11 |
| A13 | `39ca2ea` | Canonical gate; transient-ID/API-type/static leak checks | PASS | Security / 2026-07-11 |
| A14 | `39ca2ea` | Canonical gate; distinct device-error semantics | PASS | Product / 2026-07-11 |
| A15 | `39ca2ea` | Canonical gate; movement metadata round-trip | PASS | Product / 2026-07-11 |
| A16 | `39ca2ea` | Canonical gate; inclusive SYS/DIA/pulse boundaries | PASS | Product / 2026-07-11 |
| A17 | `39ca2ea` | Canonical gate; space-filled error-vital rejection | PASS | Product / 2026-07-11 |
| A18 | `39ca2ea` | Canonical gate; unknown/custom model denial | PASS | Product / 2026-07-11 |
| B01 | `39ca2ea` | `rg -n "資產與威脅模型|安全邊界" docs/security.md` | PASS | Security / 2026-07-11 |
| B02 | `39ca2ea` | Canonical gate; ID sink/type/leak inventory assertions | PASS | Security / 2026-07-11 |
| B03 | `39ca2ea` | Canonical gate; entropy/AP secret vectors | PASS | Security / 2026-07-11 |
| B04 | `39ca2ea` | Canonical gate; independent secret/failure tests | PASS | Security / 2026-07-11 |
| B05 | `39ca2ea` | Canonical gate; claim bundle/cut-point matrix | PASS | Security / 2026-07-11 |
| B06 | `39ca2ea` | Canonical gate; all read-route role matrix | PASS | Security / 2026-07-11 |
| B07 | `39ca2ea` | Canonical gate; all mutation/update admin matrix | PASS | Security / 2026-07-11 |
| B08 | `39ca2ea` | Canonical gate; staff/admin surface and direct denial | PASS | Security / 2026-07-11 |
| B09 | `39ca2ea` | Canonical gate; every route/404 Host matrix | PASS | Security / 2026-07-11 |
| B10 | `39ca2ea` | Canonical gate; method/origin/referer CSRF matrix | PASS | Security / 2026-07-11 |
| B11 | `39ca2ea` | Canonical gate; bounded request/transaction/server contracts | PASS | Security / 2026-07-11 |
| B12 | `39ca2ea` | Canonical gate; AP shutdown lifecycle tests | PASS | Security / 2026-07-11 |
| B13 | `39ca2ea` | Canonical gate; physical recovery/expiry tests | PASS | Security / 2026-07-11 |
| B14 | `39ca2ea` | Canonical gate; RAM/HTML/API/CSV/NVS ID absence | PASS | Security / 2026-07-11 |
| B15 | `39ca2ea` | Canonical gate; production log/encoding leak probes | PASS | Security / 2026-07-11 |
| B16 | `39ca2ea` | `rg -n "HTTP 與網路殘餘風險|保存與清除" docs/security.md` | PASS | Security / 2026-07-11 |
| B17 | `39ca2ea` | Canonical gate; signed manifest/stream/runtime contracts | PASS | Security / 2026-07-11 |
| B18 | `39ca2ea` | `bash test/tooling/test_vulnerability_response.sh`; pinned register, 31-day review, severity classification, and owner/decision/deadline completeness | PASS | Security / 2026-07-11 |
| C01 | `39ca2ea` | Canonical gate; no idle completion/wait contract | PASS | Firmware / 2026-07-11 |
| C02 | `39ca2ea` | Canonical gate; asynchronous scan lifecycle | PASS | Firmware / 2026-07-11 |
| C04 | `39ca2ea` | Canonical gate; USB callback POD ownership check | PASS | Firmware / 2026-07-11 |
| C05 | `39ca2ea` | Canonical gate; ordered-channel stress/TSan | PASS | Firmware / 2026-07-11 |
| C06 | `39ca2ea` | Canonical gate; handle/context ownership stress | PASS | Firmware / 2026-07-11 |
| C07 | `39ca2ea` | Canonical gate; stress suite and TSan | PASS | Firmware / 2026-07-11 |
| C08 | `39ca2ea` | Canonical gate; frame/queue/request hard bounds | PASS | Firmware / 2026-07-11 |
| C09 | `39ca2ea` | Canonical gate; terminal epoch/barrier stress | PASS | Firmware / 2026-07-11 |
| C10 | `39ca2ea` | Canonical gate; loss/drop/reconnect counter tests | PASS | Firmware / 2026-07-11 |
| C11 | `39ca2ea` | Canonical gate; transport status/UI contract | PASS | Product / 2026-07-11 |
| C12 | `39ca2ea` | Canonical gate; bounded open retry tests | PASS | Firmware / 2026-07-11 |
| C13 | `39ca2ea` | Canonical gate; host install/config/disconnect/power/network/NTP fault reducers | PASS | Firmware / 2026-07-11 |
| D01 | `39ca2ea` | Canonical gate; every persistence cut point | PASS | Firmware / 2026-07-11 |
| D02 | `39ca2ea` | Canonical gate; slot-derived startup reconstruction | PASS | Firmware / 2026-07-11 |
| D03 | `39ca2ea` | Canonical gate; CRC/version/truncation/bit-flip tests | PASS | Firmware / 2026-07-11 |
| D04 | `39ca2ea` | Canonical gate; legacy migration values/order | PASS | Firmware / 2026-07-11 |
| D05 | `39ca2ea` | Canonical gate; migration cut-point idempotence | PASS | Firmware / 2026-07-11 |
| D06 | `39ca2ea` | Canonical gate; retention/wrap/overwrite tests | PASS | Firmware / 2026-07-11 |
| D07 | `39ca2ea` | Canonical gate; uint64/device-time reboot round-trip | PASS | Firmware / 2026-07-11 |
| D08 | `39ca2ea` | Canonical gate; current/legacy clear tests | PASS | Firmware / 2026-07-11 |
| D09 | `39ca2ea` | Canonical gate; tombstone/failed-write matrix | PASS | Firmware / 2026-07-11 |
| D10 | `39ca2ea` | Canonical gate; CSV and bounded-wear contract | PASS | Firmware / 2026-07-11 |
| E01 | `39ca2ea` | Canonical gate; staff dashboard/history/export surfaces | PASS | Product / 2026-07-11 |
| E02 | `39ca2ea` | Canonical gate; admin controls/opaque handoff | PASS | Product / 2026-07-11 |
| E03 | `39ca2ea` | Canonical gate; diagnostic wording prohibition | PASS | Clinical / 2026-07-11 |
| E04 | `39ca2ea` | Canonical gate; named/versioned persisted policy | PASS | Clinical / 2026-07-11 |
| E05 | `39ca2ea` | Canonical gate; repeat/no-averaging copy | PASS | Clinical / 2026-07-11 |
| E06 | `39ca2ea` | Canonical gate; actionable error copy | PASS | Product / 2026-07-11 |
| E07 | `39ca2ea` | Canonical gate; textual movement/quality state | PASS | Product / 2026-07-11 |
| E08 | `39ca2ea` | Canonical gate; freshness/disconnect state policy | PASS | Product / 2026-07-11 |
| E09 | `39ca2ea` | Canonical gate; whole-surface revision reload | PASS | Product / 2026-07-11 |
| E10 | `39ca2ea` | Canonical gate; `scripts/check_ui_markup.sh` | PASS | Accessibility / 2026-07-11 |
| E12 | `39ca2ea` | Canonical gate; destructive confirmation/recovery copy | PASS | Product / 2026-07-11 |
| F01 | `39ca2ea` | Canonical gate; all deterministic host executables | PASS | QA / 2026-07-11 |
| F02 | `39ca2ea` | Canonical gate; parser/data/persistence/security boundaries | PASS | QA / 2026-07-11 |
| F05 | `39ca2ea` | Canonical gate; framing/loss failure injection | PASS | QA / 2026-07-11 |
| F06 | `39ca2ea` | Canonical gate; stress and TSan on every gate | PASS | QA / 2026-07-11 |
| F07 | `39ca2ea` | Canonical gate; power-cut/fuzz/corruption matrix | PASS | QA / 2026-07-11 |
| G01 | `39ca2ea` | Canonical gate from case-correct clean checkout | PASS | Release / 2026-07-11 |
| G02 | `39ca2ea` | Canonical gate; target link/warning/size evidence | PASS | Release / 2026-07-11 |
| G03 | `39ca2ea` | Canonical gate; pinned CLI/core/library and SBOM | PASS | Release / 2026-07-11 |
| G04 | `39ca2ea` | `rg -n "run_quality_gate.sh" .github/workflows/quality.yml` | PASS | Release / 2026-07-11 |
| G06 | `39ca2ea` | Canonical gate; signed OTA, pending health/rollback policy and strong hook | PASS | Release / 2026-07-11 |
| H01 | `39ca2ea` | `test -s docs/security.md && test -s docs/deployment.md` | PASS | Operations / 2026-07-11 |
| H02 | `39ca2ea` | `test -s docs/hardware.md` reference topology/power budget | PASS | Operations / 2026-07-11 |
| H03 | `39ca2ea` | `rg -n "USB2|Type-B|項目 32|format 5" docs/hardware.md docs/deployment.md` | PASS | Operations / 2026-07-11 |
| H04 | `39ca2ea` | `rg -n "WPA2|VLAN|HTTP|啟用" docs/deployment.md docs/security.md` | PASS | Operations / 2026-07-11 |
| H05 | `39ca2ea` | `test -s docs/deployment.md` normal/error/recovery runbook | PASS | Operations / 2026-07-11 |
| H06 | `39ca2ea` | `rg -n "保存|匯出|退役|隱私" docs/deployment.md docs/security.md` | PASS | Operations / 2026-07-11 |
| H08 | `39ca2ea` | `bash test/tooling/test_vulnerability_response.sh`; `docs/maintenance.md` update/rollback/key/SLA/support/EOL policy | PASS | Operations / 2026-07-11 |

## Unawarded Controls

- A20, F03, F04, F09: run the attested HBP-9030/reference-board corpus and HIL acceptance.
- A19: obtain official/observed HBP-9030 VID/PID, bind the accepted CDC interface,
  and persist explicit transport provenance; current code uses ANY VID/PID.
- C03: define an end-to-end loop latency budget and retain target timing evidence
  for clock, authentication, and worst-case bounded requests.
- C14, C15, F10: complete and attest the 24-hour transport/persistence soak.
- E11, F08: run the pinned kiosk browser route/accessibility harness.
- G05: prove bit-identical output from two isolated builds with the reviewed key.
- G07: complete actual approvals and configure approved non-empty trust pins.
- H07: publish an attested tested board/monitor/browser matrix.

## Score

`A 18 + B 18 + C 12 + D 10 + E 11 + F 5 + G 5 + H 7 = 86/100`.

Effective score is **86/100 — RELEASE BLOCKED**. The no-HIL/no-soak and open
A19 provenance P1 caps are 89, but the raw score is lower. Mandatory F09 and
F10 are absent; Measurement, Reliability, Test, and Build floors are not met.
