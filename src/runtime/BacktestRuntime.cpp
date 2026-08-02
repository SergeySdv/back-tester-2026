#include "runtime/BacktestRuntime.hpp"

#include "market/HistoricalLOBStore.hpp"
#include "market/JsonlReader.hpp"
#include "scheduler/SchedulerRuntime.hpp"
#include "trading/TradingEngine.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cmf::runtime {
namespace {

[[nodiscard]] TimestampNs checked_delivery_time(TimestampNs exchange_time,
                                                TimestampNs latency) {
  TimestampNs result{};
  if (__builtin_add_overflow(exchange_time, latency, &result)) {
    throw market::SourceError({}, 0, {}, "market delivery timestamp overflow");
  }
  return result;
}

[[nodiscard]] bool equal_levels(const std::vector<BookLevel> &left,
                                const std::vector<BookLevel> &right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](const BookLevel &a, const BookLevel &b) {
                      return a.price == b.price && a.quantity == b.quantity;
                    });
}

struct CachedDepth {
  std::vector<BookLevel> bids;
  std::vector<BookLevel> asks;
};

class ScheduledSource {
public:
  ScheduledSource(std::string path,
                  std::function<bool(market::MarketDataEvent &)> next_event,
                  market::HistoricalLOBStore &books, DateRange range,
                  BacktestConfig config)
      : path_(std::move(path)), next_event_(std::move(next_event)),
        books_(books), range_(range), config_(config) {
    group_.reserve(8);
    bids_.reserve(config.book_depth);
    asks_.reserve(config.book_depth);
    trades_.reserve(8);
    price_cross_signals_.reserve(8);
  }

  bool next(ScheduledEvent &scheduled) {
    if (group_staged_) {
      throw std::logic_error(
          "historical source advanced before staged group dispatch");
    }
    // The scheduler calls next() only after the prior delivery is
    // acknowledged, so this is the first point where its spans may be reused.
    bids_.clear();
    asks_.clear();
    trades_.clear();
    price_cross_signals_.clear();

    for (;;) {
      market::MarketDataEvent first;
      if (!next(first)) {
        return false;
      }

      const InstrumentId instrument_id = first.instrument_id;
      const TimestampNs exchange_time = first.exchange_ts_ns;
      group_.clear();
      group_.push_back(first);
      while (!group_.back().is_last_in_group()) {
        market::MarketDataEvent next_event;
        if (!next(next_event)) {
          throw market::SourceError(
              path_, row_, {},
              "unterminated atomic market group at end of file");
        }
        if (next_event.instrument_id != instrument_id ||
            next_event.exchange_ts_ns != exchange_time) {
          throw market::SourceError(
              path_, row_, {},
              "atomic market group changed instrument or exchange timestamp");
        }
        group_.push_back(next_event);
      }

      if (exchange_time < range_.start_ts_ns) {
        for (const auto &event : group_) {
          books_.apply(event);
        }
        seed_depth_cache(instrument_id);
        continue;
      }
      if (exchange_time > range_.end_ts_ns) {
        return false;
      }

      const TimestampNs engine_time =
          checked_delivery_time(exchange_time, config_.market_data_latency_ns);
      group_staged_ = true;
      scheduled = ScheduledEvent{MarketDelivery{
          instrument_id,
          exchange_time,
          engine_time,
          group_.back().source_sequence,
          std::nullopt,
          {},
          {},
      }};
      return true;
    }
  }

