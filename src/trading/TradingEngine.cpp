#include "trading/TradingEngine.hpp"

#include <limits>
#include <utility>

namespace cmf::trading {
namespace {

[[nodiscard]] bool is_terminal(OrderState state) noexcept {
  return state == OrderState::Filled || state == OrderState::Cancelled ||
         state == OrderState::Rejected;
}

} // namespace

TradingEngine::TradingEngine(std::span<const InstrumentMeta> instruments,
                             BacktestConfig config,
                             const market::HistoricalLOBStore &books,
                             Strategy &strategy, Recorder &recorder)
    : config_(config), books_(books), strategy_(strategy), recorder_(recorder),
      risk_(instruments, config.risk), simulated_lob_(instruments) {
  if (config_.market_data_latency_ns < 0 || config_.order_latency_ns <= 0 ||
      config_.book_depth == 0) {
    throw std::invalid_argument("invalid backtest configuration");
  }
  instruments_.reserve(instruments.size());
  open_order_ids_.reserve(instruments.size());
  for (const auto &meta : instruments) {
    positions_.register_instrument(meta);
    const auto [iterator, inserted] =
        instruments_.emplace(meta.instrument_id, meta);
    (void)iterator;
    if (!inserted) {
      throw std::invalid_argument("duplicate instrument metadata");
    }
    open_order_ids_.try_emplace(meta.instrument_id);
  }
  query_buffer_.reserve(32);
}

TimestampNs TradingEngine::delayed_arrival() const {
  if (config_.order_latency_ns > 0 &&
      now_ns_ >
          std::numeric_limits<TimestampNs>::max() - config_.order_latency_ns) {
    throw TradingError("scheduled command timestamp overflow");
  }
  return now_ns_ + config_.order_latency_ns;
}

Sequence TradingEngine::next_command_sequence() {
  if (next_command_sequence_ == std::numeric_limits<Sequence>::max()) {
    throw TradingError("command sequence exhausted");
  }
  return ++next_command_sequence_;
}

ClOrdId TradingEngine::next_client_order_id() {
  if (next_client_order_id_ == std::numeric_limits<ClOrdId>::max()) {
    throw TradingError("client order id exhausted");
  }
  return ++next_client_order_id_;
}

const InstrumentMeta *
TradingEngine::find_instrument(InstrumentId instrument_id) const noexcept {
  const auto iterator = instruments_.find(instrument_id);
  return iterator == instruments_.end() ? nullptr : &iterator->second;
}

RejectReason TradingEngine::validate_order(InstrumentId instrument_id,
                                           Side side, PriceTicks price,
                                           Quantity quantity) const noexcept {
  // RiskEngine owns the decision and its ordering. This function only gathers
  // the state it needs, so validity rules and limits cannot drift apart.
  Quantity net_position{};
  std::size_t open_orders{};
  if (find_instrument(instrument_id) != nullptr) {
    net_position = positions_.position(instrument_id).net_quantity;
    const auto indexed = open_order_ids_.find(instrument_id);
    if (indexed != open_order_ids_.end()) {
      open_orders = indexed->second.size();
    }
  }
  return risk_.check_new_order(instrument_id, side, price, quantity,
                               net_position, open_orders);
}

void TradingEngine::ensure_active_sink() const {
  if (active_commands_ == nullptr || callback_depth_ == 0) {
    throw TradingError("strategy command submitted outside an event callback");
  }
}

ClOrdId TradingEngine::submit_limit(InstrumentId instrument_id, Side side,
                                    PriceTicks limit_price_ticks,
                                    Quantity quantity) {
  ensure_active_sink();
  const ClOrdId client_order_id = next_client_order_id();
  OwnOrder order{OrderQueryRow{instrument_id, client_order_id,
                               OrderState::PendingNew, side, limit_price_ticks,
                               quantity, 0, quantity, 0}};
  const auto [iterator, inserted] = orders_.emplace(client_order_id, order);
  if (!inserted) {
    throw TradingError("generated duplicate client order id");
  }
  emit_order_event(iterator->second, OrderLogEventType::Submit);

  const RejectReason reason =
      validate_order(instrument_id, side, limit_price_ticks, quantity);
  if (reason != RejectReason::None) {
    reject_new(iterator->second, reason);
    return client_order_id;
  }
  open_order_ids_.at(instrument_id).insert(client_order_id);

  const Sequence command_sequence = next_command_sequence();
  const NewOrderCommand command{client_order_id,   instrument_id,   side,
                                limit_price_ticks, quantity,        now_ns_,
                                delayed_arrival(), command_sequence};
  if (!active_commands_->push(command)) {
    throw TradingError("command sink closed while submitting order");
  }
  return client_order_id;
}

bool TradingEngine::cancel_order(ClOrdId client_order_id) {
  ensure_active_sink();
  const auto iterator = orders_.find(client_order_id);
  if (iterator == orders_.end()) {
    emit_reject(0, client_order_id, RejectReason::UnknownOrder, now_ns_);
    return false;
  }
  auto &order = iterator->second;
  if (is_terminal(order.query.state)) {
    emit_reject(order.query.instrument_id, client_order_id,
                RejectReason::AlreadyTerminal, now_ns_);
    return false;
  }
  if (order.query.state == OrderState::PendingNew) {
    emit_reject(order.query.instrument_id, client_order_id,
                RejectReason::UnknownOrder, now_ns_);
    return false;
  }
  if (order.cancel_requested) {
    emit_reject(order.query.instrument_id, client_order_id,
                RejectReason::AlreadyTerminal, now_ns_);
    return false;
  }
  order.cancel_requested = true;
  order.query.state = OrderState::PendingCancel;
  emit_order_event(order, OrderLogEventType::CancelRequest);
  const Sequence command_sequence = next_command_sequence();
  const CancelCommand command{client_order_id, order.query.instrument_id,
                              now_ns_, delayed_arrival(), command_sequence};
  if (!active_commands_->push(command)) {
    throw TradingError("command sink closed while submitting cancel");
  }
  return true;
}

PositionSnapshot TradingEngine::position(InstrumentId instrument_id) const {
  return positions_.position(instrument_id);
}

std::span<const OrderQueryRow>
TradingEngine::open_orders(InstrumentId instrument_id) {
  query_buffer_.clear();
  const auto indexed = open_order_ids_.find(instrument_id);
  if (indexed == open_order_ids_.end()) {
    return query_buffer_;
  }
  for (const ClOrdId client_order_id : indexed->second) {
    const auto order = orders_.find(client_order_id);
    if (order == orders_.end() || is_terminal(order->second.query.state) ||
        order->second.query.remaining_quantity <= 0) {
      throw TradingError("open-order index is inconsistent");
    }
    query_buffer_.push_back(order->second.query);
  }
  return query_buffer_;
}

void TradingEngine::operator()(const ScheduledEvent &event,
                               scheduler::CommandSink &commands) {
  const TimestampNs event_time = event.key().scheduled_ts_ns;
  if (has_time_ && event_time < now_ns_) {
    throw TradingError("trading virtual clock moved backwards");
  }
  now_ns_ = event_time;
  has_time_ = true;
  active_commands_ = &commands;
  try {
    std::visit(
        [this](const auto &payload) {
          using Payload = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<Payload, MarketDelivery>) {
            process_market(payload);
          } else if constexpr (std::is_same_v<Payload, NewOrderCommand>) {
            process_new(payload);
          } else {
            process_cancel(payload);
          }
        },
        event.payload());
  } catch (...) {
    active_commands_ = nullptr;
    throw;
  }
  active_commands_ = nullptr;
}

