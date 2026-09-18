"""Measure the Fase 1 REST pipeline without asserting an SLO.

This small harness reports observed nanoseconds only and keeps p50/p95/p99 separate for
each concurrency profile. With ``--url`` it measures real HTTP; without a URL HTTP is marked
unavailable. The Rust companion measures decode/admission/queue/compute/encode in-process.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Iterable


def payload() -> dict:
    return {
        "context": {
            "schema": "quant.context/v1", "context_id": "bench", "revision": 0,
            "engine": {}, "markets": {"m": {"r0": 0.02}},
            "models": {"hw": {"a": 0.1, "b": 0.03, "sigma": 0.01}},
            "products": {"irs": {"notional": 1_000_000, "fixed_rate": 0.02,
                                    "payment_times": [1.0, 2.0], "accruals": [1.0, 1.0]}},
            "portfolios": {}, "runs": [],
        },
        "operation_id": "overhead-1",
        "market": {"id": "m", "hash": "", "kind": "market", "version": 1},
        "model": {"id": "hw", "hash": "", "kind": "model", "version": 1},
        "products": [{"id": "irs", "hash": "", "kind": "product", "version": 1}],
        "measures": [{"name": "PV", "params": {}}],
        "pricing": {}, "execution": {}, "output": {},
    }


def percentile(samples: Iterable[int], probability: float) -> int | None:
    ordered = sorted(samples)
    if not ordered:
        return None
    index = min(len(ordered) - 1, max(0, math.ceil(len(ordered) * probability) - 1))
    return ordered[index]


def percentiles(samples: list[int]) -> dict[str, int | None]:
    return {name: percentile(samples, probability) for name, probability in
            (("p50", 0.50), ("p95", 0.95), ("p99", 0.99))}


def saturation_concurrency() -> int:
    return max(16, (os.cpu_count() or 1) * 2)


def run_profile(url: str | None, iterations: int, concurrency: int) -> dict:
    stage_samples: dict[str, list[int]] = {"encode": [], "decode": []}
    if url:
        stage_samples["http_roundtrip"] = []
    errors = 0

    def one(_: int) -> tuple[dict[str, int], str | None]:
        encoded_start = time.perf_counter_ns()
        request_wire = json.dumps(payload(), separators=(",", ":")).encode()
        encoded = time.perf_counter_ns() - encoded_start
        decoded_start = time.perf_counter_ns()
        json.loads(request_wire)
        decoded = time.perf_counter_ns() - decoded_start
        result = {"encode": encoded, "decode": decoded}
        if url:
            request = urllib.request.Request(url, request_wire,
                                              {"content-type": "application/json"}, method="POST")
            http_start = time.perf_counter_ns()
            try:
                with urllib.request.urlopen(request) as response:
                    response.read()
            except Exception as error:  # retain successful local stage samples
                return result, str(error)
            result["http_roundtrip"] = time.perf_counter_ns() - http_start
        return result, None

    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        futures = [pool.submit(one, index) for index in range(iterations)]
        for future in as_completed(futures):
            result, error = future.result()
            if error:
                errors += 1
            for stage, elapsed in result.items():
                stage_samples[stage].append(elapsed)
    return {
        "concurrency": concurrency, "iterations": iterations,
        "completed": iterations - errors, "errors": errors,
        "percentiles_ns": {stage: percentiles(samples)
                           for stage, samples in stage_samples.items()},
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", help="running quant-api /v1/prices endpoint")
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--profiles", default="1,8,saturation",
                        help="comma-separated concurrency profiles")
    args = parser.parse_args()
    if args.iterations <= 0:
        parser.error("--iterations must be positive")
    profiles: dict[str, dict] = {}
    for profile in (item.strip() for item in args.profiles.split(",")):
        if profile == "saturation":
            concurrency = saturation_concurrency()
        else:
            try:
                concurrency = int(profile)
            except ValueError as error:
                parser.error(f"invalid profile {profile!r}: {error}")
            if concurrency <= 0:
                parser.error("profile concurrency must be positive")
        profiles[profile] = run_profile(args.url, args.iterations, concurrency)
    sample_wire = json.dumps(payload(), separators=(",", ":")).encode()
    report = {
        "schema": "quant.rest-overhead/v1", "payload_bytes": len(sample_wire),
        "percentile_definition": "nearest rank over completed samples; nanoseconds",
        "profiles": profiles,
        "unavailable_stages": [] if args.url else ["http_roundtrip", "queue", "compute"],
        "notes": [
            "No threshold or SLO is inferred from these observations.",
            "queue/compute require the in-process Rust companion; HTTP is aggregate from the client perspective.",
        ],
    }
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
