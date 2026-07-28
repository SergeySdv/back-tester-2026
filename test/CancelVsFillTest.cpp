// A cancel is not instantaneous: it leaves the strategy at t1 and reaches the
// book at t1 + order_latency. If the order trades in that window the fill wins
// and the cancel must come back as a reject, with no Cancelled row in the order
// log. Every race case is mirrored by a case where the cancel wins.
//
// NOTE ON THE REJECT REASON. The engine currently answers a lost race with
// RejectReason::AlreadyTerminal, which it also uses for a duplicate cancel on
// an already-cancelled order. Those are different events for a strategy: one
// means "you lost a race", the other means "you sent that twice". A dedicated
// CancelTooLate value is proposed but not adopted yet, so these tests assert
// today's contract and will need one line changed if it lands.

#include "EngineHarness.hpp"

#include "MiniTest.hpp"

#include <vector>

namespace {

using namespace cmf;
using namespace harness;

constexpr InstrumentId kInstrument = 1;
constexpr TimestampNs kLatency = 100;
constexpr PriceTicks kLimit = 100;

const std::vector<InstrumentMeta> kInstruments{
    InstrumentMeta{kInstrument, 1, 1, 1}};

// Submits on the first book update, cancels on the second. By then the order
// has arrived and is Open, which is the only state a cancel may target.
struct SubmitThenCancel final : RecordingStrategy {
  ClOrdId order_id{};
  int book_updates{};
  bool cancel_accepted{};
  bool cancel_sent{};

  void on_book_update(const BookUpdateView &update,
                      StrategyContext &context) override {
    RecordingStrategy::on_book_update(update, context);
    ++book_updates;
    if (book_updates == 1) {
      order_id = context.submit_limit(kInstrument, Side::Buy, kLimit, 5);
    } else if (book_updates == 2 && !cancel_sent) {
      cancel_sent = true;
      cancel_accepted = context.cancel_order(order_id);
    }
  }
};

Scenario make_scenario() {
  return Scenario{kInstruments, BacktestConfig{0, kLatency, 15}};
}

// Book updates that never cross a buy at kLimit, used only to drive callbacks.
void quiet_book(Script &script, TimestampNs ts, Sequence seq,
                ExchangeOrderId order_id, PriceTicks price) {
  script.group(
      add_order(kInstrument, ts, seq, order_id, Side::Sell, price, 10));
}

} // namespace

TEST_CASE("A fill during the cancel flight wins and the cancel is rejected",
          "[CancelRace]") {
  auto scenario = make_scenario();
  quiet_book(scenario.script(), 1000, 1, 9001, 105); // submit here
  quiet_book(scenario.script(), 1200, 2, 9002, 106); // cancel here, lands 1300
  // 1250: inside the cancel flight, a trade prints through our limit.
  scenario.script().group(trade(kInstrument, 1250, 3, Side::Sell, kLimit, 5));
  quiet_book(scenario.script(), 1500, 4, 9003, 107);

  SubmitThenCancel strategy;
  RecordingRecorder recorder;
  std::vector<OrderQueryRow> open_at_end;
  Quantity net{};
  scenario.run(strategy, recorder, [&](TradingEngine &engine) {
    const auto open = engine.open_orders(kInstrument);
    open_at_end.assign(open.begin(), open.end());
    net = engine.position(kInstrument).net_quantity;
  });

  REQUIRE(strategy.cancel_sent);
  // The synchronous return only means the cancel was queued, not that it won.
  REQUIRE(strategy.cancel_accepted);

  REQUIRE(strategy.fills.size() == 1);
  REQUIRE(strategy.fills[0].client_order_id == strategy.order_id);
  REQUIRE(strategy.fills[0].engine_ts_ns == 1250);
  REQUIRE(strategy.fills[0].remaining_quantity == 0);
  REQUIRE(net == 5);

  // The cancel loses: rejected on arrival, never applied.
  REQUIRE(strategy.rejects.size() == 1);
  REQUIRE(strategy.rejects[0].client_order_id == strategy.order_id);
  REQUIRE(strategy.rejects[0].reason == RejectReason::AlreadyTerminal);
  REQUIRE(strategy.rejects[0].engine_ts_ns == 1300);

  REQUIRE_FALSE(recorder.has(OrderLogEventType::Cancelled));
  REQUIRE(recorder.count(OrderLogEventType::Fill) == 1);
  REQUIRE(recorder.has(OrderLogEventType::CancelRequest));
  REQUIRE(open_at_end.empty());
}

