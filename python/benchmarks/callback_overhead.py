"""Cost of one strategy callback, split into the parts you can act on.

Three figures are taken over the same prebuilt payload:

* ``native``  the same loop dispatched through the C++ Strategy interface,
  never crossing into Python;
* ``gil``     acquiring and releasing the GIL and nothing else;
* ``python``  the full path: GIL, pybind dispatch, and a no-op Python method.

``python - native`` is the price of the boundary. Reporting only the Python
number leaves that price mixed up with work the engine would do anyway.

Depth is varied because the payload is handed over as spans: if a binding ever
starts copying the levels into Python lists, the top-15 figure moves away from
the top-1 figure and that shows up here first.

Latency is reported as percentiles. A mean is swallowed by the tail, which is
the part that matters for a callback on a hot path.
"""

import platform
import statistics

from back_tester import Strategy
from back_tester._backtester import (
    _benchmark_book_callbacks,
    _benchmark_gil_cycles,
    _benchmark_native_callbacks,
)


CALLBACKS_PER_SAMPLE = 100_000
WARMUP_SAMPLES = 3
MEASURED_SAMPLES = 10


class NoOp(Strategy):
    def on_book_update(self, update):
        pass


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[int(fraction * (len(ordered) - 1))]


def per_callback(values):
    return [value / CALLBACKS_PER_SAMPLE for value in values]


def report(label, values):
    normalized = per_callback(values)
    print(
        f"  {label:<8} min={min(normalized):.1f} "
        f"p50={percentile(normalized, 0.50):.1f} "
        f"p95={percentile(normalized, 0.95):.1f} "
        f"p99={percentile(normalized, 0.99):.1f} "
        f"mean={statistics.mean(normalized):.1f} ns/callback"
    )
    return statistics.mean(normalized)


def measure(callable_):
    for _ in range(WARMUP_SAMPLES):
        callable_()
    return [callable_() for _ in range(MEASURED_SAMPLES)]


def sample(depth):
    strategy = NoOp()

    python_values = measure(
        lambda: _benchmark_book_callbacks(strategy, depth, CALLBACKS_PER_SAMPLE)
    )
    native_values = measure(
        lambda: _benchmark_native_callbacks(depth, CALLBACKS_PER_SAMPLE)[0]
    )
    gil_values = measure(lambda: _benchmark_gil_cycles(CALLBACKS_PER_SAMPLE))

    print(f"top-{depth}:")
    python_mean = report("python", python_values)
    native_mean = report("native", native_values)
    gil_mean = report("gil", gil_values)
    boundary = python_mean - native_mean
    print(
        f"  boundary cost = python - native = {boundary:.1f} ns/callback "
        f"(of which GIL ~{gil_mean:.1f})"
    )
    return boundary


if __name__ == "__main__":
    print(
        f"python={platform.python_version()} "
        f"implementation={platform.python_implementation()} "
        f"os={platform.system()}-{platform.release()} arch={platform.machine()}"
    )
    print(
        f"warmup_samples={WARMUP_SAMPLES} measured_samples={MEASURED_SAMPLES} "
        f"callbacks_per_sample={CALLBACKS_PER_SAMPLE}"
    )
    print(
        "timed_region=prebuilt_payload+GIL_window+pybind_dispatch+no-op_callback; "
        "excluded=parsing,startup,logging,DataFrame"
    )
    shallow = sample(1)
    deep = sample(15)
    print(
        f"depth sensitivity: top-15 costs {deep - shallow:+.1f} ns/callback "
        "more than top-1; a large positive number means the levels are being "
        "copied rather than passed as spans"
    )
