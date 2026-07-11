# Maintenance, Vulnerability Response, and EOL

## Supported Baseline

Security support applies only to the pinned ESP32-S3/HBP-9030 format-5 product
baseline in `docs/compatibility.md`. The maintainer reviews Arduino CLI,
Arduino-ESP32, ArduinoJson, and the vendored USB CDC component at least every 31
days and before every release. Record the review and every advisory decision in
`config/vulnerability-register.json`; `scripts/check_vulnerability_response.sh`
rejects stale, incomplete, or unowned intake.

## Triage and Patch SLA

- Critical: acknowledge/triage within 24 hours; isolate affected deployments;
  provide a mitigation or signed patch within 7 days.
- High: triage within 3 days and patch within 14 days.
- Moderate/low: assess within 14 days and schedule in the next maintenance
  release, or record a reviewed not-affected decision.

Critical/high findings cannot be silently accepted. The register requires an
owner, decision, deadline, and patching/resolved/not-affected state. A release
approver checks the current SBOM and blocks promotion when the register or
quality gate fails.

## Release, Rollback, and Keys

Build and sign only through `docs/release-checklist.md`. Stage one clinic cohort
at a time, monitor operations counters, and retain the previous verified image.
If boot health, storage, USB, Web security, or clinical acceptance fails, stop
rollout and use the signed rollback image; never lower the monotonic sequence.
Suspected signing-key compromise blocks releases, rotates the reviewed public
anchor through a separately authorized recovery release, and triggers incident
review of every artifact signed by the old key.

## Maintenance Window and EOL

Publish the clinic cohort, expected outage, rollback owner, and support contact
before a maintenance window. Emergency security maintenance may shorten notice
but not bypass HIL, signature, or rollback controls.

Announce EOL at least 12 months in advance. The security-support window continues through
the published EOL date; the last supported release and export/decommission
instructions remain available for another 12 months. After EOL, block new
deployments, export required records, revoke network access, and follow the
controlled decommission process in `docs/security.md`.
