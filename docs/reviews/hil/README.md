# HIL Evidence

Store each run under a dated, immutable directory outside normal source control;
retain only de-identified frames and identifiers for the test board/monitor.
Before claiming PASS, copy or securely archive these machine-readable files:

- `corpus-summary.json`: normal, boundary, error, movement, fragmentation,
  burst, disconnect, and reconnect cases.
- `transport-faults.json` and `network-security.json`: USB/power/network faults,
  AP shutdown/recovery, old-credential rejection, and erase behavior.
- `signed-update.json` and `rollback.json`: valid update, wrong signature,
  downgrade, failed health, rollback, and previous-image boot.
- `soak-summary.json`: at least 24 hours, heap/leak trend, stack watermark,
  reset/watchdog reason, throughput, loss/reconnect counters, and record order /
  checksum validation.
- `operator-approval.json`: named reviewer and timestamp.

Validate retained evidence with:

```bash
BP_HIL_BOARD_ID=... BP_HIL_MONITOR_ID=... \
BP_HIL_LOG_DIR=/secure/evidence/run-... BP_HIL_SOAK_HOURS=24 \
bash scripts/run_hil_acceptance.sh
```

The validator intentionally refuses PASS when hardware IDs, any required file,
the rollback drill, approval, or the complete soak is absent.

`transport-faults.json` uses schema `bp-hil-transport-v1` and must bind both
hardware IDs, USB2 Type-B/function-32 verification, power/disconnect/overflow
recovery, and an all-passing `faults` array. `network-security.json` uses
`bp-hil-network-v1` and must bind the board ID plus AP shutdown, physical
recovery, expiry, old-credential rejection, SDK erase, isolated VLAN, and the
documented HTTP residual-risk acceptance.
