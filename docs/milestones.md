# Implementation milestones

Each stage must compile and pass its tests before the next stage. No network
fetching occurs from CMake. Dependencies are explicitly supplied by the builder.

1. **Core:** typed vectors, packed validity, compact selections, reusable batches,
   vector expression evaluator, scan/filter/projection and NULL semantics tests.
2. **Blocking operators:** open-addressed group index, grouped and global
   aggregation, duplicate-preserving inner hash join, limit and sort. Differential
   tests against independent scalar reference computations.
3. **Planning:** immutable expressions and distinct logical/physical trees,
   optimizer rewrites, column-ID-based binding and statistics-guided choices.
   Compare optimized and unoptimized query results.
4. **Storage:** statistics-pruned in-memory row groups plus optional Apache
   Arrow/Parquet adapter. Integration tests require the actual library.
5. **Performance:** isolated SIMD kernels with fallback, reproducible parameter
   sweeps, optional Google Benchmark suite, measured results and limitations.

The public query interface is constructed plans. SQL parsing is intentionally
outside these milestones. Integer arithmetic must never invoke signed-overflow
undefined behavior. NULLs follow SQL three-valued boolean logic. Blocking
operators are in-memory and report their retained allocations; spilling is
future work.

