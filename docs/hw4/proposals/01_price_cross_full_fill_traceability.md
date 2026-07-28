# Price-cross full-fill model: requirement and code traceability

## Status

- **State:** accepted and implemented; pending independent QA/review
- **Candidate implementation commit:** `2cfad2db86a91050b7136c6cfe04c1dac9066aff`
- **Pre-PCFF branch head:** `06e29a3e5fd5617675d3e8df54b627c58547a833`
- **Repository baseline:** `c4f4c02916f5a9fb5f2636926fd93cd28af0f46d`
- **Affected subsystem:** native trading, market-delivery contract, results,
  Python integration, and matching tests

This document traces the accepted change from the former displayed-liquidity
matching model to the deterministic full-fill-on-price-cross model.

### Accepted decisions

- raw quote/trade signals are replayed in exact source-sequence order inside an
  atomic group (Option B);
- fill price is the winning best opposite quote or trade price;
- trade crossing is price-only and ignores aggressor side;
- one signal fills every eligible own order in deterministic price-time order;
- `HistoricalDisplayed=0` remains reserved for compatibility, while new fills
  use `QuoteCross=1` or `TradeCross=2`.

The three revisions above have distinct roles: the repository baseline is the
original reviewed project, the pre-PCFF head contains the former volume-capped
matcher, and the candidate commit contains the implemented PCFF model. Sections
label historical behavior explicitly rather than calling it “current.”

## 1. Implemented requirement

After a limit order has reached the simulated exchange, the engine shall fully
fill its remaining quantity on the first eligible price-cross signal for the
same option instrument.

Eligible signals are:

- a historical best quote crossing the order limit; or
- a historical trade price crossing the order limit.

The trigger predicates are:

```text
buy limit L:
    best_ask <= L
    OR trade_price <= L

sell limit L:
    best_bid >= L
    OR trade_price >= L
```

The historical quote size and historical trade size shall not cap the
synthetic fill. When a signal qualifies, the fill quantity is the order's
entire remaining quantity.

The order quantity must still be retained and applied to position and PnL. The
change removes the historical-liquidity cap; it does not turn positions into
quantity-free state.

### Options-specific boundary

The trigger must belong to the same `instrument_id` as the order. Each option
series is a separate instrument. A move or trade in the underlying asset does
not fill an option order unless a future, separately specified cross-instrument
model explicitly introduces that behavior.

## 2. Requirement IDs

| ID | Required behavior |
|---|---|
| `PCFF-01` | Match only signals carrying the order's `instrument_id`. |
| `PCFF-02` | An order is eligible only after its delayed new-order arrival has been processed. Historical signals that occurred before arrival cannot fill it. |
| `PCFF-03` | A buy is quote-marketable when `best_ask <= limit`; a sell is quote-marketable when `best_bid >= limit`. |
| `PCFF-04` | A buy is trade-crossed when `trade_price <= limit`; a sell is trade-crossed when `trade_price >= limit`. |
| `PCFF-05` | The first eligible signal in the deterministic virtual timeline is the fill trigger. |
| `PCFF-06` | A qualifying signal fills the complete `remaining_quantity`, regardless of displayed quote size or trade size. |
| `PCFF-07` | Filled quantity continues to update order state, option position, multiplier-scaled PnL, result rows, and callbacks. |
| `PCFF-08` | The fill records the trigger's exchange time, callback-visible engine time, source type, and raw `trigger_source_sequence`. |
| `PCFF-09` | Equal-time and same-group ordering is explicit, deterministic, and covered by tests. |
| `PCFF-10` | Cancel, latency, validation, callback ordering, ready-barrier, and exception contracts remain unchanged unless this proposal explicitly says otherwise. |

## 3. Pre-patch behavior versus implemented behavior

| Area | Pre-PCFF head `06e29a3…` | Candidate implementation |
|---|---|---|
| Fill authority | `SimulatedLOB` only | Keep `SimulatedLOB` as the only fill authority |
| Quote trigger | Historical L3 orders at or through the limit | Historical best opposite quote at or through the limit |
| Trade trigger | No; trades are callbacks only | Yes; a qualifying trade may fill a resting order |
| Historical volume | Caps each fill | Ignored for fill capacity |
| Fill quantity | `min(remaining, privately_available)` | Entire `remaining_quantity` |
| Multiple price levels | Swept price by price | No depth sweep required |
| Partial fill from market depth | Supported | Removed from normal matching |
| Private historical consumption | Tracked per historical order and revision | Not required for infinite-liquidity fills |
| Fill price | Historical order price | Winning quote/trade trigger price |
| Fill time | Scheduled order arrival or market-delivery time | Time of the winning quote/trade trigger |
| Position quantity | Filled historical quantity | Full remaining order quantity; still required |
| Trigger provenance | No raw trigger sequence in fill results | `QuoteCross`/`TradeCross` plus `trigger_source_sequence` |

