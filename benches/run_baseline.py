"""Run the Phase 0 Rust baseline and validate its versioned result envelope.

The default ``smoke`` profile is intentionally bounded for local/CI use.  ``full`` is opt-in
and may allocate a large vector; it does not materialise the L dataset cartesian product.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import platform
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(profile: str) -> dict:
    command = [
        "cargo",
        "run",
        "--release",
        "--manifest-path",
        str(ROOT / "rust" / "Cargo.toml"),
        "--package",
        "engine-core",
        "--example",
        "phase0_baseline",
        "--",
        "--profile",
        profile,
    ]
    completed = subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            "El harness Rust debe emitir solo JSON por stdout; "
            f"salida recibida: {completed.stdout[-500:]}"
        ) from exc
    validate(result, profile)
    return result


def validate(result: dict, profile: str) -> None:
    required = {"schema", "harness", "profile", "status", "platform", "benchmarks"}
    missing = required.difference(result)
    if missing:
        raise ValueError(f"resultado incompleto, faltan: {sorted(missing)}")
    if result["schema"] != "quant.baseline-result/v1":
        raise ValueError(f"schema de resultado no soportado: {result['schema']!r}")
    if result["profile"] != profile or result["status"] != "measured":
        raise ValueError("el perfil/status del resultado no coincide con la ejecución")
    names = {case.get("name") for case in result["benchmarks"]}
    expected = {"bridge_copy", "pricing_batch", "monte_carlo_baseline"}
    if not expected.issubset(names):
        raise ValueError(f"faltan benchmarks requeridos: {sorted(expected - names)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("smoke", "full"), default="smoke")
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        help="optional JSON output path (relative paths are resolved from the repository root)",
    )
    args = parser.parse_args()
    result = run(args.profile)
    result["runner_environment"] = {
        "python": platform.python_version(),
        "platform": platform.platform(),
        "processor": platform.processor(),
        "git_commit": subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, check=True, capture_output=True, text=True
        ).stdout.strip(),
    }
    encoded = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        output = args.output if args.output.is_absolute() else ROOT / args.output
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(encoded, encoding="utf-8")
    else:
        sys.stdout.write(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
