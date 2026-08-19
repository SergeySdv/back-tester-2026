"""
Честная валидация voting ensemble (LogReg+LightGBM+MLP+TCN): chronological fold evaluation
(5 последовательных окон) + permutation test.

ФИКС v3: фильтруем dataset только по instrument_id==1 — feature_extraction.py
не разделяет инструменты, и без фильтра mid_prices[i]/[i+1] могли относиться
к РАЗНЫМ инструментам (1 и 2), что превращало PnL в чистый шум (отсюда
sharpe≈0 и win_rate≈50% на каждом фолде в предыдущей версии).

Запуск:
    uv run python3 validate_ensemble.py --data synthetic_signal_slow_test.jsonl
    uv run python3 validate_ensemble.py --data synthetic_signal_slow_test.jsonl --no-filter
"""

from __future__ import annotations

import argparse
import json
import random
from collections import deque
from dataclasses import dataclass

import numpy as np
import torch
import lightgbm as lgb

from research_pipeline.ml.feature_extraction import replay_and_extract_features
from strategies.neural.models import MLP, TCN

TRANSACTION_COST = 0.5
N_CHRONOLOGICAL_FOLDS = 5
N_PERMUTATIONS = 100
MODEL_DIR = "research_pipeline/ml"

CONFIRMATION_STEPS = 3
MIN_HOLD_UPDATES = 15

LOGREG_FEATURES = [
    "imbalance",
    "spread",
    "imbalance_ma_5",
    "imbalance_ma_20",
    "momentum_5",
    "momentum_20",
    "volatility_20",
    "bid_qty",
    "ask_qty",
    "trade_freq_20",
]
LGB_MLP_FEATURES = [
    "imbalance",
    "spread",
    "imbalance_ma_5",
    "imbalance_ma_20",
    "momentum_5",
    "bid_qty",
    "ask_qty",
]
TCN_RAW_FEATURES = ["imbalance", "spread", "bid_qty", "ask_qty"]
TCN_WINDOW = 20


@dataclass
class Ensemble:
    logreg_coef: np.ndarray
    logreg_intercept: float
    logreg_acc: float
    lgb_model: lgb.Booster
    lgb_acc: float
    mlp_model: MLP
    mlp_mean: torch.Tensor
    mlp_std: torch.Tensor
    mlp_acc: float
    tcn_model: TCN
    tcn_mean: torch.Tensor
    tcn_std: torch.Tensor
    tcn_acc: float


def load_ensemble() -> Ensemble:
    with open(f"{MODEL_DIR}/logistic_model.json") as f:
        logreg_meta = json.load(f)
    lgb_model = lgb.Booster(model_file=f"{MODEL_DIR}/lightgbm_model_7feat.txt")
    with open(f"{MODEL_DIR}/lightgbm_meta.json") as f:
        lgb_meta = json.load(f)
    with open(f"{MODEL_DIR}/mlp_scaler.json") as f:
        mlp_scaler = json.load(f)
    mlp_model = MLP(len(LGB_MLP_FEATURES))
    mlp_model.load_state_dict(
        torch.load(f"{MODEL_DIR}/mlp_model.pt", map_location="cpu")
    )
    mlp_model.eval()
    with open(f"{MODEL_DIR}/tcn_scaler.json") as f:
        tcn_scaler = json.load(f)
    tcn_model = TCN(n_features=len(tcn_scaler["mean"]))
    tcn_model.load_state_dict(
        torch.load(f"{MODEL_DIR}/tcn_model.pt", map_location="cpu")
    )
    tcn_model.eval()

    return Ensemble(
        logreg_coef=np.array(logreg_meta["coef"]),
        logreg_intercept=logreg_meta["intercept"],
        logreg_acc=logreg_meta["test_acc"],
        lgb_model=lgb_model,
        lgb_acc=lgb_meta["test_acc"],
        mlp_model=mlp_model,
        mlp_mean=torch.tensor(mlp_scaler["mean"], dtype=torch.float32),
        mlp_std=torch.tensor(mlp_scaler["std"], dtype=torch.float32),
        mlp_acc=mlp_scaler["test_acc"],
        tcn_model=tcn_model,
        tcn_mean=torch.tensor(tcn_scaler["mean"], dtype=torch.float32),
        tcn_std=torch.tensor(tcn_scaler["std"], dtype=torch.float32),
        tcn_acc=tcn_scaler["test_acc"],
    )


