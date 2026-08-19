#include "market/HistoricalLOBStore.hpp"
#include "scheduler/SchedulerRuntime.hpp"
#include "trading/TradingEngine.hpp"

#include "MiniTest.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace cmf;
using namespace cmf::market;
using namespace cmf::scheduler;
using namespace cmf::trading;

MarketDataEvent market_event(Sequence seq, TimestampNs time,
                             ExchangeOrderId order_id, MarketAction action,
                             Side side, PriceTicks price, Quantity quantity) {
  return MarketDataEvent{time,   time, 1,     order_id, seq,
                         action, side, price, quantity, 128};
}

struct RecordingStrategy : Strategy {
  std::vector<FillView> fills;
  std::vector<RejectView> rejects;
  std::vector<PositionSnapshot> positions_in_fill;
  std::vector<std::size_t> open_counts_in_fill;

  void on_fill(const FillView &fill, StrategyContext &context) override {
    fills.push_back(fill);
    positions_in_fill.push_back(context.position(fill.instrument_id));
    open_counts_in_fill.push_back(
        context.open_orders(fill.instrument_id).size());
  }

  void on_reject(const RejectView &reject, StrategyContext &) override {
    rejects.push_back(reject);
  }
};

struct RecordingRecorder final : Recorder {
  std::vector<OrderLogResultRow> orders;
  std::vector<FillResultRow> fills;
  std::vector<RejectView> rejects;

  void on_order_event(const OrderLogResultRow &row) override {
    orders.push_back(row);
  }
  void on_fill(const FillResultRow &row) override { fills.push_back(row); }
  void on_reject(const RejectView &row) override { rejects.push_back(row); }
};

struct SubmitOnFirstMarket : RecordingStrategy {
  ClOrdId order_id{};
  Side side{Side::Buy};
  PriceTicks price{102};
  Quantity quantity{10};
  bool submitted{};

  void on_book_update(const BookUpdateView &,
                      StrategyContext &context) override {
    if (!submitted) {
      submitted = true;
      order_id = context.submit_limit(1, side, price, quantity);
    }
  }
};

BookUpdateView empty_book_view(TimestampNs time, Sequence seq) {
  return BookUpdateView{1, time, time, seq, false, {}, {}};
}

PriceCrossSignal quote_signal(TimestampNs time, Sequence seq,
                              std::optional<PriceTicks> best_bid,
                              std::optional<PriceTicks> best_ask,
                              InstrumentId instrument_id = 1) {
  return PriceCrossSignal{
      instrument_id, time,     time,        seq, PriceCrossSource::BestQuote,
      best_bid,      best_ask, std::nullopt};
}

PriceCrossSignal trade_signal(TimestampNs time, Sequence seq, PriceTicks price,
                              InstrumentId instrument_id = 1) {
  return PriceCrossSignal{
      instrument_id,           time,         time,         seq,
      PriceCrossSource::Trade, std::nullopt, std::nullopt, price};
}

const std::array<InstrumentMeta, 1> instruments{InstrumentMeta{1, 1, 100, 10}};

} // namespace

