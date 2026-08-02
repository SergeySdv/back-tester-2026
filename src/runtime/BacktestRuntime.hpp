#pragma once

#include "core/BacktestConfig.hpp"
#include "market/MarketDataEvent.hpp"
#include "results/ResultRecorder.hpp"
#include "trading/Strategy.hpp"

#include <string>
#include <vector>

namespace cmf::runtime {

inline constexpr TimestampNs default_order_latency_ns = 1;

[[nodiscard]] std::vector<InstrumentMeta>
discover_databento_instruments(const std::string &data_path);

[[nodiscard]] results::FrozenResults
run_backtest(trading::Strategy &strategy, const std::string &data_path,
             DateRange date_range, BacktestConfig config,
             std::vector<InstrumentMeta> instruments);

[[nodiscard]] results::FrozenResults run_backtest_events(
    trading::Strategy &strategy, std::vector<market::MarketDataEvent> events,
    std::string source_name, DateRange date_range, BacktestConfig config,
    std::vector<InstrumentMeta> instruments);

} // namespace cmf::runtime
