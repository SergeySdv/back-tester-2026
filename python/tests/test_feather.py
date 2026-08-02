import json
import subprocess
import sys
from pathlib import Path

import pyarrow as pa
import pytest
from pandas.testing import assert_frame_equal, assert_series_equal

import back_tester as bt


def _row(sequence=1):
    timestamp = "1970-01-01T00:00:00.000000100Z"
    return {
        "ts_recv": timestamp,
        "ts_event": timestamp,
        "instrument_id": 1,
        "order_id": sequence,
        "action": "A",
        "side": "B",
        "price": "100",
        "size": 10,
        "flags": 128,
        "sequence": sequence,
    }


def _write_feather(tmp_path, rows, name="events.feather"):
    path = tmp_path / name
    table = pa.Table.from_pylist(rows)
    with (
        pa.OSFile(str(path), "wb") as sink,
        pa.ipc.new_file(sink, table.schema) as writer,
    ):
        writer.write_table(table)
    return path


def _write_jsonl(tmp_path, rows, name="events.jsonl"):
    nested = []
    for source in rows:
        value = dict(source)
        value["hd"] = {
            "ts_event": value.pop("ts_event"),
            "instrument_id": value.pop("instrument_id"),
        }
        nested.append(value)
    path = tmp_path / name
    path.write_text(
        "".join(json.dumps(value) + "\n" for value in nested), encoding="utf-8"
    )
    return path


def _run(path):
    return bt.run(bt.Strategy(), str(path), bt.DateRange())


def test_run_accepts_feather_and_discovers_instruments(tmp_path):
    result = _run(_write_feather(tmp_path, [_row()]))
    assert result.fills_df.empty
    assert result.order_log_df.empty


def test_explicit_empty_instruments_remains_invalid(tmp_path):
    path = _write_feather(tmp_path, [_row()])
    with pytest.raises(ValueError, match="at least one instrument"):
        bt.run(bt.Strategy(), str(path), bt.DateRange(), instruments=[])


def test_run_rejects_unknown_input_suffix(tmp_path):
    path = tmp_path / "events.csv"
    path.write_text("not,replay,data\n", encoding="utf-8")
    with pytest.raises(ValueError, match="supported formats.*jsonl.*feather"):
        _run(path)


def test_feather_requires_runtime_columns(tmp_path):
    incomplete = _row()
    del incomplete["sequence"]
    path = _write_feather(tmp_path, [incomplete])
    with pytest.raises(ValueError, match="missing required columns.*sequence"):
        _run(path)


def test_jsonl_path_still_uses_native_runtime(tmp_path):
    result = _run(_write_jsonl(tmp_path, [_row()]))
    assert result.fills_df.empty


def test_feather_matches_jsonl_callbacks_and_results(tmp_path):
    rows = [
        _row(1),
        {
            **_row(2),
            "ts_recv": "1970-01-01T00:00:00.000000200Z",
            "ts_event": "1970-01-01T00:00:00.000000200Z",
            "order_id": 2,
            "side": "A",
            "price": "101",
        },
    ]

    class Capture(bt.Strategy):
        def __init__(self):
            super().__init__()
            self.books = []

        def on_book_update(self, update):
            self.books.append(
                (update.instrument_id, update.exchange_ts_ns, update.sequence)
            )

    json_strategy = Capture()
    feather_strategy = Capture()
    json_result = bt.run(
        json_strategy, str(_write_jsonl(tmp_path, rows)), bt.DateRange()
    )
    feather_result = bt.run(
        feather_strategy, str(_write_feather(tmp_path, rows)), bt.DateRange()
    )

    assert feather_strategy.books == json_strategy.books
    assert_frame_equal(feather_result.fills_df, json_result.fills_df)
    assert_frame_equal(feather_result.order_log_df, json_result.order_log_df)
    assert_series_equal(feather_result.pnl_series, json_result.pnl_series)


def test_feather_rejects_fractional_integer_columns(tmp_path):
    value = _row()
    value["sequence"] = 1.5
    path = _write_feather(tmp_path, [value])
    with pytest.raises(ValueError, match="sequence.*integers"):
        _run(path)


def test_feather_rejects_null_required_values(tmp_path):
    value = _row()
    value["action"] = None
    path = _write_feather(tmp_path, [value])
    with pytest.raises(ValueError, match="action.*null"):
        _run(path)


def test_feather_rejects_null_timestamps(tmp_path):
    value = _row()
    value["ts_event"] = None
    path = _write_feather(tmp_path, [value])
    with pytest.raises(ValueError, match="ts_event.*null"):
        _run(path)


def test_feather_rejects_corrupt_file(tmp_path):
    path = tmp_path / "broken.feather"
    path.write_bytes(b"not-arrow-ipc")
    with pytest.raises(ValueError, match="cannot read Feather source"):
        _run(path)


def test_converter_output_runs_directly(tmp_path):
    source = tmp_path / "sample.mbo.json"
    source.write_text(
        json.dumps(
            {
                "ts_recv": "1970-01-01T00:00:00.000000100Z",
                "hd": {
                    "ts_event": "1970-01-01T00:00:00.000000100Z",
                    "instrument_id": 1,
                },
                "order_id": "1",
                "action": "A",
                "side": "B",
                "price": "100",
                "size": 10,
                "flags": 128,
                "sequence": 1,
            }
        )
        + "\n",
        encoding="utf-8",
    )
    script = Path(__file__).resolve().parents[2] / "scripts/convert_to_feather.py"
    subprocess.run([sys.executable, str(script), str(tmp_path)], check=True)

    result = _run(tmp_path / "sample.mbo.feather")
    assert result.fills_df.empty


def test_converter_defaults_missing_flags(tmp_path):
    value = {
        "ts_recv": "1970-01-01T00:00:00.000000100Z",
        "hd": {
            "ts_event": "1970-01-01T00:00:00.000000100Z",
            "instrument_id": 1,
        },
        "order_id": "1",
        "action": "A",
        "side": "B",
        "price": "100.000000001",
        "size": 10,
        "sequence": 1,
    }
    source = tmp_path / "no_flags.mbo.json"
    source.write_text(json.dumps(value) + "\n", encoding="utf-8")
    script = Path(__file__).resolve().parents[2] / "scripts/convert_to_feather.py"
    subprocess.run([sys.executable, str(script), str(tmp_path)], check=True)

    with pa.memory_map(str(tmp_path / "no_flags.mbo.feather"), "r") as source_file:
        table = pa.ipc.open_file(source_file).read_all()
    assert table["flags"].to_pylist() == [0]
    assert table["price"].to_pylist() == ["100.000000001"]
