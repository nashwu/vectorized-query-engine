#include "vqe/statistics.hpp"
#include "vqe/hash.hpp"
#include <algorithm>
#include <bit>
#include <cmath>

namespace vqe {
namespace {
int compare_value(const Value& a, const Value& b) {
  if (a.index() != b.index() || is_null(a)) throw std::invalid_argument("statistics type mismatch");
  return a < b ? -1 : (a > b ? 1 : 0);
}
struct Predicate { ColumnId column; ExprKind kind; Value value; };
std::optional<Predicate> simple(const ExprPtr& e) {
  if (!e || !e->left || !e->right || e->kind < ExprKind::Equal || e->kind > ExprKind::GreaterEqual) return {};
  if (e->left->kind == ExprKind::Column && e->right->kind == ExprKind::Literal) return Predicate{e->left->column, e->kind, e->right->literal};
  if (e->right->kind != ExprKind::Column || e->left->kind != ExprKind::Literal) return {};
  auto kind = e->kind;
  if (kind == ExprKind::Less) kind = ExprKind::Greater;
  else if (kind == ExprKind::Greater) kind = ExprKind::Less;
  else if (kind == ExprKind::LessEqual) kind = ExprKind::GreaterEqual;
  else if (kind == ExprKind::GreaterEqual) kind = ExprKind::LessEqual;
  return Predicate{e->right->column, kind, e->left->literal};
}
}
double ColumnStatistics::approximate_distinct() const {
  std::size_t bits = 0; for (auto b : distinct_bits) bits += static_cast<std::size_t>(std::popcount(b));
  if (bits == 0 || bits == 1024) return static_cast<double>(rows - nulls);
  return std::min(static_cast<double>(rows - nulls), -1024.0 * std::log(1.0 - static_cast<double>(bits) / 1024.0));
}
Statistics analyze(const Batch& b) {
  Statistics out(b.columns.size());
  for (std::size_t c = 0; c < b.columns.size(); ++c) {
    const auto& column = b.columns[c]; auto& stat = out[c]; stat.rows = b.size();
    auto min = no_row, max = no_row;
    for (std::size_t j = 0; j < b.size(); ++j) {
      const auto r = b.selection[j];
      if (!column.validity().valid(r)) { ++stat.nulls; continue; }
      const auto h = hash_cell(column, r) % 1024; stat.distinct_bits[h / 64] |= std::uint64_t{1} << (h % 64);
      if (column.type() == Type::Double && std::isnan(column.data<double>()[r])) { stat.has_nan = true; continue; }
      if (min == no_row || compare_cell(column, r, column, min) < 0) min = r;
      if (max == no_row || compare_cell(column, r, column, max) > 0) max = r;
    }
    if (min != no_row) { stat.minimum = column.value(min); stat.maximum = column.value(max); }
  }
  return out;
}
void merge_statistics(Statistics& into, const Statistics& from) {
  if (into.empty()) { into = from; return; }
  if (into.size() != from.size()) throw std::invalid_argument("statistics width mismatch");
  for (std::size_t i = 0; i < into.size(); ++i) {
    auto& a = into[i]; const auto& b = from[i];
    a.rows += b.rows; a.nulls += b.nulls; a.nulls_known &= b.nulls_known; a.has_nan |= b.has_nan;
    if (b.minimum && (!a.minimum || compare_value(*b.minimum, *a.minimum) < 0)) a.minimum = b.minimum;
    if (b.maximum && (!a.maximum || compare_value(*b.maximum, *a.maximum) > 0)) a.maximum = b.maximum;
    for (std::size_t j = 0; j < a.distinct_bits.size(); ++j) a.distinct_bits[j] |= b.distinct_bits[j];
  }
}
bool may_match(const ExprPtr& e, const Schema& schema, const Statistics& stats) {
  if (!e) return true;
  if (e->kind == ExprKind::Literal) return !is_null(e->literal) && std::get<bool>(e->literal);
  if (e->kind == ExprKind::And) return may_match(e->left, schema, stats) && may_match(e->right, schema, stats);
  if (e->kind == ExprKind::Or) return may_match(e->left, schema, stats) || may_match(e->right, schema, stats);
  if (e->kind == ExprKind::IsNull && e->left->kind == ExprKind::Column) {
    const auto& s = stats.at(column_index(schema, e->left->column)); return !s.nulls_known || s.nulls != 0;
  }
  const auto p = simple(e); if (!p) return true;
  const auto& s = stats.at(column_index(schema, p->column));
  if (is_null(p->value) || (s.nulls_known && s.nulls == s.rows)) return false;
  if (std::holds_alternative<double>(p->value) && std::isnan(std::get<double>(p->value))) return p->kind == ExprKind::NotEqual;
  if (!s.minimum || !s.maximum) return true;
  const auto lo = compare_value(*s.minimum, p->value), hi = compare_value(*s.maximum, p->value);
  switch (p->kind) {
    case ExprKind::Equal: return lo <= 0 && hi >= 0;
    case ExprKind::NotEqual: return s.has_nan || lo != 0 || hi != 0;
    case ExprKind::Less: return lo < 0; case ExprKind::LessEqual: return lo <= 0;
    case ExprKind::Greater: return hi > 0; case ExprKind::GreaterEqual: return hi >= 0;
    default: return true;
  }
}
double estimate_selectivity(const ExprPtr& e, const Schema& schema, const Statistics& stats) {
  if (!may_match(e, schema, stats)) return 0;
  if (e->kind == ExprKind::Literal) return 1;
  if (e->kind == ExprKind::And) return estimate_selectivity(e->left, schema, stats) * estimate_selectivity(e->right, schema, stats);
  if (e->kind == ExprKind::Or) { auto a = estimate_selectivity(e->left, schema, stats), b = estimate_selectivity(e->right, schema, stats); return a + b - a * b; }
  auto p = simple(e); if (!p) return 0.5;
  const auto& s = stats.at(column_index(schema, p->column));
  const auto nonnull = s.rows ? static_cast<double>(s.rows - s.nulls) / static_cast<double>(s.rows) : 0;
  if (p->kind == ExprKind::Equal) return nonnull / std::max(1.0, s.approximate_distinct());
  if (p->kind == ExprKind::NotEqual) return nonnull * (1.0 - 1.0 / std::max(1.0, s.approximate_distinct()));
  if (s.minimum && s.maximum && (std::holds_alternative<std::int64_t>(p->value) || std::holds_alternative<double>(p->value))) {
    auto number = [](const Value& v) { return std::holds_alternative<double>(v) ? std::get<double>(v) : static_cast<double>(std::get<std::int64_t>(v)); };
    const auto lo = number(*s.minimum), hi = number(*s.maximum), x = number(p->value);
    if (hi > lo && std::isfinite(hi - lo)) {
      auto fraction = std::clamp((x - lo) / (hi - lo), 0.0, 1.0);
      if (p->kind == ExprKind::Greater || p->kind == ExprKind::GreaterEqual) fraction = 1.0 - fraction;
      return nonnull * fraction;
    }
  }
  return nonnull * 0.5;
}
}
