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
// "set by the analyzer" (types and slots), and the interpreter runs it.

struct Pou;

// ---------------------------------------------------------------- Types

enum class DataType {
    Unknown,       // not analyzed yet
    Bool,          // TRUE / FALSE
    Int,           // 16-bit signed: -32768 .. 32767
    DInt,          // 32-bit signed
    Real,          // 32-bit floating point
    Time,          // a duration, in milliseconds (32-bit, like Siemens)
    AnyInt,        // an integer literal like 42, which adapts to INT, DINT or REAL
    FunctionBlock, // an instance of a function block, like `t1 : TON`
};

const char* dataTypeName(DataType type); // "BOOL", "INT", ...

// A runtime value. Every integer type (and TIME) is held in an int64 and
// every REAL in a double; the interpreter keeps them inside their type's range.
using Value = std::variant<bool, std::int64_t, double>;

std::string formatValue(const Value& value);                // TRUE, 42, 3.5
std::string formatValue(const Value& value, DataType type); // as above, but TIME as T#1s500ms
std::string formatTime(std::int64_t milliseconds);          // T#1m30s

// Marks a reference that the analyzer hasn't resolved yet.
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
    bool isTime = false; // T#... literals hold milliseconds in an int64
};

struct VariableRef {
    std::string name;
    std::size_t slot = kNoSlot; // set by the analyzer: index into the POU's variables
};

// Reads an input or output of a function block instance: t1.Q
struct MemberRef {
    std::string instance;
    std::string member;
    std::size_t instanceSlot = kNoSlot; // set by the analyzer: the instance variable
    std::size_t memberSlot = kNoSlot;   // set by the analyzer: the member inside the instance
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

// One node of an expression tree: exactly one of the kinds above.
struct Expression {
    std::variant<Literal, VariableRef, MemberRef, UnaryExpression, BinaryExpression> node;
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

// One `NAME := value` inside a call.
struct CallArgument {
    std::string name;
    ExpressionPtr value;
    SourceLocation location;
    std::size_t slot = kNoSlot; // set by the analyzer: the input inside the instance
};

// Calls a function block instance: t1(IN := start, PT := T#2s);
struct CallStatement {
    std::string instance;
    std::vector<CallArgument> arguments;
    std::size_t instanceSlot = kNoSlot; // set by the analyzer
};

struct Statement {
    std::variant<Assignment, IfStatement, CaseStatement, ForStatement, WhileStatement, RepeatStatement,
                 ExitStatement, ReturnStatement, CallStatement>
        node;
    SourceLocation location;
};

// ---------------------------------------------------------------- POUs

enum class VarSection {
    Local,  // VAR: internal memory
    Input,  // VAR_INPUT: set by the caller
    Output, // VAR_OUTPUT: read by the caller
};

struct VariableDeclaration {
    std::string name;
    DataType type = DataType::Unknown;
    std::string typeName;           // the function block's name, when type is FunctionBlock
    VarSection section = VarSection::Local;
    std::optional<Address> address; // set for `name AT %QX0.0 : BOOL`
    ExpressionPtr initialValue;     // null when there's no `:= value`
    SourceLocation location;
    const Pou* functionBlock = nullptr; // set by the analyzer, when type is FunctionBlock
};

enum class PouKind { Program, FunctionBlock };

// A Program Organization Unit: a PROGRAM or a FUNCTION_BLOCK. Both are
// variables plus a body; a function block can have many instances, each
// with its own copy of the variables.
struct Pou {
    PouKind kind = PouKind::Program;
    std::string name;
    std::vector<VariableDeclaration> variables; // `a, b : INT` is stored as two declarations
    StatementList body;
    SourceLocation location;
    bool builtin = false; // a standard block like TON, implemented in C++ (its body is empty)
};

// Everything in one source file: any number of function blocks and one program.
//
// The analyzer stores pointers to the function blocks (VariableDeclaration::
// functionBlock), so a CompilationUnit must stay where it is or be moved as a
// whole, never copied piece by piece. (It can't be copied anyway.)
struct CompilationUnit {
    std::vector<Pou> functionBlocks;
    Pou program;
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
std::string dumpTree(const Pou& pou);
std::string dumpTree(const CompilationUnit& unit);

} // namespace softplc::st
