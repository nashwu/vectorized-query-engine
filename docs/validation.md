# Validation record

Validated on macOS ARM64 with Apple Clang 21, 2026-09-16.

| Configuration | Result |
|---|---|
| Release core, no Arrow or Google Benchmark | Passed |
| Release with Arrow/Parquet 21.0.0 and Google Benchmark 1.9.4 | Passed |
| Debug with Arrow/Parquet | Passed during incremental implementation |
| AddressSanitizer + UndefinedBehaviorSanitizer with Arrow/Parquet | Passed |
| x86-64 cross-compilation of AVX2/dispatch translation unit | Passed |
| AVX2 runtime execution | Not run on ARM host |
| Linux/GCC build, Linux perf | Not run on this macOS host |

The final test executable contains 26 cases. Several cases execute thousands of
checks over deterministic input variations. `vqe_parquet_tests` is a separate
integration executable that creates, queries, and removes actual Parquet files.
Core tests do not use `assert` as their test oracle, so checks stay enabled in
Release. Engine debug assertions supplement explicit batch/schema checks.

Coverage includes:

- Packed validity boundaries, reset/reuse, misaligned bulk copying, string
  offsets, embedded NUL, sparse selections, zero-column and empty batches.
- Arithmetic overflow/division errors, three-valued boolean truth tables,
  comparisons, multi-stage filtering, projection and reused schema aliases.
- Forced same-hash collisions and resizing; grouped/global aggregation, NULL
  keys, empty inputs, overflow and independent randomized map references.
- Both join build sides, composite keys, duplicate outputs spanning batches,
  NULL exclusion, randomized nested-loop references, and build/group stores
  exceeding the 16-bit execution selection range.
- NaN and signed-zero grouping, joins, MIN/MAX, stable sort and LIMIT.
- Constant folding, predicate pushdown, projection pruning, redundant rules,
  LIMIT/global-aggregate barriers, join build choice, unchanged input plans,
  randomized optimizer results against independent scalar computations, and
  optimized/unoptimized analytical workloads.
- Randomized proof-by-testing that row-group pruning has no false negatives
  across comparison operators, reversed operands, NULLs, doubles and strings.
- Scalar/NEON parity with signed extremes, unaligned buffers and tail lengths.
- Parquet typed decoding, projected/reordered columns, min/max and NULL pruning,
  missing statistics, NaN, zero-column scans, empty files and missing-file errors.

The analytical suite compares Q1/Q3/Q6-style optimized and unoptimized results
and independently recomputes Q6 revenue. Join and aggregate randomized tests
provide independent scalar references for the operators used by those plans.
Floating-point plan results are compared with a small tolerance where valid join
order changes can change summation order.

Benchmarking followed correctness checks. The full release sweep has 102 cases
and three repetitions each. The scan-copy comparison uses the same final code
with only the saved scan-copy patch reversed for the baseline. Build/test jobs
were completed before either measured process began. RSS and buffer-accounting
limitations are stated in the performance report.

During the September 16 validation, no commits, remotes, pushes, pull requests,
releases, code uploads or resume-file edits were made. Only explicitly approved
dependency downloads used the network. The later local history reconstruction
is documented in [history reconstruction](history-reconstruction.md).