TEST_CASE("Trading engine validates instrument metadata", "[Trading]") {
  HistoricalLOBStore books;
  RecordingStrategy strategy;
  RecordingRecorder recorder;
  bool rejected = false;
  try {
    const std::array invalid{InstrumentMeta{1, 0, 100, 1}};
    TradingEngine engine(invalid, BacktestConfig{0, 1, 1}, books, strategy,
                         recorder);
    (void)engine;
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  REQUIRE(rejected);
}

TEST_CASE("Trading engine requires causal positive order latency",
          "[Trading]") {
  HistoricalLOBStore books;
  RecordingStrategy strategy;
  RecordingRecorder recorder;
  for (const TimestampNs invalid_latency : {TimestampNs{0}, TimestampNs{-1}}) {
    bool rejected = false;
    try {
      TradingEngine engine(instruments, BacktestConfig{0, invalid_latency, 1},
                           books, strategy, recorder);
      (void)engine;
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    REQUIRE(rejected);
  }
}

TEST_CASE("Delayed order fully fills at best quote without volume cap",
          "[Trading]") {
  HistoricalLOBStore books;
  books.apply(market_event(1, 1, 101, MarketAction::Add, Side::Sell, 101, 4));
  books.apply(market_event(2, 2, 102, MarketAction::Add, Side::Sell, 102, 6));

  SubmitOnFirstMarket strategy;
  RecordingRecorder recorder;
  TradingEngine engine(instruments, BacktestConfig{0, 25, 1}, books, strategy,
                       recorder);
  const std::array<BookLevel, 1> asks{{BookLevel{101, 10}}};
  const BookUpdateView view{1, 100, 100, 3, false, {}, asks};
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 3, view, {}, {}}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.order_id == 1);
  REQUIRE(strategy.fills.size() == 1);
  REQUIRE(strategy.fills[0].engine_ts_ns == 125);
  REQUIRE(strategy.fills[0].price == 101);
  REQUIRE(strategy.fills[0].quantity == 10);
  REQUIRE(strategy.fills[0].liquidity_source == LiquiditySource::QuoteCross);
  REQUIRE(strategy.fills[0].trigger_source_sequence == 2);
  REQUIRE(recorder.fills[0].liquidity_source == LiquiditySource::QuoteCross);
  REQUIRE(recorder.fills[0].trigger_source_sequence == 2);
  REQUIRE(strategy.positions_in_fill.back().net_quantity == 10);
  REQUIRE(strategy.open_counts_in_fill.back() == 0);
  REQUIRE(runtime.processed_sequence() == 2);
}

TEST_CASE("Resting oversized order fully fills on the first quote cross",
          "[Trading]") {
  HistoricalLOBStore books;
  books.apply(market_event(1, 1, 50, MarketAction::Add, Side::Sell, 101, 5));
  SubmitOnFirstMarket strategy;
  strategy.price = 100;
  strategy.quantity = 8;
  RecordingRecorder recorder;
  TradingEngine engine(instruments, BacktestConfig{0, 1, 1}, books, strategy,
                       recorder);
  const auto view100 = empty_book_view(100, 10);
  const auto view200 = empty_book_view(200, 20);
  const std::array cross{quote_signal(200, 20, std::nullopt, 100)};
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 10, view100, {}, {}}},
      ScheduledEvent{MarketDelivery{1, 200, 200, 20, view200, {}, cross}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.fills.size() == 1);
  REQUIRE(strategy.fills[0].quantity == 8);
  REQUIRE(strategy.fills[0].engine_ts_ns == 200);
  REQUIRE(strategy.fills[0].price == 100);
  REQUIRE(strategy.fills[0].trigger_source_sequence == 20);
  REQUIRE(engine.position(1).net_quantity == 8);
}

TEST_CASE("Only post-arrival same-instrument trade cross fully fills",
          "[Trading]") {
  HistoricalLOBStore books;
  struct TradeStrategy final : SubmitOnFirstMarket {
    std::vector<std::size_t> fills_before_trade;

    TradeStrategy() {
      price = 100;
      quantity = 50;
    }

    void on_trade(const TradeView &, StrategyContext &) override {
      fills_before_trade.push_back(fills.size());
    }
  } strategy;
  RecordingRecorder recorder;
  const std::array two_instruments{InstrumentMeta{1, 1, 100, 10},
                                   InstrumentMeta{2, 1, 100, 10}};
  TradingEngine engine(two_instruments, BacktestConfig{0, 5, 1}, books,
                       strategy, recorder);

  const auto initial_view = empty_book_view(100, 1);
  const std::array early_trade{TradeView{1, 102, 102, 2, Side::Buy, 99, 1}};
  const std::array early_signal{trade_signal(102, 2, 99)};
  const std::array other_trade{TradeView{2, 106, 106, 3, Side::Sell, 99, 1}};
  const std::array other_signal{trade_signal(106, 3, 99, 2)};
  const std::array winning_trade{TradeView{1, 110, 110, 4, Side::None, 100, 1}};
  const std::array winning_signal{trade_signal(110, 4, 100)};
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 1, initial_view, {}, {}}},
      ScheduledEvent{
          MarketDelivery{1, 102, 102, 2, {}, early_trade, early_signal}},
      ScheduledEvent{
          MarketDelivery{2, 106, 106, 3, {}, other_trade, other_signal}},
      ScheduledEvent{
          MarketDelivery{1, 110, 110, 4, {}, winning_trade, winning_signal}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.fills.size() == 1);
  REQUIRE(strategy.fills[0].quantity == 50);
  REQUIRE(strategy.fills[0].price == 100);
  REQUIRE(strategy.fills[0].engine_ts_ns == 110);
  REQUIRE(strategy.fills[0].liquidity_source == LiquiditySource::TradeCross);
  REQUIRE(strategy.fills[0].trigger_source_sequence == 4);
  REQUIRE(strategy.fills_before_trade == std::vector<std::size_t>({0, 0, 1}));
  REQUIRE(engine.position(1).net_quantity == 50);
  REQUIRE(engine.position(2).net_quantity == 0);
  REQUIRE(recorder.fills[0].liquidity_source == LiquiditySource::TradeCross);
  REQUIRE(recorder.fills[0].trigger_source_sequence == 4);
}

