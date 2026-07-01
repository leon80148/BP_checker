// 共用最小測試框架：CHECK 巨集 + 失敗計數。
// 每個 test_*.cpp 是獨立 binary，include 此檔並在 main 尾端呼叫 testReport()。
#ifndef HOST_TEST_SUPPORT_H
#define HOST_TEST_SUPPORT_H

#include <cstdio>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK_EQ(actual, expected, label)                                      \
  do {                                                                         \
    g_checks++;                                                                \
    auto _a = (actual);                                                        \
    auto _e = (expected);                                                      \
    if (!(_a == _e)) {                                                         \
      g_failures++;                                                            \
      printf("FAIL %s:%d  %s  (got %ld, want %ld)\n", __FILE__, __LINE__,     \
             label, (long)_a, (long)_e);                                       \
    }                                                                          \
  } while (0)

#define CHECK_TRUE(cond, label) CHECK_EQ((cond) ? 1 : 0, 1, label)

#define CHECK_STR(actual, expected, label)                                     \
  do {                                                                         \
    g_checks++;                                                                \
    String _a = (actual);                                                      \
    if (!(_a == (expected))) {                                                 \
      g_failures++;                                                            \
      printf("FAIL %s:%d  %s  (got \"%s\", want \"%s\")\n", __FILE__,         \
             __LINE__, label, _a.c_str(), expected);                           \
    }                                                                          \
  } while (0)

static inline int testReport() {
  if (g_failures == 0) {
    printf("OK: %d checks passed.\n", g_checks);
    return 0;
  }
  printf("FAILED: %d/%d checks failed.\n", g_failures, g_checks);
  return 1;
}

#endif
