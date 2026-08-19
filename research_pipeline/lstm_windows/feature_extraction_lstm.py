"""Build per-instrument LSTM windows from deterministic L3 replay."""

from collections import defaultdict, deque

import numpy as np

from research_pipeline.l3_replay import iter_book_observations


def replay_and_extract_sequences(path, horizon=20, window=20):
    tick_history = defaultdict(lambda: deque(maxlen=window))
    previous_mid_price = {}
    sequences = []

    for observation in iter_book_observations(path):
        instrument_id = observation.instrument_id
        total = observation.bid_quantity + observation.ask_quantity
        if total == 0:
            continue

        imbalance = (observation.bid_quantity - observation.ask_quantity) / total
        spread = observation.best_ask - observation.best_bid
        mid_price = (observation.best_bid + observation.best_ask) / 2
        previous = previous_mid_price.get(instrument_id)
        mid_price_delta = mid_price - previous if previous is not None else 0.0
        previous_mid_price[instrument_id] = mid_price

        history = tick_history[instrument_id]
        history.append(
            [
                imbalance,
                spread,
                observation.bid_quantity,
                observation.ask_quantity,
                mid_price_delta,
            ]
        )
        if len(history) == window:
            sequences.append(
                {
                    "instrument_id": instrument_id,
                    "sequence": list(history),
                    "mid_price": mid_price,
                }
            )

    by_instrument = defaultdict(list)
    for index, sequence in enumerate(sequences):
        by_instrument[sequence["instrument_id"]].append(index)

    labels = [None] * len(sequences)
    for indices in by_instrument.values():
        for position in range(len(indices) - horizon):
            current_index = indices[position]
            future_index = indices[position + horizon]
            labels[current_index] = int(
                sequences[future_index]["mid_price"]
                > sequences[current_index]["mid_price"]
            )

    features = [
        sequence["sequence"]
        for sequence, label in zip(sequences, labels, strict=True)
        if label is not None
    ]
    targets = [label for label in labels if label is not None]
    return (
        np.asarray(features, dtype=np.float32),
        np.asarray(targets, dtype=np.int64),
    )
