#!/usr/bin/env python3
"""Turn Google Benchmark JSON into a compact, auditable Markdown table."""
import argparse
import json
import statistics
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("input", type=Path)
parser.add_argument("--match", default="", help="Only include names containing this text")
args = parser.parse_args()
document = json.loads(args.input.read_text())
groups = {}
for row in document["benchmarks"]:
    if row.get("run_type") == "aggregate" or row.get("error_occurred"):
        continue
    if args.match not in row["name"]:
        continue
    groups.setdefault(row["name"], []).append(row)
print("| Benchmark arguments | Median CPU latency (ms) | M input rows/s | Operator buffers (KiB) |")
print("|---|---:|---:|---:|")
for name, rows in groups.items():
    factor = {"ns": 1e-6, "us": 1e-3, "ms": 1, "s": 1e3}
    latency = statistics.median(r["cpu_time"] * factor[r["time_unit"]] for r in rows)
    rate = statistics.median(r.get("items_per_second", 0) / 1e6 for r in rows)
    memory = statistics.median(r.get("operator_buffer_bytes", 0) / 1024 for r in rows)
    print(f"| {name} | {latency:.5f} | {rate:.2f} | {memory:.1f} |")
