"""
VotingEnsembleStrategy — взвешенное голосование LogReg + LightGBM + MLP + TCN
внутри самого C++ backtesting движка (не отдельный Python-скрипт поверх
offline feature_extraction, как в ensemble_voting.py).

Почему так, а не отдельным скриптом:
  - Метрики считаются той же compute_metrics() из research_pipeline/metrics.py,
    что и у остальных 8 стратегий в run_comparison.py — гарантированно
    сравнимо, без риска рассинхронизации формул (Sharpe/PnL/Profit Factor).
  - Ордера проходят через реальный ордер-матчинг движка (fills, latency),
    а не приближённую Python-эмуляцию PnL.

Веса голосования — test_acc каждой модели из её meta-файла (та же логика,
что в ensemble_voting.py): более точные модели имеют больший голос.

trade_freq_20 (нужен только LogReg) считается через on_trade — паттерн
скопирован из strategies/ml/ml_logistic.py.
"""

import json
import math
from collections import deque

import torch
import lightgbm as lgb

import back_tester as bt
from strategies.common.base_mixin import TrackingMixin
from strategies.neural.models import MLP, TCN


class VotingEnsembleStrategy(TrackingMixin, bt.Strategy):
    """Взвешенное голосование 4 моделей на каждом book update.
    Ордер выставляется по той же confirmation_steps/min_hold_updates
    логике, что и у остальных ML-стратегий проекта."""

    NAME = "ensemble_voting"
    PRICE_SCALE = 1_000_000_000
    TCN_WINDOW = 20

    def __init__(
        self,
        instrument_ids,
        logreg_path="ml/logistic_model.json",
        lgb_path="ml/lightgbm_model_7feat.txt",
        lgb_meta_path="ml/lightgbm_meta.json",
        mlp_model_path="ml/mlp_model.pt",
        mlp_scaler_path="ml/mlp_scaler.json",
        tcn_model_path="ml/tcn_model.pt",
        tcn_scaler_path="ml/tcn_scaler.json",
        prob_threshold=0.52,
        confirmation_steps=3,
        min_hold_updates=15,
        order_size=1,
    ):
        super().__init__()
        self._init_tracking(instrument_ids)

        # --- LogReg ---
        with open(logreg_path) as f:
            logreg = json.load(f)
        self.logreg_features = logreg["features"]
        self.logreg_coef = logreg["coef"]
        self.logreg_intercept = logreg["intercept"]
        self.logreg_acc = logreg["test_acc"]

        # --- LightGBM ---
        self.lgb_model = lgb.Booster(model_file=lgb_path)
        with open(lgb_meta_path) as f:
            lgb_meta = json.load(f)
        self.lgb_features = lgb_meta["features"]
        self.lgb_acc = lgb_meta["test_acc"]

        # --- MLP ---
        with open(mlp_scaler_path) as f:
            mlp_scaler = json.load(f)
        self.mlp_features = mlp_scaler.get(
            "features", self.lgb_features
        )  # тот же 7-фичевый набор
        self.mlp_mean = torch.tensor(mlp_scaler["mean"], dtype=torch.float32)
        self.mlp_std = torch.tensor(mlp_scaler["std"], dtype=torch.float32)
        self.mlp_acc = mlp_scaler["test_acc"]
        self.mlp_model = MLP(len(self.mlp_features))
        self.mlp_model.load_state_dict(torch.load(mlp_model_path, map_location="cpu"))
        self.mlp_model.eval()

        # --- TCN ---
        with open(tcn_scaler_path) as f:
            tcn_scaler = json.load(f)
        self.tcn_mean = torch.tensor(tcn_scaler["mean"], dtype=torch.float32)
        self.tcn_std = torch.tensor(tcn_scaler["std"], dtype=torch.float32)
        self.tcn_acc = tcn_scaler["test_acc"]
        self.tcn_model = TCN(n_features=len(tcn_scaler["mean"]))
        self.tcn_model.load_state_dict(torch.load(tcn_model_path, map_location="cpu"))
        self.tcn_model.eval()

        self.weights = {
            "logreg": self.logreg_acc,
            "lgb": self.lgb_acc,
            "mlp": self.mlp_acc,
            "tcn": self.tcn_acc,
        }

        self.prob_threshold = prob_threshold
        self.confirmation_steps = confirmation_steps
        self.min_hold_updates = min_hold_updates
        self.order_size = order_size

        # --- состояние на инструмент (паттерн из ml_logistic.py) ---
        self.imbalance_history = {i: [] for i in self.instrument_ids}
        self.mid_price_history = {i: deque(maxlen=21) for i in self.instrument_ids}
        self.trade_flag_history = {i: deque(maxlen=20) for i in self.instrument_ids}
        self.trades_since_last_row = {i: 0 for i in self.instrument_ids}
        self.tcn_window = {
            i: deque(maxlen=self.TCN_WINDOW) for i in self.instrument_ids
        }
        self.mid_price_prev = {i: None for i in self.instrument_ids}
        self.signal_streak = {i: 0 for i in self.instrument_ids}
        self.streak_direction = {i: 0 for i in self.instrument_ids}
        self.updates_since_entry = {i: 0 for i in self.instrument_ids}

    def _predict_logreg(self, feats: dict) -> float:
        z = self.logreg_intercept
        for name, w in zip(self.logreg_features, self.logreg_coef):
            z += w * feats[name]
        return 1.0 / (1.0 + math.exp(-z))

    def _predict_lgb(self, feats: dict) -> float:
        x = [[feats[c] for c in self.lgb_features]]
        return float(self.lgb_model.predict(x)[0])

    def _predict_mlp(self, feats: dict) -> float:
        x = torch.tensor([feats[c] for c in self.mlp_features], dtype=torch.float32)
        x_norm = (x - self.mlp_mean) / self.mlp_std
        with torch.no_grad():
            logit = self.mlp_model(x_norm.unsqueeze(0))
        return torch.sigmoid(logit).item()

    def _predict_tcn(self, window: deque) -> float:
        seq = torch.tensor(list(window), dtype=torch.float32).unsqueeze(0)
        seq_norm = (seq - self.tcn_mean) / self.tcn_std
        with torch.no_grad():
            logit = self.tcn_model(seq_norm)
        return torch.sigmoid(logit).item()

    def on_trade(self, trade):
        self.trades_seen += 1
        instrument_id = trade.instrument_id
        self.trades_since_last_row[instrument_id] = (
            self.trades_since_last_row.get(instrument_id, 0) + 1
        )

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

        momentum_5 = mp_list[-1] - mp_list[-6] if len(mp_list) > 5 else 0.0
        momentum_20 = mp_list[-1] - mp_list[-21] if len(mp_list) > 20 else 0.0

        if len(mp_list) >= 2:
            window = mp_list[-20:] if len(mp_list) >= 20 else mp_list
            mean_v = sum(window) / len(window)
            var_v = sum((v - mean_v) ** 2 for v in window) / len(window)
            volatility_20 = var_v**0.5
        else:
            volatility_20 = 0.0

        tf_hist = self.trade_flag_history[instrument_id]
        tf_hist.append(1 if self.trades_since_last_row.get(instrument_id, 0) > 0 else 0)
        self.trades_since_last_row[instrument_id] = 0
        trade_freq_20 = sum(tf_hist) / len(tf_hist) if tf_hist else 0.0

        feats = {
            "imbalance": imbalance,
            "spread": spread,
            "imbalance_ma_5": imbalance_ma_5,
            "imbalance_ma_20": imbalance_ma_20,
            "momentum_5": momentum_5,
            "momentum_20": momentum_20,
            "volatility_20": volatility_20,
            "bid_qty": bid_qty,
            "ask_qty": ask_qty,
            "trade_freq_20": trade_freq_20,
        }

        # --- TCN window (raw features + mid_price_delta) ---
        prev_mp = self.mid_price_prev[instrument_id]
        mid_price_delta = (mid_price - prev_mp) if prev_mp is not None else 0.0
        self.mid_price_prev[instrument_id] = mid_price
        tcn_window = self.tcn_window[instrument_id]
        tcn_window.append([imbalance, spread, bid_qty, ask_qty, mid_price_delta])

        # --- предсказания всех 4 моделей ---
        probs = {
            "logreg": self._predict_logreg(feats),
            "lgb": self._predict_lgb(feats),
            "mlp": self._predict_mlp(feats),
        }
        if len(tcn_window) >= self.TCN_WINDOW:
            probs["tcn"] = self._predict_tcn(tcn_window)

        score = sum((probs[m] - 0.5) * self.weights[m] for m in probs)
        total_weight = sum(self.weights[m] for m in probs)
        norm_score = score / total_weight if total_weight > 0 else 0.0
        prob_up = (
            0.5 + norm_score
        )  # обратно в шкалу вероятности для confirmation-логики ниже

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