TEST_CASE(
    "Post-arrival same-instrument non-crossing quote and trade do not fill",
    "[Trading]") {
  HistoricalLOBStore books;
  SubmitOnFirstMarket strategy;
  strategy.price = 100;
  strategy.quantity = 9;
  RecordingRecorder recorder;
  TradingEngine engine(instruments, BacktestConfig{0, 5, 1}, books, strategy,
                       recorder);

  const auto initial = empty_book_view(100, 1);
  const auto later = empty_book_view(120, 3);
  const std::array quote{quote_signal(110, 2, std::nullopt, 101)};
  const std::array trade{trade_signal(120, 3, 101)};
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 1, initial, {}, {}}},
      ScheduledEvent{MarketDelivery{1, 110, 110, 2, {}, {}, quote}},
      ScheduledEvent{MarketDelivery{1, 120, 120, 3, later, {}, trade}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.fills.empty());
  REQUIRE(recorder.fills.empty());
  const auto open = engine.open_orders(1);
  REQUIRE(open.size() == 1);
  REQUIRE(open[0].state == OrderState::Open);
  REQUIRE(open[0].remaining_quantity == 9);
}

TEST_CASE("Own price priority precedes arrival FIFO across different prices",
          "[Trading]") {
  HistoricalLOBStore books;
  struct TwoPrices final : RecordingStrategy {
    std::array<ClOrdId, 2> ids{};
    bool submitted{};

    void on_book_update(const BookUpdateView &,
                        StrategyContext &context) override {
      if (!submitted) {
        submitted = true;
        ids[0] = context.submit_limit(1, Side::Buy, 99, 1);
        ids[1] = context.submit_limit(1, Side::Buy, 100, 1);
      }
    }
  } strategy;
  RecordingRecorder recorder;
  TradingEngine engine(instruments, BacktestConfig{0, 5, 1}, books, strategy,
                       recorder);

  const auto initial = empty_book_view(100, 1);
  const std::array cross{quote_signal(110, 2, std::nullopt, 99)};
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 1, initial, {}, {}}},
      ScheduledEvent{MarketDelivery{1, 110, 110, 2, {}, {}, cross}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.fills.size() == 2);
  REQUIRE(strategy.fills[0].client_order_id == strategy.ids[1]);
  REQUIRE(strategy.fills[1].client_order_id == strategy.ids[0]);
  REQUIRE(strategy.fills[0].trigger_source_sequence == 2);
  REQUIRE(strategy.fills[1].trigger_source_sequence == 2);
}

