#pragma once

#include "core/BacktestConfig.hpp"
#include "core/Types.hpp"

#include <cstddef>
#include <span>
#include <unordered_map>

namespace cmf::trading {

// Pre-trade gate for new orders.
//
// This is the single place that decides whether an order may exist. It answers
// with the first violated rule, never with a list, because a strategy reacts to
// one reason and because a stable choice keeps the recorded order log
// reproducible when an order breaks two rules at once.
//
// Check order is fixed and part of the contract:
//
//   1. validity   instrument, side, quantity sign, price sign, tick alignment
//   2. limits     order quantity, open orders, resulting position
//
// Validity comes first so a malformed order is never reported as a limit
// breach. Within each group the order is cheapest and most specific first.
//
// The engine is stateless with respect to orders: the caller supplies the
// current net position and open-order count, so the same instance can be
// queried without it shadowing TradingEngine's bookkeeping.
class RiskEngine {
public:
  RiskEngine(std::span<const InstrumentMeta> instruments, RiskLimits limits);

  [[nodiscard]] RejectReason
  check_new_order(InstrumentId instrument_id, Side side, PriceTicks price,
                  Quantity quantity, Quantity net_position,
                  std::size_t open_orders) const noexcept;

  [[nodiscard]] const RiskLimits &limits() const noexcept { return limits_; }

private:
  [[nodiscard]] const InstrumentMeta *
  find(InstrumentId instrument_id) const noexcept;

  std::unordered_map<InstrumentId, InstrumentMeta> instruments_;
  RiskLimits limits_;
};

} // namespace cmf::trading
