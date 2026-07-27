# Verification and acceptance

## Test layers

### Native unit and integration tests

The `back-tester-tests` executable covers:

- exact timestamp/decimal parsing and input failure context;
- L3 add, cancel, modify, partial fill, clear, duplicates, and revisions;
- multi-instrument book routing and top-N extraction;
- scheduled ordering, SPSC backpressure, ready acknowledgement, stop, and
  exception recovery;
- delayed order/cancel arrival and equal-time priority;
- quote/trade price crosses, oversized full fills, raw-signal ordering,
  same-instrument isolation, and own price-time priority;
- rejects, order transitions, positions, exact PnL, buffer ownership, and
  deterministic repeated runs;
- complete runtime composition from a temporary JSONL source.

CTest also verifies CLI usage and valid/invalid checked-in fixtures.

### Python integration tests

`python/tests` covers:

- package API and bound types;
- callback payloads and callback-scoped Strategy context;
- order submission, cancellation, rejects, and multi-instrument queries;
- Python exception propagation, clean thread shutdown, and runtime reuse;
- DataFrame/Series columns, dtypes, and native-buffer lifetime;
- the real two-instrument end-to-end strategy;
- deterministic repeated results;
- benchmark output contracts.

### Sanitizers

The final verification supports ASan/UBSan and TSan builds where the host
compiler provides them. The mandatory ownership model is designed to keep the
shared historical book race-free without a hot-path mutex.

## Reproducible verification

From the repository root:

```bash
uv sync --locked
uv run pip install -e .
uv run python -c "import back_tester; print(back_tester.__file__); print(back_tester.version())"
uv run cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
uv run cmake --build build-release -j
uv run ctest --test-dir build-release --output-on-failure
uv run pytest -q python/tests
uv run python examples/mean_reversion.py
```

Benchmarks:

Benchmarks live in `bench/`, not `test/`: they are not registered with ctest
and never fail a build on a timing threshold, because CI runners are too noisy
for absolute latency assertions. They are still built so they cannot rot.
Report numbers from a Release build only.

```bash
build-release/bin/bench/back-tester-scheduler-benchmark
build-release/bin/bench/back-tester-price-cross-benchmark
build-release/bin/bench/back-tester-engine-round-trip-benchmark
uv run python python/benchmarks/callback_overhead.py
```

`back-tester-engine-round-trip-benchmark` runs the same event stream with the
engine called directly and again through the two-thread `SchedulerRuntime`; the
difference is the cost of one ready-signal round trip.
`callback_overhead.py` reports the Python callback cost against a native
baseline and a GIL-only measurement, so the boundary cost is attributable
rather than merely observed.

Development formatting/lint checks:

```bash
uv run pre-commit run --all-files
```

## Behavioral acceptance

The test suite locks the following system behavior:

- virtual time never moves backwards;
- market, new order, and cancel events use documented stable ordering;
- an order cannot arrive before `submit time + order latency`;
- the dispatcher does not mutate the next market state before acknowledgement;
- best-quote and trade signals are replayed in raw source order inside each
  atomic group;
- the first qualifying same-instrument signal fills the complete remaining
  quantity at its trigger price, independent of historical size;
- pre-arrival trades are not replayed and later resting fills are
  deterministic;
- fill results distinguish quote-cross and trade-cross sources and retain the
  winning raw `trigger_source_sequence`;
- position and order state are updated before callbacks;
- terminal orders leave the open-order index;
- Python failures cannot strand a queue or barrier;
- returned result objects retain immutable native storage;
- repeated normalized runs produce identical order/fill ordering.

## Runnable demonstration

```bash
uv run python examples/mean_reversion.py
```

The example runs the production `backtest.run()` path on
`test/data/m5_two_instrument.jsonl`. It demonstrates a delayed resting fill, an
independent cancelled order, callback ordering, per-instrument positions, and
PnL output.

## Limit of acceptance

Passing tests establish the deterministic model documented in this directory.
They do not validate unimplemented exchange behavior such as historical queue
position, market impact, stochastic fills, option exercise/expiry, or Greeks.
