// The order log is the contract a strategy author reasons about, so it must be
// byte-identical for the same input. Two things are checked here:
//
//   1. repeatability - two fresh engines fed the same script in one process
//      produce the same rows;
//   2. stability over time - those rows still match a checked-in golden file.
//
// The log is safe to freeze: every field is an integer, engine_ts_ns is virtual
// time, and nothing carries a wall clock, a PID or a path. Enums are written as
// numbers so that renaming a value does not churn the file.
//
// To regenerate after an intentional behaviour change:
//
//   BACKTESTER_UPDATE_GOLDEN=1 ctest --test-dir build -R Determinism
//
// then review the diff. Never regenerate to make a red test green without
// understanding which rows moved.

#include "EngineHarness.hpp"

#include "MiniTest.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace cmf;
using namespace harness;

constexpr InstrumentId kFirst = 1;
constexpr InstrumentId kSecond = 2;
constexpr TimestampNs kLatency = 100;

const std::vector<InstrumentMeta> kInstruments{
    InstrumentMeta{kFirst, 1, 1, 1}, InstrumentMeta{kSecond, 1, 1, 1}};

// Exercises every order-log event type in one run: an order that fills, an
// order that is cancelled, and two orders rejected up front for different
// reasons.
struct ScriptedStrategy final : RecordingStrategy {
  ClOrdId filling_order{};
  ClOrdId cancelled_order{};
  int book_updates{};

  void on_book_update(const BookUpdateView &update,
                      StrategyContext &context) override {
    RecordingStrategy::on_book_update(update, context);
    if (update.instrument_id != kFirst) {
      return;
    }
    ++book_updates;
    if (book_updates == 1) {
      filling_order = context.submit_limit(kFirst, Side::Buy, 100, 5);
      cancelled_order = context.submit_limit(kSecond, Side::Sell, 110, 3);
      // Rejected locally: no such instrument, and a non-positive quantity.
      (void)context.submit_limit(99, Side::Buy, 100, 1);
      (void)context.submit_limit(kFirst, Side::Buy, 100, 0);
    } else if (book_updates == 2) {
      (void)context.cancel_order(cancelled_order);
    }
  }
};

std::vector<OrderLogResultRow> run_reference_scenario() {
  Scenario scenario{kInstruments, BacktestConfig{0, kLatency, 15}};
  scenario.script().group(
      add_order(kFirst, 1000, 1, 7001, Side::Sell, 105, 10));
  scenario.script().group(
      add_order(kFirst, 1200, 2, 7002, Side::Sell, 106, 10));
  scenario.script().group(trade(kFirst, 1250, 3, Side::Sell, 100, 5));
  scenario.script().group(
      add_order(kFirst, 1500, 4, 7003, Side::Sell, 107, 10));

  ScriptedStrategy strategy;
  RecordingRecorder recorder;
  scenario.run(strategy, recorder);
  return recorder.order_log;
}

constexpr auto kHeader =
    "engine_ts_ns,instrument_id,client_order_id,event_type,"
    "state,side,limit_price_ticks,order_quantity,"
    "filled_quantity,remaining_quantity,reject_reason";

std::string format_row(const OrderLogResultRow &row) {
  std::ostringstream out;
  out << row.engine_ts_ns << ',' << row.instrument_id << ','
      << row.client_order_id << ',' << static_cast<int>(row.event_type) << ','
      << static_cast<int>(row.state) << ',' << static_cast<int>(row.side) << ','
      << row.limit_price_ticks << ',' << row.order_quantity << ','
      << row.filled_quantity << ',' << row.remaining_quantity << ','
      << static_cast<int>(row.reject_reason);
  return out.str();
}

bool same_row(const OrderLogResultRow &left, const OrderLogResultRow &right) {
  return format_row(left) == format_row(right);
}

std::string golden_path() {
  return std::string(BACKTESTER_TEST_DATA_DIR) + "/golden_order_log.csv";
}