def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))


def predict_logreg(ens, row):
    x = np.array([row[c] for c in LOGREG_FEATURES])
    return sigmoid(np.dot(ens.logreg_coef, x) + ens.logreg_intercept)


def predict_lgb(ens, row):
    x = np.array([[row[c] for c in LGB_MLP_FEATURES]])
    return float(ens.lgb_model.predict(x)[0])


def predict_mlp(ens, row):
    x = torch.tensor([row[c] for c in LGB_MLP_FEATURES], dtype=torch.float32)
    x_norm = (x - ens.mlp_mean) / ens.mlp_std
    with torch.no_grad():
        logit = ens.mlp_model(x_norm.unsqueeze(0))
    return torch.sigmoid(logit).item()


def predict_tcn(ens, window):
    seq = torch.tensor(list(window), dtype=torch.float32).unsqueeze(0)
    seq_norm = (seq - ens.tcn_mean) / ens.tcn_std
    with torch.no_grad():
        logit = ens.tcn_model(seq_norm)
    return torch.sigmoid(logit).item()


def weighted_vote(probs, weights):
    score = sum((probs[m] - 0.5) * weights[m] for m in probs)
    total_weight = sum(weights[m] for m in probs)
    norm_score = score / total_weight if total_weight > 0 else 0.0
    decision = "LONG" if norm_score > 0 else "SHORT" if norm_score < 0 else "FLAT"
    return decision


def get_decisions_and_prices(ens, weights, dataset):
    tcn_window = deque(maxlen=TCN_WINDOW)
    mid_price_prev = None
    decisions = []
    mid_prices = []

    for row in dataset:
        p_logreg = predict_logreg(ens, row)
        p_lgb = predict_lgb(ens, row)
        p_mlp = predict_mlp(ens, row)

        mid_price = row["mid_price"]
        mid_price_delta = (
            (mid_price - mid_price_prev) if mid_price_prev is not None else 0.0
        )
        mid_price_prev = mid_price
        raw_feat = [row[c] for c in TCN_RAW_FEATURES] + [mid_price_delta]
        tcn_window.append(raw_feat)

        probs = {"logreg": p_logreg, "lgb": p_lgb, "mlp": p_mlp}
        if len(tcn_window) >= TCN_WINDOW:
            probs["tcn"] = predict_tcn(ens, tcn_window)

        decisions.append(weighted_vote(probs, weights))
        mid_prices.append(mid_price)

    return decisions, mid_prices


DECISION_TO_POSITION = {"LONG": 1, "SHORT": -1, "FLAT": 0}
POSITION_TO_DECISION = {1: "LONG", -1: "SHORT", 0: "FLAT"}


def apply_confirmation_and_hold(
    decisions, confirmation_steps=CONFIRMATION_STEPS, min_hold_updates=MIN_HOLD_UPDATES
):
    filtered = []
    position = 0
    pending_target = None
    pending_count = 0
    hold_counter = 0

    for d in decisions:
        target = DECISION_TO_POSITION.get(d, 0)

        if hold_counter > 0:
            hold_counter -= 1
            filtered.append(POSITION_TO_DECISION[position])
            continue

        if target == position:
            pending_target = None
            pending_count = 0
            filtered.append(POSITION_TO_DECISION[position])
            continue

        if target == pending_target:
            pending_count += 1
        else:
            pending_target = target
            pending_count = 1

        if pending_count >= confirmation_steps:
            position = target
            hold_counter = min_hold_updates - 1
            pending_target = None
            pending_count = 0

        filtered.append(POSITION_TO_DECISION[position])

    return filtered


def compute_pnl(decisions, mid_prices, start, end):
    pnls = []
    position = 0
    for i in range(start, min(end, len(decisions) - 1)):
        target = DECISION_TO_POSITION.get(decisions[i], 0)
        pnl = target * (mid_prices[i + 1] - mid_prices[i])
        if target != position:
            pnl -= TRANSACTION_COST
        position = target
        pnls.append(pnl)
    return pnls


