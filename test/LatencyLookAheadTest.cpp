// Order latency must be real: an order decided at t0 may only interact with
// book state at t0 + order_latency. If the touch it aimed at disappears in
// between, it must rest instead of filling. Each look-ahead case is paired with
// a mirror case that does fill, so a permanently non-filling engine cannot make
// this file green.

#include "EngineHarness.hpp"

#include "MiniTest.hpp"

#include <array>
#include <vector>

namespace {

using namespace cmf;
using namespace harness;

constexpr InstrumentId kInstrument = 1;
constexpr TimestampNs kLatency = 1'000'000; // 1 ms
constexpr TimestampNs kStart = 1'000'000'000;
constexpr PriceTicks kLimit = 100;

const std::vector<InstrumentMeta> kInstruments{
    InstrumentMeta{kInstrument, 1, 1, 1}};

// Sends one buy limit at kLimit on the first book update it sees.
struct BuyOnFirstBook final : RecordingStrategy {
  ClOrdId order_id{};
  Quantity quantity{1};
  bool submitted{};

  void on_book_update(const BookUpdateView &update,
                      StrategyContext &context) override {
    RecordingStrategy::on_book_update(update, context);
    if (!submitted) {
      submitted = true;
      order_id = context.submit_limit(kInstrument, Side::Buy, kLimit, quantity);
    }
  }
};

// Book showing an ask resting at `price`, seeded before the run so the first
// delivery is a genuine update rather than the initial snapshot.
Scenario make_scenario(TimestampNs order_latency) {
  return Scenario{kInstruments, BacktestConfig{0, order_latency, 15}};
}

} // namespace

TEST_CASE("Order does not fill against a touch that vanished during its flight",
          "[Latency]") {
  auto scenario = make_scenario(kLatency);
  // t0: ask 100 exists, strategy submits a buy at 100; arrival is t0 + L.
  scenario.script().group(
      add_order(kInstrument, kStart, 1, 5001, Side::Sell, kLimit, 10));
  // t0 + L/2: the 100 ask is pulled and replaced at 105, before our arrival.
  scenario.script().group(
      MarketGroup{cancel_order_event(kInstrument, kStart + kLatency / 2, 2,
                                     5001, Side::Sell, kLimit, 10),
                  add_order(kInstrument, kStart + kLatency / 2, 3, 5002,
                            Side::Sell, 105, 10)});
  // t0 + 2L: a later delivery so the run outlives our order's arrival.
  scenario.script().group(add_order(kInstrument, kStart + 2 * kLatency, 4, 5003,
                                    Side::Sell, 106, 1));

  BuyOnFirstBook strategy;
  RecordingRecorder recorder;
  std::vector<OrderQueryRow> open_at_end;
  scenario.run(strategy, recorder, [&](TradingEngine &engine) {
    const auto open = engine.open_orders(kInstrument);
    open_at_end.assign(open.begin(), open.end());
  });

  REQUIRE(strategy.submitted);
  REQUIRE(strategy.fills.empty());
  REQUIRE(recorder.fills.empty());
  REQUIRE_FALSE(strategy.saw(RecordingStrategy::Kind::Fill));

  REQUIRE(recorder.has(OrderLogEventType::Submit));
  REQUIRE(recorder.has(OrderLogEventType::Accepted));
  REQUIRE_FALSE(recorder.has(OrderLogEventType::Fill));

  REQUIRE(open_at_end.size() == 1);
  REQUIRE(open_at_end[0].state == OrderState::Open);
  REQUIRE(open_at_end[0].limit_price_ticks == kLimit);
  REQUIRE(open_at_end[0].remaining_quantity == 1);
}

TEST_CASE(
    "Mirror: the same order fills when the touch is still there on arrival",
    "[Latency]") {
  auto scenario = make_scenario(kLatency);
  // Identical to the look-ahead case except the 100 ask is never pulled.
  scenario.script().group(
      add_order(kInstrument, kStart, 1, 5001, Side::Sell, kLimit, 10));
  scenario.script().group(add_order(kInstrument, kStart + kLatency / 2, 2, 5002,
                                    Side::Sell, 105, 10));
  scenario.script().group(add_order(kInstrument, kStart + 2 * kLatency, 3, 5003,
                                    Side::Sell, 106, 1));

  BuyOnFirstBook strategy;
  RecordingRecorder recorder;
  std::vector<OrderQueryRow> open_at_end;
  scenario.run(strategy, recorder, [&](TradingEngine &engine) {
    const auto open = engine.open_orders(kInstrument);
    open_at_end.assign(open.begin(), open.end());
  });

  REQUIRE(strategy.fills.size() == 1);
  REQUIRE(strategy.fills[0].client_order_id == strategy.order_id);
  REQUIRE(strategy.fills[0].price == kLimit);
  REQUIRE(strategy.fills[0].quantity == 1);
  REQUIRE(strategy.fills[0].remaining_quantity == 0);
  // The fill happens on arrival, not on submission.
  REQUIRE(strategy.fills[0].engine_ts_ns == kStart + kLatency);

  REQUIRE(recorder.has(OrderLogEventType::Fill));
  REQUIRE(recorder.order_log.back().state == OrderState::Filled);
  REQUIRE(open_at_end.empty());
}

TEST_CASE("A one-nanosecond latency still defers the fill to arrival",
          "[Latency]") {
  // The engine rejects order_latency_ns <= 0, so the smallest causal setting is
  // 1 ns. This pins the look-ahead cases to latency rather than to matching:
  // with an effectively instant order the same script fills immediately.
  auto scenario = make_scenario(1);
  scenario.script().group(
      add_order(kInstrument, kStart, 1, 5001, Side::Sell, kLimit, 10));
  scenario.script().group(
      MarketGroup{cancel_order_event(kInstrument, kStart + kLatency / 2, 2,
                                     5001, Side::Sell, kLimit, 10),
                  add_order(kInstrument, kStart + kLatency / 2, 3, 5002,
                            Side::Sell, 105, 10)});

  BuyOnFirstBook strategy;
  RecordingRecorder recorder;
  scenario.run(strategy, recorder);

  REQUIRE(strategy.fills.size() == 1);
  REQUIRE(strategy.fills[0].price == kLimit);
  REQUIRE(strategy.fills[0].engine_ts_ns == kStart + 1);
}

TEST_CASE("Equal-timestamp market data is delivered before our order arrives",
          "[Latency]") {
  // EventPriority orders MarketData(0) < NewOrder(1) < Cancel(2), so a market
  // group landing on exactly the arrival timestamp is applied first. Here the
  // touch is pulled at exactly t0 + L, which must therefore prevent the fill.
  auto scenario = make_scenario(kLatency);
  scenario.script().group(
      add_order(kInstrument, kStart, 1, 5001, Side::Sell, kLimit, 10));
  scenario.script().group(cancel_order_event(kInstrument, kStart + kLatency, 2,
                                             5001, Side::Sell, kLimit, 10));

  BuyOnFirstBook strategy;
  RecordingRecorder recorder;
  scenario.run(strategy, recorder);

  REQUIRE(strategy.fills.empty());
  REQUIRE_FALSE(recorder.has(OrderLogEventType::Fill));
  REQUIRE(recorder.has(OrderLogEventType::Accepted));
}
