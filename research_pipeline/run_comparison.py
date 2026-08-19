import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import back_tester as bt
import pandas as pd

from metrics import compute_metrics
from strategy_registry import STRATEGIES, available_strategy_names, build_strategy


INSTRUMENT_IDS = [1, 2]
INSTRUMENTS = [
    bt.InstrumentMeta(instrument_id=1, contract_multiplier=10),
    bt.InstrumentMeta(instrument_id=2, contract_multiplier=5),
]
BOOK_DEPTH = 1


def run_one(name, data_path):
    strategy = build_strategy(name, INSTRUMENT_IDS)
    result = bt.backtest.run(
        strategy,
        data_path,
        bt.DateRange(),
        bt.BacktestConfig(order_latency_ns=5, book_depth=BOOK_DEPTH),
        INSTRUMENTS,
    )
    metrics = compute_metrics(result.pnl_series, result.fills_df, strategy.trade_pnls)
    metrics["strategy"] = name
    metrics["orders_sent"] = strategy.orders_sent
    metrics["book_updates"] = strategy.book_updates
    return metrics


def _print_strategy_status():
    print("Strategies:")
    for name, spec in STRATEGIES.items():
        reason = spec.unavailable_reason()
        status = "available" if reason is None else f"unavailable ({reason})"
        print(f"  - {name}: {status}")


def _selected_strategies(argument, parser):
    if argument == "all":
        selected = available_strategy_names()
        skipped = [name for name in STRATEGIES if name not in selected]
        if skipped:
            print(
                f"Skipping unavailable strategies: {', '.join(skipped)}",
                file=sys.stderr,
            )
        return selected

    selected = [name.strip() for name in argument.split(",") if name.strip()]
    unknown = [name for name in selected if name not in STRATEGIES]
    if unknown:
        parser.error(f"unknown strategies: {', '.join(unknown)}")
    unavailable = {
        name: STRATEGIES[name].unavailable_reason()
        for name in selected
        if STRATEGIES[name].unavailable_reason() is not None
    }
    if unavailable:
        details = "; ".join(f"{name}: {reason}" for name, reason in unavailable.items())
        parser.error(f"requested strategies are unavailable: {details}")
    return selected


def main():
    parser = argparse.ArgumentParser(
        description="Compare strategies on synthetic data through the C++ engine"
    )
    parser.add_argument(
        "--strategies",
        "-s",
        default="all",
        help=f"comma-separated names: {','.join(STRATEGIES)}; default: all available",
    )
    parser.add_argument(
        "--data",
        "-d",
        default="synthetic_signal.jsonl",
        help="path to JSONL data",
    )
    parser.add_argument(
        "--list", action="store_true", help="show strategy availability"
    )
    args = parser.parse_args()

    if args.list:
        _print_strategy_status()
        return

    selected = _selected_strategies(args.strategies, parser)
    if not selected:
        parser.error("no runnable strategies are available")

    print(f"Data: {args.data}")
    print(f"Strategies: {selected}\n")

    rows = []
    for name in selected:
        print(f"--- Running {name} ---")
        rows.append(run_one(name, args.data))

    dataframe = pd.DataFrame(rows).set_index("strategy")
    columns = [
        "final_pnl",
        "max_drawdown",
        "sharpe",
        "win_rate",
        "num_fills",
        "total_volume",
        "profit_factor",
        "avg_pnl_per_trade",
        "num_closing_trades",
        "orders_sent",
        "book_updates",
    ]
    dataframe = dataframe[columns]
    pd.set_option("display.width", 200)
    pd.set_option("display.max_columns", 20)
    print(f"\n=== Comparison (book_depth={BOOK_DEPTH}) ===")
    print(dataframe.to_string(float_format=lambda value: f"{value:.4f}"))


if __name__ == "__main__":
    main()
