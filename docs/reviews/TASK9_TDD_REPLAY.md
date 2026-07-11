# Task 9 TDD Replay

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
