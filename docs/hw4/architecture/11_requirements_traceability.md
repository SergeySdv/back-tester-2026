# Assignment-to-code traceability

This matrix maps the two Homework 4 source documents to the implemented code
and executable verification:

- [`../source/01_homework_4_assignment.md`](../source/01_homework_4_assignment.md)
- [`../source/02_original_big_picture_mermaid.md`](../source/02_original_big_picture_mermaid.md)

The source assignment defines required behavior. The architecture documents
resolve its ambiguities. The implementation and tests listed here provide the
current evidence.

## Written assignment

| Assignment requirement | Implemented behavior | Code | Verification |
|---|---|---|---|
| Agree `on_book_update()` granularity | Callback after a complete atomic source group and only when the top-N view changes; configurable depth, default 15 | `src/runtime/BacktestRuntime.cpp`, `src/core/Events.hpp` | `test/RuntimeTest.cpp`, `python/tests/test_runtime.py` |
| Independent fixed market-data and order latency | Market delivery is `exchange_ts + market_data_latency`; order/cancel arrival is callback-visible engine time plus strictly positive order latency | `src/core/BacktestConfig.hpp`, `src/runtime/BacktestRuntime.cpp`, `src/trading/TradingEngine.cpp` | `test/TradingTest.cpp`, `test/SchedulerTest.cpp` |
| Consume book update, snapshot, and trade messages | One atomic `MarketDelivery` may carry a top-N book view, snapshot marker, ordered trades, and raw-order quote/trade cross signals | `src/core/Events.hpp`, `src/runtime/BacktestRuntime.cpp` | `test/CoreContractsTest.cpp`, `test/RuntimeTest.cpp`, Python callback tests |
| Advance a virtual engine clock | `TradingEngine` sets `now_ns` to the scheduled key and rejects backward movement | `src/trading/TradingEngine.cpp` | scheduler and trading chronology tests |
| Atomic ready sequence, not a queue message | Trading publishes monotonically increasing `processed_seq`; dispatcher waits before advancing shared state | `src/scheduler/ReadyBarrier.hpp`, `src/scheduler/SchedulerRuntime.hpp` | `test/SchedulerTest.cpp`, scheduler benchmark |
| Prefetch may not permit N+1 consumption before N completes | One market event may be staged, but book application and publication remain ordered by acknowledgement | `SchedulerRuntime.hpp`, runtime `JsonlScheduledSource` | scheduler prefetch/acknowledgement tests, `test/RuntimeTest.cpp` |
| `HistoricalLOB + EngineView = SimulatedLOB` | Shared historical L3 store plus `SimulatedLOB`-owned private resting orders; ordered quote/trade signals provide matching triggers | `src/market/`, `src/runtime/BacktestRuntime.cpp`, `src/trading/SimulatedLOB.*` | `test/CoreMarketTest.cpp`, `test/TradingTest.cpp`, `test/TypedSimulatedLOBTest.cpp` |
| Fill-at-touch matching | The first same-instrument best-quote or trade-price cross fully fills the remaining order at the trigger price, ignoring historical size | `src/core/Events.hpp`, `src/runtime/BacktestRuntime.cpp`, `src/trading/SimulatedLOB.cpp` | oversized quote/trade, pre-arrival, source-order, and limit-protection tests |
| Resting order matching | Uncrossed orders rest in `EngineView` price-time indexes and are reevaluated by later ordered price-cross signals | `src/trading/SimulatedLOB.*` | resting full-fill and FIFO tests in `test/TradingTest.cpp` |
| Order latency affects fills | Submission creates local `PendingNew`; matching begins only on scheduled new-order arrival | `src/trading/TradingEngine.cpp`, `src/scheduler/SchedulerRuntime.hpp` | delayed-arrival and equal-time market-priority tests |
| Order Manager / Position Keeper | `TradingEngine` owns lifecycle/open indexes; `PositionKeeper` updates signed positions and FIFO accounting inputs before callbacks | `src/trading/TradingEngine.*`, `PositionKeeper.*` | state, cancel, position, overflow, and callback-observation tests |
| Strategy queries position and open orders | Multi-instrument callback-scoped Strategy context returns immutable Python values | `src/trading/Strategy.hpp`, `src/python/bindings.cpp` | `python/tests/test_runtime.py`, `python/tests/test_end_to_end.py` |
| Strategy submits and cancels orders | Python methods call native context; commands enter the delayed scheduler ring without recursive matching | `src/python/bindings.cpp`, `src/trading/TradingEngine.cpp` | Python runtime and end-to-end tests |
| Choose one packaging workflow and use scikit-build-core | UV lock/sync workflow; scikit-build-core builds the pybind11 extension | `pyproject.toml`, `uv.lock`, `src/python/CMakeLists.txt` | editable install and import commands in root `README.md` |
| Bind core types and submission API through pybind11 | Public enums, configuration, callback payloads, Strategy, queries, and Result are bound in `_backtester` | `src/python/bindings.cpp`, `python/back_tester/__init__.py` | `python/tests/test_runtime.py` |
| `on_book_update`, `on_trade`, `on_fill`, `on_reject` | All four callbacks use typed multi-instrument payloads; state precedes fill/reject callbacks | `src/trading/Strategy.hpp`, `TradingEngine.cpp`, `src/python/bindings.cpp` | native callback-order tests and Python callback tests |
| Correct GIL discipline | GIL released around native run/waits; acquired for each Python callback; context active only on callback thread | `src/python/bindings.cpp` | Python exception, context, concurrency, and second-run tests |
| `Result.pnl_series`, `fills_df`, `order_log_df` | Typed native columns freeze into immutable shared storage and are exposed as pandas objects | `src/results/ResultRecorder.*`, `src/python/bindings.cpp` | `test/ResultsTest.cpp`, Python result schema/lifetime tests |
| Zero-copy NumPy/Arrow hand-off | NumPy views point to frozen native columns and retain a shared owner; pandas construction is bulk with `copy=False` | `src/python/bindings.cpp`, `src/results/ResultRecorder.*` | native freeze/lifetime tests and Python array lifetime tests |
| Minimal `backtest.run(strategy, data_path, date_range)` | Three-argument form plus optional config/instruments; omitted metadata triggers deterministic discovery | `src/python/bindings.cpp`, `src/runtime/BacktestRuntime.*` | Python runtime and end-to-end tests |
| Synthetic matching harness | Native tests cover quote/trade crossing, full oversized fills, limit protection, pre-arrival exclusion, resting, FIFO, rejects, and isolation | `test/TradingTest.cpp`, `test/TypedSimulatedLOBTest.cpp` | `uv run ctest --test-dir build-release --output-on-failure` |
| End-to-end Python strategy | Two-instrument example uses the production streaming runtime and returns fills/orders/PnL | `examples/mean_reversion.py`, `test/data/m5_two_instrument.jsonl` | `python/tests/test_end_to_end.py`; example command |
| Ready-signal benchmark | Warmed dispatcher-to-consumer-to-acknowledgement measurement with 100,000 samples and latency percentiles | `test/SchedulerBenchmark.cpp` | `build-release/bin/test/back-tester-scheduler-benchmark` |
| Exact raw-signal chronology performance | Reused trigger-buffer construction and `SimulatedLOB` replay for 8- and 64-signal groups, including initial reallocations | `test/PriceCrossBenchmark.cpp` | `build-release/bin/test/back-tester-price-cross-benchmark` |
| 1,000-callback benchmark | 20 warmed top-1/top-15 samples, exactly 1,000 no-op callbacks per sample | `python/benchmarks/callback_overhead.py` | `uv run python python/benchmarks/callback_overhead.py` |
| N concurrent EngineViews bonus | Not exposed by the production runtime; independent typed `SimulatedLOB` instances maintain isolated private order state under the infinite-liquidity model | `src/trading/SimulatedLOB.*` | `test/TypedSimulatedLOBTest.cpp` |