void TradingEngine::process_market(const MarketDelivery &delivery) {
  if (find_instrument(delivery.instrument_id) == nullptr) {
    throw TradingError("market delivery references unknown instrument");
  }
  Sequence previous_signal_sequence{};
  bool have_previous_signal{};
  for (const auto &signal : delivery.price_cross_signals) {
    if (signal.instrument_id != delivery.instrument_id ||
        signal.exchange_ts_ns != delivery.exchange_ts_ns ||
        signal.engine_ts_ns != delivery.engine_ts_ns ||
        (have_previous_signal &&
         signal.source_sequence <= previous_signal_sequence) ||
        signal.source_sequence > delivery.source_sequence) {
      throw TradingError("market delivery has invalid price-cross signal");
    }
    previous_signal_sequence = signal.source_sequence;
    have_previous_signal = true;
    apply_fills(simulated_lob_.on_signal(signal), signal.exchange_ts_ns);
  }
  for (const auto &trade : delivery.trades) {
    invoke_strategy_callback(
        [this, &trade] { strategy_.on_trade(trade, *this); });
  }
  if (delivery.book_update.has_value()) {
    invoke_strategy_callback([this, &delivery] {
      strategy_.on_book_update(*delivery.book_update, *this);
    });
  }
}

void TradingEngine::process_new(const NewOrderCommand &command) {
  const auto iterator = orders_.find(command.client_order_id);
  if (iterator == orders_.end()) {
    emit_reject(command.instrument_id, command.client_order_id,
                RejectReason::UnknownOrder, command.scheduled_arrival_ts_ns);
    return;
  }
  auto &order = iterator->second;
  if (order.query.state != OrderState::PendingNew) {
    emit_reject(command.instrument_id, command.client_order_id,
                RejectReason::AlreadyTerminal, command.scheduled_arrival_ts_ns);
    return;
  }
  if (command.instrument_id != order.query.instrument_id ||
      command.side != order.query.side ||
      command.limit_price_ticks != order.query.limit_price_ticks ||
      command.quantity != order.query.order_quantity) {
    reject_new(order, RejectReason::DuplicateClientOrderId);
    return;
  }

  const auto *book = books_.find(command.instrument_id);
  order.query.exchange_arrival_sequence = command.command_sequence;
  order.query.state = OrderState::Open;
  emit_order_event(order, OrderLogEventType::Accepted);
  apply_fills(simulated_lob_.accept(
                  order.query.client_order_id, order.query.instrument_id,
                  order.query.side, order.query.limit_price_ticks,
                  order.query.remaining_quantity,
                  order.query.exchange_arrival_sequence, book),
              command.scheduled_arrival_ts_ns);
}

