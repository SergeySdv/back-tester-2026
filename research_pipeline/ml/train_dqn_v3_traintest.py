"""DQN v2 — исправленная версия: фичи предвычислены заранее, обучение раз в N шагов,
меньший replay buffer. Быстрый режим (subset), 1 эпизод для начальной проверки."""

import json
import random
import time
import numpy as np
import torch
import torch.nn as nn
from collections import deque

from .feature_extraction import extract_feature_rows

DATA_PATH = "synthetic_signal_slow_train.jsonl"  # 1М событий, быстрый режим
FEATURE_COLS = [
    "imbalance",
    "spread",
    "imbalance_ma_5",
    "imbalance_ma_20",
    "momentum_5",
]
STATE_DIM = len(FEATURE_COLS) + 1  # + текущая позиция

EPISODES = 1
TRAIN_EVERY_N_STEPS = 4  # обучаемся не на каждом шаге, а раз в 4
EPSILON_START = 1.0
EPSILON_END = 0.05
EPSILON_DECAY_STEPS = 1_500_000
GAMMA = 0.99
LR = 1e-3
BATCH_SIZE = 256
REPLAY_CAPACITY = 15_000  # меньше, чем в первой попытке (была 50К)
TARGET_UPDATE_EVERY = 2000
TRANSACTION_COST = 0.5


def precompute_trajectory(path):
    """Один проход по файлу, всё считаем один раз в numpy-массив — без пересчёта в цикле обучения."""
    rows = [row for row in extract_feature_rows(path) if row["instrument_id"] == 1]
    return (
        np.asarray(
            [[row[name] for name in FEATURE_COLS] for row in rows],
            dtype=np.float32,
        ),
        np.asarray([row["mid_price"] for row in rows], dtype=np.float32),
    )


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


def main():
    device = torch.device("cpu")
    print("Предвычисление фичей траектории...")
    t0 = time.time()
    feats_arr, mid_arr = precompute_trajectory(DATA_PATH)
    print(f"Готово за {time.time() - t0:.1f}s. Длина траектории: {len(feats_arr)}")

    q_net = QNet(STATE_DIM).to(device)
    target_net = QNet(STATE_DIM).to(device)
    target_net.load_state_dict(q_net.state_dict())
    optimizer = torch.optim.Adam(q_net.parameters(), lr=LR)

    replay = deque(maxlen=REPLAY_CAPACITY)
    global_step = 0

    t_start = time.time()
    for episode in range(EPISODES):
        position = 0
        cum_reward = 0.0

        for t in range(len(feats_arr) - 1):
            feats = feats_arr[t]
            mid = mid_arr[t]
            state = np.append(feats, position).astype(np.float32)

            epsilon = max(
                EPSILON_END, EPSILON_START - global_step / EPSILON_DECAY_STEPS
            )
            if random.random() < epsilon:
                action = random.randint(0, 2)
            else:
                with torch.no_grad():
                    q = q_net(torch.tensor(state, device=device).unsqueeze(0))
                    action = int(q.argmax(dim=1).item())

            target_position = {0: position, 1: 1, 2: -1}[action]
            next_mid = mid_arr[t + 1]

            reward = position * (next_mid - mid)
            if target_position != position:
                reward -= TRANSACTION_COST
            position = target_position

            next_state = np.append(feats_arr[t + 1], position).astype(np.float32)
            replay.append((state, action, reward, next_state))
            cum_reward += reward
            global_step += 1

            if global_step % TRAIN_EVERY_N_STEPS == 0 and len(replay) >= BATCH_SIZE:
                batch = random.sample(replay, BATCH_SIZE)
                s_b = torch.tensor(np.array([b[0] for b in batch]), device=device)
                a_b = torch.tensor([b[1] for b in batch], device=device)
                r_b = torch.tensor(
                    [b[2] for b in batch], dtype=torch.float32, device=device
                )
                ns_b = torch.tensor(np.array([b[3] for b in batch]), device=device)

                q_values = q_net(s_b).gather(1, a_b.unsqueeze(1)).squeeze(1)
                with torch.no_grad():
                    next_q = target_net(ns_b).max(dim=1)[0]
                    target = r_b + GAMMA * next_q

                loss = nn.functional.mse_loss(q_values, target)
                optimizer.zero_grad()
                loss.backward()
                optimizer.step()

            if global_step % TARGET_UPDATE_EVERY == 0:
                target_net.load_state_dict(q_net.state_dict())

            if global_step % 100_000 == 0:
                print(
                    f"  step {global_step}/{len(feats_arr)}: cum_reward={cum_reward:.2f} "
                    f"epsilon={epsilon:.3f} elapsed={time.time() - t_start:.1f}s"
                )

        print(
            f"Episode {episode + 1}/{EPISODES}: cum_reward={cum_reward:.2f} epsilon={epsilon:.3f}"
        )

    torch.save(q_net.state_dict(), "research_pipeline/ml/dqn_model_traintest.pt")
    with open("research_pipeline/ml/dqn_meta_traintest.json", "w") as f:
        json.dump({"features": FEATURE_COLS, "state_dim": STATE_DIM}, f, indent=2)
    print(
        f"Готово за {time.time() - t_start:.1f}s. Модель сохранена в research_pipeline/ml/dqn_model_traintest.pt"
    )


if __name__ == "__main__":
    main()
