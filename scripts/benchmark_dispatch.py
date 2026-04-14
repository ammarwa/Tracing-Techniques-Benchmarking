#!/usr/bin/env python3
"""
Targeted benchmark for the 4 dispatch-tracer control channel options.

Tests only the new late-load dispatch options (mmap/sock/memfd/signal)
— does NOT re-run the full LTTng/eBPF/bpftime suite. Results are
appended to the existing results.json and merged into the report.

Phases measured per option:
  - noop: stub preloaded, no controller attached (0 ns design claim)
  - active: controller attached, tracing emitting every event
  - filter_rejected: controller attached, runtime filter rejects most events

Output format: same BenchmarkResult schema as the main benchmark suite,
with method names 'dispatch_<opt>_<phase>'.
"""

import os
import sys
import subprocess
import time
import json
import statistics
import argparse
from pathlib import Path
from datetime import datetime
from dataclasses import dataclass, asdict, field
from typing import List, Dict, Optional

# Ensure we can import BenchmarkResult from the main benchmark module
SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
from benchmark import BenchmarkResult  # noqa: E402


SCENARIOS = [
    # (name, simulated_work_us, iterations)
    ("Empty Function",       0,    100000),
    ("5 μs Function",        5,    100000),
    ("50 μs Function",       50,   50000),
    ("100 μs Function",      100,  10000),
    ("500 μs Function",      500,  5000),
    ("1000 μs (1ms) Function", 1000, 2000),
]

OPTIONS = ["mmap", "sock", "memfd", "signal"]

# Per-option library + controller paths
BUILD_DIR = Path(os.environ.get("BUILD_DIR", "build")).resolve()


def run(cmd, env=None, capture=True, timeout=120):
    full_env = dict(os.environ)
    if env:
        full_env.update(env)
    return subprocess.run(
        cmd, shell=True, env=full_env,
        capture_output=capture, text=True, timeout=timeout
    )


def parse_time(stderr: str) -> Dict[str, float]:
    """Parse /usr/bin/time -f output: wall_time=X user_time=Y sys_time=Z max_rss=W"""
    out = {}
    for tok in stderr.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                out[k] = float(v)
            except ValueError:
                pass
    return out


def parse_app(stdout: str) -> Dict[str, float]:
    """Parse sample_app_dispatch output: Average time per call: X nanoseconds"""
    out = {}
    for line in stdout.splitlines():
        if "Average time per call:" in line:
            try:
                out["avg_time_ns"] = float(line.split(":")[1].strip().split()[0])
            except (IndexError, ValueError):
                pass
    return out


def bench_noop(option: str, scenario_name: str, work_us: int, iters: int) -> BenchmarkResult:
    """Stub preloaded, controller not attached. Tests design claim of 0 ns overhead."""
    stub = BUILD_DIR / "lib" / f"librocp_stub_{option}.so"
    app  = BUILD_DIR / "bin" / "sample_app_dispatch"
    env = {
        "LD_PRELOAD": str(stub),
        "LD_LIBRARY_PATH": str(BUILD_DIR / "lib"),
    }
    if work_us > 0:
        env["SIMULATED_WORK_US"] = str(work_us)
    cmd = f'/usr/bin/time -f "wall_time=%e user_time=%U sys_time=%S max_rss=%M" {app} {iters}'
    r = run(cmd, env=env, timeout=60)
    td = parse_time(r.stderr)
    ad = parse_app(r.stdout)
    return BenchmarkResult(
        scenario=scenario_name,
        method=f"dispatch_{option}_noop",
        iterations=iters,
        simulated_work_us=work_us,
        wall_time_s=td.get("wall_time", 0),
        user_cpu_s=td.get("user_time", 0),
        system_cpu_s=td.get("sys_time", 0),
        max_rss_kb=int(td.get("max_rss", 0)),
        avg_time_per_call_ns=ad.get("avg_time_ns", 0),
    )