void TradingEngine::process_cancel(const CancelCommand &command) {
  const auto iterator = orders_.find(command.client_order_id);
  if (iterator == orders_.end()) {
    emit_reject(command.instrument_id, command.client_order_id,
                RejectReason::UnknownOrder, command.scheduled_arrival_ts_ns);
    return;
  }
  auto &order = iterator->second;
  if (is_terminal(order.query.state)) {
    emit_reject(order.query.instrument_id, order.query.client_order_id,
                RejectReason::AlreadyTerminal, command.scheduled_arrival_ts_ns);
    return;
  }
  if (!order.cancel_requested) {
    emit_reject(order.query.instrument_id, order.query.client_order_id,
                RejectReason::UnknownOrder, command.scheduled_arrival_ts_ns);
    return;
  }
  simulated_lob_.cancel(order.query.client_order_id);
  order.query.state = OrderState::Cancelled;
  order.cancel_requested = false;
  open_order_ids_.at(order.query.instrument_id)
      .erase(order.query.client_order_id);
  emit_order_event(order, OrderLogEventType::Cancelled);
}

void TradingEngine::apply_fills(std::span<const SyntheticFill> fills,
                                TimestampNs exchange_ts_ns) {
  for (const auto &fill : fills) {
    const auto order = orders_.find(fill.client_order_id);
    if (order == orders_.end()) {
      throw TradingError("SimulatedLOB filled unknown private order");
    }
    apply_fill(order->second, fill.price, fill.quantity, exchange_ts_ns,
               fill.liquidity_source, fill.trigger_source_sequence);
  }
}

