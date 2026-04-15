#!/usr/bin/env python3
"""Focused noop-overhead noise-floor measurement for the dispatch tracer.

Runs sample_app_dispatch many times with and without each stub preloaded,
using high iteration counts to drive the per-call timer below noise, and
reports mean / stddev / min / max / 95% CI per configuration.

Purpose: answer "is stub-loaded hot path truly equivalent to baseline when
no controller has attached?" with enough statistical power to interpret
the sub-10 ns delta.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import List, Dict


BUILD_DIR = Path(os.environ.get("BUILD_DIR", "build")).resolve()
OPTIONS   = ["mmap", "sock", "memfd", "signal"]


@dataclass
class Cell:
    config:      str
    iterations:  int
    runs:        int
    samples_ns:  List[float]

    def summary(self) -> Dict[str, float]:
        s = self.samples_ns
        mean = statistics.mean(s)
        stdev = statistics.stdev(s) if len(s) > 1 else 0.0
        # 95% CI margin for mean under normal assumption
        ci95 = 1.96 * stdev / (len(s) ** 0.5) if len(s) > 1 else 0.0
        return {
            "runs":    len(s),
            "mean_ns": mean,
            "stdev_ns": stdev,
            "ci95_ns": ci95,
            "min_ns":  min(s),
            "max_ns":  max(s),
            "median_ns": statistics.median(s),
        }


def one_run(extra_env: dict, iters: int, timeout: float = 60.0) -> float:
    app = BUILD_DIR / "bin" / "sample_app_dispatch"
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(BUILD_DIR / "lib")
    env.update(extra_env)
    # Empty function: no SIMULATED_WORK_US.
    r = subprocess.run([str(app), str(iters)],
                       env=env, capture_output=True, text=True, timeout=timeout)
    for line in r.stdout.splitlines():
        if "Average time per call:" in line:
            return float(line.split(":")[1].strip().split()[0])
    raise RuntimeError(f"no avg line in output: {r.stdout[-200:]}")


def warmup(extra_env: dict, iters: int) -> None:
    # Warm page cache, branch predictor state, CPU frequency up to steady state.
    for _ in range(2):
        try:
            one_run(extra_env, iters, timeout=30)
        except Exception:
            pass


def run_cell(name: str, extra_env: dict, iters: int, runs: int) -> Cell:
    print(f"  [{name:22s}] warming up ...", flush=True)
    warmup(extra_env, iters)
    samples = []
    for i in range(runs):
        samples.append(one_run(extra_env, iters))
    cell = Cell(config=name, iterations=iters, runs=runs, samples_ns=samples)
    s = cell.summary()
    print(f"  [{name:22s}] n={runs:3d} mean={s['mean_ns']:6.3f}  "
          f"stdev={s['stdev_ns']:5.3f}  ci95=±{s['ci95_ns']:5.3f}  "
          f"range=[{s['min_ns']:5.3f}, {s['max_ns']:5.3f}] ns", flush=True)
    return cell


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--iters", type=int, default=1_000_000,
                   help="iterations per run (default 1,000,000)")
    p.add_argument("--runs",  type=int, default=20,
                   help="runs per config (default 20)")
    p.add_argument("--output", default="report/dispatch_noise.json")
    args = p.parse_args()

    app = BUILD_DIR / "bin" / "sample_app_dispatch"
    if not app.exists():
        print(f"ERROR: {app} not found — build first"); return 1
    for opt in OPTIONS:
        stub = BUILD_DIR / "lib" / f"librocp_stub_{opt}.so"
        if not stub.exists():
            print(f"ERROR: {stub} not found — build first"); return 1

    print(f"Dispatch Tracer — noop-overhead noise-floor measurement")
    print(f"  iterations / run : {args.iters}")
    print(f"  runs / config    : {args.runs}")
    print(f"  configs          : baseline + {len(OPTIONS)} stubs (no controller attach)")
    print()

    cells: List[Cell] = []
    cells.append(run_cell("baseline (no stub)", {}, args.iters, args.runs))
    for opt in OPTIONS:
        stub = BUILD_DIR / "lib" / f"librocp_stub_{opt}.so"
        cells.append(run_cell(f"stub {opt}",
                              {"LD_PRELOAD": str(stub)},
                              args.iters, args.runs))

    # Shim design (alternative architecture): no LD_PRELOAD, but mock_register
    # dlopens libshim_mock.so which installs "atomic-load + branch + tail-call"
    # wrappers over every table entry. profiler_functor stays NULL, so each
    # call pays exactly the fast-path cost the shim design claims should be
    # noise-equivalent to baseline.
    shim = BUILD_DIR / "lib" / "libshim_mock.so"
    if shim.exists():
        cells.append(run_cell("shim (profiler=NULL)",
                              {"MOCK_REGISTER_LIB": str(shim)},
                              args.iters, args.runs))

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "iterations_per_run": args.iters,
        "runs_per_cell":      args.runs,
        "cells": [
            {**asdict(c), **{"summary": c.summary()}} for c in cells
        ],
    }
    with open(out, "w") as f:
        json.dump(payload, f, indent=2)

    # Compare each stub against baseline
    baseline = cells[0]
    bsum = baseline.summary()
    print()
    print("=" * 78)
    print(f"{'Config':<24} {'Mean (ns)':>10} {'Stdev':>8} {'95% CI':>10}  "
          f"{'Δ vs baseline':>16}")
    print("-" * 78)
    print(f"{baseline.config:<24} {bsum['mean_ns']:10.3f} {bsum['stdev_ns']:8.3f} "
          f"±{bsum['ci95_ns']:8.3f}  {'—':>16}")
    for c in cells[1:]:
        s = c.summary()
        delta = s["mean_ns"] - bsum["mean_ns"]
        # Is the delta statistically distinguishable from zero at 95%?
        margin = 1.96 * ((s["stdev_ns"]**2 / s["runs"]) +
                         (bsum["stdev_ns"]**2 / bsum["runs"])) ** 0.5
        sig = "" if abs(delta) <= margin else " *"
        print(f"{c.config:<24} {s['mean_ns']:10.3f} {s['stdev_ns']:8.3f} "
              f"±{s['ci95_ns']:8.3f}  {delta:+10.3f}±{margin:5.3f}{sig}")
    print("=" * 78)
    print("(* = delta exceeds the 95% two-sample margin, i.e. distinguishable)")
    print(f"\nRaw data: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
