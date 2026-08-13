#include "vqe/blocking.hpp"
#include <algorithm>
#include <numeric>
#include <cmath>

namespace vqe {
namespace {
Schema key_schema(const Schema& s, const std::vector<ColumnId>& keys) {
  Schema out; for (auto key : keys) out.push_back(s[column_index(s, key)]); validate_schema(out); return out;
}
}
Schema aggregate_schema(const Schema& input, const std::vector<ColumnId>& keys, const std::vector<AggregateSpec>& aggs) {
  auto out = key_schema(input, keys);
  for (const auto& a : aggs) {
    const auto type = a.input ? input[column_index(input, *a.input)].type : Type::Int64;
    if (a.kind == AggregateKind::Count) {
      if (a.output.type != Type::Int64) throw std::invalid_argument("COUNT output must be INT64");
    } else if (!a.input || (type != Type::Int64 && type != Type::Double) || a.output.type != type)
      throw std::invalid_argument("SUM/MIN/MAX require matching numeric input and output types");
    out.push_back(a.output);
  }
  validate_schema(out); return out;
}
HashAggregate::HashAggregate(OperatorPtr c, std::vector<ColumnId> keys, std::vector<AggregateSpec> aggs)
  : Operator(aggregate_schema(c->schema(), keys, aggs), c->options()), child_(std::move(c)), aggregates_(std::move(aggs)),
    keys_(key_schema(child_->schema(), keys)), input_(child_->schema(), options_.batch_size) {
  for (auto key : keys) input_keys_.push_back(column_index(child_->schema(), key));
  stored_keys_.resize(keys.size()); std::iota(stored_keys_.begin(), stored_keys_.end(), 0);
  for (const auto& a : aggregates_) input_values_.push_back(a.input ? column_index(child_->schema(), *a.input) : no_row);
}
void HashAggregate::consume() {
  consumed_ = true;
  if (input_keys_.empty()) { Batch empty({}, 1); keys_.append_key(empty, {}, 0); states_.resize(aggregates_.size()); }
  while (child_->next(input_)) for (std::size_t j = 0; j < input_.size(); ++j) {
    const auto row = input_.selection[j]; std::size_t group = 0;
    if (!input_keys_.empty()) {
      group = index_.find_or_insert(hash_row(input_.columns, input_keys_, row),
        [&](auto g) { return keys_.equal_key(g, input_, input_keys_, stored_keys_, row); },
        [&] { const auto g = keys_.size(); keys_.append_key(input_, input_keys_, row); states_.resize((g + 1) * aggregates_.size()); return g; }).first;
    }
    for (std::size_t a = 0; a < aggregates_.size(); ++a) {
      auto& state = states_[group * aggregates_.size() + a]; const auto& spec = aggregates_[a];
      const Column* c = input_values_[a] == no_row ? nullptr : &input_.columns[input_values_[a]];
      if (c && !c->validity().valid(row)) continue;
      if (spec.kind == AggregateKind::Count) {
        if (__builtin_add_overflow(state.integer, std::int64_t{1}, &state.integer)) throw std::overflow_error("COUNT overflow");
      } else if (c->type() == Type::Int64) {
        const auto x = c->data<std::int64_t>()[row];
        if (spec.kind == AggregateKind::Sum) state.overflow |= __builtin_add_overflow(state.integer, x, &state.integer);
        else if (!state.seen || (spec.kind == AggregateKind::Min ? x < state.integer : x > state.integer)) state.integer = x;
      } else {
        const auto x = c->data<double>()[row];
        if (spec.kind == AggregateKind::Sum) state.floating += x;
        else {
          const bool smaller = std::isnan(state.floating) ? !std::isnan(x) : x < state.floating;
          const bool larger = std::isnan(x) ? !std::isnan(state.floating) : x > state.floating;
          if (!state.seen || (spec.kind == AggregateKind::Min ? smaller : larger)) state.floating = x;
        }
      }
      state.seen = true;
    }
  }
}
bool HashAggregate::next(Batch& out) {
  prepare(out); if (!consumed_) consume(); std::size_t n = 0;
  while (cursor_ < keys_.size() && n < out.capacity) {
    for (std::size_t k = 0; k < keys_.columns.size(); ++k) out.columns[k].append_from(keys_.columns[k], cursor_);
    for (std::size_t a = 0; a < aggregates_.size(); ++a) {
      const auto& state = states_[cursor_ * aggregates_.size() + a]; auto& dest = out.columns[keys_.columns.size() + a];
      if (aggregates_[a].kind != AggregateKind::Count && (!state.seen || state.overflow)) dest.append_null();
      else if (dest.type() == Type::Int64) dest.append(state.integer);
      else dest.append(state.floating);
    }
    ++cursor_; ++n;
  }
  out.finish(n); return n != 0;
}
std::size_t HashAggregate::allocated_bytes() const {
  return child_->allocated_bytes() + input_.allocated_bytes() + keys_.allocated_bytes() + index_.allocated_bytes() + states_.capacity() * sizeof(State);
}
}