  void prepare_for_dispatch(ScheduledEvent &scheduled) {
    if (!group_staged_) {
      throw std::logic_error("historical source has no staged market group");
    }
    const auto *staged = std::get_if<MarketDelivery>(&scheduled.payload());
    if (staged == nullptr || group_.empty() ||
        staged->instrument_id != group_.front().instrument_id ||
        staged->exchange_ts_ns != group_.front().exchange_ts_ns ||
        staged->source_sequence != group_.back().source_sequence) {
      throw std::logic_error(
          "scheduled market event does not match staged group");
    }

    bool contains_clear = false;
    for (const auto &event : group_) {
      books_.apply(event);
      contains_clear =
          contains_clear || event.action == market::MarketAction::Clear;
      if (event.action == market::MarketAction::Trade) {
        trades_.push_back(TradeView{
            event.instrument_id,
            event.exchange_ts_ns,
            staged->engine_ts_ns,
            event.source_sequence,
            event.side,
            event.price_ticks.value(),
            event.quantity,
        });
        price_cross_signals_.push_back(PriceCrossSignal{
            event.instrument_id,
            event.exchange_ts_ns,
            staged->engine_ts_ns,
            event.source_sequence,
            PriceCrossSource::Trade,
            std::nullopt,
            std::nullopt,
            event.price_ticks,
        });
      } else {
        const auto *intermediate_book = books_.find(event.instrument_id);
        if (intermediate_book == nullptr) {
          throw std::logic_error(
              "historical store lost intermediate instrument");
        }
        const auto best_bid = intermediate_book->best_bid();
        const auto best_ask = intermediate_book->best_ask();
        price_cross_signals_.push_back(PriceCrossSignal{
            event.instrument_id,
            event.exchange_ts_ns,
            staged->engine_ts_ns,
            event.source_sequence,
            PriceCrossSource::BestQuote,
            best_bid.has_value() ? std::optional<PriceTicks>{best_bid->price}
                                 : std::nullopt,
            best_ask.has_value() ? std::optional<PriceTicks>{best_ask->price}
                                 : std::nullopt,
            std::nullopt,
        });
      }
    }

    const InstrumentId instrument_id = staged->instrument_id;
    const auto *book = books_.find(instrument_id);
    if (book == nullptr) {
      throw std::logic_error("historical store lost applied instrument");
    }
    book->write_top_bids(config_.book_depth, bids_);
    book->write_top_asks(config_.book_depth, asks_);

    auto &previous = cached_depth(instrument_id);
    const bool changed = !equal_levels(previous.bids, bids_) ||
                         !equal_levels(previous.asks, asks_);
    std::optional<BookUpdateView> book_update;
    if (changed) {
      previous.bids.assign(bids_.begin(), bids_.end());
      previous.asks.assign(asks_.begin(), asks_.end());
      book_update.emplace(BookUpdateView{
          instrument_id,
          staged->exchange_ts_ns,
          staged->engine_ts_ns,
          staged->source_sequence,
          contains_clear,
          bids_,
          asks_,
      });
    }

    scheduled = ScheduledEvent{MarketDelivery{
        instrument_id,
        staged->exchange_ts_ns,
        staged->engine_ts_ns,
        staged->source_sequence,
        book_update,
        trades_,
        price_cross_signals_,
    }};
    group_staged_ = false;
  }

private:
  bool next(market::MarketDataEvent &event) {
    if (!next_event_(event)) {
      return false;
    }
    ++row_;
    return true;
  }

  CachedDepth &cached_depth(InstrumentId instrument_id) {
    auto [iterator, inserted] =
        previous_depth_.try_emplace(instrument_id, CachedDepth{});
    if (inserted) {
      iterator->second.bids.reserve(config_.book_depth);
      iterator->second.asks.reserve(config_.book_depth);
    }
    return iterator->second;
  }

  void seed_depth_cache(InstrumentId instrument_id) {
    const auto *book = books_.find(instrument_id);
    if (book == nullptr) {
      throw std::logic_error("historical store lost warmup instrument");
    }
    auto &cached = cached_depth(instrument_id);
    book->write_top_bids(config_.book_depth, cached.bids);
    book->write_top_asks(config_.book_depth, cached.asks);
  }

  std::string path_;
  std::function<bool(market::MarketDataEvent &)> next_event_;
  market::HistoricalLOBStore &books_;
  DateRange range_;
  BacktestConfig config_;
  std::unordered_map<InstrumentId, CachedDepth> previous_depth_;
  std::vector<market::MarketDataEvent> group_;
  std::vector<BookLevel> bids_;
  std::vector<BookLevel> asks_;
  std::vector<TradeView> trades_;
  std::vector<PriceCrossSignal> price_cross_signals_;
  bool group_staged_{};
  std::size_t row_{};
};

void validate(DateRange range, BacktestConfig config,
              const std::vector<InstrumentMeta> &instruments) {
  if (range.start_ts_ns > range.end_ts_ns) {
    throw std::invalid_argument("date range start must not exceed end");
  }
  if (config.market_data_latency_ns < 0 || config.order_latency_ns <= 0 ||
      config.book_depth == 0) {
    throw std::invalid_argument("market latency must be non-negative, order "
                                "latency and depth positive");
  }
  if (instruments.empty()) {
    throw std::invalid_argument("at least one instrument is required");
  }
  std::unordered_set<InstrumentId> ids;
  ids.reserve(instruments.size());
  for (const auto &meta : instruments) {
    if (meta.instrument_id <= 0 || meta.tick_size_ticks <= 0 ||
        meta.price_scale <= 0 || meta.contract_multiplier <= 0) {
      throw std::invalid_argument("instrument metadata must be positive");
    }
    if (!ids.insert(meta.instrument_id).second) {
      throw std::invalid_argument("duplicate instrument metadata");
    }
  }
}

