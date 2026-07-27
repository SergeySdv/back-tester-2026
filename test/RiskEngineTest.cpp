// Risk-engine coverage: one case per reject reason, and for each one a mirror
// that is accepted. A reject-only suite passes just as happily against a gate
// that refuses everything, which is the failure mode worth guarding against.
//
// Limit cases deliberately test the boundary: a value exactly at the limit is
// allowed and only the next one past it is refused.

#include "EngineHarness.hpp"

#include "MiniTest.hpp"

#include <array>
#include <limits>
#include <vector>

namespace {

using namespace cmf;
using namespace cmf::trading;
using namespace harness;

constexpr InstrumentId kInstrument = 1;
constexpr TimestampNs kLatency = 100;

// tick_size_ticks = 2, so odd prices are misaligned.
const std::array<InstrumentMeta, 1> kTicked{
    InstrumentMeta{kInstrument, 2, 1, 1}};

RiskEngine make_engine(RiskLimits limits) {
  return RiskEngine{kTicked, limits};
}

RejectReason check(const RiskEngine &engine, Side side, PriceTicks price,
                   Quantity quantity, Quantity net_position = 0,
                   std::size_t open_orders = 0,
                   InstrumentId instrument_id = kInstrument) {
  return engine.check_new_order(instrument_id, side, price, quantity,
                                net_position, open_orders);
}

} // namespace

// --------------------------------------------------------------------------
// Validity rules
// --------------------------------------------------------------------------

TEST_CASE("Risk engine rejects an unknown instrument", "[Risk]") {
  const auto engine = make_engine(RiskLimits{});
  REQUIRE(check(engine, Side::Buy, 100, 1, 0, 0, 99) ==
          RejectReason::UnknownInstrument);
  REQUIRE(check(engine, Side::Buy, 100, 1) == RejectReason::None);
}

TEST_CASE("Risk engine rejects an invalid side", "[Risk]") {
  const auto engine = make_engine(RiskLimits{});
  REQUIRE(check(engine, Side::None, 100, 1) == RejectReason::InvalidSide);
  REQUIRE(check(engine, static_cast<Side>(2), 100, 1) ==
          RejectReason::InvalidSide);
  REQUIRE(check(engine, Side::Sell, 100, 1) == RejectReason::None);
}

TEST_CASE("Risk engine rejects a non-positive quantity", "[Risk]") {
  const auto engine = make_engine(RiskLimits{});
  REQUIRE(check(engine, Side::Buy, 100, 0) ==
          RejectReason::NonPositiveQuantity);
  REQUIRE(check(engine, Side::Buy, 100, -5) ==
          RejectReason::NonPositiveQuantity);
  REQUIRE(check(engine, Side::Buy, 100, 1) == RejectReason::None);
}

TEST_CASE("Risk engine rejects a non-positive price", "[Risk]") {
  const auto engine = make_engine(RiskLimits{});
  REQUIRE(check(engine, Side::Buy, 0, 1) == RejectReason::InvalidPrice);
  REQUIRE(check(engine, Side::Buy, -100, 1) == RejectReason::InvalidPrice);
  REQUIRE(check(engine, Side::Buy, 100, 1) == RejectReason::None);
}

TEST_CASE("Risk engine rejects a price off the tick grid", "[Risk]") {
  const auto engine = make_engine(RiskLimits{});
  // tick_size_ticks is 2 for this instrument.
  REQUIRE(check(engine, Side::Buy, 101, 1) == RejectReason::TickMisalignment);
  REQUIRE(check(engine, Side::Buy, 102, 1) == RejectReason::None);
}

// --------------------------------------------------------------------------
// Limits
// --------------------------------------------------------------------------

TEST_CASE("Risk engine caps the size of a single order", "[Risk]") {
  RiskLimits limits;
  limits.max_order_quantity = 10;
  const auto engine = make_engine(limits);

  REQUIRE(check(engine, Side::Buy, 100, 11) ==
          RejectReason::OrderQuantityLimitExceeded);
  // Exactly at the limit is allowed.
  REQUIRE(check(engine, Side::Buy, 100, 10) == RejectReason::None);
}

TEST_CASE("Risk engine caps the number of open orders per instrument",
          "[Risk]") {
  RiskLimits limits;
  limits.max_open_orders_per_instrument = 3;
  const auto engine = make_engine(limits);

  REQUIRE(check(engine, Side::Buy, 100, 1, 0, 3) ==
          RejectReason::TooManyOpenOrders);
  // One slot left.
  REQUIRE(check(engine, Side::Buy, 100, 1, 0, 2) == RejectReason::None);
}

