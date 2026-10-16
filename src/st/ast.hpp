#pragma once
#include "core/address.hpp"
#include "st/diagnostic.hpp"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

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
//
// The parser builds the tree. The analyzer then fills in the fields marked
// "set by the analyzer" (types and variable slots), and the interpreter runs it.

// ---------------------------------------------------------------- Types

enum class DataType {
    Unknown, // not analyzed yet
    Bool,    // TRUE / FALSE
    Int,     // 16-bit signed: -32768 .. 32767
    DInt,    // 32-bit signed
    Real,    // 32-bit floating point
    AnyInt,  // an integer literal like 42, which adapts to INT or DINT
};

const char* dataTypeName(DataType type); // "BOOL", "INT", ...

// A runtime value. Every integer type is held in an int64 and every REAL in
// a double; the interpreter keeps them inside their type's range.
using Value = std::variant<bool, std::int64_t, double>;

std::string formatValue(const Value& value); // TRUE, 42, 3.5

// Marks a variable reference that the analyzer hasn't resolved yet.
constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();

// ---------------------------------------------------------------- Expressions

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
    Value value;
};

struct VariableRef {
    std::string name;
    std::size_t slot = kNoSlot; // set by the analyzer: index into ProgramUnit::variables
};

struct UnaryExpression {
    UnaryOperator op;
    ExpressionPtr operand;
};

struct BinaryExpression {
    BinaryOperator op;
    ExpressionPtr left;
    ExpressionPtr right;
    // Set by the analyzer: the type both operands are brought to before the
    // operation, e.g. REAL for INT + REAL.
    DataType operandType = DataType::Unknown;
};

// One node of an expression tree: exactly one of the four kinds above.
struct Expression {
    std::variant<Literal, VariableRef, UnaryExpression, BinaryExpression> node;
    SourceLocation location;
    DataType type = DataType::Unknown; // set by the analyzer: the type of the result
};

// ---------------------------------------------------------------- Statements

struct Statement;
using StatementList = std::vector<Statement>;

struct Assignment {
    std::string target;
    ExpressionPtr value;
    std::size_t slot = kNoSlot; // set by the analyzer
};

struct IfBranch {
    ExpressionPtr condition;
    StatementList body;
};

struct IfStatement {
    std::vector<IfBranch> branches; // the IF, then each ELSIF, in order
    StatementList elseBody;         // empty if there's no ELSE
};

struct CaseRange {
    std::int64_t low;
    std::int64_t high; // equal to low for a single label like `3:`
};

struct CaseBranch {
    std::vector<CaseRange> labels; // `1, 3..5:` is two ranges
    StatementList body;
};

struct CaseStatement {
    ExpressionPtr selector;
    std::vector<CaseBranch> branches;
    StatementList elseBody;
};

struct ForStatement {
    std::string variable;
    ExpressionPtr from;
    ExpressionPtr to;
    ExpressionPtr step; // null when there's no BY (step 1)
    StatementList body;
    std::size_t slot = kNoSlot; // set by the analyzer
};

struct WhileStatement {
    ExpressionPtr condition;
    StatementList body;
};

struct RepeatStatement {
    StatementList body;
    ExpressionPtr condition; // the UNTIL condition
};

struct ExitStatement {};
struct ReturnStatement {};

struct Statement {
    std::variant<Assignment, IfStatement, CaseStatement, ForStatement, WhileStatement, RepeatStatement,
                 ExitStatement, ReturnStatement>
        node;
    SourceLocation location;
};

// ---------------------------------------------------------------- Programs

struct VariableDeclaration {
    std::string name;
    DataType type = DataType::Unknown;
    std::optional<Address> address; // set for `name AT %QX0.0 : BOOL`
    ExpressionPtr initialValue;     // null when there's no `:= value`
    SourceLocation location;
};

struct ProgramUnit {
    std::string name;
    std::vector<VariableDeclaration> variables; // `a, b : INT` is stored as two declarations
    StatementList body;
};

// ---------------------------------------------------------------- Helpers

const char* operatorSymbol(UnaryOperator op);  // "-", "NOT"
const char* operatorSymbol(BinaryOperator op); // "+", "<=", "AND", ...

// A deep copy of an expression tree (unique_ptr can't be copied directly).
ExpressionPtr cloneExpression(const Expression& expression);

// One line with every operation in parentheses, showing exactly how the
// parser grouped things: a + b * c  ->  (a + (b * c))
std::string formatExpression(const Expression& expression);

// Indented trees, one node per line, like the diagram at the top of this file.
std::string dumpTree(const Statement& statement);
std::string dumpTree(const ProgramUnit& program);

} // namespace softplc::st
