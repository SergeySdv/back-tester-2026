#include "core/BacktestConfig.hpp"
#include "core/Events.hpp"
#include "market/JsonlReader.hpp"
#include "market/Parsing.hpp"
#include "results/ResultRecorder.hpp"
#include "runtime/BacktestRuntime.hpp"
#include "trading/Strategy.hpp"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace py = pybind11;

#ifndef BACK_TESTER_VERSION
#define BACK_TESTER_VERSION "unknown"
#endif

namespace {

using namespace cmf;

struct OwnedBookUpdate {
  InstrumentId instrument_id{};
  TimestampNs exchange_ts_ns{};
  TimestampNs engine_ts_ns{};
  Sequence sequence{};
  bool is_snapshot{};
  std::vector<BookLevel> bids;
  std::vector<BookLevel> asks;
};

class PythonStrategyHandle {
public:
  [[nodiscard]] ClOrdId submit_limit(InstrumentId instrument_id, Side side,
                                     PriceTicks price_ticks,
                                     Quantity quantity) {
    return context().submit_limit(instrument_id, side, price_ticks, quantity);
  }

  bool cancel_order(ClOrdId client_order_id) {
    return context().cancel_order(client_order_id);
  }

  [[nodiscard]] PositionSnapshot position(InstrumentId instrument_id) const {
    return context().position(instrument_id);
  }

  [[nodiscard]] std::vector<OrderQueryRow>
  open_orders(InstrumentId instrument_id) {
    const auto rows = context().open_orders(instrument_id);
    return {rows.begin(), rows.end()};
  }

  [[nodiscard]] TimestampNs now_ns() const { return context().now_ns(); }

  trading::StrategyContext *
  activate(trading::StrategyContext *active) noexcept {
    active_thread_ =
        active == nullptr ? std::thread::id{} : std::this_thread::get_id();
    return std::exchange(active_, active);
  }

  void begin_run() {
    // Binding entry and guard destruction hold the GIL; the flag is set before
    // run() releases it, so another Python thread observes one owner.
    if (running_) {
      throw std::runtime_error("strategy is already running");
    }
    running_ = true;
  }

  void end_run() noexcept { running_ = false; }

private:
  [[nodiscard]] trading::StrategyContext &context() const {
    if (active_ == nullptr || active_thread_ != std::this_thread::get_id()) {
      throw std::runtime_error(
          "strategy context is available only during a callback");
    }
    return *active_;
  }

  trading::StrategyContext *active_{};
  std::thread::id active_thread_;
  bool running_{};
};

class ActiveContext {
public:
  ActiveContext(PythonStrategyHandle &handle,
                trading::StrategyContext &context) noexcept
      : handle_(handle), previous_(handle_.activate(&context)) {}

  ~ActiveContext() { handle_.activate(previous_); }

private:
  PythonStrategyHandle &handle_;
  trading::StrategyContext *previous_;
};

class StrategyRunGuard {
public:
  explicit StrategyRunGuard(PythonStrategyHandle &handle) : handle_(handle) {
    handle_.begin_run();
  }

  ~StrategyRunGuard() { handle_.end_run(); }

  StrategyRunGuard(const StrategyRunGuard &) = delete;
  StrategyRunGuard &operator=(const StrategyRunGuard &) = delete;

private:
  PythonStrategyHandle &handle_;
};

class PythonStrategyAdapter final : public trading::Strategy {
public:
  PythonStrategyAdapter(py::object strategy,
                        std::shared_ptr<PythonStrategyHandle> handle)
      : strategy_(std::move(strategy)), handle_(std::move(handle)) {}

  void on_book_update(const BookUpdateView &view,
                      trading::StrategyContext &context) override {
    OwnedBookUpdate owned{view.instrument_id,
                          view.exchange_ts_ns,
                          view.engine_ts_ns,
                          view.sequence,
                          view.is_snapshot,
                          {view.bids.begin(), view.bids.end()},
                          {view.asks.begin(), view.asks.end()}};
    invoke("on_book_update", std::move(owned), context);
  }

  void on_trade(const TradeView &view,
                trading::StrategyContext &context) override {
    invoke("on_trade", view, context);
  }

  void on_fill(const FillView &view,
               trading::StrategyContext &context) override {
    invoke("on_fill", view, context);
  }

