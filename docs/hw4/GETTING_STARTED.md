# Getting started

This guide takes a new contributor from a clean checkout to a verified local
run, then explains the minimum configuration needed to write a strategy or use
different market data.

## 1. Prerequisites

The project requires a native C++20 toolchain and Python 3.12. You need:

- Git;
- a C++20 compiler (recent Clang, GCC, or MSVC);
- [UV](https://docs.astral.sh/uv/).

The CMake configuration handles Clang, GCC, and MSVC warning policies. The
final documented verification was executed with AppleClang 17 on macOS arm64;
other compiler/platform combinations remain valid QA targets.

UV installs the locked Python environment and supplies CMake, Ninja, and the
Python development dependencies. It can also provision the required Python
version when Python 3.12 is not already installed.

All commands below run from the repository root.

## 2. Install and verify

```bash
uv sync --locked
uv run pip install -e .
uv run python -c "import back_tester; print(back_tester.__file__); print(back_tester.version())"
```

The editable install compiles the pybind11 extension and exposes the
`back_tester` import package. A successful import should print a path inside
the checkout or its editable build and version `0.0.1`.

Run the checked-in end-to-end example:

```bash
uv run python examples/mean_reversion.py
```

The example replays two instruments, submits delayed orders, produces a fill,
cancels an independent order, and prints callback order, final positions,
terminal order states, fill count, and final PnL.

## 3. Build and test the native runtime

```bash
uv run cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
uv run cmake --build build-release -j
uv run ctest --test-dir build-release --output-on-failure
uv run pytest -q python/tests
```

The CTest suite covers native units, runtime integration, and CLI fixtures.
The pytest suite covers the public Python API, callbacks, exception shutdown,
result ownership, determinism, and the end-to-end strategy.

For a quick ingestion-only smoke test:

```bash
build-release/bin/back-tester test/data/tiny_mbo.jsonl
```

## 4. Write a strategy

Subclass `Strategy` and override only the callbacks you need:

```python
from back_tester import BacktestConfig, DateRange, Side, Strategy, backtest


class BuyBestAsk(Strategy):
    def on_book_update(self, update):
        if update.asks:
            self.submit_limit(
                update.instrument_id,
                Side.BUY,
                update.asks[0].price,
                1,
            )

    def on_trade(self, trade):
        pass

    def on_fill(self, fill):
        print(fill.instrument_id, fill.price, fill.quantity)

    def on_reject(self, reject):
        raise RuntimeError(f"order rejected: {reject.reason}")


result = backtest.run(
    BuyBestAsk(),
    "test/data/tiny_mbo.jsonl",
    DateRange(),
    BacktestConfig(),
)
```

Inside a callback, a strategy may call:

- `submit_limit(instrument_id, side, price_ticks, quantity)`;
- `cancel_order(client_order_id)`;
- `position(instrument_id)`;
- `open_orders(instrument_id)`;
- `now_ns`.

These context operations are deliberately unavailable outside an active
callback. Orders and cancels are scheduled at `now_ns + order_latency_ns`; they
do not execute recursively inside the callback.

The returned `Result` contains:

- `fills_df`;
- `order_log_df`;
- `pnl_series`.

See
[`architecture/06_python_api_and_results.md`](architecture/06_python_api_and_results.md)
for callback payload fields and exact result schemas.

## 5. Configure a run

### Latency and callback depth

```python
config = BacktestConfig(
    market_data_latency_ns=50,
    order_latency_ns=200,
    book_depth=15,
)
```

- `market_data_latency_ns` must be non-negative.
- `order_latency_ns` must be strictly positive.
- `book_depth` must be strictly positive.
- Latencies and all public timestamps are integer nanoseconds.

### Date range

```python
date_range = DateRange(
    start_ts_ns=1_775_553_600_000_000_000,
    end_ts_ns=1_775_553_601_000_000_000,
)
```

The range is inclusive. Records before `start_ts_ns` warm the historical book
without calling the strategy. Market records and command arrivals after
`end_ts_ns` are not processed.

### Instrument metadata

The minimal three-argument `backtest.run(strategy, path, date_range)` call
discovers instrument IDs in a metadata pass and assumes Databento nanounits:

```text
tick_size_ticks=1
price_scale=1_000_000_000
contract_multiplier=1
```

For a one-pass replay or real contract parameters, pass explicit metadata:

```python
from back_tester import InstrumentMeta

instruments = [
    InstrumentMeta(
        instrument_id=42,
        tick_size_ticks=10_000_000,
        price_scale=1_000_000_000,
        contract_multiplier=100,
    )
]

result = backtest.run(strategy, path, date_range, config, instruments)
```

All metadata values must be positive, instrument IDs must be unique, and every
instrument in the input must have metadata. Strategy prices are integer
`price_ticks`, not floating-point currency values.

## 6. Input data contract

The runtime reads one JSON object per line in Databento-like MBO order. The
checked-in [`test/data/tiny_mbo.jsonl`](../../test/data/tiny_mbo.jsonl) fixture
is the smallest working example.

Every row needs:

- `ts_recv` as a UTC ISO-8601 timestamp ending in `Z`;
- `hd.ts_event` and `hd.instrument_id`;
- one-character `action`;
- strictly increasing non-negative `sequence`;
- `flags`, where bit 128 (`F_LAST`) closes an atomic market group.

Supported actions are `A` (add), `C` (cancel), `M` (modify), `T` (trade), `F`
(fill), and `R` (clear). Depending on the action, the reader also requires
`order_id`, `side`, `price`, and/or positive `size`.

Input is streamed and must already be ordered. Blank rows, malformed JSON,
timestamp or sequence regressions, incomplete atomic groups, unsupported
values, unrepresentable prices, and unknown instruments fail the run with file
and row context. The runtime does not silently sort or repair data.

## 7. Development workflow

Run the repository checks before handing off a change:

```bash
uv run pre-commit run --all-files
uv run ctest --test-dir build-release --output-on-failure
uv run pytest -q python/tests
```

When changing behavior, update the relevant architecture page, focused native
or Python tests, and
[`architecture/11_requirements_traceability.md`](architecture/11_requirements_traceability.md)
in the same change.

For performance work, use the Release-only benchmarks:

```bash
build-release/bin/bench/back-tester-scheduler-benchmark
uv run python python/benchmarks/callback_overhead.py
```

Benchmark values are machine-specific observations, not pass/fail thresholds.

## 8. Troubleshooting

- `No module named back_tester`: run `uv run pip install -e .` and repeat the
  import verification from section 2.
- C++ compiler not found: install a C++20 compiler and rerun the CMake
  configure command.
- `cannot open source file`: pass a path relative to the repository root or an
  absolute readable JSONL path.
- Configuration validation error: check that market latency is non-negative
  and order latency, depth, instrument IDs, tick sizes, scales, and
  multipliers are positive.
- Strategy context error: call submission, cancellation, position, and
  open-order methods only from a strategy callback.
- Source error with `path:row`: fix the indicated JSONL row; ingestion is
  intentionally fail-fast.

After the first successful run, read
[`architecture/README.md`](architecture/README.md) for the system model and
[`architecture/08_implementation_map.md`](architecture/08_implementation_map.md)
to navigate from concepts to source files and tests.
