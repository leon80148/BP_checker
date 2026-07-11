# Task 8 TDD Replay and Audit Record

## Fixed revisions

- Initial Task 8 implementation: `02ff21b9647d9f95e05f50830458c03417ce3ef8`
- Spec-review fix: `595ea60bb93549062de377604f05bd3abf8ec506`
- Policy-version dashboard fix: `cf30577b83fc02a77150effbf2c80aa5db59b16e`
- Polling-liveness fix: `a58ae2e709ca9ab5e40f47529f1f510ac466e7ae`
- Back-forward-cache restore fix: `a5fd37ea10565722f4f11538854283dc5cdce5e4`
- Toolchain: Arduino CLI 1.4.1, ESP32 core 3.3.7, ArduinoJson 7.4.2,
  host C++17 compiler; run date 2026-07-11.

The RED observations below were produced before their corresponding production
changes in the same isolated worktree. They are retained as exact failure
signatures. The replay procedure at the end independently demonstrates that
the final regression contracts reject the pre-fix production tree.

## Observed RED signatures

### 64-bit uptime

Command:

```bash
c++ -std=c++17 -Wall -Wextra -iquote . -Itest/host \
  -o build/host_tests/test_measurement_policy \
  test/host/test_measurement_policy.cpp
```

Observed exit 1:

```text
error: unknown type name 'MonotonicMillis64'
warning: implicit conversion from 'unsigned long long' to 'uint32_t' changes value from 8589935592 to 1000
error: non-const lvalue reference to type 'uint32_t' cannot bind to a value of unrelated type 'uint64_t'
```

`bash scripts/check_ui_markup.sh` also exited 1 with:

```text
missing main-loop monotonic clock contract: MonotonicMillis64 uptimeClock
```

### Atomic policy persistence

The same focused compile exited 1 with the new persistence contract over the
old production header. The first failures were:

```text
error: unknown type name 'MeasurementPolicyResult'
error: use of undeclared identifier 'copyMeasurementPolicyName'
error: no member named 'policyVersion' in 'MeasurementPolicyConfig'
error: unknown type name 'MeasurementPolicyStore'
```

The boot/static contract exited 1 with:

```text
missing main-loop monotonic clock contract: MeasurementPolicyStore measurementPolicyStore
```

Strict decimal form parsing separately exited 1 with three
`use of undeclared identifier 'parseMeasurementPolicyUnsigned'` errors.

### Role-to-surface and route policy

Command:

```bash
c++ -std=c++17 -Wall -Wextra -iquote . -Itest/host \
  -o build/host_tests/test_web_access_policy \
  test/host/test_web_access_policy.cpp
```

Observed exit 1:

```text
static assertion failed: expression evaluates to '16 == 18'
error: unknown type name 'WebSurface'
error: use of undeclared identifier 'surfaceVisible'
```

The UI contract exited 1 with `missing token: /measurement_policy`. After the
route existed, it continued to exit 1 on
`production Web path still uses an implicit default policy: classifyMeasurement(latest)`.

### Fail-closed runtime config

After persistence was otherwise GREEN, a focused self-review regression exited
1 with exactly two failed checks:

```text
locked policy cannot expose a usable factory fallback config  (got 0, want 1)
storage failure leaves no accidentally usable policy  (got 0, want 1)
FAILED: 2/149 checks failed.
```

### Policy-version dashboard reload

After adding the focused UI/static contract over the pre-fix dashboard,
`bash scripts/check_ui_markup.sh` exited 1 with:

```text
missing policy-version dashboard reload contract: html += "';let bpPolicyVersion='";
```

This failure demonstrated that the page had no snapshot of the active policy
version and could not reload when `/api/latest` changed policy without changing
the measurement revision.

### Bounded polling and independent freshness watchdog

After adding the polling-liveness contract over `64af22d`,
`bash scripts/check_ui_markup.sh` exited 1 with:

```text
missing bounded dashboard polling/watchdog contract: AbortController
```

The first implementation used the lower of the measurement stale threshold
and request deadline for response silence. A focused contract correction then
exited 1 with:

