# Task 9 TDD Replay

## Signed update policy core

The initial Task 9A RED can be replayed without copying any production code.
Overlay only the final test onto the direct pre-implementation revision:

```bash
git worktree add /tmp/bp-task9-policy-red \
  8d8b52eecde12c05c643c79f98724dda876a1a21
git show 39c14fd:test/host/test_firmware_update_policy.cpp > \
  /tmp/bp-task9-policy-red/test/host/test_firmware_update_policy.cpp
cd /tmp/bp-task9-policy-red
c++ -std=c++17 -Wall -Wextra -iquote . -Itest/host \
  -o /tmp/test_firmware_update_policy \
  test/host/test_firmware_update_policy.cpp
```

Observed on 2026-07-11: compilation exited 1 with
`fatal error: '../../lib/FirmwareUpdatePolicy.h' file not found`. Remove the
disposable worktree afterward:

```bash
git worktree remove --force /tmp/bp-task9-policy-red
```

At `39c14fd`, the same focused test passes 90 checks normally and with
`-Werror -fsanitize=address,undefined -fno-omit-frame-pointer`
(`ASAN_OPTIONS=detect_leaks=0` on macOS, where leak sanitizer is unsupported).
`bash scripts/run_host_tests.sh` also passes every host executable.

The Task 9A review RED can be replayed by overlaying only the final test onto
the pre-fix policy core:

```bash
git worktree add /tmp/bp-task9-policy-review-red 7ff644e
git show f9d4deb:test/host/test_firmware_update_policy.cpp > \
  /tmp/bp-task9-policy-review-red/test/host/test_firmware_update_policy.cpp
cd /tmp/bp-task9-policy-review-red
c++ -std=c++17 -Wall -Wextra -iquote . -Itest/host \
  -o /tmp/test_firmware_update_policy \
  test/host/test_firmware_update_policy.cpp
```

The final API-level authorization tests do not compile against the unrestricted
pre-fix API. Before that type-level assertion was added, the behavioral probes
ran and failed 3/96 checks: failed-begin cleanup, destructor abort, and corrupt
newest-slot rollback. At `f9d4deb`, the final focused suite passes 106 checks
normally and under ASan/UBSan. Remove the disposable worktree with
`git worktree remove --force /tmp/bp-task9-policy-review-red`.

The ambiguous-reconciliation probe can be replayed by overlaying the final test
from `b2bf543` onto `701de7d`. It fails 3/112 checks because the pre-fix store
remains ready, authorizes a lower follow-on sequence, and overwrites the newer
durable slot. At `b2bf543`, the focused normal and ASan/UBSan runs pass all 112
checks; failed reconciliation locks the runtime object until explicit reload.

The bounded consumer lifecycle RED can be replayed by overlaying
`184a0fb:test/host/test_bounded_stream_consumer.cpp` onto `f1504c3` and running
the normal focused compile. It exits 1 because `lib/BoundedStreamConsumer.h`
does not exist. At `184a0fb`, normal and ASan/UBSan focused runs pass 67 checks,
and the full host runner includes the lifecycle test automatically.

The callback re-entry review RED can be replayed by overlaying
`72e7cab:test/host/test_bounded_stream_consumer.cpp` onto `00f15d7`. The
focused run fails 4/82 checks because begin/write/finish report success after a
nested `cancel()`, and finish resurrects the state as complete. At `72e7cab`,
normal and ASan/UBSan focused runs pass all 82 checks.

The deferred-abort re-review RED overlays
`8af1ef7:test/host/test_bounded_stream_consumer.cpp` onto `f9eb5b2`. It fails
3/88 checks because external abort runs before each reentrant callback returns.
At `8af1ef7`, normal and ASan/UBSan focused runs pass 88 checks and abort is
deferred until no external callback remains on the stack.

The explicit route-body RED can be replayed by overlaying
`39e3a2c:test/host/test_web_access_policy.cpp` onto `c41c658`; compilation
fails because `RouteBodyKind` and `routeBodyPolicyIsValid` do not exist. At
`39e3a2c`, access policy passes 294 checks and request gate passes 2,201 checks.

The server-plumbing RED can be replayed by overlaying
`f143a0e:test/host/test_bounded_http_transaction.cpp` and
`f143a0e:scripts/check_bounded_web_runtime.sh` onto `404e71d`. The focused
compile fails because `BoundedHttpTransaction::rejectBody()` is absent, and the
runtime contract fails because the bounded server has no stream consumer. At
`f143a0e`, transaction passes 837 checks, the runtime contract passes, and the
pinned ESP32-S3 build uses 1,136,064 bytes (86%) with 72,572 bytes globals.

The pending-receipt RED can be replayed by overlaying
`bf1a110:test/host/test_firmware_update_policy.cpp` onto `e4d1e92`; focused
compilation fails because the receipt type, fixed size, and codec do not exist.
At `bf1a110`, normal and ASan/UBSan runs pass 602 checks, including one bit flip
at every byte offset of the 480-byte receipt.

The ESP32 runtime RED is `bash scripts/check_firmware_update_runtime.sh` at
`7ae881d`; it exits 1 because `lib/FirmwareUpdateRuntime.h` is absent. At
`281464e`, the static runtime contract passes and the pinned ESP32-S3 target
compiles at 1,137,180 bytes (86%) with 72,572 bytes globals. The runtime remains
unreachable until the administrator routes and sketch lifecycle are integrated.

The curve-pin review RED is `bash scripts/check_firmware_update_runtime.sh` on
`37eb6c7`; it exits 1 on missing `MBEDTLS_ECP_DP_SECP256R1`. At `d8212c5`, the
runtime checks the parsed EC group ID and the pinned target compiles.

## Bounded Binary HTTP Stream

The production commit is `da220d1`. Its parent, `f43c855`, still rejects every
`BodyMode::STREAM` transaction with HTTP 501. The following replay overlays only
the final focused test on that parent, leaving production code unchanged:

```bash
git worktree add /tmp/bp-task9b-red f43c855
git show da220d1:test/host/test_bounded_http_transaction.cpp \
  > /tmp/bp-task9b-red/test/host/test_bounded_http_transaction.cpp
cd /tmp/bp-task9b-red
c++ -std=c++17 -Wall -Wextra -Werror -iquote . -Itest/host \
  -o /tmp/test_bounded_http_transaction \
  test/host/test_bounded_http_transaction.cpp
```

Expected RED: compilation exits 1 because the read-only chunk view, drain API,
hard stream limit, and stream deadline do not exist. During the same TDD cycle,
runtime probes separately exposed two unsafe transitions: `Content-Length: 0`
became ready without a final drain (2/225 failures), and a final chunk could be
drained at the absolute deadline (2/830 failures).

Replay GREEN from the implementation commit:

```bash
git worktree add /tmp/bp-task9b-green da220d1
cd /tmp/bp-task9b-green
c++ -std=c++17 -Wall -Wextra -Werror -iquote . -Itest/host \
  -o /tmp/test_bounded_http_transaction \
  test/host/test_bounded_http_transaction.cpp
/tmp/test_bounded_http_transaction
c++ -std=c++17 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -iquote . -Itest/host -o /tmp/test_bounded_http_transaction_san \
  test/host/test_bounded_http_transaction.cpp
ASAN_OPTIONS=detect_leaks=0 /tmp/test_bounded_http_transaction_san
bash scripts/run_host_tests.sh
```

Expected GREEN: both focused binaries report `OK: 830 checks passed`, and every
host executable passes. This commit intentionally stops before any Web server or
OTA consumer integration.
