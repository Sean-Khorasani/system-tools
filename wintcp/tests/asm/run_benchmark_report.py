#!/usr/bin/env python3
"""
run_benchmark_report.py
SPDX-License-Identifier: Apache-2.0

Comprehensive benchmark runner + report generator for the wintcp assembly
optimization comparison.

Runs bench_orig.exe (original C++ implementations) and bench_asm.exe
(optimized assembly/intrinsic implementations) with the --json flag,
parses the JSON output, and produces a detailed markdown report comparing:

  1. Correctness - every test result side-by-side, with divergence highlighting
  2. Performance - per-function/per-workload ns/op with speedup ratios
  3. Statistics  - geometric mean, min/max, per-round data, variance
  4. Recommendations

Each executable is run N times (default 3) so the report can detect run-to-run
jitter and pick the best representative run.  Use --runs=1 for a quick single pass.

Usage:
    python run_benchmark_report.py                       # full report, 3 runs each
    python run_benchmark_report.py --runs=5              # 5 runs per variant
    python run_benchmark_report.py --runs=1 --quick      # quick pass (1 run, small)
    python run_benchmark_report.py --orig=PATH --asm=PATH  # custom exe paths
    python run_benchmark_report.py --output=report.md    # custom output file
    python run_benchmark_report.py --no-human            # JSON-only, no human output
"""

import argparse
import json
import math
import os
import statistics
import subprocess
import sys
from datetime import datetime
from pathlib import Path


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def find_exe(name, override=None):
    """Locate a benchmark executable."""
    if override:
        p = Path(override)
        if p.is_file():
            return str(p)
        print(f"ERROR: --{name}={override} does not exist.", file=sys.stderr)
        sys.exit(1)
    # Default search locations
    candidates = [
        Path("build_bench") / f"{name}.exe",
        Path(f"{name}.exe"),
    ]
    for c in candidates:
        if c.is_file():
            return str(c)
    print(f"ERROR: Could not find {name}.exe. Run build_bench.bat first.", file=sys.stderr)
    sys.exit(1)


def run_bench(exe_path, extra_args=None, timeout=300):
    """Run a benchmark executable with --json and return parsed JSON."""
    cmd = [exe_path, "--json", "--phase=all"]
    if extra_args:
        cmd.extend(extra_args)
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout, check=True
        )
    except subprocess.TimeoutExpired:
        print(f"WARNING: {exe_path} timed out after {timeout}s", file=sys.stderr)
        return None
    except subprocess.CalledProcessError as e:
        print(f"ERROR: {exe_path} exited with code {e.returncode}:", file=sys.stderr)
        print(e.stderr, file=sys.stderr)
        return None

    # The output should be pure JSON.  Find the JSON object.
    out = result.stdout.strip()
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        # Maybe there's non-JSON preamble; try to find the JSON start
        idx = out.find("{")
        if idx >= 0:
            try:
                return json.loads(out[idx:])
            except json.JSONDecodeError:
                pass
        print(f"ERROR: Could not parse JSON from {exe_path}", file=sys.stderr)
        print(f"Output (first 500 chars): {out[:500]}", file=sys.stderr)
        return None


def fmt_ns(val):
    """Format a nanosecond value for display."""
    if val >= 1000:
        return f"{val / 1000:.3f} us"
    return f"{val:.3f} ns"


def fmt_num(val, prec=3):
    """Format a number with fixed precision."""
    return f"{val:.{prec}f}"


def geo_mean(values):
    """Compute geometric mean, ignoring zeros."""
    if not values:
        return 0.0
    log_sum = sum(math.log(v) for v in values if v > 0)
    return math.exp(log_sum / len(values))


def safe_div(a, b):
    """Safe division returning 0 on division by zero."""
    return a / b if b != 0 else 0.0


# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------

class BenchRun:
    """A single run of a benchmark executable."""

    def __init__(self, data, exe_name, run_idx):
        self.data = data
        self.exe_name = exe_name
        self.run_idx = run_idx
        self.variant = data.get("variant", "unknown")
        self.build = data.get("build", {})
        self.config = data.get("config", {})
        self.correctness = data.get("correctness", {})
        self.benchmarks = data.get("benchmarks", [])
        self.tests = self.correctness.get("tests", [])

    @property
    def passed(self):
        return self.correctness.get("passed", 0)

    @property
    def failed(self):
        return self.correctness.get("failed", 0)

    @property
    def total(self):
        return self.correctness.get("total", 0)

    def get_bench(self, func, workload):
        """Find a benchmark result by function + workload name."""
        for b in self.benchmarks:
            if b["function"] == func and b["workload"] == workload:
                return b
        return None


