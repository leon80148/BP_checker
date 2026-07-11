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
- `raw-logs.sha256`, `evidence-manifest.txt`, and
  `evidence-attestation.sig.der`: retained raw-log index, canonical summary
  bindings, and the trusted harness signature.

Validate retained evidence with:

```bash
BP_HIL_BOARD_ID=... BP_HIL_MONITOR_ID=... \
BP_HIL_LOG_DIR=/secure/evidence/run-... BP_HIL_SOAK_HOURS=24 \
BP_HIL_RUN_ID=... BP_HIL_RELEASE_BUNDLE=build/release/<signed-bundle> \
BP_HIL_EVIDENCE_PUBLIC_KEY=/secure/approved-harness-public.pem \
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

The HIL harness signing key must be separated from the person preparing JSON
summaries. Review its public DER SHA-256 into
`config/evidence-trust-anchors.json`; an empty or mismatched anchor fails. This
attestation does not replace independent review of raw logs and hardware IDs.
The canonical manifest also binds a unique run ID, current source SHA, signed
firmware SHA-256, release sequence, both hardware IDs, and every summary/raw-log
index hash, so evidence from an older firmware cannot be replayed.

The soak summary must include typed duration, heap start/end/minimum and slope,
minimum stack watermark, reset reasons and unexpected-reset count, measurement
throughput, data-loss/reconnect counters, plus record order/checksum results.
