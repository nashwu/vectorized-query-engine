#include "vqe/expression.hpp"
#include <algorithm>
#include <limits>

namespace vqe {
ExprPtr col(ColumnId id) { auto e = std::make_shared<Expr>(); e->kind = ExprKind::Column; e->column = id; return e; }
ExprPtr lit(Value v) {
  const auto t = value_type(v); auto e = std::make_shared<Expr>();
  e->kind = ExprKind::Literal; e->literal = std::move(v); e->literal_type = t; return e;
}
ExprPtr null_literal(Type t) { auto e = std::make_shared<Expr>(); e->kind = ExprKind::Literal; e->literal_type = t; return e; }
ExprPtr unary(ExprKind k, ExprPtr a) { auto e = std::make_shared<Expr>(); e->kind = k; e->left = std::move(a); return e; }
ExprPtr binary(ExprKind k, ExprPtr a, ExprPtr b) {
  auto e = std::make_shared<Expr>(); e->kind = k; e->left = std::move(a); e->right = std::move(b); return e;
}
Type expression_type(const ExprPtr& e, const Schema& s) {
  if (!e) throw std::invalid_argument("missing expression");
  if (e->kind == ExprKind::Column) return s[column_index(s, e->column)].type;
  if (e->kind == ExprKind::Literal) return e->literal_type;
  const auto a = expression_type(e->left, s);
  if (e->kind == ExprKind::IsNull) return Type::Boolean;
  if (e->kind == ExprKind::Not) {
    if (a != Type::Boolean) throw std::invalid_argument("NOT requires boolean");
    return Type::Boolean;
  }
  const auto b = expression_type(e->right, s);
  if (a != b) throw std::invalid_argument("expression operands must have the same type");
  switch (e->kind) {
    case ExprKind::Add: case ExprKind::Subtract: case ExprKind::Multiply: case ExprKind::Divide:
      if (a != Type::Int64 && a != Type::Double) throw std::invalid_argument("arithmetic requires numbers");
      return a;
    case ExprKind::And: case ExprKind::Or:
      if (a != Type::Boolean) throw std::invalid_argument("boolean operands required");
      return Type::Boolean;
    default: return Type::Boolean;
  }
}
std::set<ColumnId> referenced_columns(const ExprPtr& e) {
  std::set<ColumnId> out;
  if (!e) return out;
  if (e->kind == ExprKind::Column) out.insert(e->column);
  for (const auto& child : {e->left, e->right}) { auto r = referenced_columns(child); out.insert(r.begin(), r.end()); }
  return out;
}
bool expression_equal(const ExprPtr& a, const ExprPtr& b) {
  if (a == b) return true;
  if (!a || !b) return false;
  return a->kind == b->kind && a->column == b->column && a->literal == b->literal &&
         a->literal_type == b->literal_type && expression_equal(a->left, b->left) && expression_equal(a->right, b->right);
}
std::string describe(const ExprPtr& e) {
  if (!e) return "";
  if (e->kind == ExprKind::Column) return "#" + std::to_string(e->column);
  if (e->kind == ExprKind::Literal) return to_string(e->literal);
  static constexpr const char* names[] = {"column", "literal", "+", "-", "*", "/", "=", "!=", "<", "<=", ">", ">=", "AND", "OR", "NOT", "IS NULL"};
  return std::string(names[static_cast<std::size_t>(e->kind)]) + "(" + describe(e->left) +
         (e->right ? ", " + describe(e->right) : "") + ")";
}
Evaluator::Evaluator(ExprPtr e, const Schema& s, std::size_t n, KernelMode m) : mode_(m) {
  compile(e, s, n); values_.resize(nodes_.size());
}
std::size_t Evaluator::compile(const ExprPtr& e, const Schema& s, std::size_t n) {
  const auto t = expression_type(e, s);
  const auto a = e->left ? compile(e->left, s, n) : 0;
  const auto b = e->right ? compile(e->right, s, n) : 0;
  nodes_.push_back({e, a, b, e->kind == ExprKind::Column ? column_index(s, e->column) : 0, Column(t, n)});
  return nodes_.size() - 1;
}
namespace {
template<class T> bool compare(ExprKind k, T a, T b) {
  switch (k) {
    case ExprKind::Equal: return a == b; case ExprKind::NotEqual: return a != b;
    case ExprKind::Less: return a < b; case ExprKind::LessEqual: return a <= b;
    case ExprKind::Greater: return a > b; case ExprKind::GreaterEqual: return a >= b;
    default: throw std::logic_error("not a comparison");
  }
}
void eval_arithmetic(ExprKind k, const Column& a, const Column& b, Column& out, const Selection& sel) {
  for (std::size_t j = 0; j < sel.size(); ++j) {
    const auto i = sel[j]; bool valid = a.validity().valid(i) && b.validity().valid(i);
    if (valid && a.type() == Type::Int64) {
      const auto x = a.data<std::int64_t>()[i], y = b.data<std::int64_t>()[i];
      auto& r = out.data<std::int64_t>()[i];
      switch (k) {
        case ExprKind::Add: valid = !__builtin_add_overflow(x, y, &r); break;
        case ExprKind::Subtract: valid = !__builtin_sub_overflow(x, y, &r); break;
        case ExprKind::Multiply: valid = !__builtin_mul_overflow(x, y, &r); break;
        case ExprKind::Divide:
          valid = y != 0 && !(x == std::numeric_limits<std::int64_t>::min() && y == -1);
          if (valid) r = x / y;
          break;
        default: break;
      }
    } else if (valid) {
      const auto x = a.data<double>()[i], y = b.data<double>()[i]; auto& r = out.data<double>()[i];
      switch (k) {
        case ExprKind::Add: r = x + y; break; case ExprKind::Subtract: r = x - y; break;
        case ExprKind::Multiply: r = x * y; break;
        case ExprKind::Divide: valid = y != 0; if (valid) r = x / y; break;
        default: break;
      }
    }
    out.validity().set(i, valid);
  }
}
}
const Column& Evaluator::evaluate(const Batch& batch) {
  for (std::size_t ni = 0; ni < nodes_.size(); ++ni) {
    auto& node = nodes_[ni]; const auto& e = node.expression; auto& out = node.buffer;
    if (e->kind == ExprKind::Column) { values_[ni] = &batch.columns.at(node.column_index); continue; }
    values_[ni] = &out;
    if (e->kind == ExprKind::Literal) {
      out.reset(); for (std::size_t i = 0; i < batch.physical_size; ++i) out.append_value(e->literal);
      continue;
    }
    out.reset(batch.physical_size);
    const auto& a = *values_[node.left];
    const auto* b = e->right ? values_[node.right] : nullptr;
    const bool dense_valid = batch.selection.dense() && a.validity().all_valid() && b && b->validity().all_valid();
    if (dense_valid && e->kind == ExprKind::Less && a.type() == Type::Int64 && e->right->kind == ExprKind::Literal) {
      less_i64_constant(a.data<std::int64_t>().data(), std::get<std::int64_t>(e->right->literal),
                        out.data<std::uint8_t>().data(), batch.size(), mode_); continue;
    }
    if (dense_valid && e->kind == ExprKind::Add && a.type() == Type::Double) {
      add_f64(a.data<double>().data(), b->data<double>().data(), out.data<double>().data(), batch.size(), mode_); continue;
    }
    if (e->kind >= ExprKind::Add && e->kind <= ExprKind::Divide) { eval_arithmetic(e->kind, a, *b, out, batch.selection); continue; }
    auto& dest = out.data<std::uint8_t>();
    for (std::size_t j = 0; j < batch.size(); ++j) {
      const auto i = batch.selection[j]; const bool av = a.validity().valid(i);
      if (e->kind == ExprKind::IsNull) { dest[i] = !av; continue; }
      if (e->kind == ExprKind::Not) { dest[i] = !a.data<std::uint8_t>()[i]; out.validity().set(i, av); continue; }
      const bool bv = b->validity().valid(i);
      if (e->kind == ExprKind::And || e->kind == ExprKind::Or) {
        const bool x = a.data<std::uint8_t>()[i] != 0, y = b->data<std::uint8_t>()[i] != 0;
        const bool is_and = e->kind == ExprKind::And;
        dest[i] = is_and ? x && y : x || y;
        out.validity().set(i, (av && bv) || (av && x != is_and) || (bv && y != is_and));
      } else {
        out.validity().set(i, av && bv); if (!av || !bv) continue;
        switch (a.type()) {
          case Type::Boolean: dest[i] = compare(e->kind, a.data<std::uint8_t>()[i], b->data<std::uint8_t>()[i]); break;
          case Type::Int64: dest[i] = compare(e->kind, a.data<std::int64_t>()[i], b->data<std::int64_t>()[i]); break;
          case Type::Double: dest[i] = compare(e->kind, a.data<double>()[i], b->data<double>()[i]); break;
          case Type::String: dest[i] = compare(e->kind, a.string_at(i), b->string_at(i)); break;
        }
      }
    }
  }
  return *values_.back();
}
std::size_t Evaluator::allocated_bytes() const { std::size_t n = 0; for (const auto& x : nodes_) n += x.buffer.allocated_bytes(); return n; }
} // namespace vqe
