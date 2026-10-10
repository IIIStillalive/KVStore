#pragma once
// 极简手写断言头：无第三方依赖(googletest)，配合每个测试独立 main() 使用。
#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "CHECK failed: %s @ %s:%d\n", #cond, __FILE__,      \
                   __LINE__);                                                  \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))