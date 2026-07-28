#pragma once

// Shared scaffolding for engine-level tests.
//
// Tests here drive the real TradingEngine through the real SchedulerRuntime.
// The only substitution is the market source: instead of reading JSONL, a
// scripted in-memory source replays groups of MarketDataEvent. It mutates the
// HistoricalLOBStore and derives book updates, trades and price-cross signals
// at dispatch time exactly like the production JsonlScheduledSource, so
// ordering and causality are exercised rather than mocked.

#include "core/BacktestConfig.hpp"
#include "core/Events.hpp"
#include "market/HistoricalLOBStore.hpp"
#include "scheduler/SchedulerRuntime.hpp"
#include "trading/Strategy.hpp"
#include "trading/TradingEngine.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace harness {

using namespace cmf;
using namespace cmf::market;
using namespace cmf::scheduler;
using namespace cmf::trading;

// ---------------------------------------------------------------------------
// Market-data scripting
// ---------------------------------------------------------------------------

// One atomic market group: every event shares an instrument and exchange
// timestamp, and the group is delivered to the strategy as a single unit.
using MarketGroup = std::vector<MarketDataEvent>;

// BookLevel is a plain aggregate without comparison operators.
[[nodiscard]] inline bool equal_levels(const std::vector<BookLevel> &left,
                                       const std::vector<BookLevel> &right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](const BookLevel &a, const BookLevel &b) {
                      return a.price == b.price && a.quantity == b.quantity;
                    });
}

[[nodiscard]] inline MarketDataEvent
add_order(InstrumentId instrument_id, TimestampNs exchange_ts_ns, Sequence seq,
          ExchangeOrderId order_id, Side side, PriceTicks price,
          Quantity quantity) {
  return MarketDataEvent{
      exchange_ts_ns,    exchange_ts_ns, instrument_id, order_id, seq,
      MarketAction::Add, side,           price,         quantity, 0};
}

[[nodiscard]] inline MarketDataEvent
cancel_order_event(InstrumentId instrument_id, TimestampNs exchange_ts_ns,
                   Sequence seq, ExchangeOrderId order_id, Side side,
                   PriceTicks price, Quantity quantity) {
  return MarketDataEvent{
      exchange_ts_ns,       exchange_ts_ns, instrument_id, order_id, seq,
      MarketAction::Cancel, side,           price,         quantity, 0};
}

[[nodiscard]] inline MarketDataEvent
trade(InstrumentId instrument_id, TimestampNs exchange_ts_ns, Sequence seq,
      Side aggressor_side, PriceTicks price, Quantity quantity) {
  return MarketDataEvent{
      exchange_ts_ns,      exchange_ts_ns, instrument_id, 0,        seq,
      MarketAction::Trade, aggressor_side, price,         quantity, 0};
}

// Collects groups and keeps their storage stable for the whole run.
class Script {
public:
  Script &group(MarketGroup events) {
    if (events.empty()) {
      throw std::invalid_argument("market group must not be empty");
    }
    groups_.push_back(std::move(events));
    return *this;
  }

  Script &group(MarketDataEvent event) { return group(MarketGroup{event}); }

  [[nodiscard]] const std::vector<MarketGroup> &groups() const noexcept {
    return groups_;
  }

private:
  std::vector<MarketGroup> groups_;
};

// In-memory counterpart of runtime::JsonlScheduledSource. Applies each group to
// the store at dispatch time and republishes it as a MarketDelivery carrying
// the resulting book update, trades and price-cross signals.
class ScriptedMarketSource {
public:
  ScriptedMarketSource(const Script &script, HistoricalLOBStore &books,
                       BacktestConfig config)
      : groups_(script.groups()), books_(books), config_(config) {
    bids_.reserve(config.book_depth);
    asks_.reserve(config.book_depth);
    trades_.reserve(8);
    signals_.reserve(8);
  }

