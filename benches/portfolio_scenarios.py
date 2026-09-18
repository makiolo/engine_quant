"""Small deterministic Fase-4 planning harness (no server state required).

The harness models the planner's bounded grouping and reports the quantities that are useful when
comparing provider crossings.  It intentionally does not allocate a trades x scenarios matrix.
"""
from __future__ import annotations

import argparse
import json
import sys
import time


def measure(trades: int, scenarios: int, chunk: int) -> dict[str, int | float]:
    started = time.perf_counter()
    # Stable first-seen product groups; this is the same grouping invariant as quant-engine.
    groups = (trades + 1) // 2, trades // 2
    trade_chunks = sum((size + chunk - 1) // chunk for size in groups if size)
    scenario_chunks = (scenarios + chunk - 1) // chunk
    # A real result has one scalar per pair, but planning holds one scenario chunk at a time.
    # The engine evaluates one scenario and one provider chunk at a time; this must not grow
    # with the scenario count (which would be an accidental Cartesian materialisation).
    peak_result_slots = min(max(1, chunk), max(groups))
    crossings_batch = trade_chunks * scenario_chunks
    crossings_single = trades * scenarios
    return {
        "trades": trades,
        "scenarios": scenarios,
        "chunks": crossings_batch,
        "crossings_batch": crossings_batch,
        "crossings_single": crossings_single,
        "peak_result_slots_estimate": peak_result_slots,
        "bytes_copied_estimate": (peak_result_slots * 8) + (chunk * 8),
        "elapsed_ms": round((time.perf_counter() - started) * 1000, 3),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trades", default="1,10,1000")
    parser.add_argument("--scenarios", default="1,10,1000")
    parser.add_argument("--large", type=int, default=100_000)
    parser.add_argument("--chunk", type=int, default=1024)
    args = parser.parse_args(argv)
    if args.chunk < 1 or args.large < 1:
        parser.error("--chunk and --large must be positive")
    sizes = [int(value) for value in args.trades.split(",") if value]
    scenario_sizes = [int(value) for value in args.scenarios.split(",") if value]
    rows = [measure(size, scenarios, args.chunk) for size in [*sizes, args.large] for scenarios in scenario_sizes]
    json.dump(rows, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
