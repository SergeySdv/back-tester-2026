"""
Проверка: работает ли старый 10-фичевый lightgbm_model.txt (тот же набор
фичей, что у LogReg), и как его метрики сравниваются с актуальной
7-фичевой версией (lightgbm_model_7feat.txt), которая используется в
LightGBMStrategy/run_comparison.py.

Та же методология, что validate_ensemble.py: honest test-сегмент,
confirmation_steps/min_hold_updates фильтр, chronological fold evaluation + permutation test.

Запуск:
    uv run python3 validate_lightgbm10.py --data synthetic_signal_slow_test.jsonl
"""

from __future__ import annotations

import argparse
import random
import sys

import numpy as np
import lightgbm as lgb

sys.path.insert(0, "research_pipeline/ml")
from feature_extraction import replay_and_extract_features  # noqa: E402

TRANSACTION_COST = 0.5
N_CHRONOLOGICAL_FOLDS = 5
N_PERMUTATIONS = 100
MODEL_DIR = "research_pipeline/ml"
CONFIRMATION_STEPS = 3
MIN_HOLD_UPDATES = 15

LOGREG_FEATURES_10 = [
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
LGB_FEATURES_7 = [
    "imbalance",
    "spread",
    "imbalance_ma_5",
    "imbalance_ma_20",
    "momentum_5",
    "bid_qty",
    "ask_qty",
]

DECISION_TO_POSITION = {"LONG": 1, "SHORT": -1, "FLAT": 0}
POSITION_TO_DECISION = {1: "LONG", -1: "SHORT", 0: "FLAT"}


def predict_lgb(model, features, row):
    x = np.array([[row[c] for c in features]])
    return float(model.predict(x)[0])


def get_decisions(model, features, dataset, prob_threshold=0.52):
    decisions = []
    mid_prices = []
    for row in dataset:
        prob = predict_lgb(model, features, row)
        if prob > prob_threshold:
            decisions.append("LONG")
        elif prob < (1 - prob_threshold):
            decisions.append("SHORT")
        else:
            decisions.append("FLAT")
        mid_prices.append(row["mid_price"])
    return decisions, mid_prices


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


def permutation_test(decisions, mid_prices, n_permutations=N_PERMUTATIONS, seed=42):
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


def run_full_validation(name, model, features, dataset, n):
    print(f"\n{'=' * 60}\n{name}\n{'=' * 60}")
    raw_decisions, mid_prices = get_decisions(model, features, dataset)
    decisions = apply_confirmation_and_hold(raw_decisions)
    n_raw = sum(
        1
        for i in range(1, len(raw_decisions))
        if raw_decisions[i] != raw_decisions[i - 1]
    )
    n_filt = sum(
        1 for i in range(1, len(decisions)) if decisions[i] != decisions[i - 1]
    )
    print(f"Смен решения: сырых={n_raw} -> после фильтра={n_filt}")

    full_pnls = compute_pnl(decisions, mid_prices, 0, n)
    m = compute_metrics_from_pnls(full_pnls)
    print(
        f"total_pnl={m['total_pnl']:.2f}  sharpe={m['sharpe']:.4f}  "
        f"profit_factor={m['profit_factor']:.3f}  win_rate={m['win_rate']:.1%}"
    )

    fold_size = n // N_CHRONOLOGICAL_FOLDS
    fold_sharpes = []
    for fold in range(N_CHRONOLOGICAL_FOLDS):
        start = fold * fold_size
        end = n if fold == N_CHRONOLOGICAL_FOLDS - 1 else (fold + 1) * fold_size
        pnls = compute_pnl(decisions, mid_prices, start, end)
        fm = compute_metrics_from_pnls(pnls)
        fold_sharpes.append(fm["sharpe"])
        print(f"  Fold {fold}: sharpe={fm['sharpe']:.4f} win_rate={fm['win_rate']:.1%}")
    positive_folds = sum(1 for s in fold_sharpes if s > 0)
    print(f"Положительных фолдов: {positive_folds}/{N_CHRONOLOGICAL_FOLDS}")

    pt = permutation_test(decisions, mid_prices)
    print(
        f"Permutation test: real={pt['real_total']:.2f} random_mean={pt['random_mean']:.2f} "
        f"p={pt['p_value']:.4f}"
    )
    if pt["p_value"] < 0.05:
        print(f"=> ЗНАЧИМ (p={pt['p_value']:.4f})")
    else:
        print(f"=> НЕ подтверждён (p={pt['p_value']:.4f})")

    return {
        "name": name,
        "sharpe": m["sharpe"],
        "profit_factor": m["profit_factor"],
        "win_rate": m["win_rate"],
        "p_value": pt["p_value"],
        "positive_folds": positive_folds,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", required=True)
    ap.add_argument("--instrument-id", type=int, default=1)
    args = ap.parse_args()

    print("Проверяю, загружается ли старый 10-фичевый lightgbm_model.txt...")
    try:
        model_10 = lgb.Booster(model_file=f"{MODEL_DIR}/lightgbm_model.txt")
        print(f"OK: lightgbm_model.txt загружен, num_feature={model_10.num_feature()}")
    except Exception as e:
        print(f"ОШИБКА загрузки lightgbm_model.txt: {e}")
        sys.exit(1)

    print("Загружаю актуальный 7-фичевый lightgbm_model_7feat.txt для сравнения...")
    model_7 = lgb.Booster(model_file=f"{MODEL_DIR}/lightgbm_model_7feat.txt")
    print(f"OK: num_feature={model_7.num_feature()}")

    if model_10.num_feature() != len(LOGREG_FEATURES_10):
        print(
            f"ВНИМАНИЕ: num_feature старой модели ({model_10.num_feature()}) "
            f"не совпадает с ожидаемыми 10 фичами LogReg-набора. "
            f"Порядок фичей может отличаться — результат может быть некорректным."
        )

    print(f"\nДанные: {args.data} — извлечение фичей...")
    dataset_all = replay_and_extract_features(
        args.data, horizon=20, imbalance_windows=(5, 20)
    )
    dataset = [row for row in dataset_all if row["instrument_id"] == args.instrument_id]
    n = len(dataset)
    print(f"Наблюдений (instrument_id={args.instrument_id}): {n}")

    r10 = run_full_validation(
        "LightGBM (10 фичей, старая модель)", model_10, LOGREG_FEATURES_10, dataset, n
    )
    r7 = run_full_validation(
        "LightGBM (7 фичей, актуальная модель)", model_7, LGB_FEATURES_7, dataset, n
    )

    print(f"\n{'=' * 60}\nСРАВНЕНИЕ\n{'=' * 60}")
    print(
        f"{'Модель':<35} {'Sharpe':>8} {'PF':>8} {'WinRate':>9} {'p-value':>9} {'Фолды':>7}"
    )
    for r in [r10, r7]:
        print(
            f"{r['name']:<35} {r['sharpe']:>8.4f} {r['profit_factor']:>8.3f} "
            f"{r['win_rate']:>8.1%} {r['p_value']:>9.4f} {r['positive_folds']}/5"
        )


if __name__ == "__main__":
    main()
