# VQE — Vectorized Analytical Query Engine

A standalone C++20 analytical execution engine with columnar batches, SQL-like
NULL semantics, vector expressions, hash aggregation, duplicate-preserving hash
joins, logical and physical planning, rule optimization, SIMD kernels, and an
optional Apache Arrow/Parquet scan adapter.

Queries are built as typed plans. There is no SQL parser. The project focuses on
execution mechanics and measurable tradeoffs rather than language coverage.
Everything builds locally; CMake never downloads dependencies.

## Build and run

Requires CMake 3.20+, a C++20 compiler with GCC/Clang builtins, and Make or Ninja.
The core has no third-party runtime or test dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/vqe_demo q6 100000
./build/vqe_demo q1 100000
./build/vqe_demo q3 100000
```

The demo prints the optimized physical plan, result rows, execution latency and
retained operator buffer bytes. Its synthetic Q1/Q3/Q6-style queries are not
TPC-H compliant and their timings are not TPC-H scores. Dates are integer day
IDs, and the generated data is deterministic.

For the dependencies already installed locally in this checkout:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DVQE_PARQUET=ON -DVQE_ARROW_ROOT="$PWD/.deps/python/pyarrow" \
  -DVQE_GOOGLE_BENCHMARK=ON -DCMAKE_PREFIX_PATH="$PWD/.deps/install"
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure
```

For another machine, supply installed Arrow/Parquet 21+ CMake packages via
`CMAKE_PREFIX_PATH`, or set `VQE_ARROW_ROOT` to a local PyArrow directory containing
`include/`, `libarrow` and `libparquet`. Google Benchmark is optional and located
with `find_package(benchmark CONFIG REQUIRED)` when enabled. See
[dependency versions and setup](docs/dependencies.md). Neither dependency is
vendored or fetched automatically.

## Architecture and execution

```mermaid
flowchart LR
  Q[Query plan API] --> L[Logical plan]
  L --> O[Rule optimizer]
  O --> P[Physical plan]
  P --> E[Vectorized pull operators]
  E --> S[Memory row groups / Arrow Parquet]
```

Operators consume and produce owned batches through `next(Batch&)`. At the
default 2,048-row capacity, virtual dispatch happens per batch. Filter maintains
a selection instead of copying qualifying rows; projection materializes its
output. Aggregation, join build and sort are blocking, while scan, filter,
projection, join probe and limit emit batches incrementally.

Columns support int64, double, boolean and variable-width strings. NULL validity
is bit-packed; boolean payloads are bytes. Selections use implicit identity
ranges or 16-bit physical indices. Strings use offset arrays and a byte arena.
Resetting a batch retains its capacity. Blocking operators use full-width row
IDs and can store more rows than fit in an execution batch.

Expressions bind column IDs once and evaluate each expression node over vectors.
They support column references, typed constants, checked numeric arithmetic,
comparisons, AND/OR/NOT, and IS NULL. NULL propagation and boolean truth tables
match SQL's three-valued logic. Integer overflow and division by zero yield NULL;
there are no implicit casts.

The hash index uses contiguous 16-byte buckets, linear probing, a 70% load limit
and resizing. Aggregation separates group keys from contiguous numeric state;
joins separate build columns from duplicate row-index chains. Composite keys,
NULL groups, COUNT(*), COUNT(column), SUM, MIN/MAX, and duplicate inner joins are
implemented. Numeric MIN/MAX and SUM accept int64 and double. Sort supports
multiple keys, independent NULL placement, and stable ties.

Ownership is RAII throughout: each operator owns its child operators and scratch
buffers; sources share immutable input data and metadata. Scan copies contiguous
column ranges, projection copies selected results, and hot numeric loops avoid
per-row heap allocations. There is no spilling or query memory budget.

See [architecture and tradeoffs](docs/architecture.md) for batch invariants,
hash-table layout, numeric edge cases, lifetime rules, memory accounting and
rejected alternatives.

## Planning and optimizer

Logical and physical nodes are separate from execution operators. Stable column
IDs survive pruning and join build-side swaps. The rule optimizer implements:

- Constant folding and NULL-safe boolean simplification.
- Duplicate/true filter removal and identity projection removal.
- Predicate pushdown through inner joins, grouping keys, sort, and column aliases.
- Column and unused aggregate pruning.
- Statistics-guided filter ordering and hash-join build-side selection.