  void on_reject(const RejectView &view,
                 trading::StrategyContext &context) override {
    invoke("on_reject", view, context);
  }

private:
  template <typename Payload>
  void invoke(const char *method, Payload payload,
              trading::StrategyContext &context) {
    py::gil_scoped_acquire gil;
    ActiveContext active(*handle_, context);
    strategy_.attr(method)(std::move(payload));
  }

  py::object strategy_;
  std::shared_ptr<PythonStrategyHandle> handle_;
};

class PythonResult {
public:
  explicit PythonResult(results::FrozenResults frozen)
      : frozen_(std::make_shared<results::FrozenResults>(std::move(frozen))) {}

  [[nodiscard]] py::object fills_df() const {
    const auto columns = frozen_->fills();
    py::dict values;
    values["exchange_ts_ns"] = array(columns.exchange_ts_ns);
    values["engine_ts_ns"] = array(columns.engine_ts_ns);
    values["instrument_id"] = array(columns.instrument_id);
    values["client_order_id"] = array(columns.client_order_id);
    values["side"] = enum_array<std::int8_t>(columns.side);
    values["price_ticks"] = array(columns.price_ticks);
    values["quantity"] = array(columns.quantity);
    values["remaining_quantity"] = array(columns.remaining_quantity);
    values["liquidity_source"] =
        enum_array<std::uint8_t>(columns.liquidity_source);
    values["trigger_source_sequence"] = array(columns.trigger_source_sequence);
    return py::module_::import("pandas").attr("DataFrame")(
        values, py::arg("copy") = false);
  }

  [[nodiscard]] py::object order_log_df() const {
    const auto columns = frozen_->order_log();
    py::dict values;
    values["engine_ts_ns"] = array(columns.engine_ts_ns);
    values["instrument_id"] = array(columns.instrument_id);
    values["client_order_id"] = array(columns.client_order_id);
    values["event_type"] = enum_array<std::uint8_t>(columns.event_type);
    values["state"] = enum_array<std::uint8_t>(columns.state);
    values["side"] = enum_array<std::int8_t>(columns.side);
    values["limit_price_ticks"] = array(columns.limit_price_ticks);
    values["order_quantity"] = array(columns.order_quantity);
    values["filled_quantity"] = array(columns.filled_quantity);
    values["remaining_quantity"] = array(columns.remaining_quantity);
    values["reject_reason"] = enum_array<std::uint8_t>(columns.reject_reason);
    return py::module_::import("pandas").attr("DataFrame")(
        values, py::arg("copy") = false);
  }

  [[nodiscard]] py::object pnl_series() const {
    const auto columns = frozen_->pnl();
    py::object pandas = py::module_::import("pandas");
    py::object index = pandas.attr("Index")(array(columns.engine_ts_ns),
                                            py::arg("name") = "engine_ts_ns",
                                            py::arg("copy") = false);
    return pandas.attr("Series")(
        array(columns.total_pnl), py::arg("index") = index,
        py::arg("name") = "total_pnl", py::arg("copy") = false);
  }

private:
  [[nodiscard]] py::capsule owner() const {
    auto *copy = new std::shared_ptr<results::FrozenResults>(frozen_);
    return py::capsule(copy, [](void *pointer) {
      delete static_cast<std::shared_ptr<results::FrozenResults> *>(pointer);
    });
  }

  template <typename Value>
  [[nodiscard]] py::array array(std::span<const Value> values) const {
    return py::array(
        py::dtype::of<Value>(), {static_cast<py::ssize_t>(values.size())},
        {static_cast<py::ssize_t>(sizeof(Value))}, values.data(), owner());
  }

  template <typename Storage, typename Enum>
  [[nodiscard]] py::array enum_array(std::span<const Enum> values) const {
    static_assert(sizeof(Storage) == sizeof(Enum));
    return py::array(
        py::dtype::of<Storage>(), {static_cast<py::ssize_t>(values.size())},
        {static_cast<py::ssize_t>(sizeof(Storage))}, values.data(), owner());
  }

