#pragma once
#include "vqe/operators.hpp"
namespace vqe {
// Metadata is independent of execution. Each scan owns its cursor; plans share
// immutable source metadata and table data.
class Source {
 public:
  virtual ~Source() = default;
  virtual const Schema& schema() const = 0;
  virtual const Statistics& statistics() const = 0;
  virtual std::size_t row_count() const = 0;
  virtual std::string name() const = 0;
  virtual OperatorPtr scan(std::vector<ColumnId>, ExecutionOptions, ExprPtr pruning_predicate) const = 0;
};
class MemorySource final : public Source {
 public:
  explicit MemorySource(std::shared_ptr<const Table> t, std::string name = "memory") : table_(std::move(t)), name_(std::move(name)) {}
  const Schema& schema() const override { return table_->schema(); }
  const Statistics& statistics() const override { return table_->statistics(); }
  std::size_t row_count() const override { return table_->row_count(); }
  std::string name() const override { return name_; }
  OperatorPtr scan(std::vector<ColumnId> columns, ExecutionOptions options, ExprPtr predicate) const override {
    return std::make_unique<Scan>(table_, std::move(columns), options, std::move(predicate));
  }
 private:
  std::shared_ptr<const Table> table_; std::string name_;
};
}
