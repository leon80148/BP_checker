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
