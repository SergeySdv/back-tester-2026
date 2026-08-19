"""Deterministic L3 reconstruction for offline research feature extraction.

The implementation mirrors ``cmf::market::LimitOrderBook`` for the market
actions used by JSONL replay. It deliberately exposes only top-of-book
observations because the research strategies run with ``book_depth=1``.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from datetime import date
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Iterator


_TIMESTAMP_PATTERN = re.compile(
    r"^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,9}))?Z$"
)
_EPOCH_ORDINAL = date(1970, 1, 1).toordinal()
_NANOSECONDS_PER_SECOND = 1_000_000_000


class ReplayError(ValueError):
    """Raised when an offline replay encounters corrupt market-data state."""


@dataclass(frozen=True)
class BookObservation:
    instrument_id: int
    best_bid: float
    bid_quantity: int
    best_ask: float
    ask_quantity: int
    trades_since_previous: int


@dataclass(frozen=True)
class _Order:
    side: str
    price: Decimal
    quantity: int


class _InstrumentBook:
    def __init__(self) -> None:
        self.orders: dict[int, _Order] = {}
        self.bids: dict[Decimal, int] = {}
        self.asks: dict[Decimal, int] = {}

    def _levels(self, side: str) -> dict[Decimal, int]:
        if side == "B":
            return self.bids
        if side == "A":
            return self.asks
        raise ReplayError(f"invalid historical order side {side!r}")

    def _change_level(self, side: str, price: Decimal, delta: int) -> None:
        levels = self._levels(side)
        updated = levels.get(price, 0) + delta
        if updated < 0:
            raise ReplayError("historical level quantity became negative")
        if updated == 0:
            levels.pop(price, None)
        else:
            levels[price] = updated

    def _remove(self, order_id: int, order: _Order) -> None:
        self._change_level(order.side, order.price, -order.quantity)
        del self.orders[order_id]

    def add(self, order_id: int, side: str, price: Decimal, quantity: int) -> None:
        if quantity <= 0:
            raise ReplayError("historical add quantity must be positive")
        incoming = _Order(side, price, quantity)
        existing = self.orders.get(order_id)
        if existing is not None:
            if existing == incoming:
                return
            raise ReplayError(f"conflicting duplicate historical order id {order_id}")
        self._change_level(side, price, quantity)
        self.orders[order_id] = incoming

    def cancel(self, order_id: int) -> None:
        order = self.orders.get(order_id)
        if order is not None:
            self._remove(order_id, order)

    def modify(
        self, order_id: int, side: str, price: Decimal, quantity: int
    ) -> None:
        old_order = self.orders.get(order_id)
        if old_order is None:
            raise ReplayError(f"modify references unknown order id {order_id}")
        replacement = _Order(side, price, quantity)
        if quantity <= 0:
            raise ReplayError("historical modify quantity must be positive")
        if old_order == replacement:
            return
        self._remove(order_id, old_order)
        self.add(order_id, side, price, quantity)

    def fill(self, order_id: int, quantity: int) -> None:
        order = self.orders.get(order_id)
        if order is None:
            raise ReplayError(f"fill references unknown order id {order_id}")
        if quantity <= 0 or quantity > order.quantity:
            raise ReplayError(f"invalid fill quantity for historical order id {order_id}")
        self._change_level(order.side, order.price, -quantity)
        if quantity == order.quantity:
            del self.orders[order_id]
        else:
            self.orders[order_id] = _Order(
                order.side,
                order.price,
                order.quantity - quantity,
            )

    def clear(self) -> None:
        self.orders.clear()
        self.bids.clear()
        self.asks.clear()

    def top(self) -> tuple[float | None, int, float | None, int]:
        best_bid = max(self.bids, default=None)
        best_ask = min(self.asks, default=None)
        return (
            float(best_bid) if best_bid is not None else None,
            self.bids.get(best_bid, 0),
            float(best_ask) if best_ask is not None else None,
            self.asks.get(best_ask, 0),
        )


def _required_int(event: dict, field: str) -> int:
    value = event.get(field)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ReplayError(f"field {field!r} must be an integer")
    if not -(2**63) <= value < 2**63:
        raise ReplayError(f"field {field!r} is outside int64 range")
    return value


def _required_sequence(event: dict) -> int:
    value = _required_int(event, "sequence")
    if value < 0:
        raise ReplayError("field 'sequence' must be non-negative")
    return value


def _required_order_id(event: dict) -> int:
    value = event.get("order_id")
    if isinstance(value, str):
        if not value or not value.isdecimal():
            raise ReplayError("field 'order_id' must be a decimal string or integer")
        result = int(value)
    elif isinstance(value, int) and not isinstance(value, bool):
        result = value
    else:
        raise ReplayError("field 'order_id' must be a decimal string or integer")
    if not 0 <= result < 2**64:
        raise ReplayError("field 'order_id' is outside uint64 range")
    return result


def _required_side(event: dict) -> str:
    side = event.get("side")
    if side == "B":
        return "B"
    if side in ("A", "S"):
        return "A"
    raise ReplayError("field 'side' has an unsupported value")


def _required_price(event: dict) -> Decimal:
    try:
        return Decimal(str(event["price"]))
    except (KeyError, TypeError, ValueError, InvalidOperation) as error:
        raise ReplayError("field 'price' must be numeric") from error


def _timestamp_ns(value: object) -> int:
    match = _TIMESTAMP_PATTERN.fullmatch(str(value))
    if match is None:
        raise ReplayError("timestamp must be UTC ISO-8601 ending in Z")
    year, month, day, hour, minute, second = map(int, match.groups()[:6])
    if hour > 23 or minute > 59 or second > 59:
        raise ReplayError("timestamp contains an out-of-range field")
    try:
        days = date(year, month, day).toordinal() - _EPOCH_ORDINAL
    except ValueError as error:
        raise ReplayError("timestamp contains an out-of-range field") from error
    fraction = (match.group(7) or "").ljust(9, "0")
    fractional_ns = int(fraction) if fraction else 0
    timestamp = (
        (days * 86_400 + hour * 3_600 + minute * 60 + second)
        * _NANOSECONDS_PER_SECOND
        + fractional_ns
    )
    if not -(2**63) <= timestamp < 2**63:
        raise ReplayError("timestamp is outside int64 nanosecond range")
    return timestamp


def iter_book_observations(path: str | Path) -> Iterator[BookObservation]:
    """Yield top-of-book changes after complete source groups.

    Cancels, modifies, fills, clears, same-price aggregation, and F_LAST group
    boundaries follow the native runtime. Unknown cancels are tolerated so a
    ranged replay can begin inside an order lifetime; unknown modifies/fills
    fail fast as corrupted state.
    """

    books: dict[int, _InstrumentBook] = {}
    last_published: dict[int, tuple[float | None, int, float | None, int]] = {}
    trades_since_previous: dict[int, int] = {}
    touched: dict[int, None] = {}
    previous_sequence: int | None = None
    previous_timestamp: int | None = None
    group_instrument: int | None = None
    group_timestamp: int | None = None

    def publish() -> Iterator[BookObservation]:
        for instrument_id in touched:
            book = books[instrument_id]
            current = book.top()
            if last_published.get(instrument_id) == current:
                continue
            last_published[instrument_id] = current
            best_bid, bid_quantity, best_ask, ask_quantity = current
            if best_bid is None or best_ask is None:
                continue
            yield BookObservation(
                instrument_id=instrument_id,
                best_bid=best_bid,
                bid_quantity=bid_quantity,
                best_ask=best_ask,
                ask_quantity=ask_quantity,
                trades_since_previous=trades_since_previous.get(instrument_id, 0),
            )
            trades_since_previous[instrument_id] = 0
        touched.clear()

    source_path = Path(path)
    with source_path.open(encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            try:
                event = json.loads(line)
                header = event["hd"]
                instrument_id = _required_int(header, "instrument_id")
                _timestamp_ns(event["ts_recv"])
                timestamp = _timestamp_ns(event["hd"]["ts_event"])
                sequence = _required_sequence(event)
                if previous_sequence is not None and sequence <= previous_sequence:
                    raise ReplayError("source sequence did not increase")
                if previous_timestamp is not None and timestamp < previous_timestamp:
                    raise ReplayError("exchange timestamp regressed")
                if group_instrument is None:
                    group_instrument = instrument_id
                    group_timestamp = timestamp
                elif instrument_id != group_instrument or timestamp != group_timestamp:
                    raise ReplayError(
                        "atomic market group changed instrument or exchange timestamp"
                    )
                action = event["action"]
                book = books.setdefault(instrument_id, _InstrumentBook())
                trades_since_previous.setdefault(instrument_id, 0)

                if action == "T":
                    _required_side(event)
                    _required_price(event)
                    if _required_int(event, "size") <= 0:
                        raise ReplayError("field 'size' must be positive")
                    trades_since_previous[instrument_id] += 1
                elif action == "A":
                    book.add(
                        _required_order_id(event),
                        _required_side(event),
                        _required_price(event),
                        _required_int(event, "size"),
                    )
                    touched[instrument_id] = None
                elif action == "C":
                    book.cancel(_required_order_id(event))
                    touched[instrument_id] = None
                elif action == "M":
                    book.modify(
                        _required_order_id(event),
                        _required_side(event),
                        _required_price(event),
                        _required_int(event, "size"),
                    )
                    touched[instrument_id] = None
                elif action == "F":
                    book.fill(
                        _required_order_id(event),
                        _required_int(event, "size"),
                    )
                    touched[instrument_id] = None
                elif action == "R":
                    book.clear()
                    touched[instrument_id] = None
                else:
                    raise ReplayError(f"unsupported market action {action!r}")

                flags = _required_int(event, "flags") if "flags" in event else 0
                if not 0 <= flags < 2**32:
                    raise ReplayError("field 'flags' is outside uint32 range")
                previous_sequence = sequence
                previous_timestamp = timestamp
                if flags & 128:
                    yield from publish()
                    group_instrument = None
                    group_timestamp = None
            except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
                if isinstance(error, ReplayError):
                    message = str(error)
                else:
                    message = str(error)
                raise ReplayError(
                    f"{source_path}:{line_number}: {message}"
                ) from error

    if group_instrument is not None:
        raise ReplayError(f"{source_path}: unterminated atomic market group")
