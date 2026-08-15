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
class HashJoin final : public Operator {
 public:
  HashJoin(OperatorPtr left, OperatorPtr right, std::vector<ColumnId> left_keys,
           std::vector<ColumnId> right_keys, bool build_right = true);
  bool next(Batch&) override;
  std::size_t allocated_bytes() const override;
 private:
  void build();
  bool load_probe();
  OperatorPtr build_, probe_;
  bool build_right_, built_ = false;
  std::vector<std::size_t> build_keys_, probe_keys_;
  RowStore store_;
  HashIndex index_;
  std::vector<std::size_t> next_, tails_, heads_;
  Batch build_batch_, probe_batch_;
  std::size_t probe_cursor_ = 0, match_ = no_row;
};
}
