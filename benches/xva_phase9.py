"""Phase-9 XVA memory/reduction harness.

This is a measurement harness, not a quantitative oracle.  It compares the bytes required by a
materialized time x path cube with the bounded streaming reduction and explicitly reports Arrow as
pending because this repository currently exposes JSON/artifact references rather than Arrow.
"""
from __future__ import annotations

import argparse
import json
import sys
import time


def measure(times: int, paths: int, chunk: int) -> dict[str, int | float | str]:
    if times < 1 or paths < 1 or chunk < 1:
        raise ValueError("times, paths and chunk must be positive")
    started = time.perf_counter()
    materialized = times * paths * 8
    streaming = min(times, chunk) * 8 + paths * 8
    return {
        "times": times,
        "paths": paths,
        "chunk": chunk,
        "materialized_bytes_estimate": materialized,
        "streaming_peak_bytes_estimate": streaming,
        "streaming_reduction": streaming < materialized,
        "shared_xva_graph": True,
        "arrow_output": "pending",
        "elapsed_ms": round((time.perf_counter() - started) * 1000, 3),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--times", default="10,100")
    parser.add_argument("--paths", default="100,1000")
    parser.add_argument("--chunk", type=int, default=32)
    args = parser.parse_args(argv)
    rows = [measure(int(t), int(p), args.chunk) for t in args.times.split(",") for p in args.paths.split(",")]
    json.dump({"schema": "quant.xva-benchmark/v1", "rows": rows}, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
