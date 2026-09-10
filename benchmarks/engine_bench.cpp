#include "vqe/workloads.hpp"
#include <benchmark/benchmark.h>
#include <algorithm>
#include <bit>
#include <sys/resource.h>

using namespace vqe;
namespace {
std::shared_ptr<Table> numeric(std::size_t n, std::size_t cardinality, ColumnId id = 1, int distribution = 0) {
  auto t = std::make_shared<Table>(Schema{{id, Type::Int64, "key"}, {id + 1, Type::Double, "value"}});
  Batch b(t->schema());
  for (std::size_t i = 0; i < n; ++i) {
    auto key = mix_hash(i + 1) % cardinality;
    if (distribution == 1 && i % 10 != 0) key = 0;
    if (distribution == 2) key = i % cardinality;
    b.columns[0].append(static_cast<std::int64_t>(key)); b.columns[1].append(static_cast<double>(i % 1000));
    if (++b.physical_size == b.capacity) { b.finish(b.physical_size); t->append(b); b.reset(); }
  }
  b.finish(b.physical_size); t->append(b); return t;
}
std::size_t peak_rss() {
  rusage usage{}; if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
  return static_cast<std::size_t>(usage.ru_maxrss);
#else
  return static_cast<std::size_t>(usage.ru_maxrss) * 1024;
#endif
}
void counters(benchmark::State& state, std::size_t n, std::size_t input, std::size_t retained, std::size_t output) {
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
  state.counters["input_buffer_bytes"] = static_cast<double>(input);
  state.counters["operator_buffer_bytes"] = static_cast<double>(retained);
  state.counters["output_rows"] = static_cast<double>(output);
  state.counters["process_peak_rss_bytes"] = static_cast<double>(peak_rss());
}
std::pair<std::size_t, std::size_t> drain(Operator& op) {
  Batch out(op.schema(), op.options().batch_size); std::size_t count = 0, retained = 0;
  while (op.next(out)) { count += out.size(); benchmark::DoNotOptimize(out.columns.data()); }
  retained = op.allocated_bytes() + out.allocated_bytes(); benchmark::DoNotOptimize(count); return {count, retained};
}
void KernelLess(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0)); const auto mode = state.range(1) ? KernelMode::Auto : KernelMode::Scalar;
  std::vector<std::int64_t> input(n); std::vector<std::uint8_t> output(n);
  for (std::size_t i = 0; i < n; ++i) input[i] = std::bit_cast<std::int64_t>(mix_hash(i));
  for (auto _ : state) { less_i64_constant(input.data(), 0, output.data(), n, mode); benchmark::DoNotOptimize(output.data()); benchmark::ClobberMemory(); }
  state.SetLabel(std::string(kernel_backend(mode))); state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
void KernelAdd(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0)); const auto mode = state.range(1) ? KernelMode::Auto : KernelMode::Scalar;
  std::vector<double> a(n, 0.25), b(n, 0.5), out(n);
  for (auto _ : state) { add_f64(a.data(), b.data(), out.data(), n, mode); benchmark::DoNotOptimize(out.data()); benchmark::ClobberMemory(); }
  state.SetLabel(std::string(kernel_backend(mode))); state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
void FilterScan(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0)), batch = static_cast<std::size_t>(state.range(1));
  const auto mode = state.range(3) ? KernelMode::Auto : KernelMode::Scalar;
  auto t = numeric(n, 100); auto predicate = binary(ExprKind::Less, col(1), lit(state.range(2)));
  std::size_t memory = 0, output = 0;
  for (auto _ : state) { Filter op(std::make_unique<Scan>(t, ExecutionOptions{batch, mode}), predicate); auto r = drain(op); output = r.first; memory = std::max(memory, r.second); }
  state.SetLabel(std::string(kernel_backend(mode))); counters(state, n, t->allocated_bytes(), memory, output);
}
void Aggregation(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0)), card = static_cast<std::size_t>(state.range(1));
  auto t = numeric(n, card, 1, static_cast<int>(state.range(2))); std::size_t memory = 0, output = 0;
  for (auto _ : state) {
    HashAggregate op(std::make_unique<Scan>(t), {1}, {{{3, Type::Int64, "n"}, AggregateKind::Count, {}}, {{4, Type::Double, "sum"}, AggregateKind::Sum, 2}});
    auto r = drain(op); output = r.first; memory = std::max(memory, r.second);
  }
  counters(state, n, t->allocated_bytes(), memory, output);
}
void Join(benchmark::State& state) {
  const auto probe_n = static_cast<std::size_t>(state.range(0)), build_n = static_cast<std::size_t>(state.range(1));
  const auto distribution = state.range(2);
  const auto domain = distribution == 1 ? std::max(std::size_t{1}, build_n / 4) : build_n;
  auto build = numeric(build_n, domain, 3, 2), probe = numeric(probe_n, domain, 1, distribution == 2 ? 1 : 0);
  std::size_t memory = 0, output = 0;
  for (auto _ : state) {
    HashJoin op(std::make_unique<Scan>(probe), std::make_unique<Scan>(build), {1}, {3}, true);
    auto r = drain(op); output = r.first; memory = std::max(memory, r.second);
  }
  counters(state, probe_n + build_n, probe->allocated_bytes() + build->allocated_bytes(), memory, output);
}
void Analytical(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0)); auto data = workloads::generate(n);
  Plan p = state.range(1) == 1 ? workloads::q1(data) : (state.range(1) == 3 ? workloads::q3(data) : workloads::q6(data));
  auto physical = lower(state.range(2) ? optimize(p) : p); std::size_t memory = 0, output = 0;
  for (auto _ : state) { auto op = execute(*physical); auto r = drain(*op); output = r.first; memory = std::max(memory, r.second); }
  counters(state, n, data.lineitem->allocated_bytes() + data.orders->allocated_bytes() + data.customer->allocated_bytes(), memory, output);
}
void filter_args(benchmark::internal::Benchmark* b) {
  for (auto n : {65536, 262144, 1048576}) for (auto batch : {1, 256, 1024, 2048, 8192})
    for (auto pct : {1, 50, 99}) b->Args({n, batch, pct, 1});
  for (auto batch : {256, 2048, 8192}) b->Args({262144, batch, 50, 0});
}
void aggregation_args(benchmark::internal::Benchmark* b) {
  for (auto n : {65536, 262144}) for (auto card : {1, 64, 4096, 65536}) for (auto skew : {0, 1}) b->Args({n, card, skew});
}
void join_args(benchmark::internal::Benchmark* b) {
  for (auto probe : {65536, 262144}) for (auto build : {1024, 16384, 65536}) for (auto dist : {0, 1, 2}) b->Args({probe, build, dist});
}
void analytical_args(benchmark::internal::Benchmark* b) {
  for (auto n : {65536, 262144}) for (auto q : {1, 3, 6}) for (auto optimized : {0, 1}) b->Args({n, q, optimized});
}
BENCHMARK(KernelLess)->Args({2048, 0})->Args({2048, 1})->Args({262144, 0})->Args({262144, 1});
BENCHMARK(KernelAdd)->Args({2048, 0})->Args({2048, 1})->Args({262144, 0})->Args({262144, 1});
BENCHMARK(FilterScan)->Apply(filter_args);
BENCHMARK(Aggregation)->Apply(aggregation_args);
BENCHMARK(Join)->Apply(join_args);
BENCHMARK(Analytical)->Apply(analytical_args);
}
BENCHMARK_MAIN();
