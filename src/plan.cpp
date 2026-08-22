#include "vqe/plan.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace vqe {
namespace {
Plan node(LogicalKind k, Plan left = {}, Plan right = {}) {
  auto n = std::make_shared<LogicalPlan>(); n->kind = k; n->left = std::move(left); n->right = std::move(right); return n;
}
std::set<ColumnId> ids(const Schema& s) { std::set<ColumnId> out; for (const auto& f : s) out.insert(f.id); return out; }
bool subset(const std::set<ColumnId>& a, const std::set<ColumnId>& b) { return std::includes(b.begin(), b.end(), a.begin(), a.end()); }
void add_refs(std::set<ColumnId>& into, const ExprPtr& e) { const auto r = referenced_columns(e); into.insert(r.begin(), r.end()); }
Plan clone(const Plan& p) {
  if (!p) return {}; auto out = std::make_shared<LogicalPlan>(*p); out->left = clone(p->left); out->right = clone(p->right); return out;
}
}
namespace logical {
Plan scan(std::shared_ptr<const Source> source) {
  if (!source) throw std::invalid_argument("missing scan source");
  auto p = node(LogicalKind::Scan); p->source = std::move(source); for (const auto& f : p->source->schema()) p->columns.push_back(f.id); return p;
}
Plan scan(std::shared_ptr<const Table> t, std::string name) { return scan(std::make_shared<MemorySource>(std::move(t), std::move(name))); }
Plan filter(Plan c, ExprPtr e) { auto p = node(LogicalKind::Filter, std::move(c)); p->predicate = std::move(e); return p; }
Plan project(Plan c, std::vector<NamedExpression> e) { auto p = node(LogicalKind::Projection, std::move(c)); p->expressions = std::move(e); return p; }
Plan aggregate(Plan c, std::vector<ColumnId> keys, std::vector<AggregateSpec> a) { auto p = node(LogicalKind::Aggregate, std::move(c)); p->keys = std::move(keys); p->aggregates = std::move(a); return p; }
Plan join(Plan l, Plan r, std::vector<ColumnId> lk, std::vector<ColumnId> rk) { auto p = node(LogicalKind::Join, std::move(l), std::move(r)); p->keys = std::move(lk); p->right_keys = std::move(rk); return p; }
Plan sort(Plan c, std::vector<SortKey> keys) { auto p = node(LogicalKind::Sort, std::move(c)); p->sort_keys = std::move(keys); return p; }
Plan limit(Plan c, std::size_t n) { auto p = node(LogicalKind::Limit, std::move(c)); p->count = n; return p; }
}
Schema output_schema(const Plan& p) {
  if (!p) throw std::invalid_argument("missing plan");
  if (p->kind == LogicalKind::Scan) {
    Schema s; for (auto id : p->columns) s.push_back(p->source->schema()[column_index(p->source->schema(), id)]); validate_schema(s); return s;
  }
  auto s = output_schema(p->left);
  switch (p->kind) {
    case LogicalKind::Projection: return projection_schema(p->expressions, s);
    case LogicalKind::Aggregate: return aggregate_schema(s, p->keys, p->aggregates);
    case LogicalKind::Join: {
      const auto r = output_schema(p->right);
      if (p->keys.empty() || p->keys.size() != p->right_keys.size()) throw std::invalid_argument("invalid join keys");
      for (std::size_t i = 0; i < p->keys.size(); ++i)
        if (s[column_index(s, p->keys[i])].type != r[column_index(r, p->right_keys[i])].type) throw std::invalid_argument("join key types differ");
      s.insert(s.end(), r.begin(), r.end()); validate_schema(s); return s;
    }
    case LogicalKind::Filter:
      if (expression_type(p->predicate, s) != Type::Boolean) throw std::invalid_argument("filter is not boolean");
      break;
    case LogicalKind::Sort: for (const auto& k : p->sort_keys) (void)column_index(s, k.column); break;
    default: break;
  }
  return s;
}
namespace {
const Source* underlying_source(const Plan& p) {
  if (p->kind == LogicalKind::Scan) return p->source.get();
  if (p->kind == LogicalKind::Filter || p->kind == LogicalKind::Sort || p->kind == LogicalKind::Limit) return underlying_source(p->left);
  return nullptr;
}
double selectivity(const ExprPtr& e, const Plan& p) {
  const auto* s = underlying_source(p);
  return s ? estimate_selectivity(e, s->schema(), s->statistics()) : 0.5;
}
}
double estimated_rows(const Plan& p) {
  if (p->kind == LogicalKind::Scan) return static_cast<double>(p->source->row_count());
  const auto n = estimated_rows(p->left);
  switch (p->kind) {
    case LogicalKind::Filter: return n * selectivity(p->predicate, p->left);
    case LogicalKind::Limit: return std::min(n, static_cast<double>(p->count));
    case LogicalKind::Aggregate: {
      if (p->keys.empty()) return 1;
      const auto* s = underlying_source(p->left); double estimate = 1;
      if (!s) return std::sqrt(n);
      for (auto id : p->keys) { const auto& st = s->statistics()[column_index(s->schema(), id)]; estimate *= std::max(1.0, st.approximate_distinct() + (st.nulls ? 1.0 : 0.0)); }
      return std::min(n, estimate);
    }
    case LogicalKind::Join: return std::max(n, estimated_rows(p->right));
    default: return n;
  }
}
ExprPtr simplify_expression(const ExprPtr& e) {
  if (!e || e->kind == ExprKind::Column || e->kind == ExprKind::Literal) return e;
  auto a = simplify_expression(e->left), b = simplify_expression(e->right);
  auto result = b ? binary(e->kind, a, b) : unary(e->kind, a);
  if (referenced_columns(result).empty()) {
    Batch singleton({}, 1); singleton.finish(1); Evaluator eval(result, {}); const auto& c = eval.evaluate(singleton);
    const auto v = c.value(0); return is_null(v) ? null_literal(c.type()) : lit(v);
  }
  if (e->kind == ExprKind::And || e->kind == ExprKind::Or) {
    if (expression_equal(a, b)) return a;
    for (bool left : {true, false}) {
      const auto& constant = left ? a : b; const auto& other = left ? b : a;
      if (constant->kind == ExprKind::Literal && constant->literal_type == Type::Boolean && !is_null(constant->literal)) {
        const bool v = std::get<bool>(constant->literal);
        if (e->kind == ExprKind::And) return v ? other : lit(false);
        return v ? lit(true) : other;
      }
    }
  }
  if (e->kind == ExprKind::Not && a->kind == ExprKind::Not) return a->left;
  // Do not rewrite x=x or x*0: NULL, NaN and overflow change their meaning.
  return result;
}
namespace {
void conjuncts(const ExprPtr& e, std::vector<ExprPtr>& out) {
  if (e->kind == ExprKind::And) { conjuncts(e->left, out); conjuncts(e->right, out); }
  else out.push_back(e);
}
ExprPtr substitute(const ExprPtr& e, const std::map<ColumnId, ExprPtr>& map) {
  if (e->kind == ExprKind::Column) return map.at(e->column);
  if (e->kind == ExprKind::Literal) return e;
  return e->right ? binary(e->kind, substitute(e->left, map), substitute(e->right, map)) : unary(e->kind, substitute(e->left, map));
}
bool identity_projection(const Plan& p) {
  const auto s = output_schema(p->left);
  if (s.size() != p->expressions.size()) return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto& x = p->expressions[i];
    if (x.expression->kind != ExprKind::Column || x.expression->column != s[i].id || x.field.id != s[i].id || x.field.name != s[i].name) return false;
  }
  return true;
}
Plan rewrite(Plan p) {
  if (p->left) p->left = rewrite(p->left);
  if (p->right) p->right = rewrite(p->right);
  if (p->kind == LogicalKind::Projection) {
    for (auto& e : p->expressions) e.expression = simplify_expression(e.expression);
    if (identity_projection(p)) return p->left;
  }
  if (p->kind != LogicalKind::Filter) return p;
  std::vector<ExprPtr> predicates; auto child = p;
  while (child->kind == LogicalKind::Filter) { conjuncts(simplify_expression(child->predicate), predicates); child = child->left; }
  std::vector<ExprPtr> unique;
  for (const auto& e : predicates) {
    if (e->kind == ExprKind::Literal && !is_null(e->literal) && std::get<bool>(e->literal)) continue;
    if (std::none_of(unique.begin(), unique.end(), [&](const auto& u) { return expression_equal(u, e); })) unique.push_back(e);
  }
  std::vector<ExprPtr> remaining;
  for (const auto& e : unique) {
    const auto refs = referenced_columns(e); bool pushed = false;
    if (child->kind == LogicalKind::Join && !refs.empty()) {
      if (subset(refs, ids(output_schema(child->left)))) { child->left = rewrite(logical::filter(child->left, e)); pushed = true; }
      else if (subset(refs, ids(output_schema(child->right)))) { child->right = rewrite(logical::filter(child->right, e)); pushed = true; }
    } else if (child->kind == LogicalKind::Sort ||
               (child->kind == LogicalKind::Aggregate && !refs.empty() && subset(refs, std::set<ColumnId>(child->keys.begin(), child->keys.end())))) {
      child->left = rewrite(logical::filter(child->left, e)); pushed = true;
    } else if (child->kind == LogicalKind::Projection) {
      std::map<ColumnId, ExprPtr> map;
      for (const auto& x : child->expressions) if (x.expression->kind == ExprKind::Column) map.emplace(x.field.id, x.expression);
      if (std::all_of(refs.begin(), refs.end(), [&](auto id) { return map.contains(id); })) {
        child->left = rewrite(logical::filter(child->left, substitute(e, map))); pushed = true;
      }
    }
    if (!pushed) remaining.push_back(e);
  }
  std::stable_sort(remaining.begin(), remaining.end(), [&](const auto& a, const auto& b) { return selectivity(a, child) < selectivity(b, child); });
  // Build from the innermost (most selective) predicate outward.
  for (const auto& e : remaining) child = logical::filter(child, e);
  return child;
}
void prune(Plan& p, const std::set<ColumnId>& required) {
  if (p->kind == LogicalKind::Scan) {
    std::erase_if(p->columns, [&](auto id) { return !required.contains(id); }); return;
  }
  auto needed = required;
  switch (p->kind) {
    case LogicalKind::Projection:
      std::erase_if(p->expressions, [&](const auto& e) { return !required.contains(e.field.id); });
      needed.clear(); for (const auto& e : p->expressions) add_refs(needed, e.expression); break;
    case LogicalKind::Filter: add_refs(needed, p->predicate); break;
    case LogicalKind::Sort: for (const auto& key : p->sort_keys) needed.insert(key.column); break;
    case LogicalKind::Aggregate:
      std::erase_if(p->aggregates, [&](const auto& a) { return !required.contains(a.output.id); });
      needed = {p->keys.begin(), p->keys.end()};
      for (const auto& a : p->aggregates) if (a.input) needed.insert(*a.input);
      break;
    case LogicalKind::Join: {
      needed.insert(p->keys.begin(), p->keys.end()); needed.insert(p->right_keys.begin(), p->right_keys.end());
      std::set<ColumnId> l, r;
      for (const auto& f : output_schema(p->left)) if (needed.contains(f.id)) l.insert(f.id);
      for (const auto& f : output_schema(p->right)) if (needed.contains(f.id)) r.insert(f.id);
      prune(p->left, l); prune(p->right, r); return;
    }
    default: break;
  }
  prune(p->left, needed);
}
}
Plan optimize(const Plan& p) {
  const auto original = output_schema(p); auto out = rewrite(clone(p)); prune(out, ids(original));
  // Pruning can turn projections into identities; a second rewrite removes them.
  out = rewrite(out); (void)output_schema(out); return out;
}
std::unique_ptr<PhysicalPlan> lower(const Plan& p) {
  auto out = std::make_unique<PhysicalPlan>(); out->schema = output_schema(p); out->rows = estimated_rows(p);
  if (p->left) out->left = lower(p->left);
  if (p->right) out->right = lower(p->right);
  out->source = p->source; out->columns = p->columns; out->keys = p->keys; out->right_keys = p->right_keys;
  out->predicate = p->predicate; out->expressions = p->expressions; out->aggregates = p->aggregates; out->sort_keys = p->sort_keys; out->count = p->count;
  switch (p->kind) {
    case LogicalKind::Scan: out->kind = PhysicalKind::ColumnScan; break;
    case LogicalKind::Filter: {
      out->kind = PhysicalKind::SelectionFilter;
      auto* scan = out->left.get(); while (scan->kind == PhysicalKind::SelectionFilter) scan = scan->left.get();
      if (scan->kind == PhysicalKind::ColumnScan) scan->pruning_predicate = scan->pruning_predicate ? binary(ExprKind::And, scan->pruning_predicate, p->predicate) : p->predicate;
      break;
    }
    case LogicalKind::Projection: out->kind = PhysicalKind::VectorProjection; break;
    case LogicalKind::Aggregate: out->kind = PhysicalKind::HashAggregate; break;
    case LogicalKind::Join: out->kind = PhysicalKind::HashJoin; out->build_right = out->right->rows <= out->left->rows; break;
    case LogicalKind::Sort: out->kind = PhysicalKind::MaterializedSort; break;
    case LogicalKind::Limit: out->kind = PhysicalKind::Limit; break;
  }
  return out;
}
OperatorPtr execute(const PhysicalPlan& p, ExecutionOptions options) {
  switch (p.kind) {
    case PhysicalKind::ColumnScan: return p.source->scan(p.columns, options, p.pruning_predicate);
    case PhysicalKind::SelectionFilter: return std::make_unique<Filter>(execute(*p.left, options), p.predicate);
    case PhysicalKind::VectorProjection: return std::make_unique<Projection>(execute(*p.left, options), p.expressions);
    case PhysicalKind::HashAggregate: return std::make_unique<HashAggregate>(execute(*p.left, options), p.keys, p.aggregates);
    case PhysicalKind::HashJoin: return std::make_unique<HashJoin>(execute(*p.left, options), execute(*p.right, options), p.keys, p.right_keys, p.build_right);
    case PhysicalKind::MaterializedSort: return std::make_unique<Sort>(execute(*p.left, options), p.sort_keys);
    case PhysicalKind::Limit: return std::make_unique<Limit>(execute(*p.left, options), p.count);
  }
  throw std::logic_error("unknown physical node");
}
namespace {
void explain_physical(const PhysicalPlan& p, std::ostringstream& out, std::size_t depth) {
  static constexpr const char* names[] = {"ColumnScan", "SelectionFilter", "VectorProjection", "HashAggregate", "HashJoin", "MaterializedSort", "Limit"};
  out << std::string(depth * 2, ' ') << names[static_cast<std::size_t>(p.kind)] << " rows~" << p.rows;
  if (p.source) out << " source=" << p.source->name();
  if (p.predicate) out << " predicate=" << describe(p.predicate);
  if (p.pruning_predicate) out << " pruning=" << describe(p.pruning_predicate);
  if (p.kind == PhysicalKind::HashJoin) out << " build=" << (p.build_right ? "right" : "left");
  out << " columns=["; for (const auto& f : p.schema) out << '#' << f.id << ' '; out << "]\n";
  if (p.left) explain_physical(*p.left, out, depth + 1);
  if (p.right) explain_physical(*p.right, out, depth + 1);
}
void explain_logical(const Plan& p, std::ostringstream& out, std::size_t depth) {
  static constexpr const char* names[] = {"Scan", "Filter", "Projection", "Aggregate", "Join", "Sort", "Limit"};
  out << std::string(depth * 2, ' ') << names[static_cast<std::size_t>(p->kind)];
  if (p->source) out << " source=" << p->source->name();
  if (p->predicate) out << " predicate=" << describe(p->predicate);
  out << '\n'; if (p->left) explain_logical(p->left, out, depth + 1); if (p->right) explain_logical(p->right, out, depth + 1);
}
}
std::string explain(const PhysicalPlan& p) { std::ostringstream out; explain_physical(p, out, 0); return out.str(); }
std::string explain(const Plan& p) { std::ostringstream out; explain_logical(p, out, 0); return out.str(); }
}
