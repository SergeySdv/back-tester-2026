// What does the ready-signal round trip actually cost, once a real consumer is
// on the other end?
//
// SchedulerBench measures the ring and barrier in isolation with an empty
// consumer. This one puts the real TradingEngine and a no-op C++ strategy
// behind the barrier and runs the same event stream two ways:
//
//   same-thread   the engine is called directly, no ring, no barrier;
//   cross-thread  the production SchedulerRuntime, dispatcher and consumer on
//                 separate threads with the barrier in between.
//
// The difference between the two per-event figures is the price of the
// round trip. Reporting only the cross-thread number would leave that price
// entangled with the engine's own work.
//
// No Python is involved here on purpose; the callback boundary is measured
// separately by python/benchmarks/callback_overhead.py.

#include "BenchReport.hpp"

#include "core/BacktestConfig.hpp"
#include "core/Events.hpp"
#include "market/HistoricalLOBStore.hpp"
#include "scheduler/SchedulerRuntime.hpp"
#include "trading/Strategy.hpp"
#include "trading/TradingEngine.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace cmf;

constexpr InstrumentId kInstrument = 1;
constexpr std::size_t kWarmup = 20'000;
constexpr std::size_t kMeasured = 200'000;

// Every callback is already empty in the base class; this only makes the
// intent explicit at the call site.
struct NoopStrategy final : trading::Strategy {};

struct NoopRecorder final : trading::Recorder {};

std::vector<ScheduledEvent> make_events(std::size_t count) {
  std::vector<ScheduledEvent> events;
  events.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto sequence = static_cast<Sequence>(index + 1);
    const auto time = static_cast<TimestampNs>(index + 1);
    // No book update, no trades, no signals: the engine does its dispatch and
    // clock work but no matching, so the measurement stays on the plumbing.
    events.push_back(ScheduledEvent{MarketDelivery{
        kInstrument, time, time, sequence, std::nullopt, {}, {}}});
  }
  return events;
}

std::int64_t elapsed_ns(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
      .count();
}

} // namespace

int main() {
  const std::array<InstrumentMeta, 1> instruments{
      InstrumentMeta{kInstrument, 1, 1, 1}};
  const BacktestConfig config{0, 1'000, 15, RiskLimits{}};
  const auto events = make_events(kWarmup + kMeasured);

  std::cout << "engine round trip: dispatcher -> TradingEngine -> "
               "publish_processed -> next dispatch\n";
  bench::print_environment();
  std::cout << "strategy=cpp_noop payload=empty_market_delivery"
            << " warmup=" << kWarmup << " measured=" << kMeasured << '\n';

  // --- same thread --------------------------------------------------------
  std::int64_t same_thread_total = 0;
  {
    market::HistoricalLOBStore books;
    NoopStrategy strategy;
    NoopRecorder recorder;
    trading::TradingEngine engine(instruments, config, books, strategy,
                                  recorder);
    scheduler::SpscRing<scheduler::OrderCommand> commands(64);
    scheduler::CommandSink sink(commands);

    std::vector<std::int64_t> samples;
    samples.reserve(kMeasured);
    for (std::size_t index = 0; index < events.size(); ++index) {
      const auto start = Clock::now();
      engine(events[index], sink);
      const auto end = Clock::now();
      if (index >= kWarmup) {
        samples.push_back(elapsed_ns(start, end));
      }
    }
    same_thread_total =
        std::accumulate(samples.begin(), samples.end(), std::int64_t{0});
    bench::report("same-thread   per event", samples);
  }

  // --- cross thread -------------------------------------------------------
  std::int64_t cross_thread_total = 0;
  {
    market::HistoricalLOBStore books;
    NoopStrategy strategy;
    NoopRecorder recorder;
    trading::TradingEngine engine(instruments, config, books, strategy,
                                  recorder);

    std::vector<std::int64_t> samples;
    samples.reserve(kMeasured);
    std::size_t index = 0;
    Clock::time_point previous_ack = Clock::now();

    scheduler::SchedulerRuntime runtime(
        scheduler::SchedulerRuntimeConfig{DateRange{}, 1, 64, 4096});
    const auto wall_start = Clock::now();
    runtime.run(events,
                [&](const ScheduledEvent &event, scheduler::CommandSink &sink) {
                  // Measured from the moment the previous event was
                  // acknowledged to the moment this one finishes, which is
                  // exactly one barrier round trip plus the engine's work.
                  engine(event, sink);
                  const auto now = Clock::now();
                  if (index >= kWarmup) {
                    samples.push_back(elapsed_ns(previous_ack, now));
                  }
                  previous_ack = now;
                  ++index;
                });
    const auto wall_end = Clock::now();

    cross_thread_total =
        std::accumulate(samples.begin(), samples.end(), std::int64_t{0});
    bench::report("cross-thread  per event", samples);
    std::cout << "cross-thread wall clock: "
              << elapsed_ns(wall_start, wall_end) / 1'000'000 << " ms for "
              << events.size() << " events\n";
  }

  const auto same_mean =
      static_cast<double>(same_thread_total) / static_cast<double>(kMeasured);
  const auto cross_mean =
      static_cast<double>(cross_thread_total) / static_cast<double>(kMeasured);
  std::cout << "round-trip cost (cross minus same, mean): " << std::fixed
            << std::setprecision(1) << cross_mean - same_mean << " ns/event\n";
}
