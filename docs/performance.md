# Performance measurements

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
