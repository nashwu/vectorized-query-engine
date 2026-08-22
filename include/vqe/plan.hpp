#pragma once
#include "vqe/blocking.hpp"
#include "vqe/storage.hpp"

namespace vqe {
enum class LogicalKind { Scan, Filter, Projection, Aggregate, Join, Sort, Limit };
struct LogicalPlan;
using Plan = std::shared_ptr<LogicalPlan>;
struct LogicalPlan {
  LogicalKind kind;
  Plan left, right;
  std::shared_ptr<const Source> source;
  std::vector<ColumnId> columns, keys, right_keys;
  ExprPtr predicate;
  std::vector<NamedExpression> expressions;
  std::vector<AggregateSpec> aggregates;
  std::vector<SortKey> sort_keys;
  std::size_t count = 0;
};
namespace logical {
Plan scan(std::shared_ptr<const Source>);
Plan scan(std::shared_ptr<const Table>, std::string name = "memory");
Plan filter(Plan, ExprPtr);
Plan project(Plan, std::vector<NamedExpression>);
Plan aggregate(Plan, std::vector<ColumnId>, std::vector<AggregateSpec>);
Plan join(Plan, Plan, std::vector<ColumnId>, std::vector<ColumnId>);
Plan sort(Plan, std::vector<SortKey>);
Plan limit(Plan, std::size_t);
}
Schema output_schema(const Plan&);
double estimated_rows(const Plan&);
ExprPtr simplify_expression(const ExprPtr&);
Plan optimize(const Plan&);

enum class PhysicalKind { ColumnScan, SelectionFilter, VectorProjection, HashAggregate, HashJoin, MaterializedSort, Limit };
struct PhysicalPlan {
  PhysicalKind kind;
  Schema schema;
  std::unique_ptr<PhysicalPlan> left, right;
  std::shared_ptr<const Source> source;
  std::vector<ColumnId> columns, keys, right_keys;
  ExprPtr predicate, pruning_predicate;
  std::vector<NamedExpression> expressions;
  std::vector<AggregateSpec> aggregates;
  std::vector<SortKey> sort_keys;
  std::size_t count = 0;
  double rows = 0;
  bool build_right = true;
};
std::unique_ptr<PhysicalPlan> lower(const Plan&);
OperatorPtr execute(const PhysicalPlan&, ExecutionOptions = {});
std::string explain(const Plan&);
std::string explain(const PhysicalPlan&);
}