TEST_CASE(
    "Mirror: a cancel that lands before the trade wins and blocks the fill",
    "[CancelRace]") {
  auto scenario = make_scenario();
  quiet_book(scenario.script(), 1000, 1, 9001, 105); // submit here
  quiet_book(scenario.script(), 1200, 2, 9002, 106); // cancel here, lands 1300
  // 1400: the very same trade, now after the cancel has been applied.
  scenario.script().group(trade(kInstrument, 1400, 3, Side::Sell, kLimit, 5));
  quiet_book(scenario.script(), 1500, 4, 9003, 107);

  SubmitThenCancel strategy;
  RecordingRecorder recorder;
  std::vector<OrderQueryRow> open_at_end;
  Quantity net{};
  scenario.run(strategy, recorder, [&](TradingEngine &engine) {
    const auto open = engine.open_orders(kInstrument);
    open_at_end.assign(open.begin(), open.end());
    net = engine.position(kInstrument).net_quantity;
  });

  REQUIRE(strategy.fills.empty());
  REQUIRE(recorder.fills.empty());
  REQUIRE(strategy.rejects.empty());
  REQUIRE(net == 0);

  REQUIRE(recorder.has(OrderLogEventType::Cancelled));
  REQUIRE(recorder.order_log.back().state == OrderState::Cancelled);
  REQUIRE(open_at_end.empty());
}

TEST_CASE("Cancelling an unknown order id is rejected immediately",
          "[CancelRace]") {
  struct CancelUnknown final : RecordingStrategy {
    bool done{};
    bool returned{true};
    TimestampNs called_at{};

    void on_book_update(const BookUpdateView &update,
                        StrategyContext &context) override {
      RecordingStrategy::on_book_update(update, context);
      if (!done) {
        done = true;
        called_at = context.now_ns();
        returned = context.cancel_order(999);
      }
    }
  };

  auto scenario = make_scenario();
  quiet_book(scenario.script(), 1000, 1, 9001, 105);

  CancelUnknown strategy;
  RecordingRecorder recorder;
  scenario.run(strategy, recorder);

  REQUIRE_FALSE(strategy.returned);
  REQUIRE(strategy.rejects.size() == 1);
  REQUIRE(strategy.rejects[0].reason == RejectReason::UnknownOrder);
  // A locally detected reject costs no order latency.
  REQUIRE(strategy.rejects[0].engine_ts_ns == strategy.called_at);
}

TEST_CASE(
    "A second cancel on an in-flight cancel is rejected without a queue slot",
    "[CancelRace]") {
  struct DoubleCancel final : RecordingStrategy {
    ClOrdId order_id{};
    int book_updates{};
    bool first_result{};
    bool second_result{true};

    void on_book_update(const BookUpdateView &update,
                        StrategyContext &context) override {
      RecordingStrategy::on_book_update(update, context);
      ++book_updates;
      if (book_updates == 1) {
        order_id = context.submit_limit(kInstrument, Side::Buy, kLimit, 5);
      } else if (book_updates == 2) {
        first_result = context.cancel_order(order_id);
        second_result = context.cancel_order(order_id);
      }
    }
  };

  auto scenario = make_scenario();
  quiet_book(scenario.script(), 1000, 1, 9001, 105);
  quiet_book(scenario.script(), 1200, 2, 9002, 106);
  quiet_book(scenario.script(), 1500, 3, 9003, 107);

  DoubleCancel strategy;
  RecordingRecorder recorder;
  scenario.run(strategy, recorder);

  REQUIRE(strategy.first_result);
  REQUIRE_FALSE(strategy.second_result);
  REQUIRE(strategy.rejects.size() == 1);
  REQUIRE(strategy.rejects[0].reason == RejectReason::AlreadyTerminal);
  // Exactly one cancel reached the book.
  REQUIRE(recorder.count(OrderLogEventType::CancelRequest) == 1);
  REQUIRE(recorder.count(OrderLogEventType::Cancelled) == 1);
}