def bench_active(option: str, scenario_name: str, work_us: int, iters: int,
                 *, filter_reject: bool = False) -> Optional[BenchmarkResult]:
    """Stub preloaded, controller attaches with config, tracing active.
    If filter_reject=True, configure with no domains so runtime filter rejects all events.

    For mmap/signal (which have a filesystem ctrl file), use WAIT_FOR_ATTACH=1 so the
    app blocks until the controller has attached before starting the iteration loop —
    this gives an accurate active-tracing measurement. For sock/memfd, the app runs
    immediately and we accept a blended noop+active number (noted in the report)."""
    stub = BUILD_DIR / "lib" / f"librocp_stub_{option}.so"
    app  = BUILD_DIR / "bin" / "sample_app_dispatch"
    ctrl = BUILD_DIR / "bin" / f"rocp_ctrl_{option}"

    env = {
        "LD_PRELOAD": str(stub),
        "LD_LIBRARY_PATH": str(BUILD_DIR / "lib"),
    }
    if work_us > 0:
        env["SIMULATED_WORK_US"] = str(work_us)

    # Use WAIT_FOR_ATTACH for options with a filesystem-visible ctrl file
    use_wait = option in ("mmap", "signal")
    if use_wait:
        env["WAIT_FOR_ATTACH"] = "1"

    # Start the app in the background under /usr/bin/time
    app_cmd = f'/usr/bin/time -f "wall_time=%e user_time=%U sys_time=%S max_rss=%M" {app} {iters}'

    full_env = dict(os.environ)
    full_env.update(env)
    proc = subprocess.Popen(
        app_cmd, shell=True, env=full_env,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )

    # Give the stub constructor a moment to bind its IPC channel
    # For sock/memfd with abstract sockets, we need more margin since the
    # listen socket needs to be ready before the controller connects
    time.sleep(0.5 if not use_wait else 0.3)

    # Attach the controller with configure command
    trace_path = f"/tmp/trace_{option}_{scenario_name.replace(' ', '_').replace('(', '').replace(')', '').replace('μ', 'u')}.txt"
    if filter_reject:
        # Configure with no domains enabled — runtime filter rejects everything
        cfg_args = "configure --output text --out " + trace_path
    else:
        cfg_args = "configure --hip --output text --out " + trace_path
    cfg_cmd = f"{ctrl} --pid {proc.pid} {cfg_args}"
    try:
        cr = run(cfg_cmd, timeout=10)
        if cr.returncode != 0:
            print(f"    [WARN] configure failed for {option} {scenario_name}: rc={cr.returncode}")
            print(f"           stderr: {cr.stderr[:200]}")
    except subprocess.TimeoutExpired:
        print(f"    [WARN] configure timed out for {option} {scenario_name}")

    # Wait for the app to finish the iterations
    try:
        stdout, stderr = proc.communicate(timeout=120)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate()

    # Clean trace file to not fill disk
    try:
        os.unlink(trace_path)
    except OSError:
        pass

    td = parse_time(stderr)
    ad = parse_app(stdout)
    suffix = "filter_rejected" if filter_reject else "active"
    return BenchmarkResult(
        scenario=scenario_name,
        method=f"dispatch_{option}_{suffix}",
        iterations=iters,
        simulated_work_us=work_us,
        wall_time_s=td.get("wall_time", 0),
        user_cpu_s=td.get("user_time", 0),
        system_cpu_s=td.get("sys_time", 0),
        max_rss_kb=int(td.get("max_rss", 0)),
        avg_time_per_call_ns=ad.get("avg_time_ns", 0),
    )