def compute_metrics_from_pnls(pnls):
    if not pnls:
        return {"sharpe": 0.0, "win_rate": 0.0, "total_pnl": 0.0, "profit_factor": 0.0}
    total_pnl = sum(pnls)
    mean_pnl = total_pnl / len(pnls)
    variance = (
        sum((p - mean_pnl) ** 2 for p in pnls) / len(pnls) if len(pnls) > 1 else 0.0
    )
    std_pnl = variance**0.5
    sharpe = (mean_pnl / std_pnl) if std_pnl > 0 else 0.0
    nonzero = [p for p in pnls if p != 0]
    win_rate = (sum(1 for p in nonzero if p > 0) / len(nonzero)) if nonzero else 0.0
    gains = sum(p for p in pnls if p > 0)
    losses = -sum(p for p in pnls if p < 0)
    profit_factor = (
        (gains / losses) if losses > 0 else (float("inf") if gains > 0 else 0.0)
    )
    return {
        "sharpe": sharpe,
        "win_rate": win_rate,
        "total_pnl": total_pnl,
        "profit_factor": profit_factor,
    }


def segments_from_decisions(decisions):
    segments = []
    if not decisions:
        return segments
    seg_start = 0
    seg_val = decisions[0]
    for i in range(1, len(decisions)):
        if decisions[i] != seg_val:
            segments.append((seg_start, i, seg_val))
            seg_start = i
            seg_val = decisions[i]
    segments.append((seg_start, len(decisions), seg_val))
    return segments


