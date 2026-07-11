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
  SHA, SBOM, firmware SHA-256, size, and quality-gate approval.

## Signing and Verification

- Have the authorized HSM/offline signer implement
  `signer MANIFEST_PATH SIGNATURE_OUTPUT_PATH`, then set its executable path as
  `BP_RELEASE_SIGN_COMMAND`.
- Run `scripts/package_release.sh --sign-candidate build/release/<bundle>`.
  The command verifies the DER signature against the embedded public anchor and
  refreshes `checksums.sha256`.
- A second person verifies all checksums, target `esp32:esp32:esp32s3`, source
  SHA, version, sequence, minimum sequence, artifact byte count, and SHA-256.

## Deployment Approval

- Attach passing supported-browser evidence and the current reference-board /
  HBP-9030 HIL record, including failed-update rollback and a 24-hour soak.
- Record clinical owner, security reviewer, release approver, date, clinic
  cohort, rollback image, maintenance window, and EOL/support date.
- Reject the release if the trust anchor is empty, evidence is missing, the
  worktree is dirty, or any signed value differs from the candidate bundle.
