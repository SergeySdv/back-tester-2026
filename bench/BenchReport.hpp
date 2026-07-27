#pragma once

// Shared reporting for the hand-rolled benchmarks.
//
// Numbers from different machines are only comparable if the run says what it
// ran on, so every benchmark prints the same environment header. Latency is
// reported as percentiles, never as a mean alone: a mean is dominated by the
// tail and hides exactly the behaviour these benchmarks exist to show.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace bench {

#if defined(__clang__)
constexpr auto compiler = "Clang " __clang_version__;
#elif defined(__GNUC__)
constexpr auto compiler = "GCC " __VERSION__;
#else
constexpr auto compiler = "unknown";
#endif

#if defined(__APPLE__)
constexpr auto operating_system = "macOS";
#elif defined(__linux__)
constexpr auto operating_system = "Linux";
#elif defined(_WIN32)
constexpr auto operating_system = "Windows";
#else
constexpr auto operating_system = "unknown";
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
constexpr auto architecture = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
constexpr auto architecture = "x86_64";
#else
constexpr auto architecture = "unknown";
#endif

[[nodiscard]] inline std::int64_t
percentile(const std::vector<std::int64_t> &sorted, double fraction) {
  if (sorted.empty()) {
    return 0;
  }
  const auto index = static_cast<std::size_t>(
      fraction * static_cast<double>(sorted.size() - 1));
  return sorted[index];
}

[[nodiscard]] inline std::string cpu_summary() {
#if defined(__APPLE__)
  for (const char *key : {"machdep.cpu.brand_string", "hw.model"}) {
    std::size_t size = 0;
    if (sysctlbyname(key, nullptr, &size, nullptr, 0) != 0 || size <= 1) {
      continue;
    }
    std::string value(size, '\0');
    if (sysctlbyname(key, value.data(), &size, nullptr, 0) == 0) {
      value.resize(size - 1);
      return value;
    }
  }
#elif defined(__linux__)
  std::ifstream cpu_info("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpu_info, line)) {
    constexpr auto prefix = "model name";
    if (line.starts_with(prefix)) {
      const auto separator = line.find(':');
      if (separator != std::string::npos) {
        return line.substr(separator + 2);
      }
    }
  }
#endif
  return "unavailable";
}

inline void print_environment() {
  std::cout << "build_type=" << BACKTESTER_BENCHMARK_BUILD_TYPE
            << " compiler=" << compiler << " os=" << operating_system
            << " arch=" << architecture << '\n'
            << "cpu=" << cpu_summary()
            << " hardware_threads=" << std::thread::hardware_concurrency()
            << '\n';
}

// Reports one distribution. `samples` is sorted in place.
inline void report(const std::string &label, std::vector<std::int64_t> &samples,
                   const std::string &unit = "ns") {
  if (samples.empty()) {
    std::cout << label << ": no samples\n";
    return;
  }
  std::sort(samples.begin(), samples.end());
  const auto total =
      std::accumulate(samples.begin(), samples.end(), std::int64_t{0});
  const auto mean =
      static_cast<double>(total) / static_cast<double>(samples.size());
  std::cout << label << ": n=" << samples.size() << " min=" << samples.front()
            << " p50=" << percentile(samples, 0.50)
            << " p95=" << percentile(samples, 0.95)
            << " p99=" << percentile(samples, 0.99) << " max=" << samples.back()
            << " mean=" << std::fixed << std::setprecision(1) << mean << ' '
            << unit << '\n';
}

} // namespace bench