TEST_CASE("Infinite quote liquidity fills every own order and isolated view",
          "[Trading]") {
  HistoricalLOBStore books;
  books.apply(market_event(1, 1, 90, MarketAction::Add, Side::Sell, 101, 3));

  struct TwoOrders final : RecordingStrategy {
    std::vector<ClOrdId> ids;
    bool submitted{};
    void on_book_update(const BookUpdateView &,
                        StrategyContext &context) override {
      if (!submitted) {
        submitted = true;
        ids.push_back(context.submit_limit(1, Side::Buy, 101, 2));
        ids.push_back(context.submit_limit(1, Side::Buy, 101, 2));
      }
    }
  } first, second;
  RecordingRecorder first_recorder;
  RecordingRecorder second_recorder;
  TradingEngine first_engine(instruments, BacktestConfig{0, 1, 1}, books, first,
                             first_recorder);
  TradingEngine second_engine(instruments, BacktestConfig{0, 1, 1}, books,
                              second, second_recorder);
  const auto view = empty_book_view(100, 2);
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 2, view, {}, {}}}};
  SchedulerRuntime first_runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  SchedulerRuntime second_runtime(
      SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  first_runtime.run(events, first_engine);
  second_runtime.run(events, second_engine);

  REQUIRE(first.fills.size() == 2);
  REQUIRE(first.fills[0].client_order_id == first.ids[0]);
  REQUIRE(first.fills[0].quantity == 2);
  REQUIRE(first.fills[1].client_order_id == first.ids[1]);
  REQUIRE(first.fills[1].quantity == 2);
  REQUIRE(second.fills.size() == 2);
  REQUIRE(second_engine.position(1).net_quantity == 4);
}

TEST_CASE("Cancel is delayed and equal-time market fill wins", "[Trading]") {
  HistoricalLOBStore books;
  books.apply(market_event(1, 1, 70, MarketAction::Add, Side::Sell, 101, 2));
  struct CancelStrategy final : RecordingStrategy {
    ClOrdId id{};
    ClOrdId replacement_id{};
    int callbacks{};
    std::size_t fills_seen_in_reject{};

    void on_book_update(const BookUpdateView &,
                        StrategyContext &context) override {
      ++callbacks;
      if (callbacks == 1) {
        id = context.submit_limit(1, Side::Buy, 100, 2);
      } else if (callbacks == 2) {
        REQUIRE(context.cancel_order(id));
      }
    }

    void on_reject(const RejectView &reject,
                   StrategyContext &context) override {
      RecordingStrategy::on_reject(reject, context);
      fills_seen_in_reject = fills.size();
      replacement_id = context.submit_limit(1, Side::Buy, 100, 1);
    }
  } strategy;
  RecordingRecorder recorder;
  TradingEngine engine(instruments, BacktestConfig{0, 10, 1}, books, strategy,
                       recorder);
  const auto view100 = empty_book_view(100, 10);
  const auto view111 = empty_book_view(111, 11);
  const auto view121 = empty_book_view(121, 12);
  const auto view140 = empty_book_view(140, 13);
  const std::array cross121{quote_signal(121, 12, std::nullopt, 100)};
  const std::array cross140{quote_signal(140, 13, std::nullopt, 100)};
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 10, view100, {}, {}}},
      ScheduledEvent{MarketDelivery{1, 111, 111, 11, view111, {}, {}}},
      ScheduledEvent{MarketDelivery{1, 121, 121, 12, view121, {}, cross121}},
      ScheduledEvent{MarketDelivery{1, 140, 140, 13, view140, {}, cross140}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.fills.size() == 2);
  REQUIRE(strategy.fills[0].engine_ts_ns == 121);
  REQUIRE(strategy.fills[0].remaining_quantity == 0);
  REQUIRE(strategy.fills_seen_in_reject == 1);
  REQUIRE(strategy.replacement_id == 2);
  REQUIRE(strategy.fills[1].client_order_id == strategy.replacement_id);
  REQUIRE(strategy.fills[1].engine_ts_ns == 140);
  REQUIRE(strategy.rejects.size() == 1);
  REQUIRE(strategy.rejects[0].reason == RejectReason::AlreadyTerminal);
}

