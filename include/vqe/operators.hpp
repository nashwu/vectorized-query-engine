#pragma once
#include "vqe/expression.hpp"
#include "vqe/statistics.hpp"
#include <memory>

namespace vqe {
struct ExecutionOptions { std::size_t batch_size = default_batch_size; KernelMode kernels = KernelMode::Auto; };
class Table {
 public:
  explicit Table(Schema schema) : schema_(std::move(schema)), statistics_(schema_.size()) { validate_schema(schema_); }
  void append(const Batch&);
  const Schema& schema() const { return schema_; }
  const std::vector<Batch>& groups() const { return groups_; }
  std::size_t row_count() const { return rows_; }
  const Statistics& statistics() const { return statistics_; }
  const std::vector<Statistics>& group_statistics() const { return group_statistics_; }
  std::size_t allocated_bytes() const;
 private:
  Schema schema_;
  std::vector<Batch> groups_;
  std::size_t rows_ = 0;
  Statistics statistics_;
  std::vector<Statistics> group_statistics_;
};
class Operator {
 public:
  virtual ~Operator() = default;
  virtual bool next(Batch&) = 0;
  virtual std::size_t allocated_bytes() const = 0;
  const Schema& schema() const { return schema_; }
  const ExecutionOptions& options() const { return options_; }
 protected:
  Operator(Schema s, ExecutionOptions o) : schema_(std::move(s)), options_(o) {
    validate_schema(schema_);
    if (o.batch_size == 0 || o.batch_size > max_batch_size) throw std::invalid_argument("invalid execution batch size");
  }
  void prepare(Batch&) const;
  Schema schema_;
  ExecutionOptions options_;
};
using OperatorPtr = std::unique_ptr<Operator>;
class Scan final : public Operator {
 public:
  explicit Scan(std::shared_ptr<const Table>, ExecutionOptions = {});
  Scan(std::shared_ptr<const Table>, std::vector<ColumnId> columns, ExecutionOptions = {}, ExprPtr pruning_predicate = {});
  bool next(Batch&) override;
  std::size_t allocated_bytes() const override { return 0; }
  std::size_t groups_read() const { return groups_read_; }
  std::size_t groups_skipped() const { return groups_skipped_; }
 private:
  std::shared_ptr<const Table> table_;
  std::vector<std::size_t> indices_;
  std::size_t group_ = 0, row_ = 0;
  ExprPtr pruning_predicate_;
  std::size_t groups_read_ = 0, groups_skipped_ = 0;
};
class Filter final : public Operator {
 public:
  Filter(OperatorPtr, ExprPtr);
  bool next(Batch&) override;
  std::size_t allocated_bytes() const override;
 private:
  OperatorPtr child_;
  Evaluator predicate_;
  Selection selected_;
};
struct NamedExpression { Field field; ExprPtr expression; };
Schema projection_schema(const std::vector<NamedExpression>&, const Schema& input);
class Projection final : public Operator {
 public:
  Projection(OperatorPtr, std::vector<NamedExpression>);
  bool next(Batch&) override;
  std::size_t allocated_bytes() const override;
 private:
  OperatorPtr child_;
  Batch input_;
  std::vector<Evaluator> expressions_;
};
std::vector<Batch> collect(Operator&);
} // namespace vqe
