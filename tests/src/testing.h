#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

/// Minimal test registry. There is no network access in this environment, so
/// GoogleTest cannot be vendored; this covers what the suite needs.
namespace testing {

struct Case {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}
inline int& failureCount() {
  static int f = 0;
  return f;
}

struct Registrar {
  Registrar(std::string name, std::function<void()> fn) {
    registry().push_back({std::move(name), std::move(fn)});
  }
};

template <class T>
std::string str(const T& v) {
  std::ostringstream o;
  o << v;
  return o.str();
}
inline std::string str(bool v) { return v ? "true" : "false"; }

inline void fail(const char* file, int line, const std::string& message) {
  std::printf("    FAIL %s:%d  %s\n", file, line, message.c_str());
  ++failureCount();
}

inline int run() {
  int failedCases = 0;
  for (const auto& c : registry()) {
    const int before = failureCount();
    std::printf("[ RUN  ] %s\n", c.name.c_str());
    c.fn();
    if (failureCount() > before) {
      std::printf("[ FAIL ] %s\n", c.name.c_str());
      ++failedCases;
    } else {
      std::printf("[  OK  ] %s\n", c.name.c_str());
    }
  }
  std::printf("\n%zu case(s), %d failed\n", registry().size(), failedCases);
  return failedCases == 0 ? 0 : 1;
}

}  // namespace testing

#define VAC_CAT_(a, b) a##b
#define VAC_CAT(a, b) VAC_CAT_(a, b)

#define TEST(name)                                                       \
  static void VAC_CAT(name, _body)();                                    \
  static ::testing::Registrar VAC_CAT(name, _reg)(#name,                 \
                                                  VAC_CAT(name, _body)); \
  static void VAC_CAT(name, _body)()

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) ::testing::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (false)

#define CHECK_EQ(a, b)                                                                  \
  do {                                                                                  \
    const auto va = (a);                                                                \
    const auto vb = (b);                                                                \
    if (!(va == vb)) {                                                                  \
      ::testing::fail(__FILE__, __LINE__,                                               \
                      std::string(#a " == " #b " (") + ::testing::str(va) + " vs " +     \
                          ::testing::str(vb) + ")");                                    \
    }                                                                                   \
  } while (false)

#define CHECK_NEAR(a, b, tol)                                                            \
  do {                                                                                   \
    const double va = static_cast<double>(a);                                            \
    const double vb = static_cast<double>(b);                                            \
    if (!(std::abs(va - vb) <= (tol))) {                                                 \
      ::testing::fail(__FILE__, __LINE__,                                                \
                      std::string(#a " ~= " #b " (") + ::testing::str(va) + " vs " +      \
                          ::testing::str(vb) + ")");                                     \
    }                                                                                    \
  } while (false)