## Original big-picture diagram

The diagram is broader than the written HW4 deliverable. This table records
whether each box is implemented directly, collapsed into an in-process
component, represented at the boundary, or intentionally outside scope.

| Diagram box or flow | Final implementation disposition | Code / evidence |
|---|---|---|
| Databento JSON data source | Implemented as streaming Databento-like MBO JSONL | `src/market/JsonlReader.*`, `test/data/*.jsonl` |
| Feather data source | Implemented for Python `backtest.run()` through validated Arrow IPC columns and native in-memory events; C++ CLI remains JSONL-only | `python/back_tester/_input.py`, `src/python/bindings.cpp`, `python/tests/test_feather.py` |
| Event Merger | One prefetched market source is dynamically merged with delayed strategy commands | `src/scheduler/SchedulerRuntime.hpp` |
| Chronological Dispatcher | Implemented with stable key `(scheduled time, priority, sequence)` | `ChronologicalScheduler.*`, scheduler ordering tests |
| Map of LOBs per instrument | Implemented by `HistoricalLOBStore` | `src/market/HistoricalLOBStore.*` |
| Market Data Publisher | Represented by the dispatcher-side SPSC event producer | `SchedulerRuntime.hpp`, `SpscRing.hpp` |
| Market Data Consumer | Represented by the trading-thread event consumer invoking `TradingEngine` | `SchedulerRuntime.hpp`, `TradingEngine::operator()` |
| LOB Simulation, Matching & Fills | Typed `SimulatedLOB` owns private state and creates fill decisions; `TradingEngine` applies them to lifecycle, positions, results, and callbacks | `src/trading/SimulatedLOB.*`, `src/trading/TradingEngine.*`, trading tests |
| Order Manager | Lifecycle and open-order indexes are owned by `TradingEngine` | `TradingEngine.*` |
| Position Keeper | Implemented as a dedicated native component | `PositionKeeper.*` |
| Gateway Client / Server | Collapsed into the in-process command ring and scheduled command arrivals | `CommandSink`, `SpscRing<OrderCommand>`, `SchedulerRuntime` |
| Slippage Simulator | No separate authority; fixed latency plus optimistic full-fill-on-cross matching defines execution | scope and matching architecture |
| Risk Engine | Full risk engine is outside scope; deterministic order/instrument validation is implemented | `TradingEngine::validate_order`, runtime metadata validation |
| Feature Generator | No native feature framework; strategies compute features from callbacks | `examples/mean_reversion.py` |
| Strategy Logic | Python Strategy methods invoked inside the native trading thread through pybind11 | `src/python/bindings.cpp` |
| Strategy Parameter Setup | Python construction of Strategy, `BacktestConfig`, `DateRange`, and instrument metadata | public package and example |
| API | `back_tester.backtest.run()` plus bound Strategy context and Result | `python/back_tester/__init__.py`, bindings |
| Visualization & Analysis | pandas DataFrames/Series are returned for downstream analysis; no UI framework is bundled | Python `Result` binding |
| API result return path | Native columns freeze after thread join and remain alive through shared NumPy owners | `ResultRecorder.*`, `src/python/bindings.cpp` |

## Verification commands

```bash
uv sync --locked
uv run pip install -e .
uv run cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
uv run cmake --build build-release -j
uv run ctest --test-dir build-release --output-on-failure
uv run pytest -q python/tests
uv run python examples/mean_reversion.py
build-release/bin/test/back-tester-scheduler-benchmark
build-release/bin/test/back-tester-price-cross-benchmark
uv run python python/benchmarks/callback_overhead.py
```
