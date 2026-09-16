# Performance measurements

## Environment and scope

Measured locally on 2026-09-16: Apple M3 Pro, 12 logical CPUs, 36 GiB RAM,
macOS 26.6.2 / Darwin 25.6 ARM64, Apple Clang 21.0.0, CMake 4.1.2 and Google
Benchmark 1.9.4. Release compilation uses CMake's `-O3 -DNDEBUG`, without
`VQE_NATIVE` or fast-math. Execution is single-threaded. NEON and the scalar
fallback were run; AVX2 cross-compilation passed, but AVX2 was not executed on
this host.

The full suite contains 102 configurations, each with three repetitions and a
minimum requested measurement duration of 0.05 seconds per repetition. These
are short, warm, local microbenchmarks on a shared laptop, without core affinity,
frequency control or a cold-cache protocol. CPU time is the reporting metric
below; raw JSON also contains wall time, iterations, variability and memory
counters. Google Benchmark reports an implausible 24 MHz CPU frequency on this
host; that field is not used in any calculation here. Background load and
thermal state can affect comparisons; the numbers are observations, not portable
performance guarantees.

Inputs and plans are prepared outside timed loops. Each timed query constructs
fresh execution operators, drains all results, and destroys its state. Thus hash
build, aggregate state allocation and result production are included. Scans read
already resident in-memory tables. Parquet is correctness-tested but these
measurements do not benchmark disk or Parquet decoding throughput.

## Results

All query rows below use 262,144 input fact/probe rows. Throughput for joins counts
both build and probe inputs; analytical throughput counts lineitems only, even
for Q3, which also processes orders and customers. These denominators are not
directly comparable across families. Memory is retained execution buffer
capacity including output batches, excluding shared input columns and metadata.

| Workload | Median CPU latency (ms) | M input rows/s | Operator buffers (KiB) |
|---|---:|---:|---:|
| Filter, batch 1, 50% threshold | 15.3020 | 17.13 | 0.1 |
| Filter, batch 256, 50% threshold | 2.1779 | 120.37 | 9.4 |
| Filter, batch 1024, 50% threshold | 2.2500 | 116.51 | 37.5 |
| Filter, batch 2048, 50% threshold | 2.1121 | 124.12 | 75.0 |
| Filter, batch 8192, 50% threshold | 2.1798 | 120.26 | 300.0 |
| Aggregate, key domain 64, uniform | 4.1022 | 63.90 | 86.0 |
| Aggregate, key domain 65536, uniform | 8.0342 | 32.63 | 5721.2 |
| Aggregate, key domain 65536, 90% hot key | 3.8649 | 67.83 | 2389.2 |
| Join, build 1024 unique keys | 8.5304 | 30.85 | 210.0 |
| Join, build 65536 unique keys | 12.4380 | 26.35 | 4258.0 |
| Join, build 16384 rows, four duplicates/key | 23.0643 | 12.08 | 790.0 |
| Q1 style, unoptimized | 16.5730 | 15.82 | 503.9 |
| Q1 style, optimized | 16.6733 | 15.72 | 487.7 |
| Q3 style, unoptimized | 13.3624 | 19.62 | 3574.3 |
| Q3 style, optimized | 12.3050 | 21.30 | 3500.8 |
| Q6 style, unoptimized | 10.8782 | 24.10 | 364.3 |
| Q6 style, optimized | 6.5713 | 39.89 | 330.0 |

Batch size 1 is the same pull engine with a one-row batch; it is not a separate,
highly tuned row-store engine. The observed gap includes per-batch dispatch,
buffer handling and expression evaluation overhead. The default 2,048-row size
is not the fastest in every test. Larger buffers increase memory even when
latency changes little. The key domain is an input parameter, not a guarantee
that every key appears; raw `output_rows` gives the actual number of groups.

Q1's optimized and unoptimized latency is effectively similar in this short run.
Q3 and Q6 benefit more on this synthetic input. These are limited TPC-H-style
plans, not official TPC-H queries or scores. No comparison against DuckDB,
ClickHouse, SQLite or another engine is claimed.

For 2,048 all-valid values, isolated kernel medians were:

| Kernel | Scalar (ns) | NEON (ns) |
|---|---:|---:|
| int64 `< constant` | 572.43 | 574.62 |
| double addition | 578.21 | 327.22 |

Double addition improved in this case. Integer comparison did not, consistent
with the cost of extracting two 64-bit comparison masks into byte booleans.
That explanation is a hypothesis based on the implementation; no hardware
counter evidence was collected on this Mac. The README makes no blanket SIMD
speedup claim. Sparse/NULL-bearing expressions also use scalar evaluation.

