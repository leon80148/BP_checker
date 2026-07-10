#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SOURCE="$ROOT/src/transports/UsbCdcTransport.cpp"

begin_count=$(grep -Fc 'CALLBACK_POD_BEGIN' "$SOURCE" || true)
end_count=$(grep -Fc 'CALLBACK_POD_END' "$SOURCE" || true)
if [[ "$begin_count" -lt 3 || "$begin_count" != "$end_count" ]]; then
  echo "expected at least three balanced CALLBACK_POD blocks" >&2
  exit 1
fi

blocks=$(sed -n '/CALLBACK_POD_BEGIN/,/CALLBACK_POD_END/p' "$SOURCE")
for forbidden in 'currentDetail' 'currentState' 'String' 'cdc_acm_host_close' 'delay('; do
  if grep -Fq -- "$forbidden" <<<"$blocks"; then
    echo "USB callback violates POD-only contract: $forbidden" >&2
    exit 1
  fi
done

grep -Fq 'xStreamBufferSend' <<<"$blocks"
grep -Fq 'xQueueSend' <<<"$blocks"
grep -Fq 'nextRxEvent' "$SOURCE"
grep -Fq 'UsbCdcOrderedEvent' "$SOURCE"
grep -Fq 'STREAM_RESET' "$SOURCE"

if grep -Eq 'volatile[[:space:]].*(rxHead|rxTail|connected|cdcHandle)' "$SOURCE"; then
  echo "legacy volatile cross-task ownership remains" >&2
  exit 1
fi

echo "USB callback ownership checks passed."
