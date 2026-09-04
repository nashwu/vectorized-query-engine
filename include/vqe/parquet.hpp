#pragma once
#include "vqe/storage.hpp"
namespace vqe {
// Flat bool/int64/double/UTF-8 columns. Column IDs can be rebased for joins.
class ParquetSource final : public Source {
 public:
  explicit ParquetSource(std::string path, ColumnId first_id = 1);
  const Schema& schema() const override { return schema_; }
  const Statistics& statistics() const override { return statistics_; }
  std::size_t row_count() const override { return rows_; }
  std::string name() const override { return path_; }
  OperatorPtr scan(std::vector<ColumnId>, ExecutionOptions, ExprPtr pruning_predicate) const override;
  const std::vector<Statistics>& row_groups() const { return groups_; }
  const std::vector<std::size_t>& row_group_sizes() const { return group_sizes_; }
 private:
  std::string path_;
  Schema schema_;
  Statistics statistics_;
  std::vector<Statistics> groups_;
  std::vector<std::size_t> group_sizes_;
  std::size_t rows_ = 0;
};
class ParquetScan final : public Operator {
 public:
  ParquetScan(const ParquetSource&, std::vector<ColumnId>, ExecutionOptions, ExprPtr pruning_predicate);
  ~ParquetScan() override;
  bool next(Batch&) override;
  std::size_t allocated_bytes() const override;
  std::size_t groups_selected() const;
  std::size_t groups_skipped() const;
  std::size_t columns_decoded() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
