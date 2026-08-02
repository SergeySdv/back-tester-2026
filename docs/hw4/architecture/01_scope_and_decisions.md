# System scope and implemented behavior

## Purpose

This project is a deterministic, in-process options backtesting engine built
for an HFT course assignment. It demonstrates typed market-data ingestion,
historical book reconstruction, causal order scheduling, private order
matching, Python strategy callbacks, position/PnL accounting, and bulk result
delivery.

It is a backtester, not an exchange emulator or a complete options risk system.

## Implemented runtime

- One OS process.
- One dispatcher thread and one trading-engine consumer thread.
- Multiple instruments in one replay.
- Databento-like MBO JSONL input.
- Streaming, fail-fast parsing into numeric native types.
- A per-instrument historical L3 book.
- One private `EngineView` owned by the typed `SimulatedLOB`.
- Fixed market-data latency and strictly positive fixed order latency.
- Limit GTC orders, full fills on price cross, resting orders, and cancel.
- Top-N Python book callbacks; default depth is 15.
- Full L3 replay even though Python receives an aggregated top-N view.
- Full-fill-on-price-cross matching from ordered best-quote and trade signals.
- Per-instrument positions, contract multipliers, FIFO realized PnL, and
  midpoint marking.
- Native columnar result buffers exposed as pandas DataFrames and a Series.
- Native, Python, end-to-end, determinism, sanitizer, and benchmark coverage.

Every strategy-facing event, order, position, and result row carries a numeric
`instrument_id`.

## Core behavioral rules

### One fill authority

`trading::SimulatedLOB` is the only component that creates synthetic fills.
The scheduler determines when a command arrives; the historical book provides
displayed liquidity; `TradingEngine` applies the resulting fill decisions to
order lifecycle, positions, results, and callbacks without independently
matching.

### One deterministic virtual timeline

```text
market delivery = exchange timestamp + market-data latency
order arrival   = callback-visible engine time + order latency
cancel arrival  = callback-visible engine time + order latency
```

At the same scheduled timestamp, priority is:

1. market delivery;
2. new-order arrival;
3. cancel arrival.

Source or command sequence is the stable tie-breaker within one class.

### Strict market-data barrier

The dispatcher publishes one scheduled event with a monotonically increasing
`dispatch_seq`. It does not mutate shared market state for the next event until
the trading thread publishes `processed_seq >= dispatch_seq`.

The acknowledgement occurs after matching, state updates, result recording,
Python callbacks, and command enqueue for the current event.

### Callback contract

Python strategies receive:

- `on_book_update(BookUpdate)`;
- `on_trade(Trade)`;
- `on_fill(Fill)`;
- `on_reject(Reject)`.

A book callback is emitted after a complete atomic source group and only when
the configured top-N view changes. For a market delivery, the observable order
is:

1. resting-order matching and fill callbacks;
2. trade callbacks in source order;
3. one book callback when top-N changed.

State, position, PnL inputs, and result rows are updated before `on_fill()` or
`on_reject()` runs.

### Non-recursive strategy commands

An order or cancel submitted from a callback enters the command ring with a
future scheduled arrival. It is never matched recursively on the callback's
C++ stack. Immediate validation rejects are deferred until the initiating
callback unwinds.

### Infinite-liquidity execution model

A qualifying best-quote or trade-price cross fills every eligible own order's
complete remaining quantity. Historical quote and trade sizes do not cap the
fill, and synthetic fills never mutate the source historical book. This is an
explicitly optimistic backtest model: the trader is responsible for choosing
an order size appropriate for the option instrument's liquidity and turnover.

### Failure policy

Malformed input, chronology regressions, invalid configuration, native
exceptions, and Python callback exceptions fail the run. The runtime records
the first exception, requests stop, closes/wakes queues and barriers, joins both
threads, and rethrows to the caller. It does not silently continue with a
corrupted replay.

## Supported order lifecycle

```text
PendingNew
  -> Open | Filled | Rejected
Open
  -> Filled | PendingCancel
PendingCancel
  -> Filled | Cancelled
```

Terminal states are `Filled`, `Cancelled`, and `Rejected`.
`PartiallyFilled` retains its stable public enum encoding for compatibility but
is not produced by the full-fill-on-cross matcher.

## Explicit limitations

- No sockets, IPC, multiple processes, or distributed services.
- No historical queue-position model, probabilistic fills, or market impact.
- No quote-size or trade-size capacity constraint on synthetic fills.
- No stochastic latency, jitter, or slippage.
- No replace/amend, market, stop, peg, post-only, IOC, FOK, or multi-leg
  orders.
- No self-matching between private strategy orders.
- No exercise, assignment, expiration settlement, Greeks, volatility surface,
  or complete options risk engine.
- Feather V2 input is available through the Python API; the C++ CLI remains
  JSONL-only. No database, persistence layer, UI, or generic plugin system.
- The shipped runtime has one trading `EngineView`; isolation between typed
  `SimulatedLOB` instances is tested but is not exposed by `backtest.run()`.
