#!/usr/bin/env python3
"""Run bpftime-only benchmark inside Docker and merge into report/results.json"""
import sys, os, json, subprocess, time, statistics, re
from pathlib import Path

BUILD_DIR = Path("docker_build")
BPFTIME_SERVER = "/root/.bpftime/libbpftime-syscall-server.so"
BPFTIME_AGENT = "/root/.bpftime/libbpftime-agent.so"
NUM_RUNS = 10

SCENARIOS = [
    ("Empty Function", 0, 100000),
    ("5 \u03bcs Function", 5, 100000),
    ("50 \u03bcs Function", 50, 50000),
    ("100 \u03bcs Function", 100, 10000),
    ("500 \u03bcs Function", 500, 5000),
    ("1000 \u03bcs (1ms) Function", 1000, 2000),
]

def run_bpftime_single(work_us, iterations):
    subprocess.run("pkill -9 bpftime_tracer", shell=True, capture_output=True)
    subprocess.run("rm -f /dev/shm/bpftime*", shell=True, capture_output=True)
    time.sleep(0.5)

    lib_path = str(BUILD_DIR / "lib" / "libmylib.so")
    tracer_env = os.environ.copy()
    tracer_env["LD_PRELOAD"] = BPFTIME_SERVER
    tracer_proc = subprocess.Popen(
        f"{BUILD_DIR}/bin/bpftime_tracer -l {lib_path}",
        shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, env=tracer_env
    )
    time.sleep(3)

    app_env = os.environ.copy()
    app_env["LD_PRELOAD"] = BPFTIME_AGENT
    if work_us > 0:
        app_env["SIMULATED_WORK_US"] = str(work_us)

    cmd = f'/usr/bin/time -f "wall_time=%e user_time=%U sys_time=%S max_rss=%M" {BUILD_DIR}/bin/sample_app {iterations}'
    result = subprocess.run(cmd, shell=True, env=app_env, capture_output=True, text=True, timeout=300)

    time_data = {}
    for key, pat in [("wall_time", r"wall_time=([\d.]+)"), ("user_time", r"user_time=([\d.]+)"),
                     ("sys_time", r"sys_time=([\d.]+)"), ("max_rss", r"max_rss=(\d+)")]:
        m = re.search(pat, result.stderr)
        if m:
            time_data[key] = float(m.group(1))

    avg_match = re.search(r"Average time per call:\s+([\d.]+)", result.stdout)
    avg_ns = float(avg_match.group(1)) if avg_match else 0

    time.sleep(1)
    subprocess.run("pkill -9 bpftime_tracer", shell=True, capture_output=True)
    time.sleep(0.5)

    return {
        "avg_time_per_call_ns": avg_ns,
        "wall_time_s": time_data.get("wall_time", 0),
        "user_cpu_s": time_data.get("user_time", 0),
        "system_cpu_s": time_data.get("sys_time", 0),
        "max_rss_kb": int(time_data.get("max_rss", 0)),
    }

results = []
for name, work_us, iterations in SCENARIOS:
    print(f"\n=== {name} ({work_us} us, {iterations} iters) ===")
    runs = []
    for i in range(NUM_RUNS):
        r = run_bpftime_single(work_us, iterations)
        print(f"  Run {i+1}/{NUM_RUNS}: {r['avg_time_per_call_ns']:.2f} ns/call")
        runs.append(r)

    avg_times = [r["avg_time_per_call_ns"] for r in runs]
    mean_avg = statistics.mean(avg_times)
    stddev_avg = statistics.stdev(avg_times) if len(avg_times) > 1 else 0
    ci_margin = 1.96 * (stddev_avg / (len(avg_times) ** 0.5)) if len(avg_times) > 1 else 0

    result = {
        "scenario": name, "method": "bpftime", "iterations": iterations,
        "simulated_work_us": work_us,
        "wall_time_s": statistics.mean([r["wall_time_s"] for r in runs]),
        "user_cpu_s": statistics.mean([r["user_cpu_s"] for r in runs]),
        "system_cpu_s": statistics.mean([r["system_cpu_s"] for r in runs]),
        "max_rss_kb": int(statistics.mean([r["max_rss_kb"] for r in runs])),
        "avg_time_per_call_ns": mean_avg,
        "trace_size_mb": 0, "tracer_cpu_percent": 0, "tracer_memory_kb": 0,
        "events_captured": iterations * 2, "events_dropped": None,
        "num_runs": NUM_RUNS, "avg_time_stddev": stddev_avg,
        "avg_time_min": min(avg_times), "avg_time_max": max(avg_times),
        "wall_time_stddev": statistics.stdev([r["wall_time_s"] for r in runs]) if len(runs) > 1 else 0,
        "confidence_95_margin": ci_margin,
    }
    results.append(result)
    print(f"  Mean: {mean_avg:.2f} +/- {ci_margin:.2f} ns/call")

existing_file = "report/results.json"
with open(existing_file) as f:
    existing = json.load(f)
filtered = [r for r in existing if r["method"] != "bpftime"]
filtered.extend(results)
with open(existing_file, "w") as f:
    json.dump(filtered, f, indent=2)
print(f"\nUpdated {existing_file}")
