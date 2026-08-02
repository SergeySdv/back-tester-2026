# CMF Advanced Backtesting Engine for Options

Deterministic C++20/Python backtesting engine for replaying Databento-like MBO
data, simulating latency-aware private limit orders, and returning fills, order
transitions, positions, and PnL to Python.

```text
MBO JSONL -> historical L3 books -> chronological scheduler
          -> trading engine -> Python Strategy callbacks
          -> frozen native columns -> pandas results
```

The runtime is deterministic and multi-instrument. It uses one dispatcher
thread, one trading thread, fixed market-data/order latency, an atomic processed
sequence barrier, full-fill-on-price-cross matching, and bulk
NumPy/pandas result hand-off.

- [New contributor guide](docs/hw4/GETTING_STARTED.md)
- [Architecture overview](docs/hw4/architecture/README.md)
- [Assignment-to-code traceability](docs/hw4/architecture/11_requirements_traceability.md)
- [Homework 4 source requirements](docs/hw4/source/01_homework_4_assignment.md)

## Prerequisites

- a C++20 compiler;
- [UV](https://docs.astral.sh/uv/).

UV is the repository's environment and dependency workflow. The locked
development environment includes CMake and Ninja, so no project-specific
system CMake installation is required.

## Set up and install

Create the locked environment and build the editable native Python extension:

```bash
curl -LsSf https://astral.sh/uv/install.sh | sh
uv sync --locked
uv run pip install -e .
uv run python -c "import back_tester; print(back_tester.__file__); print(back_tester.version())"
```

The distribution name is `back-tester-cmf`. Its import package is
`back_tester`, which loads `back_tester._backtester`.

## Python strategy API

`backtest.run()` drives the historical book, scheduler, trading engine, and
native result recorder from Databento-like JSONL or Feather V2 input:

```python
from back_tester import DateRange, Side, Strategy, backtest


class BuyTouch(Strategy):
    def on_book_update(self, update):
        if update.asks:
            self.submit_limit(
                update.instrument_id, Side.BUY, update.asks[0].price, 1
            )


result = backtest.run(BuyTouch(), "data/sample.mbo.jsonl", DateRange())
print(result.fills_df)
print(result.order_log_df)
print(result.pnl_series)
```

The three-argument form discovers only the unique numeric instrument IDs in a
metadata pass and then replays the file as a stream. It uses Databento
nanounits (`price_scale=1_000_000_000`, tick size 1, multiplier 1). Pass an
explicit `instruments=[InstrumentMeta(...)]` list for a one-pass replay and
real tick sizes or option multipliers.

Convert a directory of `*.mbo.json` files and replay the generated Feather
file through the same Python API:

```bash
uv run python scripts/convert_to_feather.py data/
```

```python
result = backtest.run(BuyTouch(), "data/sample.mbo.feather", DateRange())
```

Feather input must contain flattened `ts_recv`, `ts_event`, `instrument_id`,
`order_id`, `action`, `side`, `price`, `size`, `flags`, and `sequence` columns.
The Python loader validates the table, converts it once to native market events,
then releases the GIL for replay. The C++ CLI remains JSONL-only.

The optional `BacktestConfig` defaults to zero market-data latency, a strictly
positive one-nanosecond order latency, and top-15 callbacks. Strategy context
methods are intentionally available only while a callback is active. Result
DataFrames and the PnL Series are built in bulk from frozen typed native
columns; callback-scoped book levels are copied into immutable Python-owned
payloads.

## Deterministic end-to-end example

Run the checked-in two-instrument mean-reversion example after installation:

```bash
uv run python examples/mean_reversion.py
```

It exercises the real `backtest.run` path, including a delayed resting fill,
an independent cancelled order, callback ordering, positions, and PnL.

## Native build and tests

From a clean checkout, install the editable extension, verify the import, and
run the complete Release and Python suites:

```bash
uv sync --locked
uv run pip install -e .
uv run python -c "import back_tester; print(back_tester.__file__); print(back_tester.version())"
uv run cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
uv run cmake --build build-release -j
uv run ctest --test-dir build-release --output-on-failure
uv run pytest -q python/tests
uv run python examples/mean_reversion.py
```

Run the required benchmarks only from that Release setup:

```bash
build-release/bin/test/back-tester-scheduler-benchmark
build-release/bin/test/back-tester-price-cross-benchmark
uv run python python/benchmarks/callback_overhead.py
```

The scheduler benchmark reports warmed dispatcher-to-consumer round trips,
including ring capacity, waiting strategy, build/compiler/platform metadata,
and min/mean/p50/p95/p99. The price-cross benchmark reports construction and
native replay cost for 8- and 64-signal groups, including trigger-buffer
reallocations. The Python benchmark reports 20 warmed samples of
exactly 1,000 no-op callbacks for top-1 and top-15 payloads. A synthetic empty
native loop is not reported because the Release compiler elides it; callback
totals remain unadjusted. Parsing, process startup, logging, and DataFrame
construction are outside the timed region. Results are machine-specific
observations, not universal pass thresholds.

The test executable uses a small checked-in test runner. Native test
configuration does not download dependencies and does not require anything in
`3rdparty/`.

## CLI smoke run

The CLI requires exactly one data path. With no path it prints usage and exits
with status 64:

```bash
build-release/bin/back-tester
```

Run ingestion against the deterministic checked-in fixture:

```bash
build-release/bin/back-tester test/data/tiny_mbo.jsonl
```

A missing or unreadable path exits with status 2.

## Model limitations

- A same-instrument best quote or trade price crossing the limit fills the
  complete remaining order at the trigger price. Historical quote/trade size,
  queue position, market impact, probabilistic passive fills, slippage, and
  source-book mutation are not modeled.
- The mandatory runtime uses one process, one dispatcher, and one EngineView
  with deterministic fixed latency. Multi-engine simulation is only an
  extension point.
- Only limit GTC submission and cancel are supported. Replace, IOC/FOK,
  post-only, stops, pegs, and multi-leg orders are out of scope.
- Python input supports Databento-like MBO JSONL and flattened Feather V2. The
  C++ CLI remains JSONL-only. Options exercise, assignment, expiry settlement,
  Greeks, and a full risk engine are not implemented.

## Development checks

Install the pre-commit hooks or run all configured formatters and linters:

```bash
uv run pre-commit install
uv run pre-commit run --all-files
```

The hooks:

- format and lint C++ code with `clang-format`;
- format and lint Python code with `ruff`;
- strip outputs from Jupyter notebooks.
