#!/usr/bin/env bash
set -euo pipefail

: "${BP_TEST_SIGNING_KEY:?BP_TEST_SIGNING_KEY is required}"
[[ $# -eq 2 ]] || { echo "usage: release_signer_fixture MANIFEST SIGNATURE" >&2; exit 2; }
openssl dgst -sha256 -sign "$BP_TEST_SIGNING_KEY" -out "$2" "$1"