# ---------------------------------------------------------------------------
# Report sections
# ---------------------------------------------------------------------------

def build_correctness_section(orig_runs, asm_runs):
    """Build the correctness comparison markdown section."""
    lines = []
    lines.append("## Correctness Comparison\n")

    # Summary table
    lines.append("| Variant | Passed | Failed | Total |")
    lines.append("|---------|--------|--------|-------|")
    for r in orig_runs:
        lines.append(f"| {r.variant} (run {r.run_idx}) | {r.passed} | {r.failed} | {r.total} |")
    for r in asm_runs:
        lines.append(f"| {r.variant} (run {r.run_idx}) | {r.passed} | {r.failed} | {r.total} |")
    lines.append("")

    # Detailed test-by-test comparison
    lines.append("### Per-Test Results\n")

    # Use the first run as the reference for test ordering
    ref_tests = orig_runs[0].tests if orig_runs else asm_runs[0].tests

    # Build a lookup: test_name -> {variant: pass/fail, detail}
    all_tests = {}
    for r in orig_runs + asm_runs:
        for t in r.tests:
            name = t["name"]
            group = t.get("group", "")
            if name not in all_tests:
                all_tests[name] = {"group": group, "orig": [], "asm": []}
            if r.variant == "original":
                all_tests[name]["orig"].append(t)
            else:
                all_tests[name]["asm"].append(t)

    # Table header
    lines.append("| Test | Group | Original | Optimized | Match |")
    lines.append("|------|-------|----------|-----------|-------|")

    diverged = []
    all_pass = True

    for ref in ref_tests:
        name = ref["name"]
        info = all_tests.get(name, {})
        group = info.get("group", "")
        orig_results = info.get("orig", [])
        asm_results = info.get("asm", [])

        orig_passed = all(t["pass"] for t in orig_results) if orig_results else False
        asm_passed = all(t["pass"] for t in asm_results) if asm_results else False
        match = orig_passed == asm_passed

        status_o = "PASS" if orig_passed else "FAIL"
        status_a = "PASS" if asm_passed else "FAIL"
        status_m = "OK" if match else "**DIVERGENT**"

        if orig_passed and asm_passed:
            cells = ["green", "green", "green"]
        elif not orig_passed and not asm_passed:
            cells = ["red", "red", "red"]
        elif orig_passed and not asm_passed:
            cells = ["green", "red", "red"]
            diverged.append((name, "optimized regressed"))
            all_pass = False
        elif not orig_passed and asm_passed:
            cells = ["red", "green", "orange"]
            diverged.append((name, "optimized improved correctness"))
            all_pass = False
        else:
            cells = ["green", "green", "green"]

        detail_o = orig_results[0].get("detail", "") if orig_results else ""
        detail_a = asm_results[0].get("detail", "") if asm_results else ""

        row = f"| {name} | {group} | {status_o} | {status_a} | {status_m} |"
        lines.append(row)

        if detail_o:
            lines.append(f"| | | _orig: {detail_o}_ | | |")
        if detail_a:
            lines.append(f"| | | | _asm: {detail_a}_ | |")

    lines.append("")
    if diverged:
        lines.append("### Correctness Divergences Found\n")
        for name, issue in diverged:
            lines.append(f"- **{name}**: {issue}")
        lines.append("")
    else:
        lines.append("### No Correctness Divergences\n")
        lines.append("Both variants pass all tests identically. The optimized implementations are functionally equivalent to the originals.\n")

    return "\n".join(lines), diverged, all_pass