// LF only, and no trailing whitespace, so the file survives a Windows checkout.
std::string serialize(const std::vector<OrderLogResultRow> &rows) {
  std::string text(kHeader);
  text += '\n';
  for (const auto &row : rows) {
    text += format_row(row);
    text += '\n';
  }
  return text;
}

std::vector<std::string> read_lines(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("golden file not readable: " + path);
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      throw std::runtime_error("golden file has CRLF endings: " + path);
    }
    lines.push_back(line);
  }
  return lines;
}

bool update_requested() {
  const char *flag = std::getenv("BACKTESTER_UPDATE_GOLDEN");
  return flag != nullptr && std::string(flag) == "1";
}

} // namespace

TEST_CASE("Repeated runs of one scenario produce an identical order log",
          "[Determinism]") {
  const auto first = run_reference_scenario();
  const auto second = run_reference_scenario();

  REQUIRE(!first.empty());
  REQUIRE(first.size() == second.size());
  for (std::size_t index = 0; index < first.size(); ++index) {
    if (!same_row(first[index], second[index])) {
      throw std::runtime_error(
          "order log row " + std::to_string(index) +
          " differs between runs:\n  first:  " + format_row(first[index]) +
          "\n  second: " + format_row(second[index]));
    }
  }
}

TEST_CASE("The scenario covers every order-log event type", "[Determinism]") {
  // Guards the golden file's value: a log that only ever holds Submit rows
  // would freeze nothing interesting.
  const auto rows = run_reference_scenario();
  bool seen[6] = {};
  for (const auto &row : rows) {
    seen[static_cast<std::size_t>(row.event_type)] = true;
  }
  REQUIRE(seen[static_cast<std::size_t>(OrderLogEventType::Submit)]);
  REQUIRE(seen[static_cast<std::size_t>(OrderLogEventType::Accepted)]);
  REQUIRE(seen[static_cast<std::size_t>(OrderLogEventType::Fill)]);
  REQUIRE(seen[static_cast<std::size_t>(OrderLogEventType::CancelRequest)]);
  REQUIRE(seen[static_cast<std::size_t>(OrderLogEventType::Cancelled)]);
  REQUIRE(seen[static_cast<std::size_t>(OrderLogEventType::Reject)]);
}

TEST_CASE("Order log matches the checked-in golden file", "[Determinism]") {
  const auto rows = run_reference_scenario();
  const std::string text = serialize(rows);
  const std::string path = golden_path();

  if (update_requested()) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("cannot write golden file: " + path);
    }
    output << text;
    return;
  }

  const auto expected = read_lines(path);
  std::vector<std::string> actual;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    actual.push_back(line);
  }

  if (!expected.empty()) {
    REQUIRE(expected[0] == kHeader);
  }
  // Compare line by line: a whole-file REQUIRE gives an unreadable diff.
  const std::size_t common = std::min(expected.size(), actual.size());
  for (std::size_t index = 0; index < common; ++index) {
    if (expected[index] != actual[index]) {
      throw std::runtime_error(
          "golden mismatch at line " + std::to_string(index + 1) +
          "\n  expected: " + expected[index] +
          "\n  actual:   " + actual[index] +
          "\nIf this change is intended, regenerate with "
          "BACKTESTER_UPDATE_GOLDEN=1 and review the diff.");
    }
  }
  if (expected.size() != actual.size()) {
    throw std::runtime_error(
        "golden file has " + std::to_string(expected.size()) +
        " lines but the run produced " + std::to_string(actual.size()) +
        ". Regenerate with BACKTESTER_UPDATE_GOLDEN=1 and review the diff.");
  }
}

TEST_CASE("Golden file carries no wall-clock, pid or path data",
          "[Determinism]") {
  // Cheap guard against someone adding a non-reproducible column later.
  for (const auto &line : read_lines(golden_path())) {
    REQUIRE(line.find('/') == std::string::npos);
    REQUIRE(line.find('\\') == std::string::npos);
    REQUIRE(line.find(':') == std::string::npos);
  }
}
