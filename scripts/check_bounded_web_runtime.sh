#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

header=lib/BoundedWebServer.h
source=src/BoundedWebServer.cpp

test -f "$header"
test -f "$source"

grep -Fq 'class BoundedWebServer' "$header"
grep -Fq 'void handleClient() override' "$header"
grep -Fq 'size_t _currentClientWrite(' "$header"
grep -Fq 'BoundedHttpTransaction _transaction' "$header"
grep -Fq 'BoundedIngressBuffer _ingress' "$header"
grep -Fq 'BoundedSocketRuntime _socketRuntime' "$header"

grep -Fq '_server.accept()' "$source"
grep -Fq 'MSG_DONTWAIT' "$source"
grep -Fq '::send(' "$source"
grep -Fq '::recv(' "$source"
grep -Fq '::shutdown(' "$source"
grep -Fq 'BoundedIngressBuffer::kCapacity' lib/BoundedSocketRuntime.h
grep -Fq '_transaction.consume(' "$source"
grep -Fq '_gate->evaluate(' "$source"
grep -Fq '_transaction.request(), _runtimeSnapshot.security' "$source"
grep -Fq 'pumpResponse(_socketRuntime.nowMs())' "$source"
grep -Fq '_socketRuntime.beginDrain()' "$source"
grep -Fq '_socketRuntime.pollDrain(' "$source"
grep -Fq 'catch (const std::bad_alloc&)' "$source"
grep -Fq '_transaction.rejectCapture(' "$source"
grep -Fq 'mandatoryResponseHeadersAreExact()' "$source"
grep -Fq '_transaction.capturedResponseIsValidHttp1()' "$source"

if grep -Eq '_parseRequest|_currentClient\.(available|connected|read)|readString|readBytes|delay\(|yield\(' "$source"; then
  echo "bounded web runtime uses a blocking/stock parser primitive" >&2
  exit 1
fi

if grep -Eq '_currentClient\.write' "$source"; then
  echo "bounded web runtime bypasses or redefines the capture contract" >&2
  exit 1
fi

if rg -n \
    '\b(serveStatic|streamFile|sendContent|sendContent_P|chunkResponseBegin|chunkWrite|chunkResponseEnd|setContentLength|enableCORS|enableCrossOrigin)\s*\(|CONTENT_LENGTH_UNKNOWN|([.-]>?|\.)client\s*\(' \
    BP_checker.ino lib src \
    --glob '!lib/BoundedWebServer.h' \
    --glob '!lib/BoundedSocketRuntime.h' \
    --glob '!src/BoundedWebServer.cpp' \
    --glob '!src/third_party/**'; then
  echo "application code uses an unsupported inherited response API" >&2
  exit 1
fi

if rg -n '(_currentClient|_chunkedClient)\s*\.\s*(write|write_P)\s*\(' \
    BP_checker.ino lib src \
    --glob '!lib/BoundedWebServer.h' \
    --glob '!src/BoundedWebServer.cpp' \
    --glob '!src/third_party/**'; then
  echo "application handler bypasses transactional response capture" >&2
  exit 1
fi

echo "bounded web runtime contract passed"
