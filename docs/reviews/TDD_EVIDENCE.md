# TDD Evidence Log

This log records observable RED and GREEN evidence for each production behavior change. A test that first appears after implementation does not count as TDD and earns no scorecard discipline credit.

| Task | Behavior | RED command and observed failure | GREEN command and observed result | Commit |
|---|---|---|---|---|
| Task 2 | Case-sensitive canonical firmware build | `arduino-cli compile -b esp32:esp32:esp32s3 --board-options USBMode=default .` (2026-07-11) -> exit 1, `missing .../BP_checker.ino` | Pending Task 2 GREEN | Pending |
