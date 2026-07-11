# Release Evidence Manifest

## Iteration 1 Environment

- Primary gate revision: `04a2830739dd12c926acc5134b63d5546425c673`
- Vulnerability/maintenance policy supplement: `79c2416`
- Date/reviewer: 2026-07-11, independent strict scoring review
- Canonical command: `bash scripts/run_quality_gate.sh` — PASS
- Toolchain: Arduino CLI 1.4.1, Arduino-ESP32 3.3.7, ArduinoJson 7.4.2
- Target build: 1,157,032 bytes flash (88%); 73,540 bytes globals (22%)
- Artifact: 1,157,184 bytes; SHA-256
  `889cc90aec0e820d6c8a816151c4e8c5c33ca7576fd2ae0de92d12f229038c2a`
- Hardware/browser: none attested. HBP-9030 HIL, supported-browser dynamic
  run, and 24-hour soak receive zero.

Every automated row below uses the canonical command above on the audited
revision. That command runs the named host/static/stress/TSan/build/SBOM
artifact and fails on any stage or project/compiler warning.

| ID | SHA | Artifact/test and exact verification | Result | Reviewer/date |
|---|---|---|---|---|
| A01 | `04a2830` | Canonical gate; `test_hbp9030_protocol`, named target config | PASS | Product / 2026-07-11 |
| A02 | `04a2830` | Canonical gate; format-5 production allowlist tests | PASS | Product / 2026-07-11 |
| A03 | `04a2830` | Canonical gate; protocol framer split-at-every-byte cases | PASS | Firmware / 2026-07-11 |
| A04 | `04a2830` | Canonical gate; ordered burst/multi-frame tests | PASS | Firmware / 2026-07-11 |
| A05 | `04a2830` | Canonical gate; partial line/no-idle-completion tests | PASS | Firmware / 2026-07-11 |
| A06 | `04a2830` | Canonical gate; overflow rejection/no persistence | PASS | Firmware / 2026-07-11 |
| A07 | `04a2830` | Canonical gate; trusted discontinuity boundary tests | PASS | Firmware / 2026-07-11 |
| A08 | `04a2830` | Canonical gate; exact 53-byte payload offsets | PASS | Firmware / 2026-07-11 |
| A09 | `04a2830` | Canonical gate; field count/CRLF vectors | PASS | Firmware / 2026-07-11 |
| A10 | `04a2830` | Canonical gate; fixed-width ASCII digit cases | PASS | Firmware / 2026-07-11 |
| A11 | `04a2830` | Canonical gate; NUL/sign/space/trailing rejection | PASS | Firmware / 2026-07-11 |
| A12 | `04a2830` | Canonical gate; calendar/device-time validation | PASS | Product / 2026-07-11 |
| A13 | `04a2830` | Canonical gate; transient-ID/API-type/static leak checks | PASS | Security / 2026-07-11 |
| A14 | `04a2830` | Canonical gate; distinct device-error semantics | PASS | Product / 2026-07-11 |
| A15 | `04a2830` | Canonical gate; movement metadata round-trip | PASS | Product / 2026-07-11 |
| A16 | `04a2830` | Canonical gate; inclusive SYS/DIA/pulse boundaries | PASS | Product / 2026-07-11 |
| A17 | `04a2830` | Canonical gate; space-filled error-vital rejection | PASS | Product / 2026-07-11 |
| A18 | `04a2830` | Canonical gate; unknown/custom model denial | PASS | Product / 2026-07-11 |
| B01 | `04a2830` | `rg -n "資產與威脅模型|安全邊界" docs/security.md` | PASS | Security / 2026-07-11 |
| B02 | `04a2830` | Canonical gate; ID sink/type/leak inventory assertions | PASS | Security / 2026-07-11 |
| B03 | `04a2830` | Canonical gate; entropy/AP secret vectors | PASS | Security / 2026-07-11 |
| B04 | `04a2830` | Canonical gate; independent secret/failure tests | PASS | Security / 2026-07-11 |
| B05 | `04a2830` | Canonical gate; claim bundle/cut-point matrix | PASS | Security / 2026-07-11 |
| B06 | `04a2830` | Canonical gate; all read-route role matrix | PASS | Security / 2026-07-11 |
| B07 | `04a2830` | Canonical gate; all mutation/update admin matrix | PASS | Security / 2026-07-11 |
| B08 | `04a2830` | Canonical gate; staff/admin surface and direct denial | PASS | Security / 2026-07-11 |
| B09 | `04a2830` | Canonical gate; every route/404 Host matrix | PASS | Security / 2026-07-11 |
| B10 | `04a2830` | Canonical gate; method/origin/referer CSRF matrix | PASS | Security / 2026-07-11 |
| B11 | `04a2830` | Canonical gate; bounded request/transaction/server contracts | PASS | Security / 2026-07-11 |
| B12 | `04a2830` | Canonical gate; AP shutdown lifecycle tests | PASS | Security / 2026-07-11 |
| B13 | `04a2830` | Canonical gate; physical recovery/expiry tests | PASS | Security / 2026-07-11 |
| B14 | `04a2830` | Canonical gate; RAM/HTML/API/CSV/NVS ID absence | PASS | Security / 2026-07-11 |
| B15 | `04a2830` | Canonical gate; production log/encoding leak probes | PASS | Security / 2026-07-11 |
| B16 | `04a2830` | `rg -n "HTTP 與網路殘餘風險|保存與清除" docs/security.md` | PASS | Security / 2026-07-11 |
| B17 | `04a2830` | Canonical gate; signed manifest/stream/runtime contracts | PASS | Security / 2026-07-11 |
| B18 | `79c2416` | `bash test/tooling/test_vulnerability_response.sh`; pinned register, 31-day review, severity classification, and owner/decision/deadline completeness | PASS | Security / 2026-07-11 |
| C01 | `04a2830` | Canonical gate; no idle completion/wait contract | PASS | Firmware / 2026-07-11 |
| C02 | `04a2830` | Canonical gate; asynchronous scan lifecycle | PASS | Firmware / 2026-07-11 |
| C04 | `04a2830` | Canonical gate; USB callback POD ownership check | PASS | Firmware / 2026-07-11 |
| C05 | `04a2830` | Canonical gate; ordered-channel stress/TSan | PASS | Firmware / 2026-07-11 |
| C06 | `04a2830` | Canonical gate; handle/context ownership stress | PASS | Firmware / 2026-07-11 |
| C07 | `04a2830` | Canonical gate; stress suite and TSan | PASS | Firmware / 2026-07-11 |
| C08 | `04a2830` | Canonical gate; frame/queue/request hard bounds | PASS | Firmware / 2026-07-11 |
| C09 | `04a2830` | Canonical gate; terminal epoch/barrier stress | PASS | Firmware / 2026-07-11 |
| C10 | `04a2830` | Canonical gate; loss/drop/reconnect counter tests | PASS | Firmware / 2026-07-11 |
| C11 | `04a2830` | Canonical gate; transport status/UI contract | PASS | Product / 2026-07-11 |
| C12 | `04a2830` | Canonical gate; bounded open retry tests | PASS | Firmware / 2026-07-11 |
| C13 | `04a2830` | Canonical gate; host install/config/disconnect/power/network/NTP fault reducers | PASS | Firmware / 2026-07-11 |
| D01 | `04a2830` | Canonical gate; every persistence cut point | PASS | Firmware / 2026-07-11 |
| D02 | `04a2830` | Canonical gate; slot-derived startup reconstruction | PASS | Firmware / 2026-07-11 |
| D03 | `04a2830` | Canonical gate; CRC/version/truncation/bit-flip tests | PASS | Firmware / 2026-07-11 |
| D04 | `04a2830` | Canonical gate; legacy migration values/order | PASS | Firmware / 2026-07-11 |
| D05 | `04a2830` | Canonical gate; migration cut-point idempotence | PASS | Firmware / 2026-07-11 |
| D06 | `04a2830` | Canonical gate; retention/wrap/overwrite tests | PASS | Firmware / 2026-07-11 |
| D07 | `04a2830` | Canonical gate; uint64/device-time reboot round-trip | PASS | Firmware / 2026-07-11 |
| D08 | `04a2830` | Canonical gate; current/legacy clear tests | PASS | Firmware / 2026-07-11 |
| D09 | `04a2830` | Canonical gate; tombstone/failed-write matrix | PASS | Firmware / 2026-07-11 |
| D10 | `04a2830` | Canonical gate; CSV and bounded-wear contract | PASS | Firmware / 2026-07-11 |
| E01 | `04a2830` | Canonical gate; staff dashboard/history/export surfaces | PASS | Product / 2026-07-11 |
| E02 | `04a2830` | Canonical gate; admin controls/opaque handoff | PASS | Product / 2026-07-11 |
| E03 | `04a2830` | Canonical gate; diagnostic wording prohibition | PASS | Clinical / 2026-07-11 |
| E04 | `04a2830` | Canonical gate; named/versioned persisted policy | PASS | Clinical / 2026-07-11 |
| E05 | `04a2830` | Canonical gate; repeat/no-averaging copy | PASS | Clinical / 2026-07-11 |
| E06 | `04a2830` | Canonical gate; actionable error copy | PASS | Product / 2026-07-11 |
| E07 | `04a2830` | Canonical gate; textual movement/quality state | PASS | Product / 2026-07-11 |
| E08 | `04a2830` | Canonical gate; freshness/disconnect state policy | PASS | Product / 2026-07-11 |
| E09 | `04a2830` | Canonical gate; whole-surface revision reload | PASS | Product / 2026-07-11 |
| E10 | `04a2830` | Canonical gate; `scripts/check_ui_markup.sh` | PASS | Accessibility / 2026-07-11 |
| E12 | `04a2830` | Canonical gate; destructive confirmation/recovery copy | PASS | Product / 2026-07-11 |
| F01 | `04a2830` | Canonical gate; all deterministic host executables | PASS | QA / 2026-07-11 |
| F02 | `04a2830` | Canonical gate; parser/data/persistence/security boundaries | PASS | QA / 2026-07-11 |
| F05 | `04a2830` | Canonical gate; framing/loss failure injection | PASS | QA / 2026-07-11 |
| F06 | `04a2830` | Canonical gate; stress and TSan on every gate | PASS | QA / 2026-07-11 |
| F07 | `04a2830` | Canonical gate; power-cut/fuzz/corruption matrix | PASS | QA / 2026-07-11 |
| G01 | `04a2830` | Canonical gate from case-correct clean checkout | PASS | Release / 2026-07-11 |
| G02 | `04a2830` | Canonical gate; target link/warning/size evidence | PASS | Release / 2026-07-11 |
| G03 | `04a2830` | Canonical gate; pinned CLI/core/library and SBOM | PASS | Release / 2026-07-11 |
| G04 | `04a2830` | `rg -n "run_quality_gate.sh" .github/workflows/quality.yml` | PASS | Release / 2026-07-11 |
| G06 | `04a2830` | Canonical gate; signed OTA, pending health/rollback policy and strong hook | PASS | Release / 2026-07-11 |
| H01 | `04a2830` | `test -s docs/security.md && test -s docs/deployment.md` | PASS | Operations / 2026-07-11 |
| H02 | `04a2830` | `test -s docs/hardware.md` reference topology/power budget | PASS | Operations / 2026-07-11 |
| H03 | `04a2830` | `rg -n "USB2|Type-B|項目 32|format 5" docs/hardware.md docs/deployment.md` | PASS | Operations / 2026-07-11 |
| H04 | `04a2830` | `rg -n "WPA2|VLAN|HTTP|啟用" docs/deployment.md docs/security.md` | PASS | Operations / 2026-07-11 |
| H05 | `04a2830` | `test -s docs/deployment.md` normal/error/recovery runbook | PASS | Operations / 2026-07-11 |
| H06 | `04a2830` | `rg -n "保存|匯出|退役|隱私" docs/deployment.md docs/security.md` | PASS | Operations / 2026-07-11 |
| H08 | `79c2416` | `bash test/tooling/test_vulnerability_response.sh`; `docs/maintenance.md` update/rollback/key/SLA/support/EOL policy | PASS | Operations / 2026-07-11 |

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
