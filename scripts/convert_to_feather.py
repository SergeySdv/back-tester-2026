#!/usr/bin/env python3

import argparse
from pathlib import Path

import pandas as pd
import pyarrow as pa


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert Databento-like *.mbo.json files to Feather V2."
    )
    parser.add_argument("source_dir", type=Path)
    return parser.parse_args()


def iter_input_files(source_dir: Path):
    return sorted(source_dir.glob("*.mbo.json"))


def validate_source_dir(source_dir: Path) -> None:
    if not source_dir.exists():
        raise ValueError(f"{source_dir} - does not exist")
    if not source_dir.is_dir():
        raise ValueError(f"{source_dir} - is not a directory")


def convert_file(source: Path) -> Path:
    frame = pd.read_json(
        source,
        lines=True,
        convert_dates=False,
        dtype={"order_id": "string", "price": "string"},
    )
    if "hd" not in frame:
        raise ValueError(f"{source}: missing required field 'hd'")
    header = pd.json_normalize(frame.pop("hd"))
    frame = frame.join(header)

    for name in ("ts_recv", "ts_event"):
        frame[name] = pd.to_datetime(frame[name], utc=True).astype("int64")
    frame["instrument_id"] = pd.to_numeric(frame["instrument_id"]).astype("int64")
    frame["order_id"] = pd.to_numeric(frame["order_id"], errors="coerce").astype(
        "UInt64"
    )
    frame["size"] = pd.to_numeric(frame["size"]).astype("int64")
    if "flags" not in frame:
        frame["flags"] = 0
    frame["flags"] = pd.to_numeric(frame["flags"]).astype("uint32")
    frame["sequence"] = pd.to_numeric(frame["sequence"]).astype("uint64")
    frame["price"] = frame["price"].astype("string")
    frame["action"] = frame["action"].astype("string")
    frame["side"] = frame["side"].astype("string")

    destination = source.with_suffix(".feather")
    table = pa.Table.from_pandas(frame, preserve_index=False)
    with (
        pa.OSFile(str(destination), "wb") as sink,
        pa.ipc.new_file(sink, table.schema) as writer,
    ):
        writer.write_table(table)
    return destination


def main(args: argparse.Namespace) -> None:
    validate_source_dir(args.source_dir)
    for source in iter_input_files(args.source_dir):
        convert_file(source)


if __name__ == "__main__":
    main(parse_args())
