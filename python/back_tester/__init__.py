"""Deterministic options back-testing API backed by the native runtime."""

from ._backtester import (
    BacktestConfig,
    BookLevel,
    BookUpdate,
    DateRange,
    Fill,
    InstrumentMeta,
    LiquiditySource,
    OpenOrder,
    OrderState,
    Position,
    Reject,
    RejectReason,
    Result,
    Side,
    Strategy,
    Trade,
    version,
)
from ._input import run


class _Backtest:
    """Namespace preserving the documented ``backtest.run(...)`` entry point."""

    run = staticmethod(run)


backtest = _Backtest()

__all__ = [
    "BacktestConfig",
    "BookLevel",
    "BookUpdate",
    "DateRange",
    "Fill",
    "InstrumentMeta",
    "LiquiditySource",
    "OpenOrder",
    "OrderState",
    "Position",
    "Reject",
    "RejectReason",
    "Result",
    "Side",
    "Strategy",
    "Trade",
    "backtest",
    "run",
    "version",
]