  std::shared_ptr<results::FrozenResults> frozen_;
};

template <typename Value>
py::array_t<Value, py::array::c_style | py::array::forcecast>
required_array(const py::dict &columns, const char *name) {
  if (!columns.contains(name)) {
    throw std::invalid_argument(std::string("missing event column '") + name +
                                "'");
  }
  auto result =
      py::array_t<Value, py::array::c_style | py::array::forcecast>::ensure(
          columns[name]);
  if (!result || result.ndim() != 1) {
    throw std::invalid_argument(std::string("event column '") + name +
                                "' must be one-dimensional");
  }
  return result;
}

cmf::market::MarketAction parse_action(const std::string &value) {
  if (value.size() != 1) {
    throw std::invalid_argument("action must contain one character");
  }
  switch (value.front()) {
  case 'A':
    return cmf::market::MarketAction::Add;
  case 'C':
    return cmf::market::MarketAction::Cancel;
  case 'M':
    return cmf::market::MarketAction::Modify;
  case 'T':
    return cmf::market::MarketAction::Trade;
  case 'F':
    return cmf::market::MarketAction::Fill;
  case 'R':
    return cmf::market::MarketAction::Clear;
  default:
    throw std::invalid_argument("action has an unsupported value");
  }
}

cmf::Side parse_side(const py::handle &value, bool required) {
  if (value.is_none()) {
    if (required) {
      throw std::invalid_argument("side is required for this action");
    }
    return cmf::Side::None;
  }
  const auto text = py::cast<std::string>(value);
  if (text == "B") {
    return cmf::Side::Buy;
  }
  if (text == "A" || text == "S") {
    return cmf::Side::Sell;
  }
  if (!required && (text.empty() || text == "N")) {
    return cmf::Side::None;
  }
  throw std::invalid_argument("side has an unsupported value");
}

std::vector<cmf::InstrumentMeta> discover_instruments(
    const py::array_t<cmf::InstrumentId,
                      py::array::c_style | py::array::forcecast> &ids) {
  std::unordered_set<cmf::InstrumentId> unique;
  const auto values = ids.unchecked<1>();
  for (py::ssize_t index = 0; index < values.shape(0); ++index) {
    if (values(index) <= 0) {
      throw std::invalid_argument("instrument_id must be positive");
    }
    unique.insert(values(index));
  }
  const auto policy = cmf::market::JsonlReader::databento_nanounit_policy();
  std::vector<cmf::InstrumentMeta> result;
  result.reserve(unique.size());
  for (const auto id : unique) {
    result.push_back(cmf::InstrumentMeta{id, policy.tick_size_ticks,
                                         policy.price_scale,
                                         policy.contract_multiplier});
  }
  std::sort(result.begin(), result.end(),
            [](const auto &left, const auto &right) {
              return left.instrument_id < right.instrument_id;
            });
  return result;
}

std::vector<cmf::market::MarketDataEvent>
parse_events(const py::dict &columns, const std::string &source_name,
             std::vector<cmf::InstrumentMeta> &instruments) {
  const auto receive = required_array<cmf::TimestampNs>(columns, "ts_recv");
  const auto exchange = required_array<cmf::TimestampNs>(columns, "ts_event");
  const auto instrument =
      required_array<cmf::InstrumentId>(columns, "instrument_id");
  const auto quantity = required_array<cmf::Quantity>(columns, "size");
  const auto flags = required_array<std::uint32_t>(columns, "flags");
  const auto sequence = required_array<cmf::Sequence>(columns, "sequence");
  const py::sequence order_ids = columns["order_id"].cast<py::sequence>();
  const py::sequence actions = columns["action"].cast<py::sequence>();
  const py::sequence sides = columns["side"].cast<py::sequence>();
  const py::sequence prices = columns["price"].cast<py::sequence>();

  const py::ssize_t size = receive.shape(0);
  for (const py::ssize_t candidate :
       {exchange.shape(0), instrument.shape(0), quantity.shape(0),
        flags.shape(0), sequence.shape(0),
        static_cast<py::ssize_t>(py::len(order_ids)),
        static_cast<py::ssize_t>(py::len(actions)),
        static_cast<py::ssize_t>(py::len(sides)),
        static_cast<py::ssize_t>(py::len(prices))}) {
    if (candidate != size) {
      throw std::invalid_argument("event columns must have equal lengths");
    }
  }
  if (instruments.empty()) {
    instruments = discover_instruments(instrument);
  }
  std::unordered_map<cmf::InstrumentId, cmf::InstrumentMeta> metadata;
  for (const auto &meta : instruments) {
    metadata.emplace(meta.instrument_id, meta);
  }

  const auto receive_values = receive.unchecked<1>();
  const auto exchange_values = exchange.unchecked<1>();
  const auto instrument_values = instrument.unchecked<1>();
  const auto quantity_values = quantity.unchecked<1>();
  const auto flag_values = flags.unchecked<1>();
  const auto sequence_values = sequence.unchecked<1>();
  std::vector<cmf::market::MarketDataEvent> events;
  events.reserve(static_cast<std::size_t>(size));

  for (py::ssize_t index = 0; index < size; ++index) {
    try {
      cmf::market::MarketDataEvent event;
      event.receive_ts_ns = receive_values(index);
      event.exchange_ts_ns = exchange_values(index);
      event.instrument_id = instrument_values(index);
      event.source_sequence = sequence_values(index);
      event.flags = flag_values(index);
      event.action = parse_action(py::cast<std::string>(actions[index]));
      const bool needs_side =
          event.action == cmf::market::MarketAction::Add ||
          event.action == cmf::market::MarketAction::Modify ||
          event.action == cmf::market::MarketAction::Trade;
      event.side = parse_side(sides[index], needs_side);

      const auto meta = metadata.find(event.instrument_id);
      if (meta == metadata.end()) {
        throw std::invalid_argument("unknown instrument_id " +
                                    std::to_string(event.instrument_id));
      }
      const bool needs_order =
          event.action != cmf::market::MarketAction::Clear &&
          event.action != cmf::market::MarketAction::Trade;
      if (!order_ids[index].is_none()) {
        event.exchange_order_id =
            py::cast<cmf::ExchangeOrderId>(order_ids[index]);
      } else if (needs_order) {
        throw std::invalid_argument("order_id is required for this action");
      }
      const bool needs_quantity =
          event.action == cmf::market::MarketAction::Add ||
          event.action == cmf::market::MarketAction::Modify ||
          event.action == cmf::market::MarketAction::Fill ||
          event.action == cmf::market::MarketAction::Trade;
      if (needs_quantity) {
        event.quantity = quantity_values(index);
        if (event.quantity <= 0) {
          throw std::invalid_argument("size must be positive for this action");
        }
      }
      const bool needs_price =
          event.action == cmf::market::MarketAction::Add ||
          event.action == cmf::market::MarketAction::Modify ||
          event.action == cmf::market::MarketAction::Trade;
      if (needs_price) {
        if (prices[index].is_none()) {
          throw std::invalid_argument("price is required for this action");
        }
        event.price_ticks = cmf::market::parse_decimal_ticks(
            py::cast<std::string>(prices[index]), meta->second.price_scale,
            meta->second.tick_size_ticks);
      }
      if (!events.empty() &&
          event.exchange_ts_ns < events.back().exchange_ts_ns) {
        throw std::invalid_argument("exchange timestamp regressed");
      }
      if (!events.empty() &&
          event.source_sequence <= events.back().source_sequence) {
        throw std::invalid_argument(
            "source sequence is not strictly increasing");
      }
      events.push_back(event);
    } catch (const std::exception &error) {
      throw std::invalid_argument(
          source_name + ":" + std::to_string(index + 1) + ": " + error.what());
    }
  }
  return events;
}

} // namespace