Memory sources collect row/null counts, min/max and approximate distinct counts.
The estimates use simple uniformity assumptions; join output estimates are
heuristic. This is a rule optimizer with statistics, not a sophisticated cost
optimizer or join-order search engine. `explain()` exposes physical strategies,
estimated cardinalities, scan columns and pruning predicates.

A small constructed query:

```cpp
#include "vqe/plan.hpp"
using namespace vqe;
// input is a shared_ptr<const Table>, with #1 int64 key and #2 double value.
auto query = logical::aggregate(
    logical::filter(logical::scan(input),
                    binary(ExprKind::Less, col(1), lit(std::int64_t{100}))),
    {1}, {{{3, Type::Double, "total"}, AggregateKind::Sum, 2}});
auto physical = lower(optimize(query));
auto cursor = execute(*physical);
Batch batch;
while (cursor->next(batch)) {
    // Consume active rows using batch.selection[i].
}
```

The complete workload definitions are in [src/workloads.cpp](src/workloads.cpp).

## Parquet support

With `VQE_PARQUET=ON`, `ParquetSource` integrates Apache Arrow's C++ reader.
It supports flat bool/int64/double/UTF-8 columns, prunes unneeded columns before
decoding, reads bounded record batches, and skips row groups using footer min/max
and NULL statistics. Missing statistics retain groups. Exact predicates are
still evaluated by a filter. Zero-column scans can return cardinality entirely
from metadata. File contents must remain immutable during a query.

```cpp
#include "vqe/parquet.hpp"
auto file = std::make_shared<vqe::ParquetSource>("lineitem.parquet", 1);
auto query = vqe::logical::scan(file); // Column IDs start at 1 in file order.
```

There is no homemade format parser, page-index pruning, nested-type support,
dictionary-preserving execution, or Parquet writer API in the engine. Tests
create actual Parquet files through Arrow to verify decoding and pruning.

## SIMD

Dense all-valid int64 `< constant` and double addition have scalar and SIMD
implementations. x86 AVX2 is isolated behind target attributes and runtime CPU
detection; ARM64 uses NEON. Sparse or NULL-bearing expressions use general
scalar evaluation. The explicit scalar kernel translation unit disables compiler
auto-vectorization so its benchmark is a genuine scalar baseline. Other engine
code uses normal compiler optimization.

ARM NEON and scalar execution are tested on this checkout's host. The AVX2 source
cross-compiles for x86-64 but has not been executed on this ARM machine. SIMD is not claimed for hash probing,
all expressions, or the complete query pipeline. A SIMD implementation can be
neutral or slower; the measurements report that rather than assuming a win.

## Tests

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DVQE_SANITIZE=ON
cmake --build build-sanitize -j
ctest --test-dir build-sanitize --output-on-failure
```

Enable Parquet with the same flags as the release build to include its file
integration suite. There are 26 core test cases plus the Parquet integration
suite. Tests cover validity-word boundaries, selections, strings,
sparse expressions, three-valued logic, integer overflow, randomized aggregate
and join references, collisions and resizing, composite keys, duplicate chains,
payloads beyond 65,535 rows, NaN/signed zero, sort/limit, optimizer equivalence and
semantic barriers, pruning, SIMD tails, and analytical plans. Tests use checks
that remain active in Release builds. Sanitizers instrument the engine and tests;
prebuilt third-party shared libraries are not sanitizer-instrumented.

## Known limitations and next experiments

Single-threaded, in-memory execution; no SQL parser, transactions, persistence
manager, spill files, memory quotas, outer joins, DISTINCT aggregates, decimals,
timestamps, implicit casts or collation. Integer SUM overflows to NULL rather
than widening. Floating-point sums follow input order. Group and join output
order is unspecified without Sort. Plans assume unique column IDs and immutable
sources. Source statistics and query estimates can be inaccurate under skew.

Useful next experiments are x86 execution validation, SIMD mask packing,
late materialization and Arrow-backed views, aggregate specialization, bounded
Top-N, memory budgets and spill partitions, parallel pipelines, and broader
Parquet logical types. These are future work, not implemented features.