```text
missing bounded dashboard polling/watchdog contract: responseAge>=bpRequestDeadlineMs
```

This second RED prevents a one-second measurement stale policy from falsely
marking the API disconnected between normal three-second polls. Response
silence now uses the fixed application deadline; record age alone uses the
active policy stale interval.

### Back-forward-cache restoration

After adding the persisted-restore contract over `72dd74c`,
`bash scripts/check_ui_markup.sh` exited 1 with:

```text
missing bounded dashboard polling/watchdog contract: window.addEventListener('pageshow',(event)=>{if(event.persisted)location.reload();})
```

This demonstrates the one-way lifecycle: `pagehide` stopped and cleaned all
timers, but a dashboard restored from bfcache had no path to restart. The final
handler reloads only when `event.persisted` is true, so the ordinary initial
`pageshow` cannot create a reload loop.

## GREEN commands and results at `595ea60`

```bash
bash scripts/run_host_tests.sh
bash scripts/check_ui_markup.sh
bash scripts/check_security_runtime_integration.sh
bash scripts/run_quality_gate.sh
```

Observed final results:

- `test_measurement_policy`: 149 checks passed.
- `test_web_access_policy`: 276 checks passed.
- `test_web_request_gate`: 2,201 checks passed.
- Every host executable passed; all USB stress scenarios and TSan passed.
- UI/static and security integration contracts passed.
- Pinned ESP32-S3 build passed: sketch 1,131,252 bytes (86%), globals
  72,524 bytes (22%).
- Firmware artifact: 1,131,408 bytes; SHA-256
  `82c1b488d1c4dd490f1d827a04df929ea0a2361898b7032dfb3d94098c3d5e5e`.
- `strings build/firmware/BP_checker.ino.bin` contains the full
  `595ea60bb93549062de377604f05bd3abf8ec506` source identity.

At `cf30577`, `bash scripts/check_ui_markup.sh` passed after requiring the
active policy-version snapshot, decimal-string comparison immediately after
JSON parsing, and reload before dashboard state updates. The complete clean
`bash scripts/run_quality_gate.sh` also passed: every host/static/stress/TSan
stage, pinned firmware, and SBOM generation. The sketch is 1,131,380 bytes
(86%), globals are 72,524 bytes (22%), and the 1,131,536-byte firmware artifact
has SHA-256
`68e272cdb09bc0b21a6f650f09ceccc1c9aa0d0ebbce9611f66233bf1aceb58e`.
The binary contains the full
`cf30577b83fc02a77150effbf2c80aa5db59b16e` source identity.

At `a58ae2e`, `bash scripts/check_ui_markup.sh` passed. The executable contract
requires one in-flight poll, `AbortController` with an 8-second deadline, a
separate one-second watchdog, active-policy record-age staleness, visible
freshness/transport/operator action, reload checks before response state or DOM
updates, and bounded timer cleanup on `pagehide`. `bash scripts/run_host_tests.sh`
also passed every executable, including policy 149, record manager 1,199, Web
access 276, and request gate 2,201 checks. The pinned firmware/full repository
gate was intentionally deferred to the owning final review.

At `a5fd37e`, both `bash scripts/check_ui_markup.sh` and every executable in
`bash scripts/run_host_tests.sh` passed, including policy 149, record manager
1,199, Web access 276, and request gate 2,201 checks. The change is limited to
the persisted-only `pageshow` reload and its executable static contract; the
full firmware gate remains deferred to the owning final review.

## Independent RED replay against the base production tree

The initial Task 8 regression contract can first be replayed against its direct
predecessor. This overlays only the Task 8 policy test and UI/static assertions
from `02ff21b` onto production at `9f866b5`:

```bash
git worktree add /tmp/bp-task8-initial-red 9f866b5
git -C /tmp/bp-task8-initial-red checkout 02ff21b -- \
  test/host/test_measurement_policy.cpp scripts/check_ui_markup.sh
cd /tmp/bp-task8-initial-red
c++ -std=c++17 -Wall -Wextra -iquote . -Itest/host \
  -o /tmp/test_measurement_policy test/host/test_measurement_policy.cpp
bash scripts/check_ui_markup.sh
```

