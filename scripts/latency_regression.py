#!/usr/bin/env python3
"""
latency_regression.py — compare benchmark output against a baseline.

Consumes the text output of tests/benchmarks/hft_benchmarks, e.g.:

    OrderBook::apply               p50=    12 p99=    30 p999=    31 max= 12384 ns  (N=50000)
    MarketMaker::on_book_update    p50=    11 p99=    17 p999=    18 max=  6182 ns  (N=50000)

Compares it against a baseline file with lines of:

    <name>  <p50_max>  <p99_max>  <p999_max>

Exits 0 when every component stays under its p50/p99/p999 thresholds, 1 on any
regression. Used by .github/workflows/latency.yml to gate merges.

Usage:
    hft_benchmarks | python3 latency_regression.py --baseline baseline.txt
    python3 latency_regression.py --baseline baseline.txt --run run.txt
    python3 latency_regression.py --baseline baseline.txt --update run.txt
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, Tuple

# Per-line parser: name (left-aligned, may contain "::") then p50/p99/p999/max.
LINE_RE = re.compile(
    r"^(?P<name>\S.+?)\s+"
    r"p50=\s*(?P<p50>\d+)\s+"
    r"p99=\s*(?P<p99>\d+)\s+"
    r"p999=\s*(?P<p999>\d+)\s+"
    r"max=\s*(?P<max>\d+)\s+ns"
)

TRACKED = ("p50", "p99", "p999")


def parse_run(text: str) -> Dict[str, Dict[str, int]]:
    """Parse hft_benchmarks stdout into {name: {p50, p99, p999, max}}."""
    out: Dict[str, Dict[str, int]] = {}
    for line in text.splitlines():
        m = LINE_RE.match(line)
        if not m:
            continue
        out[m.group("name").strip()] = {
            "p50":  int(m.group("p50")),
            "p99":  int(m.group("p99")),
            "p999": int(m.group("p999")),
            "max":  int(m.group("max")),
        }
    return out


def parse_baseline(path: Path) -> Dict[str, Tuple[int, int, int]]:
    """Parse baseline.txt → {name: (p50_max, p99_max, p999_max)}."""
    out: Dict[str, Tuple[int, int, int]] = {}
    with path.open("r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            # Name may embed "::" but must have EXACTLY 3 trailing integers.
            parts = line.rsplit(None, 3)
            if len(parts) != 4:
                raise ValueError(f"bad baseline line: {raw!r}")
            name, p50, p99, p999 = parts
            out[name.strip()] = (int(p50), int(p99), int(p999))
    return out


def compare(run: Dict[str, Dict[str, int]],
            baseline: Dict[str, Tuple[int, int, int]]) -> int:
    """Return count of regressions. Emits a table to stdout."""
    missing = [n for n in baseline if n not in run]
    if missing:
        print(f"ERROR: benchmark did not emit: {', '.join(missing)}",
              file=sys.stderr)
        return len(missing)

    print(f"{'component':<34}{'metric':>6}{'limit':>8}{'observed':>10}  status")
    print("-" * 72)
    regressions = 0
    for name, (lim50, lim99, lim999) in baseline.items():
        observed = run[name]
        for metric, limit in zip(TRACKED, (lim50, lim99, lim999)):
            value = observed[metric]
            ok = value <= limit
            status = "OK" if ok else "REGRESS"
            print(f"{name:<34}{metric:>6}{limit:>8}{value:>10}  {status}")
            if not ok:
                regressions += 1
    return regressions


def update_baseline(run_text: str, baseline_path: Path) -> None:
    """Rewrite baseline from a run, preserving header comments."""
    run = parse_run(run_text)
    if not run:
        raise SystemExit("no benchmark lines parsed from run output")

    # Preserve leading comment block if present.
    header_lines = []
    if baseline_path.exists():
        for raw in baseline_path.read_text(encoding="utf-8").splitlines():
            if raw.startswith("#") or not raw.strip():
                header_lines.append(raw)
            else:
                break

    lines = list(header_lines)
    width = max(len(n) for n in run)
    for name, m in run.items():
        lines.append(f"{name:<{width}}  {m['p50']:>4}  {m['p99']:>4}  {m['p999']:>4}")
    baseline_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"updated {baseline_path} with {len(run)} entries")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--baseline", type=Path, required=True,
                    help="Path to baseline.txt")
    ap.add_argument("--run", type=Path,
                    help="Benchmark output file (default: read stdin)")
    ap.add_argument("--update", type=Path,
                    help="Regenerate baseline from this run file and exit")
    args = ap.parse_args(argv)

    if args.update:
        update_baseline(args.update.read_text(encoding="utf-8"), args.baseline)
        return 0

    run_text = args.run.read_text(encoding="utf-8") if args.run else sys.stdin.read()
    run = parse_run(run_text)
    if not run:
        print("ERROR: no benchmark lines parsed — is the binary wired up?",
              file=sys.stderr)
        return 2

    baseline = parse_baseline(args.baseline)
    regressions = compare(run, baseline)
    if regressions:
        print(f"\n{regressions} regression(s) detected", file=sys.stderr)
        return 1
    print("\nAll components within latency budget.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
