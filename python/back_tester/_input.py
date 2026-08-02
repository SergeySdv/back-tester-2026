from pathlib import Path

import numpy as np
import pyarrow as pa

from . import _backtester as _native


_REQUIRED_COLUMNS = (
    "ts_recv",
    "ts_event",
    "instrument_id",
    "order_id",
    "action",
    "side",
    "price",
    "size",
    "flags",
    "sequence",
)


def _timestamp_ns(column: pa.ChunkedArray, name: str) -> np.ndarray:
    if column.null_count:
        raise ValueError(f"column '{name}' contains null values")
    try:
        if pa.types.is_integer(column.type):
            values = column.cast(pa.int64())
        else:
            values = column.cast(pa.timestamp("ns", tz="UTC")).cast(pa.int64())
    except (pa.ArrowInvalid, pa.ArrowNotImplementedError) as error:
        raise ValueError(
            f"column '{name}' must contain nanosecond timestamps"
        ) from error
    return np.ascontiguousarray(values.to_numpy(zero_copy_only=False), dtype=np.int64)


def _integers(
    column: pa.ChunkedArray, name: str, dtype: np.dtype, *, nullable: bool = False
):
    if column.null_count and not nullable:
        raise ValueError(f"column '{name}' contains null values")
    if not pa.types.is_integer(column.type):
        raise ValueError(f"column '{name}' must contain integers")
    if nullable:
        return column.to_pylist()
    try:
        arrow_type = pa.from_numpy_dtype(dtype)
        values = column.cast(arrow_type, safe=True)
        return np.ascontiguousarray(values.to_numpy(zero_copy_only=False), dtype=dtype)
    except (TypeError, ValueError, pa.ArrowInvalid) as error:
        raise ValueError(f"column '{name}' must contain integers") from error


def _strings(column: pa.ChunkedArray, name: str, *, nullable: bool = False):
    if column.null_count and not nullable:
        raise ValueError(f"column '{name}' contains null values")
    values = column.to_pylist()
    if any(value is not None and not isinstance(value, str) for value in values):
        raise ValueError(f"column '{name}' must contain strings")
    return values


def _read_feather(path: Path):
    try:
        with pa.memory_map(str(path), "r") as source:
            table = pa.ipc.open_file(source).read_all()
    except (OSError, pa.ArrowException) as error:
        raise ValueError(f"cannot read Feather source '{path}': {error}") from error

    missing = sorted(set(_REQUIRED_COLUMNS) - set(table.column_names))
    if missing:
        raise ValueError(f"missing required columns: {', '.join(missing)}")
    if table.num_rows == 0:
        raise ValueError("Feather source contains no rows")

    return {
        "ts_recv": _timestamp_ns(table["ts_recv"], "ts_recv"),
        "ts_event": _timestamp_ns(table["ts_event"], "ts_event"),
        "instrument_id": _integers(
            table["instrument_id"], "instrument_id", np.dtype("int64")
        ),
        "order_id": _integers(
            table["order_id"], "order_id", np.dtype("uint64"), nullable=True
        ),
        "action": _strings(table["action"], "action"),
        "side": _strings(table["side"], "side", nullable=True),
        "price": _strings(table["price"], "price", nullable=True),
        "size": _integers(table["size"], "size", np.dtype("int64")),
        "flags": _integers(table["flags"], "flags", np.dtype("uint32")),
        "sequence": _integers(table["sequence"], "sequence", np.dtype("uint64")),
    }


def run(strategy, data_path, date_range, config=None, instruments=None):
    path = Path(data_path)
    suffix = path.suffix.lower()
    if suffix in {".json", ".jsonl"}:
        return _native.run(strategy, str(path), date_range, config, instruments)
    if suffix == ".feather":
        return _native._run_events(
            strategy,
            _read_feather(path),
            str(path),
            date_range,
            config,
            instruments,
        )
    raise ValueError("supported formats are .jsonl, .json, and .feather")
