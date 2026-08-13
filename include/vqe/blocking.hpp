#pragma once
#include "vqe/operators.hpp"
#include "vqe/hash.hpp"
#include <optional>

namespace vqe {
enum class AggregateKind { Count, Sum, Min, Max };
struct AggregateSpec { Field output; AggregateKind kind; std::optional<ColumnId> input; };
Schema aggregate_schema(const Schema&, const std::vector<ColumnId>&, const std::vector<AggregateSpec>&);
class HashAggregate final : public Operator {
 public:
  HashAggregate(OperatorPtr, std::vector<ColumnId> keys, std::vector<AggregateSpec> aggregates);
  bool next(Batch&) override;
  std::size_t allocated_bytes() const override;
 private:
  struct State { std::int64_t integer = 0; double floating = 0; bool seen = false, overflow = false; };
  void consume();
  OperatorPtr child_;
  std::vector<std::size_t> input_keys_, stored_keys_;
  std::vector<AggregateSpec> aggregates_;
  std::vector<std::size_t> input_values_;
  RowStore keys_;
  HashIndex index_;
  std::vector<State> states_;
  Batch input_;
  bool consumed_ = false;
  std::size_t cursor_ = 0;
};
}
