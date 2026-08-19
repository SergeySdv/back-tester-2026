"""
Честная out-of-sample оценка DQN на TEST-сегменте + chronological fold evaluation (5 окон)
+ permutation test.

ВАЖНО про permutation test: перемешивается НЕ последовательность действий
целиком (это искажало бы результат количеством транзакций — случайная
перестановка обычно даёт другую частоту смены позиции, а значит другую
суммарную комиссию, что делает сравнение нечестным). Вместо этого
сохраняются ТЕ ЖЕ САМЫЕ моменты смены позиции (та же частота сделок,
та же суммарная комиссия), но случайно перемешивается ЗНАК (LONG/SHORT)
каждого отрезка удержания позиции. Это изолированно проверяет: угадывает
ли DQN правильное НАПРАВЛЕНИЕ сделки лучше случайного, а не запутывается
в артефакте частоты транзакций.

Запуск:
    uv run python3 eval_dqn.py --data synthetic_signal_slow_test.jsonl \
        --model research_pipeline/ml/dqn_model_traintest.pt \
        --meta research_pipeline/ml/dqn_meta_traintest.json
"""

from __future__ import annotations

import argparse
import json
import random

import numpy as np
import torch
import torch.nn as nn

from research_pipeline.ml.feature_extraction import extract_feature_rows

TRANSACTION_COST = 0.5
N_CHRONOLOGICAL_FOLDS = 5
N_PERMUTATIONS = 100


class QNet(nn.Module):
    def __init__(self, state_dim, n_actions=3):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(state_dim, 64),
            nn.ReLU(),
            nn.Linear(64, 64),
            nn.ReLU(),
            nn.Linear(64, n_actions),
        )

    def forward(self, x):
        return self.net(x)


def precompute_trajectory(path: str):
    rows = [row for row in extract_feature_rows(path) if row["instrument_id"] == 1]
    feature_names = [
        "imbalance",
        "spread",
        "imbalance_ma_5",
        "imbalance_ma_20",
        "momentum_5",
    ]
    return (
        np.asarray(
            [[row[name] for name in feature_names] for row in rows],
            dtype=np.float32,
        ),
        np.asarray([row["mid_price"] for row in rows], dtype=np.float32),
    )


def run_policy(q_net, feats_arr, mid_arr, device, start, end):
    """Прогоняет epsilon=0 политику на диапазоне [start, end). Возвращает
    pnls и position_log (позиция на каждом шаге, не raw action)."""
    position = 0
    pnls = []
    position_log = []

    with torch.no_grad():
        for t in range(start, min(end, len(feats_arr) - 1)):
            feats = feats_arr[t]
            mid = mid_arr[t]
            state_vec = np.append(feats, position).astype(np.float32)
            q = q_net(torch.tensor(state_vec, device=device).unsqueeze(0))
            action = int(q.argmax(dim=1).item())
            target_position = {0: position, 1: 1, 2: -1}[action]
            next_mid = mid_arr[t + 1]

            pnl = position * (next_mid - mid)
            if target_position != position:
                pnl -= TRANSACTION_COST
            position = target_position
            pnls.append(pnl)
            position_log.append(position)

    return pnls, position_log


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


def segments_from_position_log(position_log):
    """Разбивает position_log на отрезки постоянной позиции:
    [(start_idx, end_idx, position_value), ...]. Число отрезков = число
    удержаний позиции; переходов между ними = число транзакций."""
    segments = []
    if not position_log:
        return segments
    seg_start = 0
    seg_val = position_log[0]
    for i in range(1, len(position_log)):
        if position_log[i] != seg_val:
            segments.append((seg_start, i, seg_val))
            seg_start = i
            seg_val = position_log[i]
    segments.append((seg_start, len(position_log), seg_val))
    return segments


