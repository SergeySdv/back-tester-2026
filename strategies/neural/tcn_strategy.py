import json
from collections import deque
import torch
import back_tester as bt
from strategies.common.base_mixin import TrackingMixin
from strategies.neural.models import TCN


class TCNStrategy(TrackingMixin, bt.Strategy):
    """Торгует по предсказанию TCN на окне из 20 последних тиков сырых фичей:
    imbalance, spread, bid_qty, ask_qty, mid_price_delta (те же, что при обучении).
    Цены переводятся из price_ticks обратно в сырой масштаб (/1e9), как в feature_extraction_tcn.py."""

    NAME = "tcn"
    PRICE_SCALE = 1_000_000_000
    WINDOW = 20

    def __init__(
        self,
        instrument_ids,
        model_path="my_scripts/ml/tcn_model.pt",
        scaler_path="my_scripts/ml/tcn_scaler.json",
        prob_threshold=0.52,
        confirmation_steps=3,
        min_hold_updates=15,
        order_size=1,
    ):
        super().__init__()
        self._init_tracking(instrument_ids)

        with open(scaler_path) as f:
            scaler = json.load(f)
        self.mean = torch.tensor(scaler["mean"], dtype=torch.float32)
        self.std = torch.tensor(scaler["std"], dtype=torch.float32)

        self.device = torch.device("cpu")
        self.model = TCN(n_features=len(scaler["mean"]))
        self.model.load_state_dict(torch.load(model_path, map_location=self.device))
        self.model.eval()

        self.prob_threshold = prob_threshold
        self.confirmation_steps = confirmation_steps
        self.min_hold_updates = min_hold_updates
        self.order_size = order_size

        self.tick_window = {i: deque(maxlen=self.WINDOW) for i in self.instrument_ids}
        self.mid_price_prev = {i: None for i in self.instrument_ids}
        self.signal_streak = {i: 0 for i in self.instrument_ids}
        self.streak_direction = {i: 0 for i in self.instrument_ids}
        self.updates_since_entry = {i: 0 for i in self.instrument_ids}

    def on_book_update(self, update):
        self.book_updates += 1
        if not update.bids or not update.asks:
            return

        instrument_id = update.instrument_id
        bid_qty = sum(level.quantity for level in update.bids)
        ask_qty = sum(level.quantity for level in update.asks)
        total = bid_qty + ask_qty
        if total == 0:
            return

        best_bid_raw = update.bids[0].price / self.PRICE_SCALE
        best_ask_raw = update.asks[0].price / self.PRICE_SCALE
        best_bid_price = update.bids[0].price
        best_ask_price = update.asks[0].price

        imbalance = (bid_qty - ask_qty) / total
        spread = best_ask_raw - best_bid_raw
        mid_price = (best_bid_raw + best_ask_raw) / 2

        prev_mp = self.mid_price_prev[instrument_id]
        mid_price_delta = (mid_price - prev_mp) if prev_mp is not None else 0.0
        self.mid_price_prev[instrument_id] = mid_price

        raw_feat = [imbalance, spread, bid_qty, ask_qty, mid_price_delta]
        window = self.tick_window[instrument_id]
        window.append(raw_feat)

        pos = self.position(instrument_id)
        current_position = pos.net_quantity
        self.last_known_position[instrument_id] = pos
        self.updates_since_entry[instrument_id] += 1

        if len(window) < self.WINDOW:
            return

        seq = torch.tensor(list(window), dtype=torch.float32).unsqueeze(
            0
        )  # (1, window, 5)
        seq_norm = (seq - self.mean) / self.std

        with torch.no_grad():
            logit = self.model(seq_norm)
            prob_up = torch.sigmoid(logit).item()

        if prob_up > self.prob_threshold:
            raw_direction = 1
        elif prob_up < (1 - self.prob_threshold):
            raw_direction = -1
        else:
            raw_direction = 0

        if raw_direction != 0 and raw_direction == self.streak_direction[instrument_id]:
            self.signal_streak[instrument_id] += 1
        elif raw_direction != 0:
            self.streak_direction[instrument_id] = raw_direction
            self.signal_streak[instrument_id] = 1
        else:
            self.signal_streak[instrument_id] = 0
            self.streak_direction[instrument_id] = 0

        confirmed = self.signal_streak[instrument_id] >= self.confirmation_steps
        can_flip = self.updates_since_entry[instrument_id] >= self.min_hold_updates

        if not confirmed:
            return

        direction = self.streak_direction[instrument_id]

        if direction == 1 and current_position <= 0 and can_flip:
            self.submit_limit(
                instrument_id, bt.Side.BUY, best_ask_price, self.order_size
            )
            self.orders_sent += 1
            self.updates_since_entry[instrument_id] = 0
        elif direction == -1 and current_position >= 0 and can_flip:
            self.submit_limit(
                instrument_id, bt.Side.SELL, best_bid_price, self.order_size
            )
            self.orders_sent += 1
            self.updates_since_entry[instrument_id] = 0