TEST_CASE("Fill during cancel flight wins before delayed cancel arrival",
          "[Trading][CancelRace]")
{
    HistoricalLOBStore books;
    struct CancelInFlight final : RecordingStrategy
    {
        ClOrdId id{};
        int callbacks{};
        bool cancel_queued{};

        void on_book_update(const BookUpdateView&,
                            StrategyContext& context) override
        {
            ++callbacks;
            if (callbacks == 1)
            {
                id = context.submit_limit(1, Side::Buy, 100, 5);
            }
            else if (callbacks == 2)
            {
                cancel_queued = context.cancel_order(id);
            }
        }
    } strategy;
    RecordingRecorder recorder;
    TradingEngine engine(instruments, BacktestConfig{0, 100, 1}, books, strategy,
                         recorder);

    const auto view100 = empty_book_view(100, 1);
    const auto view300 = empty_book_view(300, 2);
    const std::array cross350{trade_signal(350, 3, 100)};
    const std::array events{
        ScheduledEvent{MarketDelivery{1, 100, 100, 1, view100, {}, {}}},
        ScheduledEvent{MarketDelivery{1, 300, 300, 2, view300, {}, {}}},
        ScheduledEvent{MarketDelivery{1, 350, 350, 3, {}, {}, cross350}}};
    SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
    runtime.run(events, engine);

    REQUIRE(strategy.cancel_queued);
    REQUIRE(strategy.fills.size() == 1);
    REQUIRE(strategy.fills[0].client_order_id == strategy.id);
    REQUIRE(strategy.fills[0].engine_ts_ns == 350);
    REQUIRE(strategy.fills[0].remaining_quantity == 0);
    REQUIRE(strategy.rejects.size() == 1);
    REQUIRE(strategy.rejects[0].client_order_id == strategy.id);
    REQUIRE(strategy.rejects[0].reason == RejectReason::AlreadyTerminal);
    REQUIRE(strategy.rejects[0].engine_ts_ns == 400);
    REQUIRE(engine.open_orders(1).empty());
    REQUIRE(std::none_of(recorder.orders.begin(), recorder.orders.end(),
                         [](const OrderLogResultRow& row)
                         {
                             return row.event_type == OrderLogEventType::Cancelled;
                         }));
}

TEST_CASE("Cancel arriving before a later price cross prevents fill",
          "[Trading]") {
  HistoricalLOBStore books;
  struct CancelBeforeCross final : RecordingStrategy {
    ClOrdId id{};
    int callbacks{};

    void on_book_update(const BookUpdateView &,
                        StrategyContext &context) override {
      ++callbacks;
      if (callbacks == 1) {
        id = context.submit_limit(1, Side::Buy, 100, 3);
      } else if (callbacks == 2) {
        REQUIRE(context.cancel_order(id));
      }
    }
  } strategy;
  RecordingRecorder recorder;
  TradingEngine engine(instruments, BacktestConfig{0, 5, 1}, books, strategy,
                       recorder);

  const auto view100 = empty_book_view(100, 1);
  const auto view110 = empty_book_view(110, 2);
  const std::array later_cross{quote_signal(120, 3, std::nullopt, 100)};
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 1, view100, {}, {}}},
      ScheduledEvent{MarketDelivery{1, 110, 110, 2, view110, {}, {}}},
      ScheduledEvent{MarketDelivery{1, 120, 120, 3, {}, {}, later_cross}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.fills.empty());
  REQUIRE(recorder.fills.empty());
  REQUIRE(engine.open_orders(1).empty());
  REQUIRE(recorder.orders.back().event_type == OrderLogEventType::Cancelled);
  REQUIRE(recorder.orders.back().state == OrderState::Cancelled);
}

