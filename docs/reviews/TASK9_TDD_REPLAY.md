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
