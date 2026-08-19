# Options Backtester — research strategy guide

The research utilities are offline, deterministic extensions to the HW4
backtester. They do not make network calls or add services to the runtime.

## Environment and data

From the repository root, install the locked research dependency group:

```bash
uv sync --locked --group research
```

Generate an input stream when one is not already available locally:

```bash
uv run --group research python research_pipeline/generate_synthetic_signal_stream.py
```

Generated JSONL inputs and DQN/LSTM training outputs are intentionally not
tracked. `run_comparison.py --list` reports missing dependencies or artifacts;
`--strategies all` skips unavailable strategies with a warning. Naming an
unavailable strategy explicitly is an error.

## Engine comparison

```bash
uv run --group research python research_pipeline/run_comparison.py --list
uv run --group research python research_pipeline/run_comparison.py \
  --strategies all --data synthetic_signal_large_slow.jsonl
```

Strategy names are `imbalance`, `momentum`, `mean_reversion`,
`imbalance_confirmed`, `ml_logistic`, `lightgbm`, `mlp`, `tcn`,
`ensemble_voting`, `dqn`, and `lstm`.

The DQN and LSTM entries become available after their expected files are
created under `research_pipeline/ml/`:

```bash
uv run --group research python -m research_pipeline.ml.train_dqn_v3_traintest
OMP_NUM_THREADS=2 uv run --group research python \
  -m research_pipeline.lstm_windows.train_lstm
```

The training commands read `synthetic_signal_slow_train.jsonl` from the
repository root and write the exact artifact paths used by the registry.

## Offline validation

The validation programs reconstruct the historical L3 book by order ID,
including adds, cancels, modifies, fills, clears, price-level aggregation, and
atomic `F_LAST` groups. Features are emitted only from complete top-of-book
states and remain separated by `instrument_id`.

```bash
uv run --group research python eval_dqn.py \
  --data synthetic_signal_slow_test.jsonl \
  --model research_pipeline/ml/dqn_model_traintest.pt \
  --meta research_pipeline/ml/dqn_meta_traintest.json

uv run --group research python \
  -m research_pipeline.lstm_windows.validate_lstm \
  --data synthetic_signal_slow_test.jsonl

uv run --group research python validate_ensemble.py \
  --data synthetic_signal_slow_test.jsonl

uv run --group research python validate_lightgbm10.py \
  --data synthetic_signal_slow_test.jsonl
```

These scripts report a fixed model over five consecutive chronological folds;
they do not retrain the model in each fold and therefore are not walk-forward
validation. Their permutation checks shuffle the evaluated predictions while
keeping the transaction schedule fixed.

## File reference

| File | Purpose |
|---|---|
| `research_pipeline/l3_replay.py` | Shared deterministic offline L3 replay |
| `research_pipeline/run_comparison.py` | Lazy strategy registry and engine comparison |
| `research_pipeline/ml/train_dqn_v3_traintest.py` | DQN training |
| `research_pipeline/lstm_windows/train_lstm.py` | LSTM training |
| `eval_dqn.py` | DQN chronological-fold evaluation |
| `validate_ensemble.py` | Ensemble chronological-fold evaluation |
| `validate_lightgbm10.py` | LightGBM comparison |
