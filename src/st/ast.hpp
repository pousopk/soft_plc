#pragma once
#include "st/diagnostic.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <variant>

namespace softplc::st {

// The abstract syntax tree (AST): the structure of the code, without the
// details of how it was written (spaces, comments, redundant parentheses).
//
//   a := b + 3 * c;     becomes     Assignment a
//                                     Binary +
//                                       Variable b
//                                       Binary *
//                                         Integer 3
//                                         Variable c

struct Expression;
// Children are owned by their parent through unique_ptr: when the root of a
// tree is destroyed, the whole tree goes with it.
using ExpressionPtr = std::unique_ptr<Expression>;

enum class UnaryOperator { Negate, Not };

enum class BinaryOperator {
    Add, Subtract, Multiply, Divide, Modulo, Power,
    Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
    And, Or, Xor,
};

struct Literal {
    std::variant<bool, std::int64_t, double> value;
};

struct VariableRef {
    std::string name;
};

struct UnaryExpression {
    UnaryOperator op;
    ExpressionPtr operand;
};

struct BinaryExpression {
    BinaryOperator op;
    ExpressionPtr left;
    ExpressionPtr right;
};

// One node of an expression tree: exactly one of the four kinds above.
struct Expression {
    std::variant<Literal, VariableRef, UnaryExpression, BinaryExpression> node;
    SourceLocation location;
};

struct Assignment {
    std::string target;
    ExpressionPtr value;
    SourceLocation location;
};

const char* operatorSymbol(UnaryOperator op);  // "-", "NOT"
const char* operatorSymbol(BinaryOperator op); // "+", "<=", "AND", ...

// One line with every operation in parentheses, showing exactly how the
// parser grouped things: a + b * c  ->  (a + (b * c))
std::string formatExpression(const Expression& expression);

// An indented tree, one node per line, like the diagram at the top of this file.
std::string dumpTree(const Assignment& assignment);

} // namespace softplc::st