## Measured scan-copy change

Initial scanning appended one value and validity bit at a time in row order.
The revised scan copies contiguous ranges by column, copies numeric payloads in
bulk, and transfers validity bits in word-sized chunks; string bytes are copied
as one span with adjusted offsets. Mask tests cover misaligned source and
destination words, NULLs and tails before measurement.

| Same query and inputs | Before (ms) | After (ms) |
|---|---:|---:|
| Filter / 262144 rows / batch 2048 / 50% | 4.2246 | 2.1121 |
| Optimized Q6 style / 262144 rows | 11.1497 | 6.5713 |

The before and after were separate runs under the same compiler settings, after
correctness checks. No compilation was running alongside either saved benchmark
run. The improvement is not attributed solely to SIMD; SIMD kernels were the
same in both builds. A local patch records exactly the scan-copy changes.

## Raw data and reproduction

- [Full JSON](../benchmarks/results/apple-m3-pro.json): iteration measurements,
  repetitions and aggregate statistics.
- [Full table](../benchmarks/results/apple-m3-pro.md): all 102 configurations.
- [Console output](../benchmarks/results/apple-m3-pro.txt).
- [Before-scan JSON](../benchmarks/results/before-scan-bulk.json) and
  [scan-copy patch](../benchmarks/results/scan-bulk.patch).
- [Environment and source hashes](../benchmarks/results/environment.txt).

```sh
./build-release/vqe_bench --benchmark_min_time=0.05s \
  --benchmark_repetitions=3 --benchmark_out=results.json \
  --benchmark_out_format=json
python3 scripts/benchmark_report.py results.json > results.md
```

Argument order is:

| Family | Arguments |
|---|---|
| KernelLess / KernelAdd | values / mode (0 scalar, 1 auto) |
| FilterScan | input rows / batch size / percentage threshold / mode |
| Aggregation | input rows / key domain / skew (0 uniform, 1 hot key) |
| Join | probe rows / build rows / distribution (0 unique, 1 duplicate, 2 hot probe) |
| Analytical | lineitems / query number / optimizer (0 off, 1 on) |

Join's duplicate case has four build rows per key. Its hot-probe case sends
approximately 90% of probes to one key while build keys remain unique. All data
is deterministic. “Unoptimized” analytical cases skip the logical rewrite pass;
physical lowering still chooses hash-join build sides and attaches available
scan pruning metadata in both modes. Filter keys are distributed over [0,100); threshold percentage
approximates selectivity, and actual output counts are recorded.

For a focused, longer repeat, use an anchored exact filter, for example:

```sh
./build-release/vqe_bench \
  '--benchmark_filter=^FilterScan/262144/2048/50/1$' \
  --benchmark_min_time=1s --benchmark_repetitions=10
```

To reconstruct the earlier scan, use a disposable local copy, apply
`patch -R -p1 < benchmarks/results/scan-bulk.patch`, and build its benchmark with
`BUILD_TESTING=OFF` (the latest tests also exercise the newly added bulk-copy
API). No Git history or remote is needed. Restore the patch to return to the
current implementation. Do not compare a sanitized build to a release build.

## Memory interpretation

`input_buffer_bytes` measures source column capacities. `operator_buffer_bytes`
measures retained operator and output buffers after draining a query, not peak
heap usage. The hash index, payload, aggregate states, duplicate links and
scratch buffers remain owned at that point. Temporary allocations during
rehashing and sort, metadata objects, allocator overhead and expression objects
are excluded; details are in the architecture document.

`process_peak_rss_bytes` uses `getrusage` (native bytes on macOS, converted from
KiB on Linux). In a full run it is a process-wide high-water mark that can include
earlier cases, libraries, input generation and allocator retention. Run one
anchored benchmark per process when comparing RSS; do not interpret it as
per-query memory. Raw files preserve these counters without relabeling them as
exact engine heap usage.

## Linux perf

On a Linux host with perf permissions, build Release and run an isolated case:

```sh
perf stat -r 5 \
  -e cycles,instructions,cache-references,cache-misses,branches,branch-misses \
  ./build-release/vqe_bench \
  '--benchmark_filter=^Aggregation/262144/65536/0$' \
  --benchmark_min_time=2s --benchmark_repetitions=1
```

IPC is instructions divided by cycles. Cache miss rate is cache-misses divided
by cache-references; branch miss rate is branch-misses divided by branches.
Event availability depends on CPU and kernel permissions. These process-level
counters include setup and framework overhead, so lengthen the timed run and
keep the same case when comparing changes. No perf results were collected on
this macOS host.
