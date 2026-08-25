// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Minimal, dependency-free test framework for Memory Observatory.

#ifndef MEMORY_OBSERVATORY_TEST_FRAMEWORK_HPP
#define MEMORY_OBSERVATORY_TEST_FRAMEWORK_HPP

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace mo_test {

struct TestCase {
  const char* name;
  std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> body) {
    registry().push_back(TestCase{name, std::move(body)});
  }
};

inline int& checks() { static int c = 0; return c; }
inline int& failures() { static int f = 0; return f; }
inline const char*& current() { static const char* c = ""; return c; }

inline void check(bool ok, const char* expr, const char* file, int line) {
  ++checks();
  if (!ok) {
    ++failures();
    std::printf("  FAIL %s:%d  %s  [%s]\n", file, line, expr, current());
    std::fflush(stdout);
  }
}

inline int run_all() {
  for (auto& t : registry()) {
    current() = t.name;
    std::printf("[ RUN ] %s\n", t.name);
    std::fflush(stdout);
    const int before = failures();
    t.body();
    std::fflush(stdout);
    if (failures() == before) {
      std::printf("[  OK ] %s\n", t.name);
    } else {
      std::printf("[ FAIL] %s\n", t.name);
    }
    std::fflush(stdout);
  }
  std::printf("\n%d checks, %d failures\n", checks(), failures());
  return failures() == 0 ? 0 : 1;
}

}  // namespace mo_test

#define TEST(name)                                                         \
  static void name();                                                      \
  static ::mo_test::Registrar reg_##name(#name, name);                     \
  static void name()

#define CHECK(cond) ::mo_test::check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    const auto _a = (a);                                                   \
    const auto _b = (b);                                                   \
    if (!(_a == _b)) {                                                     \
      ::mo_test::check(false, #a " == " #b, __FILE__, __LINE__);           \
    } else {                                                               \
      ++::mo_test::checks();                                               \
    }                                                                      \
  } while (0)

#define REQUIRE(cond)                                                      \
  do {                                                                     \
    if (!(cond)) {                                                         \
      ::mo_test::check(false, #cond, __FILE__, __LINE__);                  \
      return;                                                              \
    }                                                                      \
  } while (0)

#endif  // MEMORY_OBSERVATORY_TEST_FRAMEWORK_HPP
