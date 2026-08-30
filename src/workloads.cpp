#include "vqe/workloads.hpp"
#include <algorithm>
#include <limits>

namespace vqe::workloads {
namespace {
ExprPtr number(std::int64_t x) { return lit(x); }
ExprPtr both(ExprPtr a, ExprPtr b) { return binary(ExprKind::And, std::move(a), std::move(b)); }
void flush(Table& t, Batch& b) { b.finish(b.physical_size); t.append(b); b.reset(); }
void row_done(Table& t, Batch& b) { if (++b.physical_size == b.capacity) flush(t, b); }
}
Data generate(std::size_t count, std::size_t group_size) {
  if (count > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) throw std::length_error("too many rows");
  Data d;
  d.lineitem = std::make_shared<Table>(Schema{{1, Type::Int64, "l_orderkey"}, {2, Type::Int64, "l_quantity"}, {3, Type::Double, "l_extendedprice"}, {4, Type::Double, "l_discount"}, {5, Type::Int64, "l_shipday"}, {6, Type::String, "l_returnflag"}, {7, Type::String, "l_linestatus"}});
  d.orders = std::make_shared<Table>(Schema{{11, Type::Int64, "o_orderkey"}, {12, Type::Int64, "o_custkey"}, {13, Type::Int64, "o_orderday"}});
  d.customer = std::make_shared<Table>(Schema{{21, Type::Int64, "c_custkey"}, {22, Type::String, "c_mktsegment"}});
  const auto orders = std::max(std::size_t{1}, count / 4), customers = std::max(std::size_t{1}, orders / 10);
  Batch l(d.lineitem->schema(), group_size), o(d.orders->schema(), group_size), c(d.customer->schema(), group_size);
  for (std::size_t i = 0; i < count; ++i) {
    l.columns[0].append(static_cast<std::int64_t>(i % orders));
    l.columns[1].append(static_cast<std::int64_t>(1 + i % 50));
    l.columns[2].append(10.0 + static_cast<double>((i * 17) % 10000) / 10.0);
    l.columns[3].append(static_cast<double>(i % 11) / 100.0);
    l.columns[4].append(static_cast<std::int64_t>(9000 + i % 365));
    l.columns[5].append_string(i % 3 == 0 ? "R" : (i % 3 == 1 ? "A" : "N"));
    l.columns[6].append_string(i % 2 ? "O" : "F"); row_done(*d.lineitem, l);
  }
  flush(*d.lineitem, l);
  for (std::size_t i = 0; i < orders; ++i) {
    o.columns[0].append(static_cast<std::int64_t>(i)); o.columns[1].append(static_cast<std::int64_t>(i % customers));
    o.columns[2].append(static_cast<std::int64_t>(9000 + i % 200)); row_done(*d.orders, o);
  }
  flush(*d.orders, o);
  for (std::size_t i = 0; i < customers; ++i) {
    c.columns[0].append(static_cast<std::int64_t>(i)); c.columns[1].append_string(i % 5 == 0 ? "BUILDING" : "OTHER"); row_done(*d.customer, c);
  }
  flush(*d.customer, c); return d;
}
Plan q1(const Data& d) {
  auto input = logical::filter(logical::scan(d.lineitem, "lineitem"), binary(ExprKind::LessEqual, col(5), number(9300)));
  input = logical::project(input, {{{6, Type::String, "l_returnflag"}, col(6)}, {{7, Type::String, "l_linestatus"}, col(7)}, {{2, Type::Int64, "l_quantity"}, col(2)}, {{3, Type::Double, "l_extendedprice"}, col(3)}, {{31, Type::Double, "discounted_price"}, binary(ExprKind::Multiply, col(3), binary(ExprKind::Subtract, lit(1.0), col(4)))}});
  auto grouped = logical::aggregate(input, {6, 7}, {{{41, Type::Int64, "sum_qty"}, AggregateKind::Sum, 2}, {{42, Type::Double, "sum_base_price"}, AggregateKind::Sum, 3}, {{43, Type::Double, "sum_disc_price"}, AggregateKind::Sum, 31}, {{44, Type::Int64, "count_order"}, AggregateKind::Count, {}}});
  return logical::sort(grouped, {{6}, {7}});
}
Plan q6(const Data& d) {
  auto predicate = both(binary(ExprKind::GreaterEqual, col(5), number(9100)), binary(ExprKind::Less, col(5), number(9200)));
  predicate = both(predicate, both(binary(ExprKind::GreaterEqual, col(4), lit(0.05)), binary(ExprKind::LessEqual, col(4), lit(0.07))));
  predicate = both(predicate, binary(ExprKind::Less, col(2), number(24)));
  auto input = logical::filter(logical::scan(d.lineitem, "lineitem"), predicate);
  input = logical::project(input, {{{31, Type::Double, "revenue"}, binary(ExprKind::Multiply, col(3), col(4))}});
  return logical::aggregate(input, {}, {{{41, Type::Double, "revenue"}, AggregateKind::Sum, 31}});
}
Plan q3(const Data& d) {
  auto customers = logical::filter(logical::scan(d.customer, "customer"), binary(ExprKind::Equal, col(22), lit(std::string("BUILDING"))));
  auto orders = logical::filter(logical::scan(d.orders, "orders"), binary(ExprKind::Less, col(13), number(9150)));
  auto lineitems = logical::filter(logical::scan(d.lineitem, "lineitem"), binary(ExprKind::Greater, col(5), number(9150)));
  auto joined = logical::join(logical::join(customers, orders, {21}, {12}), lineitems, {11}, {1});
  auto projected = logical::project(joined, {{{11, Type::Int64, "o_orderkey"}, col(11)}, {{13, Type::Int64, "o_orderday"}, col(13)}, {{31, Type::Double, "discounted_price"}, binary(ExprKind::Multiply, col(3), binary(ExprKind::Subtract, lit(1.0), col(4)))}});
  return logical::limit(logical::sort(logical::aggregate(projected, {11, 13}, {{{41, Type::Double, "revenue"}, AggregateKind::Sum, 31}}), {{41, false}, {13}, {11}}), 10);
}
}