TEST_CASE("Risk engine caps the position this order could create", "[Risk]") {
  RiskLimits limits;
  limits.max_position_abs = 10;
  const auto engine = make_engine(limits);

  // Flat, one order past the cap.
  REQUIRE(check(engine, Side::Buy, 100, 11, 0) ==
          RejectReason::PositionLimitExceeded);
  REQUIRE(check(engine, Side::Buy, 100, 10, 0) == RejectReason::None);

  // Already long 8: two more is exactly the cap, three is past it.
  REQUIRE(check(engine, Side::Buy, 100, 2, 8) == RejectReason::None);
  REQUIRE(check(engine, Side::Buy, 100, 3, 8) ==
          RejectReason::PositionLimitExceeded);

  // The cap is on absolute size, so the short side is symmetric.
  REQUIRE(check(engine, Side::Sell, 100, 2, -8) == RejectReason::None);
  REQUIRE(check(engine, Side::Sell, 100, 3, -8) ==
          RejectReason::PositionLimitExceeded);

  // Reducing an existing position is never a breach.
  REQUIRE(check(engine, Side::Sell, 100, 10, 8) == RejectReason::None);
}

TEST_CASE("Risk engine treats an unrepresentable position as a breach",
          "[Risk]") {
  RiskLimits limits;
  limits.max_position_abs = std::numeric_limits<Quantity>::max();
  const auto engine = make_engine(limits);
  REQUIRE(check(engine, Side::Buy, 100, std::numeric_limits<Quantity>::max(),
                1) == RejectReason::PositionLimitExceeded);
}

TEST_CASE("Default risk limits accept everything the validity rules allow",
          "[Risk]") {
  // Guards the promise that turning the risk engine on cannot change the
  // behaviour of an existing configuration.
  const auto engine = make_engine(RiskLimits{});
  REQUIRE(check(engine, Side::Buy, 100,
                std::numeric_limits<Quantity>::max() - 1, 0,
                1'000'000) == RejectReason::None);
}

// --------------------------------------------------------------------------
// Ordering of checks
// --------------------------------------------------------------------------

TEST_CASE("Validity is reported before limits when both are violated",
          "[Risk]") {
  RiskLimits limits;
  limits.max_order_quantity = 1;
  const auto engine = make_engine(limits);
  // Misaligned price and an oversized quantity at once.
  REQUIRE(check(engine, Side::Buy, 101, 500) == RejectReason::TickMisalignment);
}

TEST_CASE("Order size is reported before open orders and position", "[Risk]") {
  RiskLimits limits;
  limits.max_order_quantity = 1;
  limits.max_open_orders_per_instrument = 1;
  limits.max_position_abs = 1;
  const auto engine = make_engine(limits);
  REQUIRE(check(engine, Side::Buy, 100, 50, 0, 5) ==
          RejectReason::OrderQuantityLimitExceeded);
}

TEST_CASE("Open orders are reported before position", "[Risk]") {
  RiskLimits limits;
  limits.max_open_orders_per_instrument = 1;
  limits.max_position_abs = 1;
  const auto engine = make_engine(limits);
  REQUIRE(check(engine, Side::Buy, 100, 50, 0, 5) ==
          RejectReason::TooManyOpenOrders);
}