  bool next(ScheduledEvent &scheduled) {
    if (staged_) {
      throw std::logic_error("scripted source advanced before dispatch");
    }
    if (index_ == groups_.size()) {
      return false;
    }
    // Spans handed out for the previous delivery are dead only once the
    // scheduler asks for the next one, which it does after acknowledgement.
    bids_.clear();
    asks_.clear();
    trades_.clear();
    signals_.clear();

    const auto &group = groups_[index_];
    staged_ = true;
    scheduled = ScheduledEvent{MarketDelivery{
        group.front().instrument_id,
        group.front().exchange_ts_ns,
        group.front().exchange_ts_ns + config_.market_data_latency_ns,
        group.back().source_sequence,
        std::nullopt,
        {},
        {},
    }};
    return true;
  }

  void prepare_for_dispatch(ScheduledEvent &scheduled) {
    if (!staged_) {
      throw std::logic_error("scripted source has no staged group");
    }
    const auto *stage = std::get_if<MarketDelivery>(&scheduled.payload());
    if (stage == nullptr) {
      throw std::logic_error("staged event is not a market delivery");
    }
    const auto &group = groups_[index_];
    const InstrumentId instrument_id = stage->instrument_id;
    const TimestampNs engine_ts_ns = stage->engine_ts_ns;

    bool contains_clear = false;
    for (const auto &event : group) {
      books_.apply(event);
      contains_clear = contains_clear || event.action == MarketAction::Clear;
      if (event.action == MarketAction::Trade) {
        trades_.push_back(TradeView{instrument_id, event.exchange_ts_ns,
                                    engine_ts_ns, event.source_sequence,
                                    event.side, event.price_ticks.value(),
                                    event.quantity});
        signals_.push_back(
            PriceCrossSignal{instrument_id, event.exchange_ts_ns, engine_ts_ns,
                             event.source_sequence, PriceCrossSource::Trade,
                             std::nullopt, std::nullopt, event.price_ticks});
        continue;
      }
      const auto *book = books_.find(instrument_id);
      if (book == nullptr) {
        throw std::logic_error("scripted store lost instrument");
      }
      const auto bid = book->best_bid();
      const auto ask = book->best_ask();
      signals_.push_back(PriceCrossSignal{
          instrument_id, event.exchange_ts_ns, engine_ts_ns,
          event.source_sequence, PriceCrossSource::BestQuote,
          bid.has_value() ? std::optional<PriceTicks>{bid->price}
                          : std::nullopt,
          ask.has_value() ? std::optional<PriceTicks>{ask->price}
                          : std::nullopt,
          std::nullopt});
    }

    const auto *book = books_.find(instrument_id);
    if (book == nullptr) {
      throw std::logic_error("scripted store lost applied instrument");
    }
    book->write_top_bids(config_.book_depth, bids_);
    book->write_top_asks(config_.book_depth, asks_);

    auto &previous = cached_depth(instrument_id);
    std::optional<BookUpdateView> book_update;
    if (!equal_levels(previous.bids, bids_) ||
        !equal_levels(previous.asks, asks_)) {
      previous.bids = bids_;
      previous.asks = asks_;
      book_update.emplace(BookUpdateView{instrument_id, stage->exchange_ts_ns,
                                         engine_ts_ns, stage->source_sequence,
                                         contains_clear, bids_, asks_});
    }

    scheduled = ScheduledEvent{
        MarketDelivery{instrument_id, stage->exchange_ts_ns, engine_ts_ns,
                       stage->source_sequence, book_update, trades_, signals_}};
    staged_ = false;
    ++index_;
  }

private:
  struct CachedDepth {
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
  };

  CachedDepth &cached_depth(InstrumentId instrument_id) {
    return depth_.try_emplace(instrument_id, CachedDepth{}).first->second;
  }

  const std::vector<MarketGroup> &groups_;
  HistoricalLOBStore &books_;
  BacktestConfig config_;
  std::unordered_map<InstrumentId, CachedDepth> depth_;
  std::vector<BookLevel> bids_;
  std::vector<BookLevel> asks_;
  std::vector<TradeView> trades_;
  std::vector<PriceCrossSignal> signals_;
  std::size_t index_{};
  bool staged_{};
};

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