def permutation_test_ensemble(
    decisions, mid_prices, n_permutations=N_PERMUTATIONS, seed=42
):
    segments = segments_from_decisions(decisions)
    n_transitions = sum(
        1 for i in range(1, len(segments)) if segments[i][2] != segments[i - 1][2]
    )
    fixed_cost = n_transitions * TRANSACTION_COST

    def market_pnl(segs):
        total = 0.0
        for s, e, val in segs:
            target = DECISION_TO_POSITION.get(val, 0)
            for t in range(s, min(e, len(mid_prices) - 1)):
                total += target * (mid_prices[t + 1] - mid_prices[t])
        return total

    real_total = market_pnl(segments) - fixed_cost

    nonflat_idx = [i for i, s in enumerate(segments) if s[2] != "FLAT"]
    nonflat_vals = [segments[i][2] for i in nonflat_idx]

    rng = random.Random(seed)
    random_totals = []
    for _ in range(n_permutations):
        shuffled = nonflat_vals.copy()
        rng.shuffle(shuffled)
        new_segments = list(segments)
        for idx, val in zip(nonflat_idx, shuffled):
            s, e, _ = new_segments[idx]
            new_segments[idx] = (s, e, val)
        random_totals.append(market_pnl(new_segments) - fixed_cost)

    n_extreme = sum(1 for t in random_totals if abs(t) >= abs(real_total))
    p_value = n_extreme / n_permutations
    random_mean = sum(random_totals) / len(random_totals)
    return {
        "real_total": real_total,
        "random_mean": random_mean,
        "p_value": p_value,
        "n_transitions": n_transitions,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", required=True)
    ap.add_argument("--no-filter", action="store_true")
    ap.add_argument("--confirmation-steps", type=int, default=CONFIRMATION_STEPS)
    ap.add_argument("--min-hold-updates", type=int, default=MIN_HOLD_UPDATES)
    ap.add_argument(
        "--instrument-id",
        type=int,
        default=1,
        help="фильтровать датасет только по этому инструменту "
        "(feature_extraction.py не разделяет инструменты сам)",
    )
    args = ap.parse_args()

    print("Загружаю модели (LogReg, LightGBM, MLP, TCN)...")
    ens = load_ensemble()
    weights = {
        "logreg": ens.logreg_acc,
        "lgb": ens.lgb_acc,
        "mlp": ens.mlp_acc,
        "tcn": ens.tcn_acc,
    }

    print(f"Данные: {args.data} — извлечение фичей...")
    dataset_all = replay_and_extract_features(
        args.data, horizon=20, imbalance_windows=(5, 20)
    )
    print(f"Всего наблюдений (все инструменты): {len(dataset_all)}")
    dataset = [row for row in dataset_all if row["instrument_id"] == args.instrument_id]
    n = len(dataset)
    print(f"После фильтра instrument_id=={args.instrument_id}: {n} наблюдений.")

    print("Получение решений ансамбля по всем наблюдениям...")
    raw_decisions, mid_prices = get_decisions_and_prices(ens, weights, dataset)

    if args.no_filter:
        decisions = raw_decisions
        print(
            "Фильтр confirmation/hold ОТКЛЮЧЕН (--no-filter) — сырые решения на каждом баре."
        )
    else:
        decisions = apply_confirmation_and_hold(
            raw_decisions,
            confirmation_steps=args.confirmation_steps,
            min_hold_updates=args.min_hold_updates,
        )
        n_raw_changes = sum(
            1
            for i in range(1, len(raw_decisions))
            if raw_decisions[i] != raw_decisions[i - 1]
        )
        n_filtered_changes = sum(
            1 for i in range(1, len(decisions)) if decisions[i] != decisions[i - 1]
        )
        print(
            f"Фильтр: confirmation_steps={args.confirmation_steps}, "
            f"min_hold_updates={args.min_hold_updates}"
        )
        print(
            f"Смен решения: сырых={n_raw_changes} -> после фильтра={n_filtered_changes}"
        )

    full_pnls = compute_pnl(decisions, mid_prices, 0, n)
    full_metrics = compute_metrics_from_pnls(full_pnls)
    print("\n=== Полная оценка ===")
    print(
        f"total_pnl: {full_metrics['total_pnl']:.2f}  sharpe: {full_metrics['sharpe']:.3f}  "
        f"profit_factor: {full_metrics['profit_factor']:.3f}  win_rate: {full_metrics['win_rate']:.1%}"
    )

    print(
        f"\n=== Chronological fold evaluation ({N_CHRONOLOGICAL_FOLDS} последовательных окон) ==="
    )
    fold_size = n // N_CHRONOLOGICAL_FOLDS
    fold_sharpes = []
    for fold in range(N_CHRONOLOGICAL_FOLDS):
        start = fold * fold_size
        end = n if fold == N_CHRONOLOGICAL_FOLDS - 1 else (fold + 1) * fold_size
        pnls = compute_pnl(decisions, mid_prices, start, end)
        m = compute_metrics_from_pnls(pnls)
        fold_sharpes.append(m["sharpe"])
        print(
            f"  Fold {fold}: range=[{start}:{end}] total_pnl={m['total_pnl']:.2f} "
            f"sharpe={m['sharpe']:.4f} win_rate={m['win_rate']:.1%}"
        )
    mean_sharpe = sum(fold_sharpes) / len(fold_sharpes)
    positive_folds = sum(1 for s in fold_sharpes if s > 0)
    print(f"Средний Sharpe по фолдам: {mean_sharpe:.4f}")
    print(f"Положительных фолдов: {positive_folds}/{N_CHRONOLOGICAL_FOLDS}")

    print(
        f"\n=== Permutation test ({N_PERMUTATIONS} перестановок, знак при фикс. частоте) ==="
    )
    pt = permutation_test_ensemble(decisions, mid_prices, n_permutations=N_PERMUTATIONS)
    print(f"Транзакций: {pt['n_transitions']}")
    print(f"Реальный total PnL:  {pt['real_total']:.2f}")
    print(f"Случайный mean PnL:  {pt['random_mean']:.2f}")
    print(f"p-value:             {pt['p_value']:.4f}")
    if pt["p_value"] < 0.05:
        print(f"=> Итог: сигнал ЗНАЧИМ (p={pt['p_value']:.4f} < 0.05)")
    else:
        print(f"=> Итог: сигнал НЕ подтверждён (p={pt['p_value']:.4f} >= 0.05)")


if __name__ == "__main__":
    main()