## 4. Candidate runtime trace

### 4.1 Ingestion and market delivery

1. `JsonlReader` parses MBO rows into typed market events.
2. `JsonlScheduledSource::prepare_for_dispatch()` applies the complete atomic
   source group to `HistoricalLOBStore` in raw order.
3. After each book action it records the resulting best bid/ask in a
   `PriceCrossSignal`; each trade row records its trade price.
4. Trade rows are also copied into public `TradeView` values.
5. The source materializes one final top-N `BookUpdateView` if the view changed.
6. One `MarketDelivery` carries the final book view, trades, and ordered
   price-cross span to the trading thread.

Evidence:

- `src/runtime/BacktestRuntime.cpp`
- `src/core/Events.hpp`

### 4.2 Matching and provenance

`TradingEngine::process_market()` validates and replays each signal in strictly
increasing source-sequence order. `SimulatedLOB::on_signal()` applies the
same-instrument quote/trade predicate and emits a full-remaining
`SyntheticFill` at the trigger price. It attaches the winning signal's
`source_sequence`.

Evidence:

- `src/trading/TradingEngine.cpp`
- `src/trading/SimulatedLOB.cpp`
- `src/trading/SimulatedLOB.hpp`

At delayed order arrival, `SimulatedLOB::accept()` evaluates the current best
opposite quote. `LimitOrderBook::last_book_source_sequence()` supplies the raw
book row that most recently established or observed that quote state, so an
arrival-time quote fill also has public provenance.

### 4.3 Fill application and public results

`TradingEngine::apply_fill()` keeps the existing quantity-aware lifecycle,
position, PnL, result, and callback path. `FillView` exposes two distinct
sequences:

- `sequence`: monotonically increasing synthetic fill sequence;
- `trigger_source_sequence`: raw quote/trade row that caused the fill.

`FillResultRow`, `ResultRecorder`, and `fills_df` retain the trigger sequence as
a `uint64` column alongside `liquidity_source`.

Evidence:

- `src/core/Events.hpp`
- `src/core/ResultSchemas.hpp`
- `src/results/ResultRecorder.cpp`
- `src/python/bindings.cpp`

## 5. Resolved design record: “first signal” inside an atomic group

At the pre-PCFF head, the scheduled unit was an atomic `MarketDelivery`, not
each raw MBO row. Before publication, the dispatcher applied the whole source
group and exposed:

- the final stable book state; and
- individual trades in source order.

It did not expose every intermediate best bid/ask transition with its source
sequence. The following alternatives were evaluated before implementation.

Two valid contracts are possible.

### Option A — atomic-group priority

Treat one source group as one indivisible market signal:

1. evaluate the final stable best quote;
2. if no quote cross exists, inspect trades in source order;
3. emit at most one full fill per own order.

Advantages:

- smallest change;
- preserves the current atomic-book and callback model;
- no new hot-path source buffer.

Limitation:

- “first” is exact between scheduled groups, but not between raw quote and
  trade records inside one group.

### Option B — exact raw-signal chronology

Extend the scheduled market payload with an ordered, typed trigger span. Each
trigger contains:

```cpp
enum class PriceCrossSource : std::uint8_t {
  BestQuote,
  Trade,
};

struct PriceCrossSignal {
  InstrumentId instrument_id;
  TimestampNs exchange_ts_ns;
  TimestampNs engine_ts_ns;
  Sequence source_sequence;
  PriceCrossSource source;
  std::optional<PriceTicks> best_bid;
  std::optional<PriceTicks> best_ask;
  std::optional<PriceTicks> trade_price;
};
```

The dispatcher would materialize this sequence while applying the atomic group.
The trading thread would replay the signals into `SimulatedLOB` without
mutating the historical book.

Advantages:

- implements “whichever crossed first” literally;
- preserves the winning source sequence and timestamp.

Costs:

- changes the frozen `MarketDelivery` contract;
- introduces another callback-scoped buffer with lifetime requirements;
- requires core, scheduler, runtime, trading, bindings, and contract tests;
- must reconcile intermediate triggers with the rule that Python sees only a
  stable atomic book.