def build_performance_section(orig_runs, asm_runs):
    """Build the performance comparison markdown section."""
    lines = []
    lines.append("## Performance Comparison\n")

    # Collect all unique function+workload pairs
    bench_keys = []
    seen = set()
    for r in orig_runs + asm_runs:
        for b in r.benchmarks:
            key = (b["function"], b["workload"])
            if key not in seen:
                seen.add(key)
                bench_keys.append(key)

    # Table header
    lines.append("| Function | Workload | Original (ns/op) | Optimized (ns/op) | Speedup | Winner |")
    lines.append("|----------|----------|-------------------|--------------------|---------|--------|")

    speedups = []
    details = []

    for func, workload in bench_keys:
        orig_benches = []
        asm_benches = []
        for r in orig_runs:
            b = r.get_bench(func, workload)
            if b:
                orig_benches.append(b)
        for r in asm_runs:
            b = r.get_bench(func, workload)
            if b:
                asm_benches.append(b)

        # Use median of medians across runs for stability
        orig_medians = [b["ns_per_op"] for b in orig_benches if "ns_per_op" in b]
        asm_medians = [b["ns_per_op"] for b in asm_benches if "ns_per_op" in b]

        orig_ns = statistics.median(orig_medians) if orig_medians else 0
        asm_ns = statistics.median(asm_medians) if asm_medians else 0

        speedup = safe_div(orig_ns, asm_ns)
        is_faster = speedup > 1.0
        is_slower = speedup < 1.0
        winner = "optimized" if is_faster else ("original" if is_slower else "tie")

        speedups.append((func, workload, orig_ns, asm_ns, speedup))
        details.append({
            "function": func,
            "workload": workload,
            "orig_ns": orig_ns,
            "asm_ns": asm_ns,
            "orig_benches": orig_benches,
            "asm_benches": asm_benches,
        })

        if is_faster:
            winner_str = f"**{winner}** ({speedup:.2f}x)"
        elif is_slower:
            winner_str = f"{winner} ({1/speedup:.2f}x slower)"
        else:
            winner_str = "tie"

        desc = orig_benches[0].get("desc", "") if orig_benches else ""
        lines.append(f"| {func} | {workload} | {fmt_num(orig_ns)} | {fmt_num(asm_ns)} | {fmt_num(speedup, 2)}x | {winner_str} |")

    lines.append("")

    return "\n".join(lines), speedups, details


def build_statistics_section(speedups, details, orig_runs, asm_runs):
    """Build the statistics and analysis section."""
    lines = []
    lines.append("## Statistical Analysis\n")

    all_speedups = [s[4] for s in speedups if s[4] > 0]
    faster = [s for s in speedups if s[4] > 1.0]
    slower = [s for s in speedups if s[4] < 1.0]
    tied = [s for s in speedups if abs(s[4] - 1.0) < 0.01]

    lines.append("### Summary\n")
    lines.append(f"| Metric | Value |")
    lines.append(f"|--------|-------|")
    lines.append(f"| Total benchmarks | {len(speedups)} |")
    lines.append(f"| Faster (optimized wins) | {len(faster)} |")
    lines.append(f"| Slower (original wins) | {len(slower)} |")
    lines.append(f"| Tied (within 1%) | {len(tied)} |")
    if all_speedups:
        lines.append(f"| Geometric mean speedup | {geo_mean(all_speedups):.2f}x |")
        lines.append(f"| Min speedup | {min(all_speedups):.2f}x |")
        lines.append(f"| Max speedup | {max(all_speedups):.2f}x |")
        lines.append(f"| Median speedup | {statistics.median(all_speedups):.2f}x |")
        lines.append(f"| Mean speedup | {statistics.mean(all_speedups):.2f}x |")
        lines.append(f"| Stdev of speedup | {statistics.stdev(all_speedups) if len(all_speedups) > 1 else 0:.3f} |")
    lines.append("")

    # Rankings
    lines.append("### Fastest Optimized Functions\n")
    sorted_fastest = sorted([s for s in speedups if s[4] > 1.0], key=lambda x: x[4], reverse=True)
    if sorted_fastest:
        lines.append("| Rank | Function | Workload | Original | Optimized | Speedup |")
        lines.append("|------|----------|----------|----------|-----------|---------|")
        for i, s in enumerate(sorted_fastest):
            lines.append(f"| {i+1} | {s[0]} | {s[1]} | {fmt_ns(s[2])} | {fmt_ns(s[3])} | {fmt_num(s[4], 2)}x |")
    else:
        lines.append("No benchmarks showed the optimized version being faster.\n")
    lines.append("")

    lines.append("### Slowest (Regression) Functions\n")
    sorted_slowest = sorted([s for s in speedups if s[4] < 1.0], key=lambda x: x[4])
    if sorted_slowest:
        lines.append("| Rank | Function | Workload | Original | Optimized | Speedup |")
        lines.append("|------|----------|----------|----------|-----------|---------|")
        for i, s in enumerate(sorted_slowest):
            lines.append(f"| {i+1} | {s[0]} | {s[1]} | {fmt_ns(s[2])} | {fmt_ns(s[3])} | {fmt_num(s[4], 2)}x |")
    else:
        lines.append("No benchmarks showed a regression — the optimized versions are never slower.\n")
    lines.append("")

    return "\n".join(lines)


