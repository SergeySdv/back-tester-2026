#include "MiniTest.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace minitest {

std::vector<TestCase> &registry() {
  static std::vector<TestCase> tests;
  return tests;
}

Registrar::Registrar(const char *name, TestFunction function) {
  registry().push_back(TestCase{name, function});
}

void require(bool condition, const char *expression, const char *file,
             int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": requirement failed: " + expression);
  }
}

namespace {

bool run_case(const TestCase &test) {
  try {
    test.function();
    std::cout << "[PASS] " << test.name << '\n';
    return true;
  } catch (const std::exception &error) {
    std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
  } catch (...) {
    std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
  }
  return false;
}

} // namespace

int run(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "--list-tests") {
    for (const auto &test : registry()) {
      std::cout << test.name << '\n';
    }
    return 0;
  }

  if (argc > 1) {
    const std::string selected(argv[1]);
    for (const auto &test : registry()) {
      if (test.name == selected) {
        return run_case(test) ? 0 : 1;
      }
    }
    std::cerr << "unknown test case: " << selected << '\n';
    return 2;
  }

  std::size_t failures = 0;
  for (const auto &test : registry()) {
    if (!run_case(test)) {
      ++failures;
    }
  }
  std::cout << registry().size() << " test(s), " << failures << " failure(s)\n";
  return failures == 0 ? 0 : 1;
}

} // namespace minitest

int main(int argc, char **argv) { return minitest::run(argc, argv); }
