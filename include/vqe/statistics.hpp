#pragma once
#include "vqe/expression.hpp"
#include <array>
#include <optional>
namespace vqe {
struct ColumnStatistics {
  std::size_t rows = 0, nulls = 0;
  bool nulls_known = true, has_nan = false;
  std::optional<Value> minimum, maximum;
  std::array<std::uint64_t, 16> distinct_bits{};
  double approximate_distinct() const;
};
using Statistics = std::vector<ColumnStatistics>;
Statistics analyze(const Batch&);
void merge_statistics(Statistics& into, const Statistics& from);
bool may_match(const ExprPtr& predicate, const Schema&, const Statistics&);
double estimate_selectivity(const ExprPtr&, const Schema&, const Statistics&);
}
