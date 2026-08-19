"""Extract deterministic top-of-book features and forward labels."""

from __future__ import annotations

from collections import defaultdict, deque

from research_pipeline.l3_replay import iter_book_observations


def extract_feature_rows(
    path,
    imbalance_windows=(5, 20),
    momentum_windows=(5, 20),
    volatility_window=20,
    trade_freq_window=20,
):
    """Replay L3 JSONL and return one row per top-of-book callback."""

    imbalance_history = defaultdict(lambda: deque(maxlen=max(imbalance_windows) + 1))
    mid_price_history = defaultdict(
        lambda: deque(maxlen=max(max(momentum_windows), volatility_window) + 1)
    )
    trade_flag_history = defaultdict(lambda: deque(maxlen=trade_freq_window))
    rows = []

    for observation in iter_book_observations(path):
        instrument_id = observation.instrument_id
        bid_quantity = observation.bid_quantity
        ask_quantity = observation.ask_quantity
        total = bid_quantity + ask_quantity
        if total == 0:
            continue

        imbalance = (bid_quantity - ask_quantity) / total
        spread = observation.best_ask - observation.best_bid
        mid_price = (observation.best_bid + observation.best_ask) / 2

        imbalance_values = imbalance_history[instrument_id]
        imbalance_values.append(imbalance)
        mid_prices = mid_price_history[instrument_id]
        mid_prices.append(mid_price)
        trade_flags = trade_flag_history[instrument_id]
        trade_flags.append(1 if observation.trades_since_previous > 0 else 0)

        row = {
            "instrument_id": instrument_id,
            "imbalance": imbalance,
            "spread": spread,
            "mid_price": mid_price,
            "bid_qty": bid_quantity,
            "ask_qty": ask_quantity,
        }
        for window in imbalance_windows:
            values = list(imbalance_values)[-window:]
            row[f"imbalance_ma_{window}"] = sum(values) / len(values)

        for window in momentum_windows:
            row[f"momentum_{window}"] = (
                mid_prices[-1] - mid_prices[-window - 1]
                if len(mid_prices) > window
                else 0.0
            )

        volatility_values = list(mid_prices)[-volatility_window:]
        if len(volatility_values) >= 2:
            mean = sum(volatility_values) / len(volatility_values)
            variance = sum((value - mean) ** 2 for value in volatility_values) / len(
                volatility_values
            )
            row[f"volatility_{volatility_window}"] = variance**0.5
        else:
            row[f"volatility_{volatility_window}"] = 0.0

        row[f"trade_freq_{trade_freq_window}"] = sum(trade_flags) / len(trade_flags)
        rows.append(row)

    return rows


def replay_and_extract_features(
    path,
    horizon=20,
    imbalance_windows=(5, 20),
    momentum_windows=(5, 20),
    volatility_window=20,
    trade_freq_window=20,
):
    rows = extract_feature_rows(
        path,
        imbalance_windows=imbalance_windows,
        momentum_windows=momentum_windows,
        volatility_window=volatility_window,
        trade_freq_window=trade_freq_window,
    )

    by_instrument_indices = defaultdict(list)
    for index, row in enumerate(rows):
        by_instrument_indices[row["instrument_id"]].append(index)

    labels = [None] * len(rows)
    for indices in by_instrument_indices.values():
        for position in range(len(indices) - horizon):
            current_index = indices[position]
            future_index = indices[position + horizon]
            labels[current_index] = int(
                rows[future_index]["mid_price"] > rows[current_index]["mid_price"]
            )

    return [
        {**row, "label": label}
        for row, label in zip(rows, labels, strict=True)
        if label is not None
    ]
