# Release Checklist

Use this checklist for every clinic firmware candidate. A software build alone is
not approval for deployment.

## Candidate

- Confirm a clean, reviewed commit and non-development semantic `VERSION`.
- Set canonical unsigned integers `BP_RELEASE_SEQUENCE` and
  `BP_RELEASE_MINIMUM_SEQUENCE`. The sequence must exceed the installed fleet
  maximum; credential, factory, and decommission resets must not lower it.
- Set `BP_RELEASE_PUBLIC_KEY_DER_HEX` to the approved P-256 public SPKI DER hex.
  Never place the private key in this repository, firmware, workflow, or logs.
- Run `scripts/package_release.sh --candidate`. Record the bundle path, source
  SHA, SBOM, firmware SHA-256, size, quality-gate approval, and emitted
  `candidate_checksums_sha256`. Transfer that digest to the signing approver by
  a separate authenticated channel.

## Signing and Verification

- Have the authorized HSM/offline signer implement
  `signer MANIFEST_PATH SIGNATURE_OUTPUT_PATH`, then set its executable path as
  `BP_RELEASE_SIGN_COMMAND`. Set the separately approved checksum digest as
  `BP_RELEASE_CANDIDATE_SHA256`.
- Run `scripts/package_release.sh --sign-candidate build/release/<bundle>`.
  The command verifies the DER signature against the embedded public anchor and
  refreshes `checksums.sha256`.
- A second person verifies all checksums, target `esp32:esp32:esp32s3`, source
  SHA, version, sequence, minimum sequence, artifact byte count, and SHA-256.

## Deployment Approval

- Attach passing supported-browser evidence and the current reference-board /
  HBP-9030 HIL record, including failed-update rollback and a 24-hour soak.
- Browser automation must use a new evidence directory and one unique
  `BP_BROWSER_RUN_ID`; its runner receives browser path, base URL, output path,
  run ID, and source SHA. Both JSON reports must bind those run values. A
  separately controlled harness signs the canonical evidence manifest; its
  approved public DER hash must be reviewed into
  `config/evidence-trust-anchors.json` before evidence can pass.
- Record clinical owner, security reviewer, release approver, date, clinic
  cohort, rollback image, maintenance window, and EOL/support date.
- Reject the release if the trust anchor is empty, evidence is missing, the
  worktree is dirty, or any signed value differs from the candidate bundle.
