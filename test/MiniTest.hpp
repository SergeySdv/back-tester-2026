#pragma once

#include <string>
#include <vector>

namespace minitest {

using TestFunction = void (*)();

struct TestCase {
  std::string name;
  TestFunction function;
};

std::vector<TestCase> &registry();

class Registrar {
public:
  Registrar(const char *name, TestFunction function);
};

void require(bool condition, const char *expression, const char *file,
             int line);

// Entry point shared by every test binary.
//
// With no arguments every registered case runs. `--list-tests` prints one case
// name per line and exits, which lets CMake register each case as its own
// ctest entry after the build. Any other argument selects the single case with
// that exact name, so `ctest -R` and a manual run agree on granularity.
int run(int argc, char **argv);

} // namespace minitest

#define MINITEST_CONCAT_IMPL(left, right) left##right
#define MINITEST_CONCAT(left, right) MINITEST_CONCAT_IMPL(left, right)

#define TEST_CASE(name, tags)                                                  \
  static void MINITEST_CONCAT(minitest_case_, __LINE__)();                     \
  static const ::minitest::Registrar MINITEST_CONCAT(minitest_registrar_,      \
                                                     __LINE__)(                \
      name, &MINITEST_CONCAT(minitest_case_, __LINE__));                       \
  static void MINITEST_CONCAT(minitest_case_, __LINE__)()

#define REQUIRE(expression)                                                    \
  ::minitest::require(static_cast<bool>(expression), #expression, __FILE__,    \
                      __LINE__)

#define REQUIRE_FALSE(expression) REQUIRE(!(expression))