TEST_CASE("Invalid orders and unknown cancels reject deterministically",
          "[Trading]") {
  HistoricalLOBStore books;
  struct InvalidStrategy final : RecordingStrategy {
    bool done{};
    int callback_depth{};
    int maximum_callback_depth{};
    std::vector<std::string> callback_order;

    void on_book_update(const BookUpdateView &,
                        StrategyContext &context) override {
      if (!done) {
        done = true;
        ++callback_depth;
        maximum_callback_depth =
            std::max(maximum_callback_depth, callback_depth);
        callback_order.push_back("book");
        const ClOrdId terminal = context.submit_limit(2, Side::Buy, 100, 1);
        (void)context.submit_limit(1, Side::None, 100, 1);
        (void)context.submit_limit(1, static_cast<Side>(2), 100, 1);
        (void)context.submit_limit(1, Side::Buy, 101, 1);
        (void)context.submit_limit(1, Side::Buy, 100, 0);
        REQUIRE_FALSE(context.cancel_order(999));
        REQUIRE_FALSE(context.cancel_order(terminal));
        callback_order.push_back("book_done");
        --callback_depth;
      }
    }

    void on_reject(const RejectView &reject,
                   StrategyContext &context) override {
      ++callback_depth;
      maximum_callback_depth = std::max(maximum_callback_depth, callback_depth);
      callback_order.push_back("reject");
      RecordingStrategy::on_reject(reject, context);
      --callback_depth;
    }
  } strategy;
  RecordingRecorder recorder;
  const std::array ticked{InstrumentMeta{1, 2, 100, 1}};
  TradingEngine engine(ticked, BacktestConfig{0, 1, 1}, books, strategy,
                       recorder);
  const auto view = empty_book_view(100, 1);
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 1, view, {}, {}}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.rejects.size() == 7);
  REQUIRE(strategy.rejects[0].reason == RejectReason::UnknownInstrument);
  REQUIRE(strategy.rejects[1].reason == RejectReason::InvalidSide);
  REQUIRE(strategy.rejects[2].reason == RejectReason::InvalidSide);
  REQUIRE(strategy.rejects[3].reason == RejectReason::TickMisalignment);
  REQUIRE(strategy.rejects[4].reason == RejectReason::NonPositiveQuantity);
  REQUIRE(strategy.rejects[5].reason == RejectReason::UnknownOrder);
  REQUIRE(strategy.rejects[6].reason == RejectReason::AlreadyTerminal);
  REQUIRE(strategy.maximum_callback_depth == 1);
  REQUIRE(strategy.callback_order ==
          std::vector<std::string>({"book", "book_done", "reject", "reject",
                                    "reject", "reject", "reject", "reject",
                                    "reject"}));
}

TEST_CASE("Sell fully fills at best bid without volume cap", "[Trading]") {
  HistoricalLOBStore books;
  books.apply(market_event(1, 1, 81, MarketAction::Add, Side::Buy, 100, 2));
  books.apply(market_event(2, 2, 82, MarketAction::Add, Side::Buy, 99, 4));
  books.apply(market_event(3, 3, 83, MarketAction::Add, Side::Buy, 98, 20));
  SubmitOnFirstMarket strategy;
  strategy.side = Side::Sell;
  strategy.price = 99;
  strategy.quantity = 8;
  RecordingRecorder recorder;
  TradingEngine engine(instruments, BacktestConfig{0, 1, 1}, books, strategy,
                       recorder);
  const auto view = empty_book_view(100, 4);
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 4, view, {}, {}}}};
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, engine);

  REQUIRE(strategy.fills.size() == 1);
  REQUIRE(strategy.fills[0].price == 100);
  REQUIRE(strategy.fills[0].quantity == 8);
  REQUIRE(strategy.fills[0].liquidity_source == LiquiditySource::QuoteCross);
  REQUIRE(engine.open_orders(1).empty());
  REQUIRE(engine.position(1).net_quantity == -8);
}

TEST_CASE("Commands are globally monotonic and normal cancel is terminal",
          "[Trading]") {
  HistoricalLOBStore books;
  struct SequencingStrategy final : RecordingStrategy {
    std::array<ClOrdId, 2> ids{};
    int callbacks{};

    void on_book_update(const BookUpdateView &,
                        StrategyContext &context) override {
      ++callbacks;
      if (callbacks == 1) {
        ids[0] = context.submit_limit(1, Side::Buy, 90, 1);
        ids[1] = context.submit_limit(2, Side::Sell, 110, 1);
      } else if (callbacks == 2) {
        REQUIRE(context.cancel_order(ids[0]));
      }
    }
  } strategy;
  RecordingRecorder recorder;
  const std::array two_instruments{InstrumentMeta{1, 1, 100, 10},
                                   InstrumentMeta{2, 1, 100, 10}};
  TradingEngine engine(two_instruments, BacktestConfig{0, 5, 1}, books,
                       strategy, recorder);
  const auto view100 = empty_book_view(100, 1);
  const auto view110 = empty_book_view(110, 2);
  const std::array events{
      ScheduledEvent{MarketDelivery{1, 100, 100, 1, view100, {}, {}}},
      ScheduledEvent{MarketDelivery{1, 110, 110, 2, view110, {}, {}}}};
  std::vector<Sequence> command_sequences;
  SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
  runtime.run(events, [&](const ScheduledEvent &event, CommandSink &commands) {
    if (event.priority() != EventPriority::MarketData) {
      command_sequences.push_back(event.key().source_or_command_sequence);
    }
    engine(event, commands);
  });

  REQUIRE(command_sequences == std::vector<Sequence>({1, 2, 3}));
  REQUIRE(engine.open_orders(1).empty());
  const auto open_second = engine.open_orders(2);
  REQUIRE(open_second.size() == 1);
  REQUIRE(open_second[0].client_order_id == strategy.ids[1]);
  REQUIRE(recorder.orders.back().event_type == OrderLogEventType::Cancelled);
  REQUIRE(recorder.orders.back().state == OrderState::Cancelled);
}

