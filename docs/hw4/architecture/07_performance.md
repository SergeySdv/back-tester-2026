# Performance design and benchmarks

## Hot-path design

The implementation gives deterministic correctness priority over raw latency,
then removes avoidable allocation and conversion from the event path.

- JSON strings, ISO timestamps, and decimal prices are parsed once by
  `JsonlReader`.
- Timestamps, price ticks, quantities, IDs, and sequences remain integer native
  values.
- The reader streams physical rows and stages one atomic group; it does not
  load and sort the full file.
- Matching compares typed quote/trade trigger prices against the best eligible
  private order and does not traverse historical displayed volume.
- Resting private orders use price-time ordered maps.
- Top-N callback vectors and source-group buffers are reserved and reused.
- Result data is appended to typed, pre-reserved columns.
- Event and command communication uses bounded SPSC rings.
- The shared book uses single-writer ownership plus the ready barrier instead
  of a per-update mutex.
- The GIL is held only for Python interaction.
- DataFrames are materialized once, after the native run.

The historical L3 price indexes and private order indexes use standard-library
ordered containers. Matching removes crossed private orders from the best price
forward in deterministic price-time order. Custom allocators, SIMD, and custom
trees are intentionally absent because the measured course-project workload
does not justify their complexity.

## Ready-signal round-trip benchmark

Executable:

```bash
build-release/bin/bench/back-tester-scheduler-benchmark
```

The measured interval is:

```text
dispatcher publishes a synthetic event
  -> trading thread consumes and performs a no-op reaction
  -> trading thread publishes processed_seq
  -> dispatcher observes acknowledgement
```

The benchmark performs warm-up iterations and 100,000 measured iterations. It
reports build type, compiler, OS/CPU when available, ring capacity, waiting
strategy, minimum, mean, p50, p95, and p99 nanoseconds.

## Price-cross buffer and replay benchmark

Executable:

```bash
build-release/bin/bench/back-tester-price-cross-benchmark
```

This measures the Option B hot path that the ready benchmark deliberately
omits: construction of a reusable typed `PriceCrossSignal` vector followed by
ordered replay through `SimulatedLOB`. It reports groups of 8 and 64 signals,
retained vector capacity, first-group reallocations after the runtime's
`reserve(8)`, mean nanoseconds per group, and mean nanoseconds per signal.

The benchmark uses alternating non-crossing quote/trade signals and one resting
order, so it measures trigger materialization and predicate replay without
conflating the result with fill accounting or Python callbacks.

## Python callback benchmark

Command:

```bash
uv run python python/benchmarks/callback_overhead.py
```

It measures 20 warmed samples of exactly 1,000 no-op `on_book_update()`
callbacks for top-1 and top-15 payloads. Payload construction, JSON parsing,
process startup, logging, and DataFrame construction are outside the timed
region.

The totals are unadjusted. A side-effect-free native empty loop is not
subtracted because an optimizing Release compiler can eliminate it.

## Interpreting results

Benchmark values are machine-specific observations for regression comparison,
not universal latency pass thresholds. Compare results only with the same
compiler, build type, host, and benchmark configuration. Correctness fixes take
precedence over speed; a material same-machine regression still requires an
explanation.

Useful profiling counters are events per second, allocations per million
events, callbacks after top-N filtering, private orders scanned per update, and
result-buffer reallocations.