// Records every callback the engine fires, in order, so a test can assert on
// the exact sequence rather than only on a final snapshot.
class RecordingStrategy : public Strategy {
public:
  enum class Kind { BookUpdate, Trade, Fill, Reject };

  struct Callback {
    Kind kind{Kind::BookUpdate};
    TimestampNs engine_ts_ns{};
    ClOrdId client_order_id{};
    RejectReason reason{RejectReason::None};
  };

  std::vector<Callback> callbacks;
  std::vector<FillView> fills;
  std::vector<RejectView> rejects;

  void on_book_update(const BookUpdateView &update,
                      StrategyContext &) override {
    callbacks.push_back(
        Callback{Kind::BookUpdate, update.engine_ts_ns, 0, RejectReason::None});
  }

  void on_trade(const TradeView &view, StrategyContext &) override {
    callbacks.push_back(
        Callback{Kind::Trade, view.engine_ts_ns, 0, RejectReason::None});
  }

  void on_fill(const FillView &fill, StrategyContext &) override {
    callbacks.push_back(Callback{Kind::Fill, fill.engine_ts_ns,
                                 fill.client_order_id, RejectReason::None});
    fills.push_back(fill);
  }

  void on_reject(const RejectView &reject, StrategyContext &) override {
    callbacks.push_back(Callback{Kind::Reject, reject.engine_ts_ns,
                                 reject.client_order_id, reject.reason});
    rejects.push_back(reject);
  }

  [[nodiscard]] bool saw(Kind kind) const {
    return std::any_of(callbacks.begin(), callbacks.end(),
                       [kind](const Callback &c) { return c.kind == kind; });
  }
};

// Captures the result rows without going through the pandas/pybind layer.
struct RecordingRecorder final : Recorder {
  std::vector<OrderLogResultRow> order_log;
  std::vector<FillResultRow> fills;
  std::vector<RejectView> rejects;

  void on_order_event(const OrderLogResultRow &row) override {
    order_log.push_back(row);
  }
  void on_fill(const FillResultRow &row) override { fills.push_back(row); }
  void on_reject(const RejectView &row) override { rejects.push_back(row); }

  [[nodiscard]] std::size_t count(OrderLogEventType type) const {
    return static_cast<std::size_t>(
        std::count_if(order_log.begin(), order_log.end(),
                      [type](const OrderLogResultRow &row) {
                        return row.event_type == type;
                      }));
  }

  [[nodiscard]] bool has(OrderLogEventType type) const {
    return count(type) != 0;
  }
};

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

// Owns a whole scenario so a test reads as data plus assertions. The store is
// exposed because some assertions care about final historical book state.
class Scenario {
public:
  Scenario(std::vector<InstrumentMeta> instruments, BacktestConfig config)
      : instruments_(std::move(instruments)), config_(config) {}

  [[nodiscard]] Script &script() noexcept { return script_; }
  [[nodiscard]] HistoricalLOBStore &books() noexcept { return books_; }

  // Seeds the historical book before the run, without generating deliveries.
  Scenario &warmup(const MarketDataEvent &event) {
    books_.apply(event);
    return *this;
  }

  // `inspect` runs after the scheduler threads have joined, while the engine is
  // still alive, so a test can query final positions and open orders.
  template <typename Inspect>
  void run(Strategy &strategy, Recorder &recorder, Inspect &&inspect) {
    TradingEngine engine(instruments_, config_, books_, strategy, recorder);
    ScriptedMarketSource source(script_, books_, config_);
    SchedulerRuntime runtime(SchedulerRuntimeConfig{DateRange{}, 1, 64, 4096});
    runtime.run(source, engine);
    inspect(engine);
  }

  void run(Strategy &strategy, Recorder &recorder) {
    run(strategy, recorder, [](TradingEngine &) {});
  }

private:
  std::vector<InstrumentMeta> instruments_;
  BacktestConfig config_;
  Script script_;
  HistoricalLOBStore books_;
};

} // namespace harness