def permutation_test_dqn(
    position_log, mid_arr, start, n_permutations=N_PERMUTATIONS, seed=42
):
    """Сохраняет ТЕ ЖЕ моменты смены позиции (ту же частоту сделок, ту же
    суммарную комиссию), но случайно перемешивает ЗНАК каждого отрезка
    удержания (LONG <-> SHORT). FLAT-отрезки (position=0) не трогаем.
    Это изолированно проверяет направленческую точность DQN, не смешивая
    её с артефактом частоты транзакций."""
    segments = segments_from_position_log(position_log)
    # число смен позиции идентично во всех перестановках -> комиссия постоянна
    n_transitions = sum(
        1 for i in range(1, len(segments)) if segments[i][2] != segments[i - 1][2]
    )
    fixed_transaction_cost = n_transitions * TRANSACTION_COST

    def market_pnl_for_segments(segs):
        """PnL БЕЗ учёта комиссии (она добавляется один раз отдельно, т.к. постоянна)."""
        total = 0.0
        for seg_start, seg_end, val in segs:
            for t in range(start + seg_start, start + seg_end):
                if t + 1 >= len(mid_arr):
                    break
                total += val * (mid_arr[t + 1] - mid_arr[t])
        return total

    real_market_pnl = market_pnl_for_segments(segments)
    real_total = real_market_pnl - fixed_transaction_cost

    nonzero_indices = [i for i, s in enumerate(segments) if s[2] != 0]
    nonzero_values = [segments[i][2] for i in nonzero_indices]

    rng = random.Random(seed)
    random_totals = []
    for _ in range(n_permutations):
        shuffled_values = nonzero_values.copy()
        rng.shuffle(shuffled_values)
        new_segments = list(segments)
        for idx, val in zip(nonzero_indices, shuffled_values):
            s, e, _ = new_segments[idx]
            new_segments[idx] = (s, e, val)
        market_pnl = market_pnl_for_segments(new_segments)
        random_totals.append(market_pnl - fixed_transaction_cost)

    n_extreme = sum(1 for t in random_totals if abs(t) >= abs(real_total))
    p_value = n_extreme / n_permutations
    random_mean = sum(random_totals) / len(random_totals)
    return {
        "real_total": real_total,
        "random_mean": random_mean,
        "p_value": p_value,
        "n_segments": len(segments),
        "n_transitions": n_transitions,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", required=True)
    ap.add_argument("--model", default="research_pipeline/ml/dqn_model_traintest.pt")
    ap.add_argument("--meta", default="research_pipeline/ml/dqn_meta_traintest.json")
    args = ap.parse_args()

    with open(args.meta) as f:
        meta = json.load(f)
    state_dim = meta["state_dim"]

    device = torch.device("cpu")
    q_net = QNet(state_dim).to(device)
    q_net.load_state_dict(torch.load(args.model, map_location=device))
    q_net.eval()

    print(f"Модель загружена: {args.model} (state_dim={state_dim})")
    print(f"Данные: {args.data} — HONEST OUT-OF-SAMPLE")
    print("Предвычисление траектории...")
    feats_arr, mid_arr = precompute_trajectory(args.data)
    n = len(feats_arr)
    print(f"Длина траектории: {n}")

    full_pnls, full_positions = run_policy(q_net, feats_arr, mid_arr, device, 0, n)
    full_metrics = compute_metrics_from_pnls(full_pnls)
    print("\n=== Полная оценка (весь test-сегмент) ===")
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
        pnls, _ = run_policy(q_net, feats_arr, mid_arr, device, start, end)
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
        f"\n=== Permutation test ({N_PERMUTATIONS} перестановок, знак сделок при фикс. частоте) ==="
    )
    pt = permutation_test_dqn(
        full_positions, mid_arr, start=0, n_permutations=N_PERMUTATIONS
    )
    print(
        f"Число отрезков позиции: {pt['n_segments']}, транзакций: {pt['n_transitions']}"
    )
    print(f"Реальный total PnL:  {pt['real_total']:.2f}")
    print(f"Случайный mean PnL:  {pt['random_mean']:.2f}")
    print(f"p-value:             {pt['p_value']:.4f}")
    if pt["p_value"] < 0.05:
        print(f"=> Итог: сигнал ЗНАЧИМ (p={pt['p_value']:.4f} < 0.05)")
    else:
        print(f"=> Итог: сигнал НЕ подтверждён (p={pt['p_value']:.4f} >= 0.05)")


if __name__ == "__main__":
    main()
