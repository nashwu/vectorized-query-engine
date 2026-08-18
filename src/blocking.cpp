#include "vqe/blocking.hpp"
#include <algorithm>
#include <numeric>
#include <cmath>

namespace vqe {
namespace {
Schema key_schema(const Schema& s, const std::vector<ColumnId>& keys) {
  Schema out; for (auto key : keys) out.push_back(s[column_index(s, key)]); validate_schema(out); return out;
}
Schema join_schema(const Schema& a, const Schema& b) {
  auto out = a; out.insert(out.end(), b.begin(), b.end()); validate_schema(out); return out;
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
HashJoin::HashJoin(OperatorPtr left, OperatorPtr right, std::vector<ColumnId> lk, std::vector<ColumnId> rk, bool br)
  : Operator(join_schema(left->schema(), right->schema()), left->options()),
    build_(br ? std::move(right) : std::move(left)), probe_(br ? std::move(left) : std::move(right)), build_right_(br),
    store_(build_->schema()), build_batch_(build_->schema(), build_->options().batch_size), probe_batch_(probe_->schema(), probe_->options().batch_size) {
  if (lk.empty() || lk.size() != rk.size()) throw std::invalid_argument("join requires equal nonempty key lists");
  const auto& bk = br ? rk : lk; const auto& pk = br ? lk : rk;
  for (std::size_t i = 0; i < bk.size(); ++i) {
    build_keys_.push_back(column_index(build_->schema(), bk[i])); probe_keys_.push_back(column_index(probe_->schema(), pk[i]));
    if (build_->schema()[build_keys_.back()].type != probe_->schema()[probe_keys_.back()].type) throw std::invalid_argument("join key type mismatch");
  }
}
void HashJoin::build() {
  built_ = true;
  while (build_->next(build_batch_)) for (std::size_t j = 0; j < build_batch_.size(); ++j) {
    const auto r = build_batch_.selection[j];
    if (!joinable_row(build_batch_.columns, build_keys_, r)) continue;
    const auto id = store_.size();
    const auto [head, inserted] = index_.find_or_insert(hash_row(build_batch_.columns, build_keys_, r),
      [&](auto h) { return store_.equal_key(h, build_batch_, build_keys_, build_keys_, r); }, [&] { return id; });
    store_.append(build_batch_, r); next_.push_back(no_row); tails_.push_back(id);
    if (!inserted) { next_[tails_[head]] = id; tails_[head] = id; }
  }
}
bool HashJoin::load_probe() {
  if (!probe_->next(probe_batch_)) return false;
  // Hash and locate a batch of probe keys before expanding duplicate chains.
  heads_.resize(probe_batch_.size()); probe_cursor_ = 0;
  for (std::size_t j = 0; j < probe_batch_.size(); ++j) {
    const auto r = probe_batch_.selection[j]; heads_[j] = no_row;
    if (joinable_row(probe_batch_.columns, probe_keys_, r))
      heads_[j] = index_.find(hash_row(probe_batch_.columns, probe_keys_, r),
        [&](auto h) { return store_.equal_key(h, probe_batch_, probe_keys_, build_keys_, r); });
  }
  match_ = heads_[0]; return true;
}
bool HashJoin::next(Batch& out) {
  prepare(out); if (!built_) build(); if (!store_.size()) return false;
  std::size_t n = 0;
  while (n < out.capacity) {
    if (probe_cursor_ >= heads_.size()) { if (!load_probe()) break; }
    if (match_ == no_row) { ++probe_cursor_; if (probe_cursor_ < heads_.size()) match_ = heads_[probe_cursor_]; continue; }
    const auto probe_row = probe_batch_.selection[probe_cursor_];
    const auto& first = build_right_ ? probe_batch_.columns : store_.columns;
    const auto& second = build_right_ ? store_.columns : probe_batch_.columns;
    const auto first_row = build_right_ ? probe_row : match_, second_row = build_right_ ? match_ : probe_row;
    for (std::size_t c = 0; c < first.size(); ++c) out.columns[c].append_from(first[c], first_row);
    for (std::size_t c = 0; c < second.size(); ++c) out.columns[first.size() + c].append_from(second[c], second_row);
    match_ = next_[match_]; ++n;
  }
  out.finish(n); return n != 0;
}
std::size_t HashJoin::allocated_bytes() const {
  return build_->allocated_bytes() + probe_->allocated_bytes() + store_.allocated_bytes() + index_.allocated_bytes() +
    (next_.capacity() + tails_.capacity() + heads_.capacity()) * sizeof(std::size_t) + build_batch_.allocated_bytes() + probe_batch_.allocated_bytes();
}
Limit::Limit(OperatorPtr c, std::size_t n) : Operator(c->schema(), c->options()), child_(std::move(c)), remaining_(n) {}
bool Limit::next(Batch& out) {
  if (!remaining_) { prepare(out); return false; }
  if (!child_->next(out)) return false;
  out.selection.truncate(std::min(remaining_, out.size())); remaining_ -= out.size(); return out.size() != 0;
}
Sort::Sort(OperatorPtr c, std::vector<SortKey> keys)
  : Operator(c->schema(), c->options()), child_(std::move(c)), store_(schema_), input_(schema_, options_.batch_size), keys_(std::move(keys)) {
  for (const auto& k : keys_) key_indices_.push_back(column_index(schema_, k.column));
}
bool Sort::next(Batch& out) {
  prepare(out);
  if (!sorted_) {
    while (child_->next(input_)) for (std::size_t i = 0; i < input_.size(); ++i) store_.append(input_, input_.selection[i]);
    order_.resize(store_.size()); std::iota(order_.begin(), order_.end(), 0);
    std::stable_sort(order_.begin(), order_.end(), [&](auto a, auto b) {
      for (std::size_t k = 0; k < keys_.size(); ++k) {
        const auto& c = store_.columns[key_indices_[k]]; const bool av = c.validity().valid(a), bv = c.validity().valid(b);
        if (av != bv) return keys_[k].nulls_first ? !av : av;
        const auto cmp = compare_cell(c, a, c, b); if (cmp) return keys_[k].ascending ? cmp < 0 : cmp > 0;
      }
      return false;
    }); sorted_ = true;
  }
  std::size_t n = 0;
  while (cursor_ < order_.size() && n < out.capacity) {
    for (std::size_t c = 0; c < schema_.size(); ++c) out.columns[c].append_from(store_.columns[c], order_[cursor_]);
    ++cursor_; ++n;
  }
  out.finish(n); return n != 0;
}
std::size_t Sort::allocated_bytes() const {
  return child_->allocated_bytes() + input_.allocated_bytes() + store_.allocated_bytes() + order_.capacity() * sizeof(std::size_t);
}
}