**Implemented decision:** Option B. The runtime materializes and replays ordered
typed signals, so the first raw quote/trade cross is exact inside a group, and
the winning raw sequence is retained in callback and result contracts.

## 6. Implemented code traceability matrix

| Requirement | Candidate implementation | Executable evidence |
|---|---|---|
| `PCFF-01` | Instrument-keyed private indexes and delivery/signal validation | same-instrument and other-instrument trade tests |
| `PCFF-02` | Delayed `process_new()` plus no replay of past signals | pre-arrival trade and immediate-arrival quote tests |
| `PCFF-03` | `SimulatedLOB::accept()` and quote `on_signal()` predicates | oversized buy/sell quote tests |
| `PCFF-04` | Trade `PriceCrossSignal` routed only through `SimulatedLOB` | buy/sell trade-only tests |
| `PCFF-05` | Ordered `MarketDelivery::price_cross_signals` replay | both same-group quote/trade permutations |
| `PCFF-06` | `match_prices()` emits complete `remaining_quantity` | oversized and multiple-order tests |
| `PCFF-07` | Existing `apply_fill()` and `PositionKeeper` application path | position, PnL, callback, and result assertions |
| `PCFF-08` | `liquidity_source` plus public `trigger_source_sequence` | core contract, native result, and Python DataFrame tests |
| `PCFF-09` | source-sequence validation plus scheduler priority | equal-time and twenty-run determinism tests |
| `PCFF-10` | unchanged scheduler, cancel, barrier, and exception paths | cancel-before/cancel-after-cross and exception suites |

## 7. Candidate file-level trace

### Implemented production changes

| File | Candidate responsibility |
|---|---|---|
| `src/runtime/BacktestRuntime.cpp` | Materializes raw-order quote/trade trigger records while applying each atomic group. |
| `src/trading/SimulatedLOB.*` | Owns private price-time indexes and is the sole authority for quote/trade full fills. |
| `src/trading/TradingEngine.*` | Replays validated signals and applies matcher decisions to lifecycle/accounting/callbacks. |
| `src/market/LimitOrderBook.*` | Retains the latest raw book source sequence for arrival-time quote provenance. |
| `src/core/Types.hpp` | Defines stable quote-cross and trade-cross source encodings. |
| `src/core/Events.hpp` | Defines ordered triggers and callback-visible trigger provenance. |
| `src/core/ResultSchemas.hpp`, `src/results/ResultRecorder.*` | Retain trigger provenance in native columnar results. |
| `src/python/bindings.cpp` | Exposes source enum and `trigger_source_sequence` without per-row appends. |

### Production files expected to remain behaviorally unchanged

| File/area | Reason |
|---|---|
| `src/market/JsonlReader.*` | Trade price, side, quantity, timestamp, sequence, and instrument are already parsed into typed fields. |
| `src/scheduler/ChronologicalScheduler.*` | Scheduled market/new/cancel ordering remains valid. |
| `src/scheduler/ReadyBarrier.hpp` and `SchedulerRuntime.hpp` | Matching still completes before acknowledgement. |
| `src/trading/PositionKeeper.*` | It must apply the actual synthetic order quantity; only the matcher decides that quantity. |
| `src/results/ResultRecorder.*` | PnL logic stays intact; fill column storage gains trigger provenance. |

## 8. Fill price decision record

The original request defined when to fill but not the fill price. The following
policies were evaluated before implementation.

| Policy | Quote trigger | Trade trigger | Consequence |
|---|---|---|---|
| Trigger price | best opposite quote | historical trade price | Maximum price improvement; fills an arbitrary quantity at one observed price |
| Limit price | own limit | own limit | Conservative and simple; ignores favorable observed prices |
| Source-aware | best opposite quote | own limit | Common deterministic compromise for quote execution versus inferred passive trade execution |

Whichever policy is selected must preserve limit protection:

```text
buy fill price <= buy limit
sell fill price >= sell limit
```

**Accepted decision:** trigger price for both quote and trade triggers.

## 9. Trade-side decision record

`TradeView` carries `aggressor_side`, but the original statement says
that any trade price crossing the limit is sufficient. Two interpretations are
possible:

- price-only: ignore aggressor side and use only the predicates in `PCFF-04`;
- side-aware: require a compatible aggressor side as additional evidence.

Price-only behavior matches the stated requirement most closely. Side-aware
behavior is less permissive but needs an explicitly agreed mapping for the
source feed.

**Accepted decision:** price-only trade triggers.

## 10. Multiple own orders and infinite liquidity

If several own orders are crossed by one signal and historical volume is
ignored, the model must define whether the signal fills:

- every eligible own order in deterministic price-time order; or
- only the first eligible own order.

The natural infinite-liquidity interpretation is to fill every eligible order.
This can create synthetic filled quantity far above the observed market size,
so it must be an explicit documented property rather than an incidental loop
effect.

**Accepted contract:** fill every eligible own order, ordered by own price
priority and exchange-arrival FIFO.

## 11. Test migration and implemented evidence

### Historical pre-PCFF expectations that were replaced

| Test | Pre-PCFF assertion | Candidate assertion |
|---|---|---|
| `test/TradingTest.cpp`, “Real runtime delays order and sweeps multiple historical orders” | Two fills of 4 and 6 across two asks | One full fill for 10 from the selected quote trigger |
| `test/TradingTest.cpp`, “Resting order fills later and private consumption tracks identity” | Partial fills across later liquidity revisions | First qualifying signal fully fills the remainder |
| `test/TradingTest.cpp`, “Own orders use price-time priority and EngineViews stay isolated” | Shared displayed size is privately depleted | Every crossed order/view follows the chosen infinite-liquidity rule |
| `test/TradingTest.cpp`, “Sell sweep respects limit and leaves only protected remainder” | Fills 2 + 4 and leaves 2 resting | First qualifying bid signal fills all 8 |
| `test/TypedSimulatedLOBTest.cpp` | Golden fill quantity equals displayed quantity | Oversized private order fills fully when price crosses |
| `python/tests/test_runtime.py`, “real submission delayed fill state and bulk results” | Order 6 fills 4 and leaves 2 | Order 6 fills 6 and becomes terminal |
| `python/tests/test_end_to_end.py` | Displayed-volume fill rows and callback order | Recalculate fill quantity, position, PnL, source, and callback expectations |

### Mandatory scenarios and executable evidence

| # | Scenario | Evidence |
|---:|---|---|
| 1 | Oversized buy fully fills on `best_ask <= limit` | `TypedSimulatedLOBTest.cpp`: M5 golden matching decision |
| 2 | Oversized sell fully fills on `best_bid >= limit` | `TradingTest.cpp`: sell fully fills at best bid |
| 3 | Buy fills on trade-only cross | `TradingTest.cpp`: post-arrival same-instrument trade cross |
| 4 | Sell fills on trade-only cross | `TypedSimulatedLOBTest.cpp`: one trade fills eligible buys and sells |
| 5 | Post-arrival non-crossing same-instrument quote and trade do not fill | `TradingTest.cpp`: non-crossing quote and trade |
| 6 | Other option instrument cannot fill | `TradingTest.cpp`: post-arrival same-instrument trade cross |
| 7 | Pre-arrival trade is not replayed | `TradingTest.cpp`: post-arrival same-instrument trade cross |
| 8 | Crossed quote at delayed arrival fills immediately | `TradingTest.cpp`: delayed order fully fills at best quote |
| 9 | Small historical size does not cap oversized fill | `TypedSimulatedLOBTest.cpp`: M5 golden matching decision |
| 10 | Full quantity reaches position, PnL, results, and callback | `TradingTest.cpp`, `ResultsTest.cpp`, and `test_end_to_end.py` |
| 11 | Fill callback sees terminal state and no open order | `test_runtime.py`: real submission delayed fill |
| 12 | Successful cancel before later cross prevents fill | `TradingTest.cpp`: cancel arriving before later price cross |
| 13 | Equal-time market/new/cancel priority is retained | `TradingTest.cpp`: equal-time market fill wins; scheduler tests |
| 14 | Both quote-before-trade and trade-before-quote group orders | `test_runtime.py`: delivery callback order and trade-wins tests |
| 15 | Own price priority and same-price FIFO are deterministic | `TradingTest.cpp`: own price priority and infinite quote liquidity tests |
| 16 | Twenty runs normalize identically | native Trading/Results tests and Python E2E determinism test |
| 17 | Trade-triggered callback exception stops and joins | parametrized `test_runtime.py` callback exception test |
| 18 | Source type and exact raw trigger sequence reach results | CoreContracts, Results, Trading, and Python result tests |

## 12. Completed documentation updates

The candidate updates these normative documents in the same change:

- `docs/hw4/architecture/01_scope_and_decisions.md`
  - replace partial/displayed-depth behavior with full-fill-on-cross;
  - name the infinite-liquidity limitation.
- `docs/hw4/architecture/04_event_time_and_concurrency.md`
  - define exact quote/trade trigger ordering and callback order.
