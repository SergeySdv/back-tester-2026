#include "runtime/BacktestRuntime.hpp"

#include "MiniTest.hpp"
#include "TempFile.hpp"
#include "market/JsonlReader.hpp"

#include <array>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace cmf;

class RuntimeStrategy final : public trading::Strategy {
public:
  void on_book_update(const BookUpdateView &view,
                      trading::StrategyContext &context) override {
    callback_order.push_back("book");
    sequences.push_back(view.sequence);
    bid_counts.push_back(view.bids.size());
    ask_counts.push_back(view.asks.size());
    if (order_id == 0) {
      order_id = context.submit_limit(view.instrument_id, Side::Buy,
                                      101'000'000'000, 2);
      submit_time = context.now_ns();
    }
  }

  void on_fill(const FillView &fill,
               trading::StrategyContext &context) override {
    callback_order.push_back("fill");
    fill_time = fill.engine_ts_ns;
    position_in_fill = context.position(fill.instrument_id).net_quantity;
  }

  std::vector<std::string> callback_order;
  std::vector<Sequence> sequences;
  std::vector<std::size_t> bid_counts;
  std::vector<std::size_t> ask_counts;
  ClOrdId order_id{};
  TimestampNs submit_time{};
  TimestampNs fill_time{};
  Quantity position_in_fill{};
};

} // namespace

