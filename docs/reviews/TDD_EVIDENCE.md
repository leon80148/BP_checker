# TDD Evidence Log

This log records observable RED and GREEN evidence for each production behavior change. A test that first appears after implementation does not count as TDD and earns no scorecard discipline credit.

| Task | Behavior | RED command and observed failure | GREEN command and observed result | Commit |
|---|---|---|---|---|
| Task 2 | Case-sensitive canonical firmware build | `arduino-cli compile -b esp32:esp32:esp32s3 --board-options USBMode=default .` (2026-07-11) -> exit 1, `missing .../BP_checker.ino` | `arduino-cli compile .` -> exit 0, flash 1,078,376 bytes (82%), globals 48,540 bytes (14%) | `3c02f84` |
| Task 2 | `VERSION` must not shadow C++ `<version>` on case-insensitive filesystems | First `bash scripts/run_quality_gate.sh` -> host compile read `./version:1` as a header and failed with 19 errors | `bash scripts/run_host_tests.sh` after using `-iquote .` -> 175 checks pass | `3c02f84` |
| Task 2 | Pinned warning-audited build and traceable SBOM | First full firmware gate -> project warnings for ArduinoJson v7 APIs, storage key truncation, and USB config initialization; malformed quoted flags also warned | `bash scripts/run_quality_gate.sh` -> exit 0, zero project warnings, firmware 1,078,544 bytes, clean source `3c02f84`, artifact SHA-256 `4c4dc4961a7caa04ff43e3b9f7926f7bc342a2481f51345d3637b24907278dab` | `3c02f84` |