- `docs/hw4/architecture/05_order_matching_and_state.md`
  - replace the depth sweep and private-consumption algorithm;
  - revise the state machine if `PartiallyFilled` becomes unreachable.
- `docs/hw4/architecture/09_testing_and_acceptance.md`
  - replace partial-fill acceptance with price-cross cases.
- `docs/hw4/architecture/11_requirements_traceability.md`
  - map the new model to its implementation and verification.
- `docs/SimulatedLOB_design.md`
  - describe the accepted matcher and explicitly state its optimism.
- root `README.md` and examples
  - avoid claiming displayed-depth execution.

The source assignment says to “start with fill-at-touch” and does not explicitly
require either volume-capped or infinite-liquidity execution. The adopted
architecture explicitly selects infinite-liquidity full-fill-on-cross.

## 13. Compatibility and integration risks

| Risk | Impact | Required control |
|---|---|---|
| Optimistic fills on illiquid option series | Strategies may report positions that could not have traded historically | Name the model explicitly in config/docs/results |
| Underlying/option confusion | An underlying trade could incorrectly fill an option | Strict same-`instrument_id` tests |
| Atomic-group information loss | “First signal” could be implemented incorrectly | Ordered triggers plus both same-group permutations |
| Ambiguous fill price | PnL changes depending on an unstated assumption | Trigger-price contract and tests |
| Multiple own orders | One tiny trade could fill unlimited aggregate quantity | Document infinite-liquidity semantics |
| Result source compatibility | Existing `HistoricalDisplayed = 0` consumers may misread new fills | Preserve enum value and add new values deliberately |
| Callback ordering drift | Strategies may observe trade before state/fill update | Keep fill application before the winning signal's public callback |
| Removed partial states | Downstream code may still expect `PartiallyFilled` | Keep enum/schema compatibility unless separately versioned |
| Performance regression | Per-group trigger buffers may allocate in the hot path | Reserve/reuse typed native buffers and benchmark Option B |

### Option B performance evidence

`back-tester-price-cross-benchmark` measures the construction and replay of the
actual typed `PriceCrossSignal` buffer for groups of 8 and 64 signals. It
reports retained capacity, first-group reallocations, mean time per group, and
mean time per signal. This complements the scheduler no-op round trip and
prebuilt Python callback benchmarks; neither of those is used as evidence for
Option B buffer cost.

Command:

```bash
build-release/bin/bench/back-tester-price-cross-benchmark
```

Observed on the Release build used for this candidate:

| Group size | Retained capacity | First-group reallocations | Mean ns/group | Mean ns/signal |
|---:|---:|---:|---:|---:|
| 8 | 8 | 0 | 72.9 | 9.1 |
| 64 | 64 | 3 | 565.2 | 8.8 |

These values are machine-local regression evidence, not a portable performance
guarantee. The three reallocations occur only while the reusable buffer first
grows beyond its initial capacity of eight; measured replay follows warmup.

## 14. Completed implementation sequence

1. Resolve fill price, trade-side, same-group ordering, and multiple-order
   decisions.
2. Add RED native tests for trade-only crossing, oversized full fill,
   same-instrument isolation, and ordering.
3. Change the core payload only if exact raw-signal chronology is selected.
4. Extend `SimulatedLOB` so it remains the sole fill authority for both quote
   and trade signals.
5. Remove the historical-volume cap from the selected model while retaining
   order quantity in lifecycle and accounting.
6. Extend `LiquiditySource` and result verification.
7. Update native integration tests, Python tests, example expectations, and
   deterministic repeated-run tests.
8. Update normative architecture only after the behavior is accepted.
9. Run Release, full CTest, Python tests, sanitizers, example, and benchmarks.
10. Obtain independent QA and review against the exact candidate commit.

## 15. Acceptance checklist

- [x] Fill-price policy accepted.
- [x] Trade-side policy accepted.
- [x] Atomic-group ordering policy accepted.
- [x] Multiple-own-order policy accepted.
- [x] Same-instrument option boundary locked by tests.
- [x] `SimulatedLOB` remains the sole fill authority.
- [x] Full remaining quantity is used for fill, position, and PnL.
- [x] Historical quote/trade volume does not cap the fill.
- [x] Pre-arrival events cannot fill a later order.
- [x] Fill timestamp and source are recorded deterministically.
- [x] Callback and scheduler ordering remain causal.
- [x] Native and Python regression suites pass.
- [x] Determinism and sanitizer gates pass.
- [x] Architecture and public result contracts are updated.
- [ ] Independent QA and review have no unresolved P0/P1 findings.
