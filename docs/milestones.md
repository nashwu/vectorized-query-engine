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

## Delivered and validated

All five stages are implemented. Core, blocking operators and planning were
compiled and tested before Parquet integration. Arrow 21.0.0 and Google
Benchmark 1.9.4 were downloaded only after explicit approval and stored under
ignored `.deps/`. Parquet tests use actual generated files. NEON and scalar
kernels were differentially tested before performance measurement; AVX2 remains
unexecuted on this ARM64 host. The measured scan-copy optimization has before/
after data and a reversible patch in `benchmarks/results/`.

There are no commits, remotes, pull requests or published artifacts. Future
experiments and limits are recorded in the README and architecture document.
