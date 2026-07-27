#include "trading/SimulatedLOB.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Measurement {
  std::size_t group_size{};
  std::size_t retained_capacity{};
  std::size_t initial_reallocations{};
  std::int64_t total_ns{};
  std::size_t checksum{};
};

Measurement measure(std::size_t group_size) {
  constexpr std::size_t warmup_groups = 1'000;
  constexpr std::size_t measured_groups = 20'000;
  constexpr std::array<cmf::InstrumentMeta, 1> instruments{
      cmf::InstrumentMeta{1, 1, 100, 1}};

  cmf::trading::SimulatedLOB simulated(instruments);
  if (!simulated.accept(1, 1, cmf::Side::Buy, 100, 1, 1, nullptr).empty()) {
    return {};
  }

  std::vector<cmf::PriceCrossSignal> signals;
  signals.reserve(8);
  std::size_t initial_reallocations{};
  std::size_t checksum{};

  const auto build_and_replay = [&](bool count_reallocations) {
    signals.clear();
    for (std::size_t index = 0; index < group_size; ++index) {
      const std::size_t old_capacity = signals.capacity();
      const auto sequence = static_cast<cmf::Sequence>(index + 1);
      if ((index & 1U) == 0U) {
        signals.push_back(cmf::PriceCrossSignal{
            1, 100, 100, sequence, cmf::PriceCrossSource::BestQuote,
            std::nullopt, cmf::PriceTicks{101}, std::nullopt});
      } else {
        signals.push_back(cmf::PriceCrossSignal{
            1, 100, 100, sequence, cmf::PriceCrossSource::Trade, std::nullopt,
            std::nullopt, cmf::PriceTicks{101}});
      }
      if (count_reallocations && signals.capacity() != old_capacity) {
        ++initial_reallocations;
      }
    }
    for (const auto &signal : signals) {
      checksum += simulated.on_signal(signal).size();
    }
  };

  build_and_replay(true);
  for (std::size_t iteration = 1; iteration < warmup_groups; ++iteration) {
    build_and_replay(false);
  }

  const auto start = Clock::now();
  for (std::size_t iteration = 0; iteration < measured_groups; ++iteration) {
    build_and_replay(false);
  }
  const auto end = Clock::now();
  return Measurement{
      group_size,
      signals.capacity(),
      initial_reallocations,
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(),
      checksum,
  };
}

void print(const Measurement &measurement) {
  constexpr std::size_t measured_groups = 20'000;
  const auto total_signals = measured_groups * measurement.group_size;
  const double ns_per_group =
      static_cast<double>(measurement.total_ns) / measured_groups;
  const double ns_per_signal = static_cast<double>(measurement.total_ns) /
                               static_cast<double>(total_signals);
  std::cout << "group_size=" << measurement.group_size
            << " retained_capacity=" << measurement.retained_capacity
            << " initial_reallocations=" << measurement.initial_reallocations
            << " measured_groups=" << measured_groups
            << " mean_ns_per_group=" << std::fixed << std::setprecision(1)
            << ns_per_group << " mean_ns_per_signal=" << ns_per_signal
            << " checksum=" << measurement.checksum << '\n';
}

} // namespace

int main() {
  std::cout << "price-cross signal buffer construction + SimulatedLOB replay\n"
            << "build_type=" << BACKTESTER_BENCHMARK_BUILD_TYPE
            << " initial_reserve=8 warmup_groups=1000\n";
  print(measure(8));
  print(measure(64));
}
