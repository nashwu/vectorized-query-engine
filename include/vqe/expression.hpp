#pragma once
#include "vqe/vector.hpp"
#include "vqe/kernels.hpp"
#include <memory>
#include <set>

namespace vqe {
enum class ExprKind { Column, Literal, Add, Subtract, Multiply, Divide, Equal,
  NotEqual, Less, LessEqual, Greater, GreaterEqual, And, Or, Not, IsNull };
struct Expr;
using ExprPtr = std::shared_ptr<const Expr>;
struct Expr {
  ExprKind kind;
  ColumnId column = 0;
  Value literal;
  Type literal_type = Type::Int64;
  ExprPtr left, right;
};
ExprPtr col(ColumnId);
ExprPtr lit(Value);
ExprPtr null_literal(Type);
ExprPtr unary(ExprKind, ExprPtr);
ExprPtr binary(ExprKind, ExprPtr, ExprPtr);
Type expression_type(const ExprPtr&, const Schema&);
std::set<ColumnId> referenced_columns(const ExprPtr&);
bool expression_equal(const ExprPtr&, const ExprPtr&);
std::string describe(const ExprPtr&);

// Compilation binds column IDs once and reserves scratch vectors per expression
// node. Evaluation visits whole vectors in postorder, never dispatching an AST
// per tuple. Returned references live until the next evaluation/input mutation.
class Evaluator {
 public:
  Evaluator(ExprPtr expression, const Schema&, std::size_t capacity = default_batch_size,
            KernelMode mode = KernelMode::Auto);
  const Column& evaluate(const Batch&);
  std::size_t allocated_bytes() const;
 private:
  struct Node {
    ExprPtr expression;
    std::size_t left = 0, right = 0, column_index = 0;
    Column buffer;
  };
  std::size_t compile(const ExprPtr&, const Schema&, std::size_t capacity);
  std::vector<Node> nodes_;
  std::vector<const Column*> values_;
  KernelMode mode_;
};
} // namespace vqe
