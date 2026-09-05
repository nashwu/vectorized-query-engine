#include "vqe/vector.hpp"
#include "vqe/hash.hpp"
#include "vqe/kernels.hpp"
#include "vqe/expression.hpp"
#include "vqe/operators.hpp"
#include "vqe/blocking.hpp"
#include "vqe/plan.hpp"
#include "vqe/workloads.hpp"
#include <bit>
#include <cmath>
#include <map>
#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>

using namespace vqe;
namespace {
std::vector<std::pair<std::string, std::function<void()>>>& tests() {
  static std::vector<std::pair<std::string, std::function<void()>>> t; return t;
}
struct Register { Register(std::string n, std::function<void()> f) { tests().emplace_back(std::move(n), std::move(f)); } };
#define TEST(name) void name(); Register reg_##name(#name, name); void name()
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #__VA_ARGS__); } while (false)
template<class F> void throws(F f) { bool caught = false; try { f(); } catch (const std::exception&) { caught = true; } CHECK(caught); }
Value I(std::int64_t x) { return x; }
std::vector<std::vector<Value>> rows(Operator& op) {
  std::vector<std::vector<Value>> result;
  for (const auto& b : collect(op)) for (std::size_t i = 0; i < b.size(); ++i) {
    std::vector<Value> row; for (const auto& c : b.columns) row.push_back(c.value(b.selection[i])); result.push_back(std::move(row));
  }
  return result;
}
std::shared_ptr<Table> table(const Schema& s, const std::vector<std::vector<Value>>& data, std::size_t group = 3) {
  auto t = std::make_shared<Table>(s); Batch b(s, group);
  for (const auto& r : data) {
    for (std::size_t i = 0; i < s.size(); ++i) b.columns[i].append_value(r.at(i));
    ++b.physical_size;
    if (b.physical_size == group) { b.finish(b.physical_size); t->append(b); b.reset(); }
  }
  b.finish(b.physical_size); t->append(b); return t;
}
TEST(validity_boundaries_and_reuse) {
  Validity v;
  for (auto n : {0U, 1U, 63U, 64U, 65U, 2048U}) {
    v.reset(n); CHECK(v.all_valid());
    for (std::size_t i = 0; i < n; ++i) v.set(i, i % 3 != 0);
    for (std::size_t i = 0; i < n; ++i) CHECK(v.valid(i) == (i % 3 != 0));
    v.reset(0); for (std::size_t i = 0; i < n; ++i) v.append(i % 2 == 0);
    CHECK(v.null_count() == n / 2);
  }
}
TEST(selection_and_batch_invariants) {
  Selection s; s.identity(17); CHECK(s[16] == 16); s.truncate(3); CHECK(s.size() == 3);
  s.clear(); s.push(31); s.push(2); CHECK(s[0] == 31 && s[1] == 2);
  throws([&] { s.push(65535); }); throws([&] { s.identity(65536); });
  throws([] { Batch b({}, 0); }); throws([] { Batch b({{1, Type::Int64, "a"}, {1, Type::Int64, "b"}}); });
  Batch b({{1, Type::Int64, "x"}}, 4); b.columns[0].append_value(I(7)); b.finish(1);
  b.selection.clear(); b.selection.push(2); throws([&] { b.validate(); });
}
TEST(strings_and_buffer_reuse) {
  Column c(Type::String); c.append_string("alpha"); c.append_null(); c.append_string(std::string("a\0b", 3)); c.append_string("");
  CHECK(c.string_at(0) == "alpha"); CHECK(is_null(c.value(1))); CHECK(c.string_at(2).size() == 3);
  Column d(Type::String); for (std::size_t i = 0; i < c.size(); ++i) d.append_from(c, i);
  CHECK(d.value(2) == c.value(2)); const auto bytes = c.allocated_bytes(); c.reset(); CHECK(c.allocated_bytes() == bytes);
}
TEST(expressions_and_overflow) {
  Schema s{{1, Type::Int64, "x"}, {2, Type::Int64, "y"}};
  auto t = table(s, {{I(7), I(2)}, {I(1), {}}, {I(9), I(0)}, {I(std::numeric_limits<std::int64_t>::max()), I(1)}} , 8);
  auto b = t->groups()[0];
  Evaluator div(binary(ExprKind::Divide, col(1), col(2)), s); auto& d = div.evaluate(b);
  CHECK(d.value(0) == I(3)); CHECK(is_null(d.value(1))); CHECK(is_null(d.value(2)));
  Evaluator add(binary(ExprKind::Add, col(1), col(2)), s); CHECK(is_null(add.evaluate(b).value(3)));
  throws([&] { Evaluator e(binary(ExprKind::Add, col(1), lit(1.0)), s); });
}
TEST(boolean_three_valued_truth_tables) {
  Schema s{{1, Type::Boolean, "a"}, {2, Type::Boolean, "b"}};
  std::vector<Value> values{false, true, {}};
  std::vector<std::vector<Value>> data; for (const auto& a : values) for (const auto& b : values) data.push_back({a, b});
  auto t = table(s, data, 16); const auto& b = t->groups()[0];
  Evaluator a(binary(ExprKind::And, col(1), col(2)), s), o(binary(ExprKind::Or, col(1), col(2)), s), n(unary(ExprKind::Not, col(1)), s);
  const auto& av = a.evaluate(b); const auto& ov = o.evaluate(b); const auto& nv = n.evaluate(b);
  std::vector<Value> expected_and{false, false, false, false, true, {}, false, {}, {}};
  std::vector<Value> expected_or{false, true, {}, true, true, true, {}, true, {}};
  for (std::size_t i = 0; i < 9; ++i) { CHECK(av.value(i) == expected_and[i]); CHECK(ov.value(i) == expected_or[i]); }
  CHECK(is_null(nv.value(8)));
}
TEST(scan_filter_projection_and_sparse_input) {
  Schema s{{1, Type::Int64, "x"}, {2, Type::String, "label"}};
  auto t = table(s, {{I(1), std::string("a")}, {{}, std::string("null")}, {I(3), std::string("c")}, {I(7), std::string("g")}});
  for (std::size_t batch : {1U, 2U, 3U, 16U}) {
    OperatorPtr op = std::make_unique<Scan>(t, ExecutionOptions{batch});
    op = std::make_unique<Filter>(std::move(op), binary(ExprKind::Greater, col(1), lit(I(1))));
    op = std::make_unique<Filter>(std::move(op), binary(ExprKind::Less, col(1), lit(I(5))));
    op = std::make_unique<Projection>(std::move(op), std::vector<NamedExpression>{{{3, Type::Int64, "twice"}, binary(ExprKind::Multiply, col(1), lit(I(2)))}, {{2, Type::String, "label"}, col(2)}});
    CHECK(rows(*op) == std::vector<std::vector<Value>>{{I(6), std::string("c")}});
    Batch end; CHECK(!op->next(end));
  }
}
TEST(empty_and_zero_column_batches) {
  auto t = table({{1, Type::Int64, "x"}}, {}); Scan scan(t); CHECK(rows(scan).empty());
  auto nonempty = table({{1, Type::Int64, "x"}}, {{I(1)}, {I(2)}});
  Scan pruned(nonempty, std::vector<ColumnId>{}); CHECK(rows(pruned).size() == 2);
}
TEST(hash_collisions_and_resizing) {
  HashIndex h;
  for (std::size_t i = 0; i < 500; ++i) CHECK(h.find_or_insert(7, [i](auto r) { return i == r; }, [i] { return i; }).second);
  CHECK(h.capacity() >= 1024); CHECK(h.size() == 500);
  for (std::size_t i = 0; i < 500; ++i) CHECK(h.find(7, [i](auto r) { return r == i; }) == i);
  CHECK(h.find(7, [](auto) { return false; }) == no_row);
}
TEST(grouped_aggregate_nulls_composite_and_empty) {
  Schema s{{1, Type::Int64, "key"}, {2, Type::String, "tag"}, {3, Type::Int64, "value"}};
  auto t = table(s, {{I(1), std::string("a"), I(3)}, {I(1), std::string("a"), {}}, {{}, std::string("b"), I(8)}, {{}, std::string("b"), I(2)}, {I(1), std::string("b"), {}}});
  std::vector<AggregateSpec> aggs{{{4, Type::Int64, "count"}, AggregateKind::Count, {}}, {{5, Type::Int64, "count_v"}, AggregateKind::Count, 3}, {{6, Type::Int64, "sum"}, AggregateKind::Sum, 3}, {{7, Type::Int64, "min"}, AggregateKind::Min, 3}, {{8, Type::Int64, "max"}, AggregateKind::Max, 3}};
  HashAggregate agg(std::make_unique<Scan>(t, ExecutionOptions{2}), {1, 2}, aggs);
  CHECK(rows(agg) == std::vector<std::vector<Value>>{{I(1), std::string("a"), I(2), I(1), I(3), I(3), I(3)}, {{}, std::string("b"), I(2), I(2), I(10), I(2), I(8)}, {I(1), std::string("b"), I(1), I(0), {}, {}, {}}});
  auto empty = table(s, {});
  HashAggregate global(std::make_unique<Scan>(empty), {}, aggs);
  CHECK(rows(global) == std::vector<std::vector<Value>>{{I(0), I(0), {}, {}, {}}});
  HashAggregate grouped(std::make_unique<Scan>(empty), {1}, aggs); CHECK(rows(grouped).empty());
}
TEST(aggregate_randomized_reference_and_overflow) {
  Schema s{{1, Type::Int64, "k"}, {2, Type::Int64, "v"}};
  std::mt19937 rng(42); std::vector<std::vector<Value>> data; std::map<std::int64_t, std::pair<std::int64_t, std::int64_t>> expected;
  for (int i = 0; i < 10000; ++i) { auto k = static_cast<std::int64_t>(rng() % 1500), v = static_cast<std::int64_t>(rng() % 100); data.push_back({I(k), I(v)}); ++expected[k].first; expected[k].second += v; }
  auto t = table(s, data, 127);
  HashAggregate agg(std::make_unique<Scan>(t, ExecutionOptions{63}), {1}, {{{3, Type::Int64, "n"}, AggregateKind::Count, {}}, {{4, Type::Int64, "sum"}, AggregateKind::Sum, 2}});
  auto actual = rows(agg); CHECK(actual.size() == expected.size());
  for (const auto& r : actual) { const auto e = expected.at(std::get<std::int64_t>(r[0])); CHECK(r[1] == I(e.first)); CHECK(r[2] == I(e.second)); }
  auto big = table(s, {{I(1), I(std::numeric_limits<std::int64_t>::max())}, {I(1), I(1)}});
  HashAggregate overflow(std::make_unique<Scan>(big), {}, {{{4, Type::Int64, "sum"}, AggregateKind::Sum, 2}}); CHECK(is_null(rows(overflow)[0][0]));
}
TEST(join_duplicates_nulls_and_both_build_sides) {
  auto l = table({{1, Type::Int64, "k"}, {2, Type::String, "l"}}, {{I(2), std::string("a")}, {I(2), std::string("b")}, {{}, std::string("n")}, {I(3), std::string("c")}});
  auto r = table({{3, Type::Int64, "k"}, {4, Type::Int64, "r"}}, {{I(2), I(10)}, {I(2), I(11)}, {I(2), I(12)}, {{}, I(99)}, {I(4), I(4)}});
  std::vector<std::vector<Value>> expected;
  for (const auto& name : {std::string("a"), std::string("b")}) for (int v : {10, 11, 12}) expected.push_back({I(2), name, I(2), I(v)});
  std::sort(expected.begin(), expected.end());
  for (bool br : {false, true}) for (std::size_t n : {1U, 2U, 8U}) {
    HashJoin join(std::make_unique<Scan>(l, ExecutionOptions{n}), std::make_unique<Scan>(r, ExecutionOptions{n}), {1}, {3}, br);
    auto actual = rows(join); std::sort(actual.begin(), actual.end()); CHECK(actual == expected);
    Batch end; CHECK(!join.next(end));
  }
}
TEST(join_randomized_reference_composite) {
  Schema ls{{1, Type::Int64, "k"}, {2, Type::Int64, "v"}}, rs{{3, Type::Int64, "k"}, {4, Type::Int64, "v"}};
  std::mt19937 rng(71); std::vector<std::vector<Value>> a, b, expected;
  for (int i = 0; i < 170; ++i) a.push_back({i % 13 ? I(rng() % 19) : Value{}, I(rng() % 5)});
  for (int i = 0; i < 93; ++i) b.push_back({i % 7 ? I(rng() % 19) : Value{}, I(rng() % 5)});
  for (const auto& x : a) for (const auto& y : b) if (!is_null(x[0]) && x == y) expected.push_back({x[0], x[1], y[0], y[1]});
  std::sort(expected.begin(), expected.end());
  for (bool br : {true, false}) {
    HashJoin join(std::make_unique<Scan>(table(ls, a, 23), ExecutionOptions{17}), std::make_unique<Scan>(table(rs, b, 11), ExecutionOptions{7}), {1, 2}, {3, 4}, br);
    auto actual = rows(join); std::sort(actual.begin(), actual.end()); CHECK(actual == expected);
  }
}
TEST(sort_limit_null_order_and_stability) {
  auto t = table({{1, Type::Int64, "k"}, {2, Type::Int64, "v"}}, {{I(2), I(7)}, {{}, I(8)}, {I(1), I(9)}, {I(2), I(10)}});
  OperatorPtr op = std::make_unique<Sort>(std::make_unique<Scan>(t, ExecutionOptions{2}), std::vector<SortKey>{{1, false, false}});
  op = std::make_unique<Limit>(std::move(op), 3);
  CHECK(rows(*op) == std::vector<std::vector<Value>>{{I(2), I(7)}, {I(2), I(10)}, {I(1), I(9)}});
  Limit zero(std::make_unique<Scan>(t), 0); CHECK(rows(zero).empty());
}
TEST(storage_statistics_and_pruning) {
  Schema s{{1, Type::Int64, "x"}, {2, Type::String, "text"}};
  auto t = table(s, {{I(1), std::string("a")}, {I(2), std::string("b")}, {I(20), std::string("c")}, {I(21), std::string("d")}, {{}, std::string("e")}, {{}, std::string("f")}}, 2);
  auto pred = binary(ExprKind::Less, col(1), lit(I(5)));
  Scan scan(t, {1}, ExecutionOptions{1}, pred); CHECK(rows(scan).size() == 2);
  CHECK(scan.groups_read() == 1); CHECK(scan.groups_skipped() == 2);
  CHECK(t->statistics()[0].nulls == 2); CHECK(t->statistics()[0].minimum == I(1));
  CHECK(t->statistics()[0].maximum == I(21)); CHECK(t->statistics()[0].approximate_distinct() > 3.0);
  CHECK(!may_match(binary(ExprKind::Greater, lit(I(0)), col(1)), s, t->statistics()));
  CHECK(may_match(unary(ExprKind::IsNull, col(1)), s, t->statistics()));
  CHECK(!may_match(binary(ExprKind::Equal, col(1), null_literal(Type::Int64)), s, t->statistics()));
}
TEST(optimizer_folding_and_null_safe_simplification) {
  auto folded = simplify_expression(binary(ExprKind::Add, lit(I(3)), lit(I(4)))); CHECK(folded->literal == I(7));
  auto zero = simplify_expression(binary(ExprKind::Divide, lit(I(3)), lit(I(0)))); CHECK(is_null(zero->literal));
  CHECK(simplify_expression(binary(ExprKind::And, lit(true), col(1)))->kind == ExprKind::Column);
  CHECK(simplify_expression(binary(ExprKind::Or, lit(false), col(1)))->kind == ExprKind::Column);
  CHECK(simplify_expression(binary(ExprKind::Equal, col(1), col(1)))->kind == ExprKind::Equal);
  CHECK(simplify_expression(binary(ExprKind::Multiply, col(1), lit(I(0))))->kind == ExprKind::Multiply);
}
TEST(optimizer_projection_pushdown_pruning_and_redundancy) {
  auto t = table({{1, Type::Int64, "x"}, {2, Type::Int64, "unused"}}, {{I(1), I(99)}, {I(3), I(99)}, {{}, I(99)}});
  auto p = logical::project(logical::scan(t), {{{7, Type::Int64, "alias"}, col(1)}});
  auto pred = binary(ExprKind::Greater, col(7), lit(I(1)));
  p = logical::filter(logical::filter(p, pred), binary(ExprKind::And, pred, lit(true)));
  auto optimized = optimize(p); auto expected = execute(*lower(p)), actual = execute(*lower(optimized));
  CHECK(rows(*actual) == rows(*expected)); CHECK(optimized->kind == LogicalKind::Projection);
  CHECK(optimized->left->kind == LogicalKind::Filter); CHECK(optimized->left->left->kind == LogicalKind::Scan);
  CHECK(optimized->left->left->columns == std::vector<ColumnId>{1});
  CHECK(p->left->left->left->columns.size() == 2); // Input tree is unchanged.
  auto identity = logical::project(logical::scan(t), {{{1, Type::Int64, "x"}, col(1)}, {{2, Type::Int64, "unused"}, col(2)}});
  CHECK(optimize(identity)->kind == LogicalKind::Scan);
}
TEST(optimizer_join_pushdown_build_choice_and_end_to_end) {
  auto a = table({{1, Type::Int64, "k"}, {2, Type::Int64, "v"}}, {{I(1), I(1)}, {I(2), I(20)}, {I(2), I(30)}, {I(3), I(40)}});
  auto b = table({{3, Type::Int64, "k"}}, {{I(2)}, {I(3)}});
  auto joined = logical::join(logical::scan(a), logical::scan(b), {1}, {3});
  CHECK(lower(joined)->build_right);
  auto filtered = logical::filter(joined, binary(ExprKind::Less, col(2), lit(I(2))));
  auto optimized = optimize(filtered); CHECK(optimized->kind == LogicalKind::Join); CHECK(optimized->left->kind == LogicalKind::Filter);
  CHECK(!lower(optimized)->build_right);
  auto p = logical::aggregate(logical::filter(joined, binary(ExprKind::Greater, col(2), lit(I(10)))), {3}, {{{5, Type::Int64, "sum"}, AggregateKind::Sum, 2}});
  p = logical::limit(logical::sort(p, {{5, false, false}}), 1);
  auto plain = execute(*lower(p), {1}), opt = execute(*lower(optimize(p)), {3});
  CHECK(rows(*plain) == std::vector<std::vector<Value>>{{I(2), I(50)}});
  CHECK(rows(*opt) == std::vector<std::vector<Value>>{{I(2), I(50)}});
}
TEST(optimizer_respects_limit_and_global_aggregate_boundaries) {
  auto t = table({{1, Type::Int64, "x"}}, {{I(1)}, {I(2)}, {I(3)}});
  auto p = logical::filter(logical::limit(logical::scan(t), 1), binary(ExprKind::Greater, col(1), lit(I(1))));
  auto opt = optimize(p); CHECK(opt->kind == LogicalKind::Filter && opt->left->kind == LogicalKind::Limit);
  CHECK(rows(*execute(*lower(opt))).empty());
  auto global = logical::filter(logical::aggregate(logical::scan(t), {}, {{{2, Type::Int64, "n"}, AggregateKind::Count, {}}}), lit(false));
  CHECK(rows(*execute(*lower(optimize(global)))).empty());
  auto grouped = logical::filter(logical::aggregate(logical::scan(t), {1}, {{{2, Type::Int64, "n"}, AggregateKind::Count, {}}}), binary(ExprKind::Greater, col(1), lit(I(1))));
  CHECK(optimize(grouped)->kind == LogicalKind::Aggregate);
}
TEST(analytical_workloads_optimized_and_reference) {
  auto data = workloads::generate(5000, 127);
  for (const auto& p : {workloads::q1(data), workloads::q3(data), workloads::q6(data)}) {
    auto plain = execute(*lower(p), {31, KernelMode::Scalar}), opt = execute(*lower(optimize(p)), {257, KernelMode::Auto});
    auto a = rows(*plain), b = rows(*opt); CHECK(a.size() == b.size());
    for (std::size_t r = 0; r < a.size(); ++r) for (std::size_t c = 0; c < a[r].size(); ++c) {
      if (std::holds_alternative<double>(a[r][c])) CHECK(std::abs(std::get<double>(a[r][c]) - std::get<double>(b[r][c])) < 1e-7);
      else CHECK(a[r][c] == b[r][c]);
    }
  }
  double revenue = 0;
  for (std::size_t i = 0; i < 5000; ++i) {
    const auto day = 9000 + i % 365, qty = 1 + i % 50; const auto disc = static_cast<double>(i % 11) / 100.0;
    if (day >= 9100 && day < 9200 && disc >= 0.05 && disc <= 0.07 && qty < 24) revenue += (10.0 + static_cast<double>((i * 17) % 10000) / 10.0) * disc;
  }
  auto actual = rows(*execute(*lower(optimize(workloads::q6(data))), {63}));
  CHECK(actual.size() == 1); CHECK(std::abs(std::get<double>(actual[0][0]) - revenue) < 1e-9);
}
TEST(blocking_payload_exceeds_selection_index_range) {
  auto t = std::make_shared<Table>(Schema{{1, Type::Int64, "k"}}); Batch b(t->schema(), 2048);
  for (std::int64_t i = 0; i < 70000; ++i) {
    b.columns[0].append(i); if (++b.physical_size == b.capacity) { b.finish(b.physical_size); t->append(b); b.reset(); }
  }
  b.finish(b.physical_size); t->append(b);
  HashAggregate aggregate(std::make_unique<Scan>(t), {1}, {{{2, Type::Int64, "n"}, AggregateKind::Count, {}}});
  auto actual = rows(aggregate); CHECK(actual.size() == 70000); CHECK(actual.back()[0] == I(69999)); CHECK(actual.back()[1] == I(1));
  auto probe = table({{3, Type::Int64, "k"}}, {{I(69999)}, {I(65536)}, {I(1)}});
  HashJoin join(std::make_unique<Scan>(t), std::make_unique<Scan>(probe), {1}, {3}, false); CHECK(rows(join).size() == 3);
}
TEST(ieee_special_values_grouping_join_and_minmax) {
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  auto t = table({{1, Type::Double, "x"}}, {{-0.0}, {0.0}, {nan}, {nan}, {1.0}});
  HashAggregate grouped(std::make_unique<Scan>(t), {1}, {{{2, Type::Int64, "n"}, AggregateKind::Count, {}}});
  auto groups = rows(grouped); CHECK(groups.size() == 3); CHECK(groups[0][1] == I(2)); CHECK(groups[1][1] == I(2));
  HashAggregate minmax(std::make_unique<Scan>(t), {}, {{{2, Type::Double, "min"}, AggregateKind::Min, 1}, {{3, Type::Double, "max"}, AggregateKind::Max, 1}});
  auto values = rows(minmax); CHECK(std::get<double>(values[0][0]) == 0.0); CHECK(std::isnan(std::get<double>(values[0][1])));
  auto r = table({{2, Type::Double, "x"}}, {{0.0}, {nan}});
  HashJoin join(std::make_unique<Scan>(t), std::make_unique<Scan>(r), {1}, {2}); CHECK(rows(join).size() == 2);
}
TEST(pruning_has_no_false_negatives_randomized) {
  std::mt19937 random(991); Schema s{{1, Type::Int64, "integer"}, {2, Type::Double, "floating"}, {3, Type::String, "string"}};
  for (int trial = 0; trial < 80; ++trial) {
    Batch b(s, 127);
    for (int i = 0; i < 127; ++i) {
      const auto x = static_cast<std::int64_t>(random() % 40) - 20;
      b.columns[0].append(x, random() % 5 != 0);
      b.columns[1].append(i % 13 ? static_cast<double>(x) : std::nan(""), random() % 5 != 0);
      b.columns[2].append_string(std::to_string(x), random() % 5 != 0);
    }
    b.finish(127); const auto stats = analyze(b);
    for (auto kind : {ExprKind::Equal, ExprKind::NotEqual, ExprKind::Less, ExprKind::LessEqual, ExprKind::Greater, ExprKind::GreaterEqual}) {
      const auto value = static_cast<std::int64_t>(random() % 100) - 50;
      for (const auto& p : {binary(kind, col(1), lit(value)), binary(kind, lit(value), col(1)), binary(kind, col(2), lit(static_cast<double>(value))), binary(kind, col(3), lit(std::to_string(value)))}) {
        Evaluator evaluator(p, s); const auto& result = evaluator.evaluate(b); bool any = false;
        for (std::size_t i = 0; i < b.size(); ++i) any |= result.validity().valid(i) && result.data<std::uint8_t>()[i];
        CHECK(may_match(p, s, stats) || !any);
      }
    }
  }
}
TEST(optimizer_randomized_scalar_reference) {
  std::mt19937 random(9911);
  for (int trial = 0; trial < 50; ++trial) {
    std::vector<std::vector<Value>> data, expected;
    const auto lo = static_cast<std::int64_t>(random() % 10), hi = 10 + static_cast<std::int64_t>(random() % 10);
    for (int i = 0; i < 150; ++i) {
      Value a = i % 7 ? I(random() % 30) : Value{}, b = i % 11 ? I(random() % 30) : Value{}; data.push_back({a, b});
      if (!is_null(a) && !is_null(b) && std::get<std::int64_t>(a) > lo && std::get<std::int64_t>(b) < hi) expected.push_back({a});
    }
    std::sort(expected.begin(), expected.end()); if (expected.size() > 9) expected.resize(9);
    auto t = table({{1, Type::Int64, "a"}, {2, Type::Int64, "b"}}, data, 17);
    auto predicate = binary(ExprKind::And, binary(ExprKind::Greater, col(1), lit(lo)), binary(ExprKind::Less, col(2), lit(hi)));
    auto p = logical::limit(logical::sort(logical::project(logical::filter(logical::filter(logical::scan(t), predicate), predicate), {{{3, Type::Int64, "alias"}, col(1)}}), {{3}}), 9);
    auto op = execute(*lower(optimize(p)), {static_cast<std::size_t>(1 + random() % 65)});
    CHECK(rows(*op) == expected);
  }
}
TEST(reusing_output_batch_preserves_schema_aliases) {
  auto first = table({{1, Type::Int64, "old_name"}}, {{I(1)}});
  auto second = table({{1, Type::Int64, "new_name"}}, {{I(2)}});
  Batch output; Scan a(first), b(second); CHECK(a.next(output)); CHECK(b.next(output));
  CHECK(output.schema[0].name == "new_name"); CHECK(output.columns[0].value(0) == I(2));
}
}
int main() {
  int failures = 0;
  for (const auto& [name, test] : tests()) try { test(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
  std::cout << tests().size() << " tests, " << failures << " failures\n";
  return failures ? 1 : 0;
}
