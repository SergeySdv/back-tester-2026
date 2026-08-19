import json
import subprocess
import sys
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]


def _event(
    sequence,
    *,
    action,
    side="N",
    price=None,
    size=None,
    order_id=None,
    flags=128,
    instrument_id=1,
    timestamp_sequence=None,
):
    timestamp_value = sequence if timestamp_sequence is None else timestamp_sequence
    timestamp = f"1970-01-01T00:00:00.{timestamp_value:09d}Z"
    value = {
        "ts_recv": timestamp,
        "hd": {"ts_event": timestamp, "instrument_id": instrument_id},
        "action": action,
        "side": side,
        "flags": flags,
        "sequence": sequence,
    }
    if price is not None:
        value["price"] = str(price)
    if size is not None:
        value["size"] = size
    if order_id is not None:
        value["order_id"] = str(order_id)
    return value


def _write_jsonl(tmp_path, events):
    path = tmp_path / "events.jsonl"
    path.write_text(
        "".join(json.dumps(event) + "\n" for event in events),
        encoding="utf-8",
    )
    return path


def test_research_replay_reconstructs_l3_top_of_book(tmp_path):
    from research_pipeline.l3_replay import iter_book_observations

    path = _write_jsonl(
        tmp_path,
        [
            _event(
                1,
                action="A",
                side="B",
                price=100,
                size=5,
                order_id=1,
                flags=0,
                timestamp_sequence=1,
            ),
            _event(
                2,
                action="A",
                side="B",
                price=99,
                size=7,
                order_id=2,
                flags=0,
                timestamp_sequence=1,
            ),
            _event(
                3,
                action="A",
                side="A",
                price=101,
                size=4,
                order_id=3,
                timestamp_sequence=1,
            ),
            _event(4, action="A", side="B", price=100, size=3, order_id=4),
            _event(5, action="C", order_id=1),
            _event(6, action="T", side="B", price=101, size=1),
            _event(7, action="M", side="B", price=102, size=6, order_id=2),
            _event(8, action="F", size=2, order_id=2),
            _event(9, action="R"),
        ],
    )

    observations = list(iter_book_observations(path))
    assert [
        (
            row.best_bid,
            row.bid_quantity,
            row.best_ask,
            row.ask_quantity,
            row.trades_since_previous,
        )
        for row in observations
    ] == [
        (100.0, 5, 101.0, 4, 0),
        (100.0, 8, 101.0, 4, 0),
        (100.0, 3, 101.0, 4, 0),
        (102.0, 6, 101.0, 4, 1),
        (102.0, 4, 101.0, 4, 0),
    ]


def test_research_replay_rejects_unknown_modify(tmp_path):
    from research_pipeline.l3_replay import ReplayError, iter_book_observations

    path = _write_jsonl(
        tmp_path,
        [_event(1, action="M", side="B", price=100, size=5, order_id=99)],
    )

    with pytest.raises(ReplayError, match="unknown order id 99"):
        list(iter_book_observations(path))


def test_research_replay_rejects_unterminated_atomic_group(tmp_path):
    from research_pipeline.l3_replay import ReplayError, iter_book_observations

    event = _event(1, action="A", side="B", price=100, size=5, order_id=1)
    event["flags"] = 0
    path = _write_jsonl(tmp_path, [event])

    with pytest.raises(ReplayError, match="unterminated atomic market group"):
        list(iter_book_observations(path))


def test_research_replay_normalizes_equivalent_timestamp_fractions(tmp_path):
    from research_pipeline.l3_replay import iter_book_observations

    first = _event(1, action="A", side="B", price=100, size=5, order_id=1)
    first["flags"] = 0
    first["hd"]["ts_event"] = "1970-01-01T00:00:00.1Z"
    second = _event(2, action="A", side="A", price=101, size=5, order_id=2)
    second["hd"]["ts_event"] = "1970-01-01T00:00:00.100Z"

    observations = list(iter_book_observations(_write_jsonl(tmp_path, [first, second])))
    assert len(observations) == 1


def test_strategy_list_works_without_optional_research_dependencies():
    result = subprocess.run(
        [sys.executable, "run_comparison.py", "--list"],
        cwd=REPO_ROOT / "research_pipeline",
        check=False,
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stderr
    assert "dqn" in result.stdout
    assert "lstm" in result.stdout
    assert "unavailable" in result.stdout.lower()
