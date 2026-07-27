#include "trading/RiskEngine.hpp"

#include <cstdint>
#include <stdexcept>

namespace cmf::trading {
namespace {

[[nodiscard]] bool valid_side(Side side) noexcept {
  return side == Side::Buy || side == Side::Sell;
}

// Absolute value without relying on std::abs overflowing at the type minimum.
[[nodiscard]] Quantity magnitude(Quantity value) noexcept {
  return value < 0 ? -value : value;
}

} // namespace

RiskEngine::RiskEngine(std::span<const InstrumentMeta> instruments,
                       RiskLimits limits)
    : limits_(limits) {
  if (limits_.max_order_quantity <= 0 || limits_.max_position_abs < 0) {
    throw std::invalid_argument("risk limits must be positive");
  }
  instruments_.reserve(instruments.size());
  for (const auto &meta : instruments) {
    if (!instruments_.emplace(meta.instrument_id, meta).second) {
      throw std::invalid_argument("duplicate risk instrument metadata");
    }
  }
}

const InstrumentMeta *
RiskEngine::find(InstrumentId instrument_id) const noexcept {
  const auto iterator = instruments_.find(instrument_id);
  return iterator == instruments_.end() ? nullptr : &iterator->second;
}

RejectReason RiskEngine::check_new_order(
    InstrumentId instrument_id, Side side, PriceTicks price, Quantity quantity,
    Quantity net_position, std::size_t open_orders) const noexcept {
  // --- validity -----------------------------------------------------------
  const auto *meta = find(instrument_id);
  if (meta == nullptr) {
    return RejectReason::UnknownInstrument;
  }
  if (!valid_side(side)) {
    return RejectReason::InvalidSide;
  }
  if (quantity <= 0) {
    return RejectReason::NonPositiveQuantity;
  }
  if (price <= 0) {
    return RejectReason::InvalidPrice;
  }
  if (price % meta->tick_size_ticks != 0) {
    return RejectReason::TickMisalignment;
  }

  // --- limits -------------------------------------------------------------
  if (quantity > limits_.max_order_quantity) {
    return RejectReason::OrderQuantityLimitExceeded;
  }
  if (open_orders >= limits_.max_open_orders_per_instrument) {
    return RejectReason::TooManyOpenOrders;
  }

  // Worst case if this order fills completely. Overflow means the projection
  // cannot be represented, which is itself past any representable limit.
  const Quantity signed_quantity = side == Side::Buy ? quantity : -quantity;
  Quantity projected{};
  if (__builtin_add_overflow(net_position, signed_quantity, &projected)) {
    return RejectReason::PositionLimitExceeded;
  }
  if (magnitude(projected) > limits_.max_position_abs) {
    return RejectReason::PositionLimitExceeded;
  }

  return RejectReason::None;
}

} // namespace cmf::trading