TEST_CASE("Risk engine rejects a non-positive order quantity limit", "[Risk]") {
  RiskLimits limits;
  limits.max_order_quantity = 0;
  bool rejected = false;
  try {
    const auto engine = make_engine(limits);
    (void)engine;
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  REQUIRE(rejected);
}

// --------------------------------------------------------------------------
// Behaviour through the whole engine
// --------------------------------------------------------------------------

namespace {

const std::vector<InstrumentMeta> kInstruments{
    InstrumentMeta{kInstrument, 1, 1, 1}};

// Sends `count` buy orders on the first book update.
struct BurstStrategy final : RecordingStrategy {
  int count{1};
  Quantity quantity{1};
  std::vector<ClOrdId> ids;
  bool done{};
  TimestampNs submitted_at{};

  void on_book_update(const BookUpdateView &update,
                      StrategyContext &context) override {
    RecordingStrategy::on_book_update(update, context);
    if (done) {
      return;
    }
    done = true;
    submitted_at = context.now_ns();
    for (int index = 0; index < count; ++index) {
      ids.push_back(context.submit_limit(kInstrument, Side::Buy, 90, quantity));
    }
  }
};

Scenario limited_scenario(RiskLimits limits) {
  BacktestConfig config{0, kLatency, 15, limits};
  return Scenario{kInstruments, config};
}

} // namespace

TEST_CASE("A risk-rejected order never reaches the book", "[Risk]") {
  RiskLimits limits;
  limits.max_order_quantity = 5;
  auto scenario = limited_scenario(limits);
  scenario.script().group(
      add_order(kInstrument, 1000, 1, 8001, Side::Sell, 105, 10));
  scenario.script().group(
      add_order(kInstrument, 1500, 2, 8002, Side::Sell, 106, 10));

  BurstStrategy strategy;
  strategy.quantity = 6; // one past the limit
  RecordingRecorder recorder;
  std::vector<OrderQueryRow> open_at_end;
  scenario.run(strategy, recorder, [&](TradingEngine &engine) {
    const auto open = engine.open_orders(kInstrument);
    open_at_end.assign(open.begin(), open.end());
  });

  REQUIRE(strategy.rejects.size() == 1);
  REQUIRE(strategy.rejects[0].reason ==
          RejectReason::OrderQuantityLimitExceeded);
  // Rejected locally, so it never became a scheduled command and never
  // reached the open-order index.
  REQUIRE_FALSE(recorder.has(OrderLogEventType::Accepted));
  REQUIRE(open_at_end.empty());
  REQUIRE(recorder.order_log.back().event_type == OrderLogEventType::Reject);
  REQUIRE(recorder.order_log.back().state == OrderState::Rejected);
}

TEST_CASE("A risk reject costs no order latency", "[Risk]") {
  RiskLimits limits;
  limits.max_order_quantity = 5;
  auto scenario = limited_scenario(limits);
  scenario.script().group(
      add_order(kInstrument, 1000, 1, 8001, Side::Sell, 105, 10));

  BurstStrategy strategy;
  strategy.quantity = 6;
  RecordingRecorder recorder;
  scenario.run(strategy, recorder);

  REQUIRE(strategy.rejects.size() == 1);
  // The reject is known locally, so it lands at submit time, not submit + L.
  REQUIRE(strategy.rejects[0].engine_ts_ns == strategy.submitted_at);
  REQUIRE(strategy.submitted_at == 1000);
}

TEST_CASE("Mirror: an order inside the limits reaches the book", "[Risk]") {
  RiskLimits limits;
  limits.max_order_quantity = 5;
  auto scenario = limited_scenario(limits);
  scenario.script().group(
      add_order(kInstrument, 1000, 1, 8001, Side::Sell, 105, 10));
  scenario.script().group(
      add_order(kInstrument, 1500, 2, 8002, Side::Sell, 106, 10));

  BurstStrategy strategy;
  strategy.quantity = 5; // exactly at the limit
  RecordingRecorder recorder;
  std::vector<OrderQueryRow> open_at_end;
  scenario.run(strategy, recorder, [&](TradingEngine &engine) {
    const auto open = engine.open_orders(kInstrument);
    open_at_end.assign(open.begin(), open.end());
  });

  REQUIRE(strategy.rejects.empty());
  REQUIRE(recorder.has(OrderLogEventType::Accepted));
  REQUIRE(open_at_end.size() == 1);
  REQUIRE(open_at_end[0].state == OrderState::Open);
}

TEST_CASE("The open-order limit counts orders already resting", "[Risk]") {
  RiskLimits limits;
  limits.max_open_orders_per_instrument = 2;
  auto scenario = limited_scenario(limits);
  scenario.script().group(
      add_order(kInstrument, 1000, 1, 8001, Side::Sell, 105, 10));
  scenario.script().group(
      add_order(kInstrument, 1500, 2, 8002, Side::Sell, 106, 10));

  BurstStrategy strategy;
  strategy.count = 3; // third one has no slot left
  RecordingRecorder recorder;
  std::vector<OrderQueryRow> open_at_end;
  scenario.run(strategy, recorder, [&](TradingEngine &engine) {
    const auto open = engine.open_orders(kInstrument);
    open_at_end.assign(open.begin(), open.end());
  });

  REQUIRE(strategy.rejects.size() == 1);
  REQUIRE(strategy.rejects[0].reason == RejectReason::TooManyOpenOrders);
  REQUIRE(open_at_end.size() == 2);
}

TEST_CASE("Limits come from the config rather than a hard-coded default",
          "[Risk]") {
  // Same script, same strategy, two different configs: only the limit differs.
  auto run_with = [](Quantity cap) {
    RiskLimits limits;
    limits.max_order_quantity = cap;
    auto scenario = limited_scenario(limits);
    scenario.script().group(
        add_order(kInstrument, 1000, 1, 8001, Side::Sell, 105, 10));
    BurstStrategy strategy;
    strategy.quantity = 4;
    RecordingRecorder recorder;
    scenario.run(strategy, recorder);
    return strategy.rejects.size();
  };

  REQUIRE(run_with(3) == 1);
  REQUIRE(run_with(4) == 0);
}