PYBIND11_MODULE(_backtester, module) {
  module.doc() = "Deterministic native options back-testing runtime";
  module.def("version", []() { return BACK_TESTER_VERSION; });

  py::enum_<cmf::Side>(module, "Side")
      .value("SELL", cmf::Side::Sell)
      .value("NONE", cmf::Side::None)
      .value("BUY", cmf::Side::Buy);
  py::enum_<cmf::LiquiditySource>(module, "LiquiditySource")
      .value("HISTORICAL_DISPLAYED", cmf::LiquiditySource::HistoricalDisplayed)
      .value("QUOTE_CROSS", cmf::LiquiditySource::QuoteCross)
      .value("TRADE_CROSS", cmf::LiquiditySource::TradeCross);
  py::enum_<cmf::OrderState>(module, "OrderState")
      .value("PENDING_NEW", cmf::OrderState::PendingNew)
      .value("OPEN", cmf::OrderState::Open)
      .value("PARTIALLY_FILLED", cmf::OrderState::PartiallyFilled)
      .value("FILLED", cmf::OrderState::Filled)
      .value("PENDING_CANCEL", cmf::OrderState::PendingCancel)
      .value("CANCELLED", cmf::OrderState::Cancelled)
      .value("REJECTED", cmf::OrderState::Rejected);
  py::enum_<cmf::RejectReason>(module, "RejectReason")
      .value("NONE", cmf::RejectReason::None)
      .value("UNKNOWN_INSTRUMENT", cmf::RejectReason::UnknownInstrument)
      .value("INVALID_SIDE", cmf::RejectReason::InvalidSide)
      .value("NON_POSITIVE_QUANTITY", cmf::RejectReason::NonPositiveQuantity)
      .value("INVALID_PRICE", cmf::RejectReason::InvalidPrice)
      .value("TICK_MISALIGNMENT", cmf::RejectReason::TickMisalignment)
      .value("DUPLICATE_CLIENT_ORDER_ID",
             cmf::RejectReason::DuplicateClientOrderId)
      .value("UNSUPPORTED_ORDER_TYPE", cmf::RejectReason::UnsupportedOrderType)
      .value("UNSUPPORTED_TIME_IN_FORCE",
             cmf::RejectReason::UnsupportedTimeInForce)
      .value("UNKNOWN_ORDER", cmf::RejectReason::UnknownOrder)
      .value("ALREADY_TERMINAL", cmf::RejectReason::AlreadyTerminal);

  py::class_<cmf::BacktestConfig>(module, "BacktestConfig")
      .def(py::init([](cmf::TimestampNs market_data_latency_ns,
                       cmf::TimestampNs order_latency_ns,
                       std::uint32_t book_depth) {
             return cmf::BacktestConfig{market_data_latency_ns,
                                        order_latency_ns, book_depth};
           }),
           py::arg("market_data_latency_ns") = 0,
           py::arg("order_latency_ns") = cmf::runtime::default_order_latency_ns,
           py::arg("book_depth") = 15)
      .def_readwrite("market_data_latency_ns",
                     &cmf::BacktestConfig::market_data_latency_ns)
      .def_readwrite("order_latency_ns", &cmf::BacktestConfig::order_latency_ns)
      .def_readwrite("book_depth", &cmf::BacktestConfig::book_depth);

  py::class_<cmf::DateRange>(module, "DateRange")
      .def(py::init<cmf::TimestampNs, cmf::TimestampNs>(),
           py::arg("start_ts_ns") =
               std::numeric_limits<cmf::TimestampNs>::lowest(),
           py::arg("end_ts_ns") = std::numeric_limits<cmf::TimestampNs>::max())
      .def_readwrite("start_ts_ns", &cmf::DateRange::start_ts_ns)
      .def_readwrite("end_ts_ns", &cmf::DateRange::end_ts_ns);

  py::class_<cmf::InstrumentMeta>(module, "InstrumentMeta")
      .def(py::init<cmf::InstrumentId, cmf::PriceTicks, cmf::PriceTicks,
                    cmf::Quantity>(),
           py::arg("instrument_id"), py::arg("tick_size_ticks") = 1,
           py::arg("price_scale") = 1'000'000'000,
           py::arg("contract_multiplier") = 1)
      .def_readonly("instrument_id", &cmf::InstrumentMeta::instrument_id)
      .def_readonly("tick_size_ticks", &cmf::InstrumentMeta::tick_size_ticks)
      .def_readonly("price_scale", &cmf::InstrumentMeta::price_scale)
      .def_readonly("contract_multiplier",
                    &cmf::InstrumentMeta::contract_multiplier);

  py::class_<cmf::BookLevel>(module, "BookLevel")
      .def_readonly("price", &cmf::BookLevel::price)
      .def_readonly("quantity", &cmf::BookLevel::quantity);
  py::class_<OwnedBookUpdate>(module, "BookUpdate")
      .def_readonly("instrument_id", &OwnedBookUpdate::instrument_id)
      .def_readonly("exchange_ts_ns", &OwnedBookUpdate::exchange_ts_ns)
      .def_readonly("engine_ts_ns", &OwnedBookUpdate::engine_ts_ns)
      .def_readonly("sequence", &OwnedBookUpdate::sequence)
      .def_readonly("is_snapshot", &OwnedBookUpdate::is_snapshot)
      .def_readonly("bids", &OwnedBookUpdate::bids)
      .def_readonly("asks", &OwnedBookUpdate::asks);
  py::class_<cmf::TradeView>(module, "Trade")
      .def_readonly("instrument_id", &cmf::TradeView::instrument_id)
      .def_readonly("exchange_ts_ns", &cmf::TradeView::exchange_ts_ns)
      .def_readonly("engine_ts_ns", &cmf::TradeView::engine_ts_ns)
      .def_readonly("sequence", &cmf::TradeView::sequence)
      .def_readonly("aggressor_side", &cmf::TradeView::aggressor_side)
      .def_readonly("price", &cmf::TradeView::price)
      .def_readonly("quantity", &cmf::TradeView::quantity);
  py::class_<cmf::FillView>(module, "Fill")
      .def_readonly("instrument_id", &cmf::FillView::instrument_id)
      .def_readonly("client_order_id", &cmf::FillView::client_order_id)
      .def_readonly("side", &cmf::FillView::side)
      .def_readonly("price", &cmf::FillView::price)
      .def_readonly("quantity", &cmf::FillView::quantity)
      .def_readonly("remaining_quantity", &cmf::FillView::remaining_quantity)
      .def_readonly("exchange_ts_ns", &cmf::FillView::exchange_ts_ns)
      .def_readonly("engine_ts_ns", &cmf::FillView::engine_ts_ns)
      .def_readonly("sequence", &cmf::FillView::fill_sequence)
      .def_readonly("liquidity_source", &cmf::FillView::liquidity_source)
      .def_readonly("trigger_source_sequence",
                    &cmf::FillView::trigger_source_sequence);
  py::class_<cmf::RejectView>(module, "Reject")
      .def_readonly("instrument_id", &cmf::RejectView::instrument_id)
      .def_readonly("client_order_id", &cmf::RejectView::client_order_id)
      .def_readonly("reason", &cmf::RejectView::reason)
      .def_readonly("exchange_ts_ns", &cmf::RejectView::exchange_ts_ns)
      .def_readonly("engine_ts_ns", &cmf::RejectView::engine_ts_ns)
      .def_readonly("sequence", &cmf::RejectView::sequence);
  py::class_<cmf::PositionSnapshot>(module, "Position")
      .def_readonly("instrument_id", &cmf::PositionSnapshot::instrument_id)
      .def_readonly("net_quantity", &cmf::PositionSnapshot::net_quantity)
      .def_readonly("average_open_price_ticks",
                    &cmf::PositionSnapshot::average_open_price_ticks)
      .def_readonly("realized_pnl", &cmf::PositionSnapshot::realized_pnl)
      .def_readonly("unrealized_pnl", &cmf::PositionSnapshot::unrealized_pnl);
  py::class_<cmf::OrderQueryRow>(module, "OpenOrder")
      .def_readonly("instrument_id", &cmf::OrderQueryRow::instrument_id)
      .def_readonly("client_order_id", &cmf::OrderQueryRow::client_order_id)
      .def_readonly("state", &cmf::OrderQueryRow::state)
      .def_readonly("side", &cmf::OrderQueryRow::side)
      .def_readonly("limit_price_ticks", &cmf::OrderQueryRow::limit_price_ticks)
      .def_readonly("order_quantity", &cmf::OrderQueryRow::order_quantity)
      .def_readonly("filled_quantity", &cmf::OrderQueryRow::filled_quantity)
      .def_readonly("remaining_quantity",
                    &cmf::OrderQueryRow::remaining_quantity);

  py::class_<PythonStrategyHandle, std::shared_ptr<PythonStrategyHandle>>(
      module, "Strategy")
      .def(py::init<>())
      .def("on_book_update",
           [](PythonStrategyHandle &, const OwnedBookUpdate &) {})
      .def("on_trade", [](PythonStrategyHandle &, const cmf::TradeView &) {})
      .def("on_fill", [](PythonStrategyHandle &, const cmf::FillView &) {})
      .def("on_reject", [](PythonStrategyHandle &, const cmf::RejectView &) {})
      .def("submit_limit", &PythonStrategyHandle::submit_limit,
           py::arg("instrument_id"), py::arg("side"), py::arg("price_ticks"),
           py::arg("quantity"))
      .def("cancel_order", &PythonStrategyHandle::cancel_order)
      .def("position", &PythonStrategyHandle::position)
      .def("open_orders", &PythonStrategyHandle::open_orders)
      .def_property_readonly("now_ns", &PythonStrategyHandle::now_ns);

  py::class_<PythonResult>(module, "Result")
      .def_property_readonly("fills_df", &PythonResult::fills_df)
      .def_property_readonly("order_log_df", &PythonResult::order_log_df)
      .def_property_readonly("pnl_series", &PythonResult::pnl_series);

  module.def(
      "run",
      [](py::object strategy, const std::string &data_path,
         cmf::DateRange date_range,
         std::optional<cmf::BacktestConfig> optional_config,
         std::optional<std::vector<cmf::InstrumentMeta>> optional_instruments) {
        auto handle = strategy.cast<std::shared_ptr<PythonStrategyHandle>>();
        StrategyRunGuard run_guard(*handle);
        cmf::BacktestConfig config = optional_config.value_or(
            cmf::BacktestConfig{0, cmf::runtime::default_order_latency_ns, 15});
        PythonStrategyAdapter adapter(std::move(strategy), handle);
        cmf::results::FrozenResults frozen;
        {
          py::gil_scoped_release release;
          std::vector<cmf::InstrumentMeta> instruments =
              optional_instruments.has_value()
                  ? std::move(*optional_instruments)
                  : cmf::runtime::discover_databento_instruments(data_path);
          frozen = cmf::runtime::run_backtest(adapter, data_path, date_range,
                                              config, std::move(instruments));
        }
        return PythonResult{std::move(frozen)};
      },
      py::arg("strategy"), py::arg("data_path"), py::arg("date_range"),
      py::arg("config") = py::none(), py::arg("instruments") = py::none());

  module.def(
      "_run_events",
      [](py::object strategy, const py::dict &columns,
         const std::string &source_name, cmf::DateRange date_range,
         std::optional<cmf::BacktestConfig> optional_config,
         std::optional<std::vector<cmf::InstrumentMeta>> optional_instruments) {
        auto handle = strategy.cast<std::shared_ptr<PythonStrategyHandle>>();
        StrategyRunGuard run_guard(*handle);
        cmf::BacktestConfig config = optional_config.value_or(
            cmf::BacktestConfig{0, cmf::runtime::default_order_latency_ns, 15});
        if (optional_instruments.has_value() && optional_instruments->empty()) {
          throw std::invalid_argument("at least one instrument is required");
        }
        std::vector<cmf::InstrumentMeta> instruments =
            optional_instruments.value_or(std::vector<cmf::InstrumentMeta>{});
        auto events = parse_events(columns, source_name, instruments);
        PythonStrategyAdapter adapter(std::move(strategy), handle);
        cmf::results::FrozenResults frozen;
        {
          py::gil_scoped_release release;
          frozen = cmf::runtime::run_backtest_events(
              adapter, std::move(events), source_name, date_range, config,
              std::move(instruments));
        }
        return PythonResult{std::move(frozen)};
      },
      py::arg("strategy"), py::arg("columns"), py::arg("source_name"),
      py::arg("date_range"), py::arg("config") = py::none(),
      py::arg("instruments") = py::none());

  module.def(
      "_benchmark_book_callbacks",
      [](py::object strategy, std::size_t depth, std::size_t iterations) {
        (void)strategy.cast<std::shared_ptr<PythonStrategyHandle>>();
        if (depth == 0 || iterations == 0) {
          throw std::invalid_argument(
              "benchmark depth and iterations must be positive");
        }
        OwnedBookUpdate payload;
        payload.instrument_id = 1;
        payload.sequence = 1;
        payload.bids.reserve(depth);
        payload.asks.reserve(depth);
        for (std::size_t index = 0; index < depth; ++index) {
          const auto offset = static_cast<PriceTicks>(index);
          payload.bids.push_back(BookLevel{100 - offset, 1});
          payload.asks.push_back(BookLevel{101 + offset, 1});
        }

        std::chrono::nanoseconds elapsed;
        {
          py::gil_scoped_release release;
          const auto start = std::chrono::steady_clock::now();
          for (std::size_t index = 0; index < iterations; ++index) {
            py::gil_scoped_acquire acquire;
            strategy.attr("on_book_update")(payload);
          }
          elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - start);
        }
        return elapsed.count();
      },
      py::arg("strategy"), py::arg("depth"), py::arg("iterations") = 1'000);
}
