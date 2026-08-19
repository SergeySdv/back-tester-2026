"""
Честная валидация LSTM: chronological fold evaluation (5 последовательных окон) + permutation
test — в стиле Task 3 (валидация MLP) и eval_dqn.py.

Запуск (CPU):
    uv run python3 validate_lstm.py --data ../../synthetic_signal_slow_test.jsonl
"""

from __future__ import annotations

import argparse
import json
import random
from pathlib import Path

import numpy as np
import torch
from strategies.neural.models import LSTM

from .feature_extraction_lstm import replay_and_extract_sequences

N_CHRONOLOGICAL_FOLDS = 5
N_PERMUTATIONS = 100


def evaluate_accuracy(model, X, y, mean, std, device, batch_size=8192):
    model.eval()
    correct = 0
    with torch.no_grad():
        for i in range(0, len(X), batch_size):
            xb = (X[i : i + batch_size] - mean) / std
            xb = torch.tensor(xb, dtype=torch.float32, device=device)
            yb = y[i : i + batch_size]
            pred = (torch.sigmoid(model(xb)) > 0.5).cpu().numpy().astype(int)
            correct += (pred == yb).sum()
    return correct / len(X)


def get_predictions(model, X, mean, std, device, batch_size=8192):
    """Возвращает бинарные предсказания (0/1) на всём X."""
    model.eval()
    preds = []
    with torch.no_grad():
        for i in range(0, len(X), batch_size):
            xb = (X[i : i + batch_size] - mean) / std
            xb = torch.tensor(xb, dtype=torch.float32, device=device)
            pred = (torch.sigmoid(model(xb)) > 0.5).cpu().numpy().astype(int)
            preds.append(pred)
    return np.concatenate(preds)


def permutation_test_lstm(y_true, y_pred, n_permutations=N_PERMUTATIONS, seed=42):
    """Перемешивает предсказания случайным образом и сравнивает accuracy
    с реальной — та же логика permutation test, что в Task 3."""
    real_acc = (y_pred == y_true).mean()
    rng = random.Random(seed)
    n = len(y_pred)
    random_accs = []
    for _ in range(n_permutations):
        shuffled = y_pred.copy()
        idx = list(range(n))
        rng.shuffle(idx)
        shuffled = shuffled[idx]
        random_accs.append((shuffled == y_true).mean())

    random_accs = np.array(random_accs)
    n_extreme = (random_accs >= real_acc).sum()
    p_value = n_extreme / n_permutations
    return {
        "real_acc": real_acc,
        "random_mean_acc": random_accs.mean(),
        "p_value": p_value,
    }


def main():
    model_root = Path(__file__).resolve().parents[1] / "ml"
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--data",
        required=True,
        help="honest test-сегмент (модель не видела при обучении)",
    )
    ap.add_argument("--model", default=model_root / "lstm_model.pt")
    ap.add_argument("--scaler", default=model_root / "lstm_scaler.json")
    ap.add_argument("--horizon", type=int, default=20)
    ap.add_argument("--window", type=int, default=20)
    args = ap.parse_args()

    with open(args.scaler) as f:
        scaler = json.load(f)
    mean = np.array(scaler["mean"], dtype=np.float32)
    std = np.array(scaler["std"], dtype=np.float32)

    device = torch.device("cpu")
    model = LSTM(n_features=len(mean))
    model.load_state_dict(torch.load(args.model, map_location=device))
    model.eval()

    print(f"Модель загружена: {args.model}")
    print(f"Данные: {args.data} — HONEST OUT-OF-SAMPLE")
    print("Извлечение окон последовательностей...")
    X, y = replay_and_extract_sequences(
        args.data, horizon=args.horizon, window=args.window
    )
    n = len(X)
    print(f"Собрано {n} окон.")

    # -------------------- Полная оценка --------------------
    full_acc = evaluate_accuracy(model, X, y, mean, std, device)
    baseline = y.mean()
    print("\n=== Полная оценка (весь test-сегмент) ===")
    print(f"accuracy: {full_acc:.4f}  baseline (доля класса 1): {baseline:.4f}")

    # -------------------- Chronological fold evaluation --------------------
    print(
        f"\n=== Chronological fold evaluation ({N_CHRONOLOGICAL_FOLDS} последовательных окон) ==="
    )
    fold_size = n // N_CHRONOLOGICAL_FOLDS
    fold_accs = []
    for fold in range(N_CHRONOLOGICAL_FOLDS):
        start = fold * fold_size
        end = n if fold == N_CHRONOLOGICAL_FOLDS - 1 else (fold + 1) * fold_size
        acc = evaluate_accuracy(model, X[start:end], y[start:end], mean, std, device)
        fold_accs.append(acc)
        print(
            f"  Fold {fold}: range=[{start}:{end}] size={end - start} accuracy={acc:.4f}"
        )
    mean_acc = sum(fold_accs) / len(fold_accs)
    positive_folds = sum(1 for a in fold_accs if a > baseline)
    print(f"Средняя accuracy по фолдам: {mean_acc:.4f}")
    print(f"Фолдов лучше baseline: {positive_folds}/{N_CHRONOLOGICAL_FOLDS}")

    # -------------------- Permutation test --------------------
    print(f"\n=== Permutation test ({N_PERMUTATIONS} перестановок) ===")
    y_pred = get_predictions(model, X, mean, std, device)
    pt = permutation_test_lstm(y, y_pred, n_permutations=N_PERMUTATIONS)
    print(f"Реальная accuracy:      {pt['real_acc']:.4f}")
    print(f"Случайная mean accuracy: {pt['random_mean_acc']:.4f}")
    print(f"p-value:                {pt['p_value']:.4f}")
    if pt["p_value"] < 0.05:
        print(
            f"=> Итог: сигнал ЗНАЧИМ (p={pt['p_value']:.4f} < 0.05), подтверждено на honest out-of-sample данных"
        )
    else:
        print(f"=> Итог: сигнал НЕ подтверждён (p={pt['p_value']:.4f} >= 0.05)")


if __name__ == "__main__":
    main()
