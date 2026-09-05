#include "vqe/parquet.hpp"
#include "vqe/plan.hpp"
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <filesystem>
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <random>

using namespace vqe;
namespace {
void check(const arrow::Status& s) { if (!s.ok()) throw std::runtime_error(s.ToString()); }
template<class T> T unwrap(arrow::Result<T> r) { if (!r.ok()) throw std::runtime_error(r.status().ToString()); return std::move(r).ValueOrDie(); }
void require(bool b) { if (!b) throw std::runtime_error("Parquet integration assertion failed"); }
struct TempDir {
  std::string path;
  TempDir() {
    std::random_device random;
    for (int attempt = 0; attempt < 100; ++attempt) {
      path = (std::filesystem::temp_directory_path() / ("vqe-tests-" + std::to_string(random()))).string();
      if (std::filesystem::create_directory(path)) return;
    }
    throw std::runtime_error("could not create temporary test directory");
  }
  ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
void fixture(const std::string& path, bool stats, bool empty = false) {
  arrow::Int64Builder ids; arrow::DoubleBuilder values; arrow::StringBuilder labels; arrow::BooleanBuilder flags;
  for (std::int64_t i = 0; i < (empty ? 0 : 12); ++i) {
    if (i >= 8) check(ids.AppendNull()); else check(ids.Append(i));
    check(values.Append(i == 2 ? std::nan("") : static_cast<double>(i)));
    if (i == 1) check(labels.AppendNull()); else check(labels.Append("row" + std::to_string(i)));
    check(flags.Append(i % 2 == 0));
  }
  auto schema = arrow::schema({arrow::field("id", arrow::int64()), arrow::field("value", arrow::float64()), arrow::field("label", arrow::utf8()), arrow::field("flag", arrow::boolean())});
  auto table = arrow::Table::Make(schema, {unwrap(ids.Finish()), unwrap(values.Finish()), unwrap(labels.Finish()), unwrap(flags.Finish())});
  parquet::WriterProperties::Builder props; if (!stats) props.disable_statistics();
  auto file = unwrap(arrow::io::FileOutputStream::Open(path));
  check(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), file, 4, props.build())); check(file->Close());
}
std::size_t count(Operator& op) { std::size_t n = 0; Batch b; while (op.next(b)) { b.validate(); n += b.size(); } return n; }
}
int main() {
  try {
    TempDir temp;
    for (bool stats : {false, true}) {
      const auto path = temp.path + (stats ? "/stats.parquet" : "/no-stats.parquet"); fixture(path, stats);
      auto source = std::make_shared<ParquetSource>(path, 10); require(source->row_count() == 12);
      const auto pred = binary(ExprKind::Less, col(10), lit(std::int64_t{3}));
      ParquetScan pruned(*source, {12, 10}, {2}, pred);
      require(count(pruned) == (stats ? 4U : 12U)); require(pruned.groups_skipped() == (stats ? 2U : 0U)); require(pruned.columns_decoded() == 2);
      auto plan = logical::project(logical::filter(logical::scan(source), pred), {{{12, Type::String, "label"}, col(12)}});
      auto op = execute(*lower(optimize(plan)), {1}); auto batches = collect(*op); require(batches.size() == 3);
      require(batches[0].columns[0].value(0) == Value(std::string("row0"))); require(is_null(batches[1].columns[0].value(0)));
      ParquetScan count_only(*source, {}, {3}, {}); require(count(count_only) == 12); require(count_only.columns_decoded() == 0);
      ParquetScan impossible(*source, {10}, {7}, binary(ExprKind::Greater, col(10), lit(std::int64_t{100})));
      require(count(impossible) == (stats ? 0U : 12U));
      auto nulls = execute(*lower(optimize(logical::filter(logical::scan(source), unary(ExprKind::IsNull, col(10))))), {3}); require(count(*nulls) == 4);
      auto nan = execute(*lower(optimize(logical::filter(logical::scan(source), binary(ExprKind::NotEqual, col(11), col(11))))), {5}); require(count(*nan) == 1);
    }
    auto path = temp.path + "/empty.parquet"; fixture(path, true, true); ParquetSource empty(path);
    auto scan = empty.scan({1}, {2}, {}); require(count(*scan) == 0);
    bool error = false; try { ParquetSource missing(temp.path + "/missing.parquet"); } catch (const std::exception&) { error = true; } require(error);
    std::cout << "PASS Parquet decoding, pruning, reordering, NULLs, NaN, missing statistics, empty files and plan integration\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
