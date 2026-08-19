"""Обучение LSTM (PyTorch) на окнах последовательностей — та же архитектура
входа (window=20, 5 raw-фичей), что и у TCN, для честного сравнения.
Запуск (WSL, CPU): uv run python3 train_lstm.py"""

import json
import time
from pathlib import Path
import numpy as np
import torch
import torch.nn as nn
from strategies.neural.models import LSTM

from .feature_extraction_lstm import replay_and_extract_sequences

REPO_ROOT = Path(__file__).resolve().parents[2]
DATA_PATH = REPO_ROOT / "synthetic_signal_slow_train.jsonl"
MODEL_PATH = REPO_ROOT / "research_pipeline/ml/lstm_model.pt"
SCALER_PATH = REPO_ROOT / "research_pipeline/ml/lstm_scaler.json"

WINDOW = 20
HORIZON = 20
TRAIN_FRAC = 0.6
VAL_FRAC = 0.2
EMBARGO_ROWS = 200
EPOCHS = 8
BATCH_SIZE = 2048
LR = 1e-3


def split_with_embargo(n, train_frac, val_frac, embargo):
    train_end = int(n * train_frac)
    val_start = train_end + embargo
    val_end = val_start + int(n * val_frac)
    test_start = val_end + embargo
    train_idx = np.arange(0, train_end)
    val_idx = np.arange(val_start, min(val_end, n))
    test_idx = np.arange(test_start, n)
    return train_idx, val_idx, test_idx


def evaluate(model, X, y, mean, std, device, batch_size=8192):
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


def main():
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"Device: {device}")
    print("Извлечение окон последовательностей из", DATA_PATH, "...")
    t0 = time.time()
    X, y = replay_and_extract_sequences(DATA_PATH, horizon=HORIZON, window=WINDOW)
    print(f"Готово за {time.time() - t0:.1f}s. X.shape={X.shape}, y.shape={y.shape}")

    n = len(X)
    train_idx, val_idx, test_idx = split_with_embargo(
        n, TRAIN_FRAC, VAL_FRAC, EMBARGO_ROWS
    )
    print(f"train={len(train_idx)}  val={len(val_idx)}  test={len(test_idx)}")

    X_train, y_train = X[train_idx], y[train_idx]
    X_val, y_val = X[val_idx], y[val_idx]
    X_test, y_test = X[test_idx], y[test_idx]

    mean = X_train.reshape(-1, X_train.shape[-1]).mean(axis=0)
    std = X_train.reshape(-1, X_train.shape[-1]).std(axis=0)
    std[std == 0] = 1.0

    model = LSTM(n_features=X.shape[-1]).to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=LR)
    criterion = nn.BCEWithLogitsLoss()

    n_train = len(X_train)
    print("Обучение LSTM...")
    for epoch in range(EPOCHS):
        model.train()
        perm = np.random.permutation(n_train)
        total_loss = 0.0
        t_epoch = time.time()
        for i in range(0, n_train, BATCH_SIZE):
            idx = perm[i : i + BATCH_SIZE]
            xb = (X_train[idx] - mean) / std
            xb = torch.tensor(xb, dtype=torch.float32, device=device)
            yb = torch.tensor(y_train[idx], dtype=torch.float32, device=device)

            optimizer.zero_grad()
            logits = model(xb)
            loss = criterion(logits, yb)
            loss.backward()
            optimizer.step()
            total_loss += loss.item() * len(idx)

        avg_loss = total_loss / n_train
        val_acc = evaluate(model, X_val, y_val, mean, std, device)
        print(
            f"  Epoch {epoch + 1}/{EPOCHS}: loss={avg_loss:.4f} val_acc={val_acc:.4f} ({time.time() - t_epoch:.1f}s)"
        )

    train_acc = evaluate(model, X_train, y_train, mean, std, device)
    val_acc = evaluate(model, X_val, y_val, mean, std, device)
    test_acc = evaluate(model, X_test, y_test, mean, std, device)

    print(f"\nTrain accuracy: {train_acc:.4f}")
    print(f"Val accuracy:   {val_acc:.4f}")
    print(f"Test accuracy:  {test_acc:.4f}")
    print(f"Baseline (доля класса 1 в test): {y_test.mean():.4f}")

    torch.save(model.state_dict(), MODEL_PATH)
    with open(SCALER_PATH, "w") as f:
        json.dump(
            {
                "mean": mean.tolist(),
                "std": std.tolist(),
                "window": WINDOW,
                "train_acc": train_acc,
                "val_acc": val_acc,
                "test_acc": test_acc,
            },
            f,
            indent=2,
        )

    print(f"\nМодель сохранена в {MODEL_PATH}")
    print(f"Scaler сохранён в {SCALER_PATH}")


if __name__ == "__main__":
    main()