Observed on 2026-07-11: both commands exited 1. The focused compile reported
`fatal error: 'lib/MeasurementPolicy.h' file not found`, and the UI/static
contract reported `missing token: :focus-visible`. Remove the disposable
worktree afterward with
`git worktree remove --force /tmp/bp-task8-initial-red`.

The spec-fix contract can then be replayed against the initial Task 8
production revision. This overlays only the final tests/static assertions from
`595ea60` onto production at `02ff21b`:

```bash
git worktree add /tmp/bp-task8-red 02ff21b
git show 595ea60:test/host/test_measurement_policy.cpp > \
  /tmp/bp-task8-red/test/host/test_measurement_policy.cpp
git show 595ea60:test/host/test_web_access_policy.cpp > \
  /tmp/bp-task8-red/test/host/test_web_access_policy.cpp
git show 595ea60:scripts/check_ui_markup.sh > \
  /tmp/bp-task8-red/scripts/check_ui_markup.sh
cd /tmp/bp-task8-red
c++ -std=c++17 -Wall -Wextra -iquote . -Itest/host \
  -o /tmp/test_measurement_policy test/host/test_measurement_policy.cpp
c++ -std=c++17 -Wall -Wextra -iquote . -Itest/host \
  -o /tmp/test_web_access_policy test/host/test_web_access_policy.cpp
bash scripts/check_ui_markup.sh
```

Expected: all three final commands exit nonzero with the missing clock/store,
16-vs-18 route/surface, and main-loop/static failure classes recorded above.
Remove the disposable worktree afterward with
`git worktree remove --force /tmp/bp-task8-red`.

Finally, the policy-version reload contract can be replayed against the direct
pre-fix revision while leaving its production Web handler unchanged:

```bash
git worktree add /tmp/bp-task8-policy-reload-red \
  6dbcf558aaf8833a24d4f21eb270c8d0a8b991dc
git -C /tmp/bp-task8-policy-reload-red checkout \
  cf30577b83fc02a77150effbf2c80aa5db59b16e -- \
  scripts/check_ui_markup.sh
cd /tmp/bp-task8-policy-reload-red
bash scripts/check_ui_markup.sh
```

Observed on 2026-07-11: the UI/static command exited 1 with the exact missing
`bpPolicyVersion` snapshot signature recorded above. Remove the disposable
worktree with
`git worktree remove --force /tmp/bp-task8-policy-reload-red`.

The polling-liveness contract can be replayed against its direct pre-fix
revision while retaining the old production Web handler:

```bash
git worktree add /tmp/bp-task8-poll-watchdog-red \
  64af22d85964d77aac41948366d6fd4c79bec9e9
git -C /tmp/bp-task8-poll-watchdog-red checkout \
  a58ae2e709ca9ab5e40f47529f1f510ac466e7ae -- \
  scripts/check_ui_markup.sh
cd /tmp/bp-task8-poll-watchdog-red
bash scripts/check_ui_markup.sh
```

Observed on 2026-07-11: the UI/static command exited 1 with the exact missing
`AbortController` signature above. Remove the disposable worktree with
`git worktree remove --force /tmp/bp-task8-poll-watchdog-red`.

The bfcache restoration contract can be replayed against its direct pre-fix
revision while retaining the stopped-timer production behavior:

```bash
git worktree add /tmp/bp-task8-bfcache-red \
  72dd74c67ccdb0bfcdce5a0c6f9624b4bbaaf66e
git -C /tmp/bp-task8-bfcache-red checkout \
  a5fd37ea10565722f4f11538854283dc5cdce5e4 -- \
  scripts/check_ui_markup.sh
cd /tmp/bp-task8-bfcache-red
bash scripts/check_ui_markup.sh
```

Observed on 2026-07-11: the command exited 1 with the exact missing persisted
`pageshow` handler signature above. Remove the disposable worktree with
`git worktree remove --force /tmp/bp-task8-bfcache-red`.
