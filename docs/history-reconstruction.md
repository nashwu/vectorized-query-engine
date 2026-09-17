# Reconstructed development history

This local history was created on September 17, 2026 at the author's request.
The author reported working on this project from August 1 through September 17,
2026, without making commits. The repository had no commits or remotes when the
reconstruction began.

The 48 commits group the existing implementation, tests and documentation
into a dependency-ordered sequence. They are assigned across 35 days, with one
or two commits on those days and no commits on 13 days of the reported period.
Related changes share dates; this grouping does not establish which days the
author actually worked or made individual changes.

These are reconstructed snapshots, not recovered historical revisions. Exact
daily work dates and intermediate source versions were not recorded. Author and
committer dates are estimated allocations within the reported period. Times are
00:00:00 for a day's first commit and 00:01:00 for its second, in America/Chicago
(UTC-05:00); these are ordering placeholders, not recorded work times. Each
commit body identifies the reconstruction. File modification times were not
treated as evidence of daily development.

The initial reconstruction used one commit per day. On September 17, at the
author's request, the commit subjects were rewritten in plain language and the
same changes were assigned to fewer days. The uneven spacing is also an
estimate; it is not more historically certain than the initial schedule.

The sequence follows the documented core, blocking operators, planning, storage
and performance stages. Shared hashing and statistics foundations appear when
needed by earlier components. Test commits introduce the already existing tests.
The pre-optimization scan comes from reversing the saved scan-copy patch; other
intermediate subsets were assembled from the final source. No empty commits or
new features were added to fill days.

Original benchmark timestamps, results, source hashes and the September 16
validation record are preserved. Those records describe when measurements were
actually recorded; assigned commit dates do not move those events. Historical
statements about having no commits were clarified to refer to September 16.

## Verification performed during reconstruction

Every snapshot was configured and built with CMake on September 17, 2026.
The available existing tests passed at each snapshot from their introduction
onwards. Builds enabled the local Arrow/Parquet dependency after its integration
and Google Benchmark after its introduction. The final suite has 26 core cases
and a separate Parquet integration test. Verification dates are actual current
dates, not the assigned development dates. No new performance measurements were
substituted for the archived results.

All original source code, tests, build configuration, scripts and benchmark files
are byte-for-byte identical at the final commit. Changes to existing files are
limited to the README and three documentation files clarifying history, plus
this new document. Ignored dependencies, build directories and local artifacts
were excluded from commits. A source-and-Git-metadata backup was saved outside
the repository before reconstruction; generated directories remain in place.

## Assigned commit sequence

| Assigned date | Commit |
|---|---|
| 2026-08-01 | Set up the C++20 project |
| 2026-08-01 | Add column vectors and reusable batches |
| 2026-08-03 | Test validity masks and selection boundaries |
| 2026-08-03 | Check string storage and buffer reuse |
| 2026-08-04 | Add the hash index and columnar row storage |
| 2026-08-06 | Add scalar comparison and addition kernels |
| 2026-08-06 | Evaluate expressions over column vectors |
| 2026-08-07 | Add table statistics, scans, filters and projections |
| 2026-08-09 | Test arithmetic overflow and division errors |
| 2026-08-09 | Check boolean expressions with NULL values |
| 2026-08-10 | Test filtering and projection across batch sizes |
| 2026-08-12 | Check empty scans and zero-column results |
| 2026-08-13 | Add grouped and global aggregation |
| 2026-08-13 | Compare aggregation with a scalar reference |
| 2026-08-15 | Add hash joins that preserve duplicate matches |
| 2026-08-16 | Check composite joins against a nested-loop reference |
| 2026-08-18 | Add limits and stable sorting |
| 2026-08-19 | Test blocking operators with more than 65535 rows |
| 2026-08-19 | Check NaN and signed-zero behavior |
| 2026-08-20 | Explain batch ownership and blocking operators |
| 2026-08-22 | Add logical plans and query optimization |
| 2026-08-23 | Test constant folding and NULL-safe simplification |
| 2026-08-23 | Check projection pushdown and column pruning |
| 2026-08-24 | Test join pushdown and build-side selection |
| 2026-08-26 | Keep LIMIT and global aggregates as optimizer barriers |
| 2026-08-27 | Compare optimized queries with scalar results |
| 2026-08-28 | Test row-group statistics and pruning |
| 2026-08-28 | Check that pruning keeps all matching rows |
| 2026-08-30 | Add deterministic Q1, Q3 and Q6 workloads |
| 2026-08-31 | Compare analytical queries with reference results |
| 2026-08-31 | Add a query demo with plan output |
| 2026-09-01 | Explain optimizer rules and estimate limitations |
| 2026-09-03 | Set up local Arrow dependency discovery |
| 2026-09-04 | Add Parquet scans through Arrow |
| 2026-09-05 | Test Parquet decoding and metadata pruning |
| 2026-09-05 | Check schema aliases when reusing output batches |
| 2026-09-07 | Add SIMD kernels and compare them with scalar results |
| 2026-09-08 | Add sanitizer and native CPU build options |
| 2026-09-10 | Add benchmarks for kernels and analytical queries |
| 2026-09-10 | Add a script to summarize benchmark results |
| 2026-09-11 | Copy validity masks a word at a time |
| 2026-09-12 | Add bulk column copies and boundary tests |
| 2026-09-12 | Use bulk copies in scans and save the baseline patch |
| 2026-09-13 | Explain storage behavior and memory accounting |
| 2026-09-15 | Expand the build guide and document limitations |
| 2026-09-16 | Explain memory counters and Linux perf commands |
| 2026-09-16 | Save the September 16 benchmarks and validation results |
| 2026-09-17 | Document the reconstructed development history |