TEST_CASE("PositionKeeper uses exact multiplier-scaled FIFO realized inputs",
          "[Trading]") {
  PositionKeeper positions;
  positions.register_instrument(InstrumentMeta{1, 1, 100, 10});
  positions.apply_fill(1, Side::Buy, 100, 2);
  positions.apply_fill(1, Side::Sell, 110, 1);
  auto snapshot = positions.position(1);
  REQUIRE(snapshot.net_quantity == 1);
  REQUIRE(snapshot.average_open_price_ticks == 100.0);
  REQUIRE(snapshot.realized_pnl == 1.0);
  positions.apply_fill(1, Side::Sell, 90, 2);
  snapshot = positions.position(1);
  REQUIRE(snapshot.net_quantity == -1);
  REQUIRE(snapshot.average_open_price_ticks == 90.0);
  REQUIRE(snapshot.realized_pnl == 0.0);
}

TEST_CASE("PositionKeeper overflow is defined and leaves state unchanged",
          "[Trading]") {
  PositionKeeper positions;
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  positions.register_instrument(InstrumentMeta{1, 1, 1, maximum});
  positions.apply_fill(1, Side::Buy, 1, maximum);
  const PositionSnapshot before = positions.position(1);

  bool overflowed = false;
  try {
    positions.apply_fill(1, Side::Sell, maximum, maximum);
  } catch (const PositionError &) {
    overflowed = true;
  }
  REQUIRE(overflowed);
  const PositionSnapshot after = positions.position(1);
  REQUIRE(after.net_quantity == before.net_quantity);
  REQUIRE(after.average_open_price_ticks == before.average_open_price_ticks);
  REQUIRE(after.realized_pnl == before.realized_pnl);

  bool invalid_side = false;
  try {
    positions.apply_fill(1, static_cast<Side>(2), 1, 1);
  } catch (const std::invalid_argument &) {
    invalid_side = true;
  }
  REQUIRE(invalid_side);
  REQUIRE(positions.position(1).net_quantity == before.net_quantity);
}

TEST_CASE("Twenty scripted trading runs are deterministic", "[Trading]") {
  std::vector<PriceTicks> baseline_prices;
  std::vector<Quantity> baseline_quantities;
  for (int iteration = 0; iteration < 20; ++iteration) {
    HistoricalLOBStore books;
    books.apply(market_event(1, 1, 101, MarketAction::Add, Side::Sell, 101, 2));
    books.apply(market_event(2, 2, 102, MarketAction::Add, Side::Sell, 102, 3));
    SubmitOnFirstMarket strategy;
    strategy.quantity = 5;
    RecordingRecorder recorder;
    TradingEngine engine(instruments, BacktestConfig{0, 7, 1}, books, strategy,
                         recorder);
    const auto view = empty_book_view(100, 3);
    const std::array events{
        ScheduledEvent{MarketDelivery{1, 100, 100, 3, view, {}, {}}}};
    SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 8, 16});
    runtime.run(events, engine);

    std::vector<PriceTicks> prices;
    std::vector<Quantity> quantities;
    for (const auto &fill : strategy.fills) {
      prices.push_back(fill.price);
      quantities.push_back(fill.quantity);
    }
    if (iteration == 0) {
      baseline_prices = prices;
      baseline_quantities = quantities;
    } else {
      REQUIRE(prices == baseline_prices);
      REQUIRE(quantities == baseline_quantities);
    }
  }
}