def build_per_round_section(details, orig_runs, asm_runs):
    """Build detailed per-round timing data section."""
    lines = []
    lines.append("## Per-Round Timing Data\n")

    for d in details:
        func = d["function"]
        workload = d["workload"]
        lines.append(f"### {func} / {workload}\n")

        orig_benches = d["orig_benches"]
        asm_benches = d["asm_benches"]

        # Collect rounds from each run
        lines.append("#### Original\n")
        if orig_benches:
            lines.append("| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |")
            lines.append("|-----|-------|--------|--------|---------|-------|------------|")
            for i, b in enumerate(orig_benches):
                rounds_str = ", ".join(fmt_num(r) for r in b.get("rounds", []))
                lines.append(f"| {i+1} | {fmt_num(b['ns_per_op'])} | {fmt_num(b['ns_min'])} | "
                           f"{fmt_num(b['ns_p90'])} | {fmt_num(b['ns_mean'])} | "
                           f"{fmt_num(b['ns_stdev'])} | {b['iterations']} |")
                lines.append(f"| | rounds: [{rounds_str}] | | | | | |")
        else:
            lines.append("_No data available_\n")

        lines.append("\n#### Optimized\n")
        if asm_benches:
            lines.append("| Run | ns/op | ns/min | ns/p90 | ns/mean | stdev | iterations |")
            lines.append("|-----|-------|--------|--------|---------|-------|------------|")
            for i, b in enumerate(asm_benches):
                rounds_str = ", ".join(fmt_num(r) for r in b.get("rounds", []))
                lines.append(f"| {i+1} | {fmt_num(b['ns_per_op'])} | {fmt_num(b['ns_min'])} | "
                           f"{fmt_num(b['ns_p90'])} | {fmt_num(b['ns_mean'])} | "
                           f"{fmt_num(b['ns_stdev'])} | {b['iterations']} |")
                lines.append(f"| | rounds: [{rounds_str}] | | | | | |")
        else:
            lines.append("_No data available_\n")
        lines.append("")

    return "\n".join(lines)


