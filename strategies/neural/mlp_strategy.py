import json
from collections import deque
import torch
import back_tester as bt
from strategies.common.base_mixin import TrackingMixin
from strategies.neural.models import MLP


class MLPStrategy(TrackingMixin, bt.Strategy):
    """Торгует по предсказанию MLP (PyTorch) на 7 фичах, тех же что LightGBM/LogReg.
    Загружает веса mlp_model.pt и параметры нормализации mlp_scaler.json (mean/std с train)."""

    NAME = "mlp"
    PRICE_SCALE = 1_000_000_000
    FEATURE_NAMES = [
        "imbalance",
        "spread",
        "imbalance_ma_5",
        "imbalance_ma_20",
        "momentum_5",
        "bid_qty",
        "ask_qty",
    ]

    def __init__(
        self,
        instrument_ids,
        model_path="my_scripts/ml/mlp_model.pt",
        scaler_path="my_scripts/ml/mlp_scaler.json",
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
        self.model = MLP(len(self.FEATURE_NAMES))
        self.model.load_state_dict(torch.load(model_path, map_location=self.device))
        self.model.eval()

        self.prob_threshold = prob_threshold
        self.confirmation_steps = confirmation_steps
        self.min_hold_updates = min_hold_updates
        self.order_size = order_size

        self.imbalance_history = {i: [] for i in self.instrument_ids}
        self.mid_price_history = {i: deque(maxlen=21) for i in self.instrument_ids}
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

        hist = self.imbalance_history[instrument_id]
        hist.append(imbalance)
        if len(hist) > 21:
            hist.pop(0)
        imbalance_ma_5 = sum(hist[-5:]) / len(hist[-5:]) if hist else 0.0
        imbalance_ma_20 = sum(hist[-20:]) / len(hist[-20:]) if hist else 0.0

        mp_hist = self.mid_price_history[instrument_id]
        mp_hist.append(mid_price)
        mp_list = list(mp_hist)

        if len(mp_list) > 5:
            momentum_5 = mp_list[-1] - mp_list[-6]
        else:
            momentum_5 = 0.0

        feats = torch.tensor(
            [
                imbalance,
                spread,
                imbalance_ma_5,
                imbalance_ma_20,
                momentum_5,
                bid_qty,
                ask_qty,
            ],
            dtype=torch.float32,
        )
        feats_norm = (feats - self.mean) / self.std

        with torch.no_grad():
            logit = self.model(feats_norm.unsqueeze(0))
            prob_up = torch.sigmoid(logit).item()

        pos = self.position(instrument_id)
        current_position = pos.net_quantity
        self.last_known_position[instrument_id] = pos
        self.updates_since_entry[instrument_id] += 1

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
