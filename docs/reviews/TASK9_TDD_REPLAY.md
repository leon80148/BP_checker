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