def build_recommendations_section(speedups, correct_diverged, all_correct):
    """Build recommendations based on results."""
    lines = []
    lines.append("## Recommendations\n")

    all_speedups = [s[4] for s in speedups if s[4] > 0]
    faster = len([s for s in speedups if s[4] > 1.05])
    slower = len([s for s in speedups if s[4] < 0.95])
    tied = len([s for s in speedups if 0.95 <= s[4] <= 1.05])

    if correct_diverged and not all_correct:
        lines.append("### CRITICAL: Correctness Divergence Detected")
        lines.append("")
        lines.append("The optimized implementation diverges from the original in at least one correctness test.")
        lines.append("**Do NOT integrate** the optimized code until these divergences are resolved.")
        lines.append("Review the correctness comparison above for details.")
        for name, issue in correct_diverged:
            lines.append(f"- {name}: {issue}")
        return "\n".join(lines)

    lines.append(f"Of {len(speedups)} benchmarked workloads:")
    lines.append(f"- {faster} faster (speedup > 5%)")
    lines.append(f"- {slower} slower (regression > 5%)")
    lines.append(f"- {tied} statistically tied (within 5%)")
    lines.append("")

    if all_speedups:
        gm = geo_mean(all_speedups)
        lines.append(f"**Overall geometric mean speedup: {gm:.2f}x**")
        lines.append("")

    if slower == 0 and faster > 0:
        lines.append("### Recommendation: Integrate")
        lines.append("")
        lines.append("All optimized functions are faster than or equal to the originals, with no regressions.")
        lines.append("The optimized implementations are safe to integrate into the main wintcp source code.")
        lines.append("See `wintcp/tests/asm/HOWTO-INTEGRATION.md` for integration steps.")
    elif faster > slower:
        lines.append("### Recommendation: Integrate with Monitoring")
        lines.append("")
        lines.append(f"Most functions ({faster}/{len(speedups)}) are faster, but {slower} show regressions.")
        lines.append("Consider integrating the faster functions individually, starting with the highest speedups.")
    elif slower > faster:
        lines.append("### Recommendation: Do Not Integrate")
        lines.append("")
        lines.append(f"The majority of functions ({slower}/{len(speedups)}) are slower than the originals.")
        lines.append("The current optimized implementations should not be integrated. Review the algorithm choices.")
    else:
        lines.append("### Recommendation: Evaluate Individually")
        lines.append("")
        lines.append("Results are mixed. Review the per-function table and integrate only the functions with clear wins.")

    lines.append("")
    lines.append("### Integration Checklist")
    lines.append("1. Follow `wintcp/tests/asm/HOWTO-INTEGRATION.md` for step-by-step instructions.")
    lines.append("2. Add `WINTCP_ENABLE_ASM_OPTIMIZATIONS` guard for CPU feature detection.")
    lines.append("3. Run `wintcp\\tests\\cli.bat` and `wintcp-tests.exe unit` to verify.")
    lines.append("4. Monitor production performance after deployment.")

    return "\n".join(lines)


