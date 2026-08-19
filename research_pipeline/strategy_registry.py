"""Lazy, self-describing registry for synthetic research strategies."""

from __future__ import annotations

import importlib
import importlib.util
from dataclasses import dataclass, field
from pathlib import Path


RESEARCH_ROOT = Path(__file__).resolve().parent
MODEL_ROOT = RESEARCH_ROOT / "ml"


@dataclass(frozen=True)
class StrategySpec:
    module: str
    class_name: str
    kwargs: dict = field(default_factory=dict)
    required_modules: tuple[str, ...] = ()
    required_artifacts: tuple[str, ...] = ()

    def unavailable_reason(self) -> str | None:
        missing_modules = [
            name
            for name in self.required_modules
            if importlib.util.find_spec(name) is None
        ]
        missing_artifacts = [
            name for name in self.required_artifacts if not (MODEL_ROOT / name).is_file()
        ]
        reasons = []
        if missing_modules:
            reasons.append(f"missing modules: {', '.join(missing_modules)}")
        if missing_artifacts:
            reasons.append(f"missing artifacts: {', '.join(missing_artifacts)}")
        return "; ".join(reasons) or None

    def build(self, instrument_ids):
        reason = self.unavailable_reason()
        if reason is not None:
            raise RuntimeError(reason)
        strategy_class = getattr(importlib.import_module(self.module), self.class_name)
        resolved_kwargs = {
            key: str(MODEL_ROOT / value.removeprefix("model://"))
            if isinstance(value, str) and value.startswith("model://")
            else value
            for key, value in self.kwargs.items()
        }
        return strategy_class(instrument_ids, **resolved_kwargs)


STRATEGIES = {
    "imbalance": StrategySpec(
        "strategies.rule_based.imbalance",
        "ImbalanceStrategy",
        {"threshold": 0.3, "order_size": 1},
    ),
    "momentum": StrategySpec(
        "strategies.rule_based.momentum",
        "MomentumStrategy",
        {"lookback": 5, "order_size": 1},
    ),
    "mean_reversion": StrategySpec(
        "strategies.rule_based.mean_reversion",
        "MeanReversionStrategy",
        {"lookback": 10, "threshold_ticks": 2, "order_size": 1},
    ),
    "imbalance_confirmed": StrategySpec(
        "strategies.rule_based.imbalance_confirmed",
        "ImbalanceConfirmedStrategy",
        {
            "threshold": 0.3,
            "confirmation_steps": 3,
            "min_hold_updates": 15,
            "order_size": 1,
        },
    ),
    "ml_logistic": StrategySpec(
        "strategies.ml.ml_logistic",
        "MLLogisticStrategy",
        {
            "model_path": "model://logistic_model.json",
            "prob_threshold": 0.52,
            "confirmation_steps": 3,
            "min_hold_updates": 15,
            "order_size": 1,
        },
        required_artifacts=("logistic_model.json",),
    ),
    "lightgbm": StrategySpec(
        "strategies.ml.lightgbm_strategy",
        "LightGBMStrategy",
        {
            "model_path": "model://lightgbm_model_7feat.txt",
            "prob_threshold": 0.52,
            "confirmation_steps": 3,
            "min_hold_updates": 15,
            "order_size": 1,
        },
        required_modules=("lightgbm",),
        required_artifacts=("lightgbm_model_7feat.txt",),
    ),
    "mlp": StrategySpec(
        "strategies.neural.mlp_strategy",
        "MLPStrategy",
        {
            "model_path": "model://mlp_model.pt",
            "scaler_path": "model://mlp_scaler.json",
            "prob_threshold": 0.52,
            "confirmation_steps": 3,
            "min_hold_updates": 15,
            "order_size": 1,
        },
        required_modules=("torch",),
        required_artifacts=("mlp_model.pt", "mlp_scaler.json"),
    ),
    "tcn": StrategySpec(
        "strategies.neural.tcn_strategy",
        "TCNStrategy",
        {
            "model_path": "model://tcn_model.pt",
            "scaler_path": "model://tcn_scaler.json",
            "prob_threshold": 0.52,
            "confirmation_steps": 3,
            "min_hold_updates": 15,
            "order_size": 1,
        },
        required_modules=("torch",),
        required_artifacts=("tcn_model.pt", "tcn_scaler.json"),
    ),
    "ensemble_voting": StrategySpec(
        "strategies.meta.ensemble_strategy",
        "VotingEnsembleStrategy",
        {
            "logreg_path": "model://logistic_model.json",
            "lgb_path": "model://lightgbm_model_7feat.txt",
            "lgb_meta_path": "model://lightgbm_meta.json",
            "mlp_model_path": "model://mlp_model.pt",
            "mlp_scaler_path": "model://mlp_scaler.json",
            "tcn_model_path": "model://tcn_model.pt",
            "tcn_scaler_path": "model://tcn_scaler.json",
            "prob_threshold": 0.52,
            "confirmation_steps": 3,
            "min_hold_updates": 15,
            "order_size": 1,
        },
        required_modules=("lightgbm", "torch"),
        required_artifacts=(
            "logistic_model.json",
            "lightgbm_model_7feat.txt",
            "lightgbm_meta.json",
            "mlp_model.pt",
            "mlp_scaler.json",
            "tcn_model.pt",
            "tcn_scaler.json",
        ),
    ),
    "dqn": StrategySpec(
        "strategies.rl.dqn_strategy",
        "DQNStrategy",
        {
            "model_path": "model://dqn_model_traintest.pt",
            "meta_path": "model://dqn_meta_traintest.json",
            "order_size": 1,
        },
        required_modules=("torch",),
        required_artifacts=("dqn_model_traintest.pt", "dqn_meta_traintest.json"),
    ),
    "lstm": StrategySpec(
        "strategies.neural.lstm_strategy",
        "LSTMStrategy",
        {
            "model_path": "model://lstm_model.pt",
            "scaler_path": "model://lstm_scaler.json",
            "prob_threshold": 0.52,
            "confirmation_steps": 3,
            "min_hold_updates": 15,
            "order_size": 1,
        },
        required_modules=("torch",),
        required_artifacts=("lstm_model.pt", "lstm_scaler.json"),
    ),
}


def available_strategy_names():
    return [name for name, spec in STRATEGIES.items() if spec.unavailable_reason() is None]


def build_strategy(name, instrument_ids):
    return STRATEGIES[name].build(instrument_ids)
