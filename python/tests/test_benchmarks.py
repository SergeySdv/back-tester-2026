import pytest

from back_tester import Strategy
from back_tester._backtester import (
    _benchmark_book_callbacks,
    _benchmark_gil_cycles,
    _benchmark_native_callbacks,
)


def test_internal_benchmark_helpers_validate_and_execute():
    assert _benchmark_book_callbacks(Strategy(), 1, 1_000) >= 0
    assert _benchmark_book_callbacks(Strategy(), 15, 1_000) >= 0

    with pytest.raises(ValueError, match="depth and iterations must be positive"):
        _benchmark_book_callbacks(Strategy(), 0, 1_000)


def test_native_baseline_reports_timing_and_observed_work():
    elapsed_ns, seen = _benchmark_native_callbacks(15, 1_000)
    assert elapsed_ns >= 0
    # The accumulator proves the loop was not optimized away: 15 bid levels
    # visited on each of 1,000 iterations.
    assert seen == 15 * 1_000

    with pytest.raises(ValueError, match="depth and iterations must be positive"):
        _benchmark_native_callbacks(0, 1_000)


def test_native_baseline_is_cheaper_than_crossing_into_python():
    native_ns, _ = _benchmark_native_callbacks(1, 20_000)
    python_ns = _benchmark_book_callbacks(Strategy(), 1, 20_000)
    assert native_ns < python_ns


def test_gil_cycle_helper_validates_and_executes():
    assert _benchmark_gil_cycles(1_000) >= 0

    with pytest.raises(ValueError, match="iterations must be positive"):
        _benchmark_gil_cycles(0)