def build_environment_section(orig_runs, asm_runs):
    """Build the environment section."""
    lines = []
    lines.append("## Environment\n")

    # Use the first orig run (they should all be the same environment)
    bi = orig_runs[0].build if orig_runs else {}

    lines.append("| Property | Value |")
    lines.append("|----------|-------|")
    lines.append(f"| Compiler | {bi.get('compiler', 'unknown')} |")
    lines.append(f"| Architecture | {bi.get('arch', 'unknown')} |")
    lines.append(f"| CPU | {bi.get('cpuName', 'unknown')} |")
    lines.append(f"| RAM | {bi.get('ramMb', 0)} MB |")
    lines.append(f"| OS | {bi.get('osName', 'unknown')} |")
    feats = bi.get("features", {})
    feat_str = ", ".join(f"{k}={v}" for k, v in feats.items())
    lines.append(f"| CPU Features | {feat_str} |")
    cfg = orig_runs[0].config if orig_runs else {}
    lines.append(f"| Benchmark config | {cfg.get('rounds', '?')} rounds x {cfg.get('time_ms', '?')} ms (warmup: {cfg.get('warmup_ms', '?')} ms) |")
    lines.append("")

    lines.append(f"- Original executable: `{orig_runs[0].exe_name}`")
    lines.append(f"- Optimized executable: `{asm_runs[0].exe_name}`")
    lines.append("")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Run wintcp assembly optimization benchmarks and generate a report."
    )
    parser.add_argument("--runs", type=int, default=3,
                        help="Number of times to run each executable (default: 3)")
    parser.add_argument("--quick", action="store_true",
                        help="Quick mode: pass --quick to executables for fast runs")
    parser.add_argument("--orig", type=str, default=None,
                        help="Path to bench_orig.exe")
    parser.add_argument("--asm", type=str, default=None,
                        help="Path to bench_asm.exe")
    parser.add_argument("--output", type=str, default=None,
                        help="Output report file (default: benchmark_report.md)")
    parser.add_argument("--no-human", action="store_true",
                        help="Skip human-readable output to stdout")
    parser.add_argument("--timeout", type=int, default=300,
                        help="Timeout per execution in seconds (default: 300)")
    args = parser.parse_args()

    report_path = args.output or "benchmark_report.md"

    orig_exe = find_exe("bench_orig", args.orig)
    asm_exe = find_exe("bench_asm", args.asm)

    extra_args = ["--quick"] if args.quick else []

    print(f"=== wintcp assembly optimization benchmark report ===")
    print(f"Original executable:  {orig_exe}")
    print(f"Optimized executable: {asm_exe}")
    print(f"Runs per variant:     {args.runs}")
    print()

    # --- Run both executables multiple times ---
    orig_runs = []
    asm_runs = []

    for i in range(args.runs):
        print(f"[run {i+1}/{args.runs}] Running original...")
        data = run_bench(orig_exe, extra_args, timeout=args.timeout)
        if data:
            orig_runs.append(BenchRun(data, os.path.basename(orig_exe), i + 1))
        else:
            print(f"  FAILED")

        print(f"[run {i+1}/{args.runs}] Running optimized...")
        data = run_bench(asm_exe, extra_args, timeout=args.timeout)
        if data:
            asm_runs.append(BenchRun(data, os.path.basename(asm_exe), i + 1))
        else:
            print(f"  FAILED")
        print()

    if not orig_runs or not asm_runs:
        print("ERROR: One or both executables failed to produce valid output.")
        sys.exit(1)

    # --- Build the report ---
    report = []
    report.append("# WinTCP Assembly Optimization Benchmark Report")
    report.append("")
    report.append(f"**Generated:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    report.append(f"**Original:** `{orig_exe}`")
    report.append(f"**Optimized:** `{asm_exe}`")
    report.append(f"**Runs per variant:** {args.runs}")
    report.append("")
    report.append("---")
    report.append("")

    # Executive summary
    report.append("## Executive Summary\n")
    orig_all_pass = all(r.failed == 0 for r in orig_runs)
    asm_all_pass = all(r.failed == 0 for r in asm_runs)
    all_correct = orig_all_pass and asm_all_pass

    # Correctness
    if orig_all_pass and asm_all_pass:
        report.append("Both variants pass **all** correctness tests. ")
        report.append("The optimized implementations are functionally equivalent to the original C++ code.\n")
    elif orig_all_pass and not asm_all_pass:
        report.append("WARNING: The original passes all tests but the optimized version has failures. ")
        report.append("Correctness regressions must be fixed before integration.\n")
    elif not orig_all_pass:
        report.append("WARNING: The original implementation has test failures. ")
        report.append("These should be investigated before drawing performance conclusions.\n")

    # Performance summary
    _, speedups, _ = build_performance_section(orig_runs, asm_runs)
    all_speedups = [s[4] for s in speedups if s[4] > 0]
    faster_count = len([s for s in speedups if s[4] > 1.05])
    slower_count = len([s for s in speedups if s[4] < 0.95])

    if all_speedups:
        gm = geo_mean(all_speedups)
        report.append(f"\n**Overall speedup (geometric mean): {gm:.2f}x**")
        report.append(f"- {faster_count} workload(s) faster (optimized)")
        report.append(f"- {slower_count} workload(s) slower (optimized)")
        report.append(f"- {len(speedups) - faster_count - slower_count} tied (within 5%)")
    report.append("")
    report.append("---")
    report.append("")

    # Environment
    report.append(build_environment_section(orig_runs, asm_runs))
    report.append("---")
    report.append("")

    # Correctness
    correct_section, correct_diverged, all_correct_flag = build_correctness_section(orig_runs, asm_runs)
    report.append(correct_section)
    report.append("---")
    report.append("")

    # Performance
    perf_section, speedups, details = build_performance_section(orig_runs, asm_runs)
    report.append(perf_section)
    report.append("---")
    report.append("")

    # Statistics
    stats_section = build_statistics_section(speedups, details, orig_runs, asm_runs)
    report.append(stats_section)
    report.append("---")
    report.append("")

    # Per-round data
    rounds_section = build_per_round_section(details, orig_runs, asm_runs)
    report.append(rounds_section)
    report.append("---")
    report.append("")

    # Recommendations
    rec_section = build_recommendations_section(speedups, correct_diverged, all_correct_flag)
    report.append(rec_section)
    report.append("")

    full_report = "\n".join(report)

    # Write the report
    with open(report_path, "w", encoding="utf-8") as f:
        f.write(full_report)

    print(f"Report written to: {os.path.abspath(report_path)}")
    print()

    if not args.no_human:
        print(full_report)

    # Exit code: 0 if no correctness divergences, 1 otherwise
    sys.exit(0 if all_correct_flag else 1)


if __name__ == "__main__":
    main()
