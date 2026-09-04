#include "vqe/parquet.hpp"
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>
#include <parquet/statistics.h>
#include <algorithm>
#include <limits>

namespace vqe {
namespace {
void check(const arrow::Status& s) { if (!s.ok()) throw std::runtime_error(s.ToString()); }
template<class T> T unwrap(arrow::Result<T> r) { if (!r.ok()) throw std::runtime_error(r.status().ToString()); return std::move(r).ValueOrDie(); }
std::unique_ptr<parquet::arrow::FileReader> open(const std::string& path, arrow::MemoryPool* pool) {
  return unwrap(parquet::arrow::OpenFile(unwrap(arrow::io::ReadableFile::Open(path, pool)), pool));
}
Type type(const std::shared_ptr<arrow::DataType>& t) {
  switch (t->id()) {
    case arrow::Type::BOOL: return Type::Boolean; case arrow::Type::INT64: return Type::Int64;
    case arrow::Type::DOUBLE: return Type::Double; case arrow::Type::STRING: return Type::String;
    default: throw std::invalid_argument("unsupported Parquet column type: " + t->ToString());
  }
}
Schema select(const Schema& s, const std::vector<ColumnId>& ids) { Schema out; for (auto id : ids) out.push_back(s[column_index(s, id)]); return out; }
void bounds(ColumnStatistics& out, Type t, const std::shared_ptr<parquet::Statistics>& s) {
  if (!s || !s->HasMinMax()) return;
  switch (t) {
    case Type::Boolean: {
      auto v = std::static_pointer_cast<parquet::BoolStatistics>(s); out.minimum = v->min(); out.maximum = v->max(); break;
    }
    case Type::Int64: {
      auto v = std::static_pointer_cast<parquet::Int64Statistics>(s); out.minimum = v->min(); out.maximum = v->max(); break;
    }
    case Type::Double: {
      auto v = std::static_pointer_cast<parquet::DoubleStatistics>(s); out.minimum = v->min(); out.maximum = v->max(); break;
    }
    case Type::String: {
      auto v = std::static_pointer_cast<parquet::ByteArrayStatistics>(s);
      out.minimum = std::string(reinterpret_cast<const char*>(v->min().ptr), v->min().len);
      out.maximum = std::string(reinterpret_cast<const char*>(v->max().ptr), v->max().len); break;
    }
  }
}
}
ParquetSource::ParquetSource(std::string path, ColumnId first) : path_(std::move(path)) {
  auto reader = open(path_, arrow::default_memory_pool()); std::shared_ptr<arrow::Schema> schema; check(reader->GetSchema(&schema));
  if (static_cast<std::uint64_t>(first) + static_cast<std::uint64_t>(schema->num_fields()) > std::uint64_t{1} + std::numeric_limits<ColumnId>::max()) throw std::length_error("column ID overflow");
  for (int i = 0; i < schema->num_fields(); ++i) schema_.push_back({first + static_cast<ColumnId>(i), type(schema->field(i)->type()), schema->field(i)->name()});
  auto metadata = reader->parquet_reader()->metadata(); rows_ = static_cast<std::size_t>(metadata->num_rows());
  statistics_.resize(schema_.size()); std::vector<bool> unknown_bounds(schema_.size(), false);
  for (int g = 0; g < metadata->num_row_groups(); ++g) {
    const auto group = metadata->RowGroup(g); Statistics stats(schema_.size()); group_sizes_.push_back(static_cast<std::size_t>(group->num_rows()));
    for (std::size_t c = 0; c < schema_.size(); ++c) {
      auto& s = stats[c]; auto ps = group->ColumnChunk(static_cast<int>(c))->statistics();
      s.rows = group_sizes_.back(); s.nulls_known = ps && ps->HasNullCount();
      if (s.nulls_known) s.nulls = static_cast<std::size_t>(ps->null_count());
      // Parquet min/max does not establish absence of NaNs; never prune != on
      // constant double ranges without decoding them.
      s.has_nan = schema_[c].type == Type::Double;
      bounds(s, schema_[c].type, ps);
      if ((!s.minimum || !s.maximum) && (!s.nulls_known || s.rows != s.nulls)) unknown_bounds[c] = true;
    }
    merge_statistics(statistics_, stats); groups_.push_back(std::move(stats));
  }
  for (std::size_t c = 0; c < schema_.size(); ++c) if (unknown_bounds[c]) { statistics_[c].minimum.reset(); statistics_[c].maximum.reset(); }
}
OperatorPtr ParquetSource::scan(std::vector<ColumnId> columns, ExecutionOptions options, ExprPtr predicate) const {
  return std::make_unique<ParquetScan>(*this, std::move(columns), options, std::move(predicate));
}
struct ParquetScan::Impl {
  arrow::ProxyMemoryPool pool{arrow::default_memory_pool()};
  std::unique_ptr<parquet::arrow::FileReader> file;
  std::unique_ptr<arrow::RecordBatchReader> reader;
  std::shared_ptr<arrow::RecordBatch> current;
  std::size_t selected = 0, skipped = 0, columns = 0, remaining = 0;
};
ParquetScan::ParquetScan(const ParquetSource& source, std::vector<ColumnId> columns, ExecutionOptions options, ExprPtr predicate)
  : Operator(select(source.schema(), columns), options), impl_(std::make_unique<Impl>()) {
  if (predicate && expression_type(predicate, source.schema()) != Type::Boolean) throw std::invalid_argument("Parquet pruning predicate must be boolean");
  std::vector<int> groups, indices;
  for (std::size_t g = 0; g < source.row_groups().size(); ++g) {
    if (may_match(predicate, source.schema(), source.row_groups()[g])) { groups.push_back(static_cast<int>(g)); impl_->remaining += source.row_group_sizes()[g]; }
    else ++impl_->skipped;
  }
  impl_->selected = groups.size(); impl_->columns = columns.size();
  if (groups.empty() || columns.empty()) return;
  for (auto id : columns) indices.push_back(static_cast<int>(column_index(source.schema(), id)));
  impl_->file = open(source.name(), &impl_->pool);
  impl_->file->set_use_threads(false); impl_->file->set_batch_size(static_cast<std::int64_t>(options.batch_size));
  impl_->reader = unwrap(impl_->file->GetRecordBatchReader(groups, indices));
}
ParquetScan::~ParquetScan() = default;
bool ParquetScan::next(Batch& out) {
  prepare(out);
  if (!impl_->remaining) return false;
  if (schema_.empty()) { const auto n = std::min(out.capacity, impl_->remaining); impl_->remaining -= n; out.finish(n); return true; }
  check(impl_->reader->ReadNext(&impl_->current));
  if (!impl_->current) { impl_->remaining = 0; return false; }
  const auto n = impl_->current->num_rows();
  for (std::size_t c = 0; c < schema_.size(); ++c) {
    const auto& array = impl_->current->column(static_cast<int>(c)); auto& dest = out.columns[c];
    for (std::int64_t r = 0; r < n; ++r) {
      if (array->IsNull(r)) { dest.append_null(); continue; }
      switch (dest.type()) {
        case Type::Boolean: dest.append<std::uint8_t>(static_cast<const arrow::BooleanArray&>(*array).Value(r)); break;
        case Type::Int64: dest.append(static_cast<const arrow::Int64Array&>(*array).Value(r)); break;
        case Type::Double: dest.append(static_cast<const arrow::DoubleArray&>(*array).Value(r)); break;
        case Type::String: dest.append_string(static_cast<const arrow::StringArray&>(*array).GetView(r)); break;
      }
    }
  }
  impl_->remaining -= static_cast<std::size_t>(n); out.finish(static_cast<std::size_t>(n)); return n != 0;
}
std::size_t ParquetScan::allocated_bytes() const { return static_cast<std::size_t>(impl_->pool.bytes_allocated()); }
std::size_t ParquetScan::groups_selected() const { return impl_->selected; }
std::size_t ParquetScan::groups_skipped() const { return impl_->skipped; }
std::size_t ParquetScan::columns_decoded() const { return impl_->columns; }
}