template <typename SourceFactory>
results::FrozenResults
replay(trading::Strategy &strategy, DateRange date_range, BacktestConfig config,
       std::vector<InstrumentMeta> instruments, SourceFactory make_source) {
  validate(date_range, config, instruments);
  market::HistoricalLOBStore books;
  results::ResultRecorder recorder(
      instruments, results::ResultReserveEstimate{64, 128, 128, 16});
  trading::TradingEngine engine(instruments, config, books, strategy, recorder);
  auto source = make_source(books);
  scheduler::SchedulerRuntime scheduler(
      scheduler::SchedulerRuntimeConfig{date_range, 1, 64, 4096});
  scheduler.run(source, [&](const ScheduledEvent &event,
                            scheduler::CommandSink &commands) {
    engine(event, commands);
    const auto *delivery = std::get_if<MarketDelivery>(&event.payload());
    if (delivery == nullptr || !delivery->book_update.has_value()) {
      return;
    }
    const auto *book = books.find(delivery->instrument_id);
    const auto bid = book->best_bid();
    const auto ask = book->best_ask();
    recorder.on_book_mark(
        delivery->instrument_id, delivery->engine_ts_ns,
        bid.has_value() ? std::optional<PriceTicks>{bid->price} : std::nullopt,
        ask.has_value() ? std::optional<PriceTicks>{ask->price} : std::nullopt);
  });
  return recorder.freeze();
}

} // namespace

std::vector<InstrumentMeta>
discover_databento_instruments(const std::string &data_path) {
  market::JsonlReader reader(data_path,
                             market::JsonlReader::databento_nanounit_policy());
  std::unordered_set<InstrumentId> unique_ids;
  market::MarketDataEvent event;
  while (reader.next(event)) {
    unique_ids.insert(event.instrument_id);
  }
  std::vector<InstrumentMeta> instruments;
  instruments.reserve(unique_ids.size());
  const auto policy = market::JsonlReader::databento_nanounit_policy();
  for (const InstrumentId instrument_id : unique_ids) {
    instruments.push_back(InstrumentMeta{
        instrument_id,
        policy.tick_size_ticks,
        policy.price_scale,
        policy.contract_multiplier,
    });
  }
  std::sort(instruments.begin(), instruments.end(),
            [](const InstrumentMeta &left, const InstrumentMeta &right) {
              return left.instrument_id < right.instrument_id;
            });
  return instruments;
}

results::FrozenResults run_backtest(trading::Strategy &strategy,
                                    const std::string &data_path,
                                    DateRange date_range, BacktestConfig config,
                                    std::vector<InstrumentMeta> instruments) {
  market::JsonlReader::InstrumentMap metadata;
  metadata.reserve(instruments.size());
  for (const auto &meta : instruments) {
    metadata.emplace(meta.instrument_id, meta);
  }

  return replay(strategy, date_range, config, std::move(instruments),
                [data_path, metadata = std::move(metadata), date_range,
                 config](market::HistoricalLOBStore &books) mutable {
                  auto reader = std::make_shared<market::JsonlReader>(
                      data_path, std::move(metadata));
                  return ScheduledSource{
                      data_path,
                      [reader](market::MarketDataEvent &event) {
                        return reader->next(event);
                      },
                      books, date_range, config};
                });
}

results::FrozenResults run_backtest_events(
    trading::Strategy &strategy, std::vector<market::MarketDataEvent> events,
    std::string source_name, DateRange date_range, BacktestConfig config,
    std::vector<InstrumentMeta> instruments) {
  return replay(
      strategy, date_range, config, std::move(instruments),
      [events = std::move(events), source_name = std::move(source_name),
       date_range, config](market::HistoricalLOBStore &books) mutable {
        auto rows = std::make_shared<std::vector<market::MarketDataEvent>>(
            std::move(events));
        auto index = std::make_shared<std::size_t>(0);
        return ScheduledSource{source_name,
                               [rows, index](market::MarketDataEvent &event) {
                                 if (*index == rows->size()) {
                                   return false;
                                 }
                                 event = (*rows)[(*index)++];
                                 return true;
                               },
                               books, date_range, config};
      });
}

} // namespace cmf::runtime