void TradingEngine::apply_fill(OwnOrder &order, PriceTicks price,
                               Quantity quantity, TimestampNs exchange_ts_ns,
                               LiquiditySource liquidity_source,
                               Sequence trigger_source_sequence) {
  order.query.filled_quantity += quantity;
  order.query.remaining_quantity -= quantity;
  order.query.state = order.query.remaining_quantity == 0
                          ? OrderState::Filled
                          : OrderState::PartiallyFilled;
  positions_.apply_fill(order.query.instrument_id, order.query.side, price,
                        quantity);
  if (order.query.remaining_quantity == 0) {
    open_order_ids_.at(order.query.instrument_id)
        .erase(order.query.client_order_id);
  }
  emit_order_event(order, OrderLogEventType::Fill);

  if (next_fill_sequence_ == std::numeric_limits<Sequence>::max()) {
    throw TradingError("fill sequence exhausted");
  }
  const FillView fill{order.query.instrument_id,
                      order.query.client_order_id,
                      order.query.side,
                      price,
                      quantity,
                      order.query.remaining_quantity,
                      exchange_ts_ns,
                      now_ns_,
                      ++next_fill_sequence_,
                      liquidity_source,
                      trigger_source_sequence};
  recorder_.on_fill(
      FillResultRow{fill.exchange_ts_ns, fill.engine_ts_ns, fill.instrument_id,
                    fill.client_order_id, fill.side, fill.price, fill.quantity,
                    fill.remaining_quantity, fill.liquidity_source,
                    fill.trigger_source_sequence});
  invoke_strategy_callback([this, &fill] { strategy_.on_fill(fill, *this); });
}

void TradingEngine::emit_order_event(const OwnOrder &order,
                                     OrderLogEventType event_type,
                                     RejectReason reason) {
  recorder_.on_order_event(OrderLogResultRow{
      now_ns_,
      order.query.instrument_id,
      order.query.client_order_id,
      event_type,
      order.query.state,
      order.query.side,
      order.query.limit_price_ticks,
      order.query.order_quantity,
      order.query.filled_quantity,
      order.query.remaining_quantity,
      reason,
  });
}

void TradingEngine::emit_reject(InstrumentId instrument_id,
                                ClOrdId client_order_id, RejectReason reason,
                                TimestampNs exchange_ts_ns) {
  if (next_reject_sequence_ == std::numeric_limits<Sequence>::max()) {
    throw TradingError("reject sequence exhausted");
  }
  const RejectView reject{instrument_id, client_order_id,
                          reason,        exchange_ts_ns,
                          now_ns_,       ++next_reject_sequence_};
  recorder_.on_reject(reject);
  if (callback_depth_ != 0) {
    deferred_rejects_.push_back(reject);
    return;
  }
  invoke_strategy_callback(
      [this, &reject] { strategy_.on_reject(reject, *this); });
}

void TradingEngine::drain_deferred_rejects() {
  if (callback_depth_ != 0 || draining_rejects_) {
    return;
  }
  draining_rejects_ = true;
  try {
    while (!deferred_rejects_.empty()) {
      const RejectView reject = deferred_rejects_.front();
      deferred_rejects_.pop_front();
      invoke_strategy_callback(
          [this, &reject] { strategy_.on_reject(reject, *this); });
    }
  } catch (...) {
    draining_rejects_ = false;
    throw;
  }
  draining_rejects_ = false;
}

void TradingEngine::reject_new(OwnOrder &order, RejectReason reason) {
  order.query.state = OrderState::Rejected;
  const auto indexed = open_order_ids_.find(order.query.instrument_id);
  if (indexed != open_order_ids_.end()) {
    indexed->second.erase(order.query.client_order_id);
  }
  emit_order_event(order, OrderLogEventType::Reject, reason);
  emit_reject(order.query.instrument_id, order.query.client_order_id, reason,
              now_ns_);
}

} // namespace cmf::trading