TEST_CASE("Runtime streams atomic group into real trading and results",
          "[Runtime]") {
  TempFile source("back-tester-runtime-atomic.jsonl");
  {
    std::ofstream output(source.getPath());
    output
        << R"({"ts_recv":"1970-01-01T00:00:00.000000100Z","hd":{"ts_event":"1970-01-01T00:00:00.000000100Z","instrument_id":1},"action":"A","side":"B","price":"99","size":3,"order_id":"10","flags":0,"sequence":1})"
        << '\n'
        << R"({"ts_recv":"1970-01-01T00:00:00.000000100Z","hd":{"ts_event":"1970-01-01T00:00:00.000000100Z","instrument_id":1},"action":"A","side":"A","price":"101","size":2,"order_id":"11","flags":128,"sequence":2})"
        << '\n';
  }

  RuntimeStrategy strategy;
  const std::vector instruments{InstrumentMeta{1, 1, 1'000'000'000, 1}};
  const auto frozen =
      runtime::run_backtest(strategy, source.getPath().string(), DateRange{},
                            BacktestConfig{0, 5, 15}, instruments);

  REQUIRE(strategy.callback_order ==
          std::vector<std::string>({"book", "fill"}));
  REQUIRE(strategy.sequences == std::vector<Sequence>({2}));
  REQUIRE(strategy.bid_counts == std::vector<std::size_t>({1}));
  REQUIRE(strategy.ask_counts == std::vector<std::size_t>({1}));
  REQUIRE(strategy.fill_time == strategy.submit_time + 5);
  REQUIRE(strategy.position_in_fill == 2);
  REQUIRE(frozen.fills().size() == 1);
  REQUIRE(frozen.order_log().size() == 3);
}

TEST_CASE("Runtime applies a prefetched market group only at dispatch",
          "[Runtime]") {
  TempFile source("back-tester-runtime-causality.jsonl");
  {
    std::ofstream output(source.getPath());
    output
        << R"({"ts_recv":"1970-01-01T00:00:00.000000100Z","hd":{"ts_event":"1970-01-01T00:00:00.000000100Z","instrument_id":1},"action":"A","side":"B","price":"99","size":3,"order_id":"10","flags":0,"sequence":1})"
        << '\n'
        << R"({"ts_recv":"1970-01-01T00:00:00.000000100Z","hd":{"ts_event":"1970-01-01T00:00:00.000000100Z","instrument_id":1},"action":"A","side":"A","price":"102","size":2,"order_id":"11","flags":128,"sequence":2})"
        << '\n'
        << R"({"ts_recv":"1970-01-01T00:00:00.000000200Z","hd":{"ts_event":"1970-01-01T00:00:00.000000200Z","instrument_id":1},"action":"M","side":"A","price":"101","size":2,"order_id":"11","flags":128,"sequence":3})"
        << '\n';
  }

  class CausalityStrategy final : public trading::Strategy {
  public:
    void on_book_update(const BookUpdateView &view,
                        trading::StrategyContext &context) override {
      if (order_id == 0) {
        order_id = context.submit_limit(view.instrument_id, Side::Buy,
                                        101'000'000'000, 1);
      }
    }

    void on_fill(const FillView &fill, trading::StrategyContext &) override {
      fill_exchange_time = fill.exchange_ts_ns;
      fill_engine_time = fill.engine_ts_ns;
    }

    ClOrdId order_id{};
    TimestampNs fill_exchange_time{};
    TimestampNs fill_engine_time{};
  } strategy;

  const auto frozen = runtime::run_backtest(
      strategy, source.getPath().string(), DateRange{}, BacktestConfig{0, 5, 1},
      std::vector{InstrumentMeta{1, 1, 1'000'000'000, 1}});

  REQUIRE(strategy.fill_exchange_time == 200);
  REQUIRE(strategy.fill_engine_time == 200);
  REQUIRE(frozen.fills().size() == 1);
  REQUIRE(frozen.fills().exchange_ts_ns.front() == 200);
  REQUIRE(frozen.fills().engine_ts_ns.front() == 200);
}

TEST_CASE("Order does not fill against touch removed before delayed arrival",
          "[Runtime][Latency]")
{
    TempFile source("back-tester-runtime-vanished-touch.jsonl");
    {
        std::ofstream output(source.getPath());
        output
            << R"({"ts_recv":"1970-01-01T00:00:00.000000100Z","hd":{"ts_event":"1970-01-01T00:00:00.000000100Z","instrument_id":1},"action":"A","side":"A","price":"100","size":10,"order_id":"11","flags":128,"sequence":1})"
            << '\n'
            << R"({"ts_recv":"1970-01-01T00:00:00.000000150Z","hd":{"ts_event":"1970-01-01T00:00:00.000000150Z","instrument_id":1},"action":"C","side":"A","price":"100","size":10,"order_id":"11","flags":0,"sequence":2})"
            << '\n'
            << R"({"ts_recv":"1970-01-01T00:00:00.000000150Z","hd":{"ts_event":"1970-01-01T00:00:00.000000150Z","instrument_id":1},"action":"A","side":"A","price":"105","size":10,"order_id":"12","flags":128,"sequence":3})"
            << '\n'
            << R"({"ts_recv":"1970-01-01T00:00:00.000000300Z","hd":{"ts_event":"1970-01-01T00:00:00.000000300Z","instrument_id":1},"action":"A","side":"A","price":"104","size":1,"order_id":"13","flags":128,"sequence":4})"
            << '\n';
    }

    class VanishedTouchStrategy final : public trading::Strategy
    {
      public:
        void on_book_update(const BookUpdateView& view,
                            trading::StrategyContext& context) override
        {
            if (order_id == 0)
            {
                order_id = context.submit_limit(view.instrument_id, Side::Buy,
                                                100'000'000'000, 1);
            }
            const auto open = context.open_orders(view.instrument_id);
            open_at_last_callback.assign(open.begin(), open.end());
        }

        void on_fill(const FillView&, trading::StrategyContext&) override
        {
            ++fill_count;
        }

        ClOrdId order_id{};
        int fill_count{};
        std::vector<OrderQueryRow> open_at_last_callback;
    } strategy;

    const auto frozen = runtime::run_backtest(
        strategy, source.getPath().string(), DateRange{},
        BacktestConfig{0, 100, 1},
        std::vector{InstrumentMeta{1, 1, 1'000'000'000, 1}});

    REQUIRE(strategy.fill_count == 0);
    REQUIRE(frozen.fills().size() == 0);
    REQUIRE(strategy.open_at_last_callback.size() == 1);
    REQUIRE(strategy.open_at_last_callback[0].client_order_id ==
            strategy.order_id);
    REQUIRE(strategy.open_at_last_callback[0].state == OrderState::Open);
    REQUIRE(strategy.open_at_last_callback[0].limit_price_ticks ==
            100'000'000'000);
}

TEST_CASE("Runtime suppresses empty depth until the first actual change",
          "[Runtime]") {
  TempFile source("back-tester-runtime-empty-depth.jsonl");
  {
    std::ofstream output(source.getPath());
    output
        << R"({"ts_recv":"1970-01-01T00:00:00.000000100Z","hd":{"ts_event":"1970-01-01T00:00:00.000000100Z","instrument_id":1},"action":"T","side":"B","price":"100","size":1,"flags":128,"sequence":1})"
        << '\n'
        << R"({"ts_recv":"1970-01-01T00:00:00.000000200Z","hd":{"ts_event":"1970-01-01T00:00:00.000000200Z","instrument_id":1},"action":"A","side":"B","price":"99","size":2,"order_id":"10","flags":128,"sequence":2})"
        << '\n';
  }

  class DepthStrategy final : public trading::Strategy {
  public:
    void on_trade(const TradeView &trade, trading::StrategyContext &) override {
      callbacks.push_back("trade");
      trade_sequences.push_back(trade.sequence);
    }

    void on_book_update(const BookUpdateView &view,
                        trading::StrategyContext &) override {
      callbacks.push_back("book");
      book_sequences.push_back(view.sequence);
    }

    std::vector<std::string> callbacks;
    std::vector<Sequence> trade_sequences;
    std::vector<Sequence> book_sequences;
  } strategy;

  (void)runtime::run_backtest(
      strategy, source.getPath().string(), DateRange{}, BacktestConfig{0, 5, 1},
      std::vector{InstrumentMeta{1, 1, 1'000'000'000, 1}});

  REQUIRE(strategy.callbacks == std::vector<std::string>({"trade", "book"}));
  REQUIRE(strategy.trade_sequences == std::vector<Sequence>({1}));
  REQUIRE(strategy.book_sequences == std::vector<Sequence>({2}));
}

TEST_CASE("Runtime warms historical state before inclusive range start",
          "[Runtime]") {
  TempFile source("back-tester-runtime-range-warmup.jsonl");
  {
    std::ofstream output(source.getPath());
    output
        << R"({"ts_recv":"1970-01-01T00:00:00.000000100Z","hd":{"ts_event":"1970-01-01T00:00:00.000000100Z","instrument_id":1},"action":"A","side":"A","price":"102","size":3,"order_id":"10","flags":128,"sequence":1})"
        << '\n'
        << R"({"ts_recv":"1970-01-01T00:00:00.000000200Z","hd":{"ts_event":"1970-01-01T00:00:00.000000200Z","instrument_id":1},"action":"M","side":"A","price":"101","size":2,"order_id":"10","flags":128,"sequence":2})"
        << '\n';
  }

  class Capture final : public trading::Strategy {
  public:
    void on_book_update(const BookUpdateView &view,
                        trading::StrategyContext &) override {
      sequences.push_back(view.sequence);
      asks.assign(view.asks.begin(), view.asks.end());
    }
    std::vector<Sequence> sequences;
    std::vector<BookLevel> asks;
  } strategy;

  (void)runtime::run_backtest(
      strategy, source.getPath().string(), DateRange{200, 200},
      BacktestConfig{0, 5, 1},
      std::vector{InstrumentMeta{1, 1, 1'000'000'000, 1}});

  REQUIRE(strategy.sequences == std::vector<Sequence>({2}));
  REQUIRE(strategy.asks.size() == 1);
  REQUIRE(strategy.asks[0].price == 101'000'000'000);
  REQUIRE(strategy.asks[0].quantity == 2);
}

TEST_CASE("Range warmup supports in-range historical fill and cancel",
          "[Runtime]") {
  for (const char action : {'F', 'C'}) {
    TempFile source(action == 'F' ? "back-tester-range-fill.jsonl"
                                  : "back-tester-range-cancel.jsonl");
    {
      std::ofstream output(source.getPath());
      output
          << R"({"ts_recv":"1970-01-01T00:00:00.000000100Z","hd":{"ts_event":"1970-01-01T00:00:00.000000100Z","instrument_id":1},"action":"A","side":"A","price":"101","size":3,"order_id":"10","flags":128,"sequence":1})"
          << '\n'
          << "{\"ts_recv\":\"1970-01-01T00:00:00.000000200Z\","
             "\"hd\":{\"ts_event\":\"1970-01-01T00:00:00.000000200Z\","
             "\"instrument_id\":1},\"action\":\""
          << action
          << "\",\"side\":\"A\",\"price\":\"101\",\"size\":1,"
             "\"order_id\":\"10\",\"flags\":128,\"sequence\":2}\n";
    }

    class Capture final : public trading::Strategy {
    public:
      void on_book_update(const BookUpdateView &view,
                          trading::StrategyContext &) override {
        asks.assign(view.asks.begin(), view.asks.end());
      }
      std::vector<BookLevel> asks;
    } strategy;

    (void)runtime::run_backtest(
        strategy, source.getPath().string(), DateRange{200, 200},
        BacktestConfig{0, 5, 1},
        std::vector{InstrumentMeta{1, 1, 1'000'000'000, 1}});

    if (action == 'F') {
      REQUIRE(strategy.asks.size() == 1);
      REQUIRE(strategy.asks[0].quantity == 2);
    } else {
      REQUIRE(strategy.asks.empty());
    }
  }
}

TEST_CASE("Clear immediately before start warms empty state; clear at start is "
          "delivered",
          "[Runtime]") {
  for (const TimestampNs clear_time : {TimestampNs{199}, TimestampNs{200}}) {
    TempFile source(clear_time == 199 ? "back-tester-range-clear-before.jsonl"
                                      : "back-tester-range-clear-at.jsonl");
    {
      std::ofstream output(source.getPath());
      output
          << R"({"ts_recv":"1970-01-01T00:00:00.000000100Z","hd":{"ts_event":"1970-01-01T00:00:00.000000100Z","instrument_id":1},"action":"A","side":"B","price":"99","size":3,"order_id":"10","flags":128,"sequence":1})"
          << '\n'
          << "{\"ts_recv\":\"1970-01-01T00:00:00.000000"
          << (clear_time == 199 ? "199" : "200")
          << "Z\",\"hd\":{\"ts_event\":\"1970-01-01T00:00:00.000000"
          << (clear_time == 199 ? "199" : "200")
          << "Z\",\"instrument_id\":1},\"action\":\"R\",\"side\":\"N\","
             "\"price\":\"0\",\"size\":0,\"flags\":128,\"sequence\":2}\n";
      if (clear_time == 199) {
        output
            << R"({"ts_recv":"1970-01-01T00:00:00.000000200Z","hd":{"ts_event":"1970-01-01T00:00:00.000000200Z","instrument_id":1},"action":"T","side":"B","price":"99","size":1,"flags":128,"sequence":3})"
            << '\n';
      }
    }

    class Capture final : public trading::Strategy {
    public:
      void on_book_update(const BookUpdateView &view,
                          trading::StrategyContext &) override {
        ++books;
        clear = view.is_snapshot;
      }
      void on_trade(const TradeView &, trading::StrategyContext &) override {
        ++trades;
      }
      int books{};
      int trades{};
      bool clear{};
    } strategy;

    (void)runtime::run_backtest(
        strategy, source.getPath().string(), DateRange{200, 200},
        BacktestConfig{0, 5, 1},
        std::vector{InstrumentMeta{1, 1, 1'000'000'000, 1}});

    if (clear_time == 199) {
      REQUIRE(strategy.books == 0);
      REQUIRE(strategy.trades == 1);
    } else {
      REQUIRE(strategy.books == 1);
      REQUIRE(strategy.trades == 0);
      REQUIRE(strategy.clear);
    }
  }
}

TEST_CASE("Range warmup seeds depth cache and emits no callbacks or marks",
          "[Runtime]") {
  TempFile source("back-tester-runtime-range-cache.jsonl");
  {
    std::ofstream output(source.getPath());
    output
        << R"({"ts_recv":"1970-01-01T00:00:00.000000100Z","hd":{"ts_event":"1970-01-01T00:00:00.000000100Z","instrument_id":1},"action":"A","side":"B","price":"99","size":3,"order_id":"10","flags":128,"sequence":1})"
        << '\n'
        << R"({"ts_recv":"1970-01-01T00:00:00.000000200Z","hd":{"ts_event":"1970-01-01T00:00:00.000000200Z","instrument_id":1},"action":"T","side":"B","price":"99","size":1,"flags":128,"sequence":2})"
        << '\n';
  }

  class Capture final : public trading::Strategy {
  public:
    void on_trade(const TradeView &, trading::StrategyContext &) override {
      ++trades;
    }
    void on_book_update(const BookUpdateView &,
                        trading::StrategyContext &) override {
      ++books;
    }
    int trades{};
    int books{};
  } strategy;

  const auto frozen = runtime::run_backtest(
      strategy, source.getPath().string(), DateRange{200, 200},
      BacktestConfig{0, 5, 1},
      std::vector{InstrumentMeta{1, 1, 1'000'000'000, 1}});

  REQUIRE(strategy.trades == 1);
  REQUIRE(strategy.books == 0);
  REQUIRE(frozen.pnl().empty());
}

TEST_CASE("Runtime validates unterminated groups before range start",
          "[Runtime]") {
  TempFile source("back-tester-runtime-range-unterminated.jsonl");
  {
    std::ofstream output(source.getPath());
    output
        << R"({"ts_recv":"1970-01-01T00:00:00.000000100Z","hd":{"ts_event":"1970-01-01T00:00:00.000000100Z","instrument_id":1},"action":"A","side":"B","price":"99","size":3,"order_id":"10","flags":0,"sequence":1})"
        << '\n';
  }
  RuntimeStrategy strategy;
  bool rejected = false;
  try {
    (void)runtime::run_backtest(
        strategy, source.getPath().string(), DateRange{200, 300},
        BacktestConfig{0, 5, 1},
        std::vector{InstrumentMeta{1, 1, 1'000'000'000, 1}});
  } catch (const market::SourceError &) {
    rejected = true;
  }
  REQUIRE(rejected);
}

TEST_CASE("Runtime rejects an unterminated atomic source group", "[Runtime]") {
  TempFile source("back-tester-runtime-unterminated.jsonl");
  {
    std::ofstream output(source.getPath());
    output
        << R"({"ts_recv":"1970-01-01T00:00:00.000000100Z","hd":{"ts_event":"1970-01-01T00:00:00.000000100Z","instrument_id":1},"action":"A","side":"B","price":"99","size":3,"order_id":"10","flags":0,"sequence":1})"
        << '\n';
  }
  RuntimeStrategy strategy;
  bool rejected = false;
  try {
    (void)runtime::run_backtest(
        strategy, source.getPath().string(), DateRange{},
        BacktestConfig{0, 5, 15},
        std::vector{InstrumentMeta{1, 1, 1'000'000'000, 1}});
  } catch (const market::SourceError &) {
    rejected = true;
  }
  REQUIRE(rejected);
}
