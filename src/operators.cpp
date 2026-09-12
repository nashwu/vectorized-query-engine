#include "vqe/operators.hpp"
#include <algorithm>

namespace vqe {
namespace {
Schema select_schema(const Schema& s, const std::vector<ColumnId>& ids) {
  Schema out; for (auto id : ids) out.push_back(s[column_index(s, id)]); return out;
}
std::vector<ColumnId> all_ids(const Schema& s) {
  std::vector<ColumnId> out; for (const auto& f : s) out.push_back(f.id); return out;
}
}
void Table::append(const Batch& b) {
  b.validate();
  if (b.schema.size() != schema_.size()) throw std::invalid_argument("table schema width mismatch");
  for (std::size_t i = 0; i < schema_.size(); ++i)
    if (b.schema[i].type != schema_[i].type || b.schema[i].id != schema_[i].id) throw std::invalid_argument("table schema mismatch");
  if (!b.size()) return;
  Batch dense(schema_, b.capacity);
  for (std::size_t i = 0; i < b.size(); ++i) dense.append_row(b, b.selection[i]);
  dense.finish(b.size());
  auto stats = analyze(dense); merge_statistics(statistics_, stats); group_statistics_.push_back(std::move(stats));
  rows_ += b.size(); groups_.push_back(std::move(dense));
}
std::size_t Table::allocated_bytes() const { std::size_t n = 0; for (const auto& b : groups_) n += b.allocated_bytes(); return n; }
void Operator::prepare(Batch& b) const {
  bool same = b.schema.size() == schema_.size() && b.capacity == options_.batch_size;
  if (same) for (std::size_t i = 0; i < schema_.size(); ++i)
    if (schema_[i].id != b.schema[i].id || schema_[i].type != b.schema[i].type || schema_[i].name != b.schema[i].name) same = false;
  if (!same) b = Batch(schema_, options_.batch_size);
  else b.reset();
}
Scan::Scan(std::shared_ptr<const Table> t, ExecutionOptions o) : Scan(t, all_ids(t->schema()), o) {}
Scan::Scan(std::shared_ptr<const Table> t, std::vector<ColumnId> ids, ExecutionOptions o, ExprPtr p)
  : Operator(select_schema(t->schema(), ids), o), table_(std::move(t)), pruning_predicate_(std::move(p)) {
  if (pruning_predicate_ && expression_type(pruning_predicate_, table_->schema()) != Type::Boolean) throw std::invalid_argument("pruning predicate must be boolean");
  for (auto id : ids) indices_.push_back(column_index(table_->schema(), id));
}
bool Scan::next(Batch& out) {
  prepare(out); std::size_t n = 0;
  while (n < out.capacity && group_ < table_->groups().size()) {
    if (row_ == 0) {
      if (!may_match(pruning_predicate_, table_->schema(), table_->group_statistics()[group_])) { ++group_; ++groups_skipped_; continue; }
      ++groups_read_;
    }
    const auto& b = table_->groups()[group_];
    const auto count = std::min(b.physical_size - row_, out.capacity - n);
    for (std::size_t c = 0; c < indices_.size(); ++c) out.columns[c].append_range(b.columns[indices_[c]], row_, count);
    row_ += count; n += count;
    if (row_ == b.physical_size) { row_ = 0; ++group_; }
  }
  out.finish(n); return n != 0;
}
Filter::Filter(OperatorPtr c, ExprPtr p)
  : Operator(c->schema(), c->options()), child_(std::move(c)), predicate_(p, schema_, options_.batch_size, options_.kernels) {
  if (expression_type(p, schema_) != Type::Boolean) throw std::invalid_argument("filter requires boolean predicate");
}
bool Filter::next(Batch& out) {
  while (child_->next(out)) {
    const auto& p = predicate_.evaluate(out); selected_.clear();
    for (std::size_t j = 0; j < out.size(); ++j) {
      const auto i = out.selection[j]; if (p.validity().valid(i) && p.data<std::uint8_t>()[i]) selected_.push(i);
    }
    std::swap(out.selection, selected_);
    if (out.size()) return true;
  }
  return false;
}
std::size_t Filter::allocated_bytes() const { return child_->allocated_bytes() + predicate_.allocated_bytes() + selected_.allocated_bytes(); }
Schema projection_schema(const std::vector<NamedExpression>& expressions, const Schema& input) {
  Schema out;
  for (const auto& e : expressions) {
    if (expression_type(e.expression, input) != e.field.type) throw std::invalid_argument("projection output type mismatch");
    out.push_back(e.field);
  }
  validate_schema(out); return out;
}
Projection::Projection(OperatorPtr c, std::vector<NamedExpression> expressions)
  : Operator(projection_schema(expressions, c->schema()), c->options()), child_(std::move(c)), input_(child_->schema(), options_.batch_size) {
  for (const auto& e : expressions) expressions_.emplace_back(e.expression, child_->schema(), options_.batch_size, options_.kernels);
}
bool Projection::next(Batch& out) {
  prepare(out); if (!child_->next(input_)) return false;
  for (std::size_t c = 0; c < expressions_.size(); ++c) {
    const auto& v = expressions_[c].evaluate(input_);
    for (std::size_t j = 0; j < input_.size(); ++j) out.columns[c].append_from(v, input_.selection[j]);
  }
  out.finish(input_.size()); return true;
}
std::size_t Projection::allocated_bytes() const {
  auto n = child_->allocated_bytes() + input_.allocated_bytes(); for (const auto& e : expressions_) n += e.allocated_bytes(); return n;
}
std::vector<Batch> collect(Operator& op) {
  std::vector<Batch> result; Batch b(op.schema(), op.options().batch_size);
  while (op.next(b)) { b.validate(); result.push_back(b); }
  return result;
}
} // namespace vqe
