#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

for file in \
  scripts/package_release.sh \
  scripts/run_browser_checks.sh \
  scripts/run_hil_acceptance.sh \
  docs/release-checklist.md \
  docs/compatibility.md \
  docs/reviews/hil/README.md \
  config/evidence-trust-anchors.json
do
  test -f "$file" || {
    echo "missing release evidence contract: $file" >&2
    exit 1
  }
done

for script in scripts/package_release.sh scripts/run_browser_checks.sh scripts/run_hil_acceptance.sh; do
  test -x "$script" || { echo "release script is not executable: $script" >&2; exit 1; }
  if "$script" > /dev/null 2>&1; then
    echo "release evidence script must fail closed without required inputs: $script" >&2
    exit 1
  fi
done

package=scripts/package_release.sh
for token in \
  '--candidate' '--sign-candidate' \
  'BP_RELEASE_SEQUENCE' 'BP_RELEASE_MINIMUM_SEQUENCE' \
  'BP_RELEASE_PUBLIC_KEY_DER_HEX' 'BP_RELEASE_SIGN_COMMAND' \
  'BP_RELEASE_CANDIDATE_SHA256' \
  'bash scripts/run_quality_gate.sh' \
  'schema=bp-update-v1' 'target=esp32:esp32:esp32s3' \
  'source_sha=' 'minimum_sequence=' 'sha256=' \
  'source_dirty' 'trust_anchor_sha256' 'checksums.sha256' \
  'candidate bundle must be under build/release' \
  'candidate checksum verification failed' \
  'candidate trust anchor binding failed' \
  'candidate manifest binding failed' \
  'candidate file set invalid'
do
  grep -Fq -- "$token" "$package" || {
    echo "missing package contract: $token" >&2
    exit 1
  }
done
grep -Fq 'trust_anchor_sha256' scripts/generate_sbom.sh || {
  echo "SBOM must bind the release trust anchor" >&2
  exit 1
}
if grep -Eq '(^|[[:space:]])eval([[:space:]]|$)' "$package"; then
  echo "release signer must not be invoked through eval" >&2
  exit 1
fi

for token in BP_BROWSER_EXECUTABLE BP_BROWSER_VERSION BP_BROWSER_EVIDENCE_DIR BP_BROWSER_RUN_ID BP_BROWSER_EVIDENCE_PUBLIC_KEY source_sha base_url routes.json accessibility.json screenshots evidence-manifest.txt evidence-attestation.sig.der; do
  grep -Fq -- "$token" scripts/run_browser_checks.sh || {
    echo "missing browser evidence contract: $token" >&2
    exit 1
  }
done

for token in BP_HIL_BOARD_ID BP_HIL_MONITOR_ID BP_HIL_LOG_DIR BP_HIL_SOAK_HOURS BP_HIL_EVIDENCE_PUBLIC_KEY 24 signed-update rollback soak-summary.json raw-logs.sha256 evidence-manifest.txt evidence-attestation.sig.der bp-hil-transport-v1 bp-hil-network-v1 old_credentials_rejected ap_shutdown_passed; do
  grep -Fq -- "$token" scripts/run_hil_acceptance.sh || {
    echo "missing HIL evidence contract: $token" >&2
    exit 1
  }
done

grep -Fq 'scripts/package_release.sh --candidate' .github/workflows/quality.yml
echo "Release evidence contract checks passed."