def bench_attach_latency(option: str) -> Dict[str, float]:
    """Measure attach latency: time from CMD_CONFIGURE send to context_active=1.
    Uses a long-running app + timing the controller command itself."""
    stub = BUILD_DIR / "lib" / f"librocp_stub_{option}.so"
    app  = BUILD_DIR / "bin" / "sample_app_dispatch"
    ctrl = BUILD_DIR / "bin" / f"rocp_ctrl_{option}"

    env = dict(os.environ)
    env["LD_PRELOAD"] = str(stub)
    env["LD_LIBRARY_PATH"] = str(BUILD_DIR / "lib")
    env["SIMULATED_WORK_US"] = "100"  # moderate work so app stays alive

    # Launch a long-running app (100k iters x 100us = 10s)
    proc = subprocess.Popen(
        f"{app} 100000", shell=True, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(0.3)

    latencies = []
    for trial in range(5):
        trace_path = f"/tmp/trace_{option}_attach_lat_{trial}.txt"
        cfg_cmd = f"{ctrl} --pid {proc.pid} configure --hip --output text --out {trace_path}"
        t0 = time.perf_counter()
        try:
            subprocess.run(cfg_cmd, shell=True, timeout=10,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
            t1 = time.perf_counter()
            latencies.append((t1 - t0) * 1000.0)  # ms
        except (subprocess.TimeoutExpired, subprocess.CalledProcessError):
            pass
        # Deactivate between trials so CMD_CONFIGURE matters (though after 1st call
        # it becomes a CMD_RECONFIGURE internally — still good measure)
        subprocess.run(f"{ctrl} --pid {proc.pid} deactivate", shell=True,
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.1)
        try:
            os.unlink(trace_path)
        except OSError:
            pass

    proc.kill()
    proc.wait(timeout=5)

    if not latencies:
        return {"mean_ms": 0, "median_ms": 0, "min_ms": 0}
    return {
        "mean_ms": statistics.mean(latencies),
        "median_ms": statistics.median(latencies),
        "min_ms": min(latencies),
    }


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--runs", type=int, default=5, help="Runs per cell (default 5)")
    p.add_argument("--quick", action="store_true",
                   help="Quick mode: fewer scenarios, 3 runs")
    p.add_argument("--output", default="report/dispatch_results.json")
    p.add_argument("--options", default=",".join(OPTIONS),
                   help="Comma-separated options to benchmark")
    args = p.parse_args()

    scenarios = SCENARIOS
    runs = args.runs
    if args.quick:
        scenarios = [
            ("Empty Function", 0, 100000),
            ("100 μs Function", 100, 10000),
        ]
        runs = 3

    options = [o.strip() for o in args.options.split(",") if o.strip() in OPTIONS]

    print("=" * 78)
    print("Dispatch Tracer Benchmark — 4 Control Channel Options")
    print(f"Options: {', '.join(options)}")
    print(f"Scenarios: {len(scenarios)}    Runs/cell: {runs}")
    print(f"Build dir: {BUILD_DIR}")
    print("=" * 78)

    # Verify binaries exist
    missing = []
    for opt in options:
        for p_ in [BUILD_DIR / "lib" / f"librocp_stub_{opt}.so",
                   BUILD_DIR / "bin" / f"rocp_ctrl_{opt}"]:
            if not p_.exists():
                missing.append(str(p_))
    if not (BUILD_DIR / "bin" / "sample_app_dispatch").exists():
        missing.append(str(BUILD_DIR / "bin" / "sample_app_dispatch"))
    if missing:
        print("ERROR: missing build artifacts:")
        for m in missing:
            print(f"  {m}")
        print("Run: cmake --build build -j")
        return 1

    all_results: List[BenchmarkResult] = []
    attach_latencies: Dict[str, Dict[str, float]] = {}

    for opt in options:
        print(f"\n### Option: {opt.upper()} ###")

        # Attach latency measurement (once per option)
        print(f"  [attach latency] measuring 5 trials...")
        try:
            attach_latencies[opt] = bench_attach_latency(opt)
            print(f"    mean: {attach_latencies[opt]['mean_ms']:.2f} ms, "
                  f"median: {attach_latencies[opt]['median_ms']:.2f} ms")
        except Exception as e:
            print(f"    [WARN] attach latency measurement failed: {e}")
            attach_latencies[opt] = {"mean_ms": 0, "median_ms": 0, "min_ms": 0}

        for (sc_name, work_us, iters) in scenarios:
            print(f"  {sc_name} (work={work_us}us, iters={iters})")

            # noop phase
            noop_results = []
            for run_i in range(runs):
                noop_results.append(bench_noop(opt, sc_name, work_us, iters))
            avg_noop_ns = statistics.mean(r.avg_time_per_call_ns for r in noop_results)
            print(f"    noop:     {avg_noop_ns:9.2f} ns/call")
            # Aggregate: use the median run as representative
            noop_results.sort(key=lambda r: r.avg_time_per_call_ns)
            all_results.append(noop_results[len(noop_results) // 2])

            # active phase
            active_results = []
            for run_i in range(runs):
                r = bench_active(opt, sc_name, work_us, iters, filter_reject=False)
                if r is not None:
                    active_results.append(r)
            if active_results:
                avg_active_ns = statistics.mean(r.avg_time_per_call_ns for r in active_results)
                print(f"    active:   {avg_active_ns:9.2f} ns/call")
                active_results.sort(key=lambda r: r.avg_time_per_call_ns)
                all_results.append(active_results[len(active_results) // 2])

            # filter_rejected phase (only for scenarios where active mattered)
            if work_us <= 100:  # skip slow scenarios to save time
                fr_results = []
                for run_i in range(runs):
                    r = bench_active(opt, sc_name, work_us, iters, filter_reject=True)
                    if r is not None:
                        fr_results.append(r)
                if fr_results:
                    avg_fr_ns = statistics.mean(r.avg_time_per_call_ns for r in fr_results)
                    print(f"    filtered: {avg_fr_ns:9.2f} ns/call")
                    fr_results.sort(key=lambda r: r.avg_time_per_call_ns)
                    all_results.append(fr_results[len(fr_results) // 2])

    # Save results
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "timestamp": datetime.now().isoformat(),
        "options": options,
        "scenarios": [{"name": s[0], "work_us": s[1], "iterations": s[2]} for s in scenarios],
        "runs_per_cell": runs,
        "attach_latencies_ms": attach_latencies,
        "results": [asdict(r) for r in all_results],
    }
    with open(out, "w") as f:
        json.dump(payload, f, indent=2)
    print(f"\nResults written to {out}")

    # Summary table
    print("\n" + "=" * 78)
    print("SUMMARY")
    print("=" * 78)
    print(f"{'Option':<10} {'Attach (ms)':<14} {'Noop (ns)':<14} {'Active (ns)':<14}")
    for opt in options:
        noop_cells = [r for r in all_results
                      if r.method == f"dispatch_{opt}_noop" and r.simulated_work_us == 0]
        active_cells = [r for r in all_results
                        if r.method == f"dispatch_{opt}_active" and r.simulated_work_us == 0]
        noop_ns = noop_cells[0].avg_time_per_call_ns if noop_cells else 0
        active_ns = active_cells[0].avg_time_per_call_ns if active_cells else 0
        lat_ms = attach_latencies.get(opt, {}).get("mean_ms", 0)
        print(f"{opt:<10} {lat_ms:<14.2f} {noop_ns:<14.2f} {active_ns:<14.2f}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
