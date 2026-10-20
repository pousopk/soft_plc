#pragma once
#include "core/address.hpp"
#include "st/diagnostic.hpp"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <string>
#include <string_view>
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
    SInt,          // 8-bit signed: -128 .. 127
    USInt,         // 8-bit unsigned: 0 .. 255
    UInt,          // 16-bit unsigned: 0 .. 65535
    UDInt,         // 32-bit unsigned
    LInt,          // 64-bit signed
    Byte,          // 8 bits: a bit string, not a number
    Word,          // 16 bits
    DWord,         // 32 bits
    Real,          // 32-bit floating point
    Time,          // a duration, in milliseconds (32-bit, like Siemens)
    String,        // text of 1-byte characters, at most a fixed length: STRING[20] (254 without a length)
    WString,       // text of 2-byte characters: WSTRING[20]
    Char,          // one 1-byte character, held as its code
    WChar,         // one 2-byte character
    AnyInt,        // an integer literal like 42, which adapts to any integer type, bit string or REAL
    FunctionBlock, // an instance of a function block, like `t1 : TON`
    Array,         // ARRAY[1..10] OF INT
    Struct,        // a STRUCT declared in a TYPE block
};

const char* dataTypeName(DataType type); // "BOOL", "INT", ...

// The elementary type with that name (any case), nothing otherwise.
std::optional<DataType> elementaryTypeFromName(std::string_view name);

// Every elementary type: BOOL, SINT, ..., REAL, TIME.
std::span<const DataType> elementaryTypes();

// Families of types, as IEC groups them.
bool isIntegerType(DataType type);  // SINT .. LINT, signed or not (not integer literals)
bool isUnsignedType(DataType type); // USINT, UINT, UDINT
bool isBitString(DataType type);    // BYTE, WORD, DWORD (BOOL is a bit string too, but treated apart)
bool isStringType(DataType type);   // STRING, WSTRING
bool isCharType(DataType type);     // CHAR, WCHAR
std::size_t stringBits(DataType type, std::size_t length); // the size of STRING[length] or WSTRING[length] in memory

// The length of STRING and WSTRING without one, and the most they can have.
constexpr std::size_t kDefaultStringLength = 254;
constexpr std::size_t kMaxStringLength = 254;
constexpr std::size_t kMaxWStringLength = 16382;

// Size in bits: 1 for BOOL, 8 for BYTE, ..., 64 for LINT; 0 for non-elementary types.
unsigned bitWidth(DataType type);

// The smallest and largest value of an integer type, bit string (as an
// unsigned number) or TIME (in milliseconds).
std::pair<std::int64_t, std::int64_t> integerRange(DataType type);

// A runtime value. Every integer type (and TIME) is held in an int64 and
// every REAL in a double; the interpreter keeps them inside their type's range.
using Value = std::variant<bool, std::int64_t, double>;

std::string formatValue(const Value& value);                // TRUE, 42, 3.5
std::string formatValue(const Value& value, DataType type); // as above, but TIME as T#1s500ms, bit strings as 16#00FF
std::string formatTime(std::int64_t milliseconds);          // T#1m30s

// Marks a reference that the analyzer hasn't resolved yet.
constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();

// One dimension of an array: ARRAY[low..high].
struct ArrayDimension {
    std::int64_t low = 0;
    std::int64_t high = 0;

    std::int64_t length() const { return high - low + 1; }
    bool operator==(const ArrayDimension&) const = default;
};

// A type as written in the source: a name (BOOL, Point, TON), or
// ARRAY[dimensions] OF a name.
struct TypeSpec {
    std::string name;
    std::vector<ArrayDimension> dimensions; // empty when it's not an array
    SourceLocation location;
    std::optional<std::int64_t> stringLength; // STRING[20]: the 20
};

std::string formatTypeSpec(const TypeSpec& spec); // "INT", "ARRAY[1..10, 0..3] OF Point"

struct Initializer;

// The analyzed form of a type, built by the analyzer.
//
// Every variable of a type is stored as a row of *leaves*: one per BOOL,
// INT, DINT, REAL or TIME inside it. An INT is 1 leaf; a struct with three
// REALs is 3 leaves; ARRAY[1..10] OF that struct is 30 leaves, element after
// element. An array element or struct field is then simply "the leaves from
// offset N", which makes indexing a multiplication.
//
// For variables placed in memory with AT, the type also has a layout in
// bits, following Siemens' standard (non-optimized) access rules, so S7
// clients find the fields where TIA Portal would put them.
struct Type {
    enum class Kind { Elementary, Array, Struct };

    struct Field {
        std::string name;
        const Type* type = nullptr;
        std::size_t leafOffset = 0;              // where it starts, in leaves
        std::size_t bitOffset = 0;               // where it starts, in bits (S7 layout)
        const Initializer* initialValue = nullptr; // the default from the TYPE declaration, if any
    };

    struct Leaf {
        DataType type;          // an elementary type: BOOL, INT, ..., STRING
        std::size_t bitOffset;  // from the start of the type (S7 layout)
        std::string suffix;     // the path from the type to the leaf: "", ".x", "[3].y"
        std::size_t stringLength = 0; // for STRING and WSTRING: how many characters fit
    };

    Kind kind = Kind::Elementary;
    std::string name;                        // "INT", "Point", "ARRAY[1..3] OF INT"
    DataType elementary = DataType::Unknown; // Elementary: which one; otherwise Array or Struct
    std::vector<ArrayDimension> dimensions;  // Array
    const Type* element = nullptr;           // Array
    std::size_t elementLeaves = 0;           // Array: leaves per element
    std::size_t elementBits = 0;             // Array: bits per element, S7 layout
    std::vector<Field> fields;               // Struct
    std::vector<Leaf> leaves;                // every leaf, in order
    std::size_t bitSize = 0;                 // size in the S7 layout, in bits
    std::size_t stringLength = 0;            // STRING and WSTRING: how many characters fit

    std::size_t leafCount() const { return leaves.size(); }
    bool isAggregate() const { return kind != Kind::Elementary; }
    const Field* field(std::string_view name) const; // case-insensitive; null if there's none
};

// The shared Type for BOOL, INT, DINT, REAL or TIME.
const Type* elementaryType(DataType type);

// Two types are the same if they're the same declared type, or arrays with
// the same dimensions of the same element type.
bool sameType(const Type& a, const Type& b);

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
    bool isTime = false;                // T#... literals hold milliseconds in an int64
    DataType type = DataType::Unknown;  // set for typed literals like INT#5, and for all text literals
    std::u16string text{};              // the text of a STRING or WSTRING literal
};

// `.name` after a variable: a struct field, or a function block's input or output.
struct FieldSelector {
    std::string name;
    SourceLocation location;
};

// `[i]` or `[i, j]` after a variable: array indices.
struct IndexSelector {
    std::vector<ExpressionPtr> indices;
    SourceLocation location;
};

// `.%X3`, `.%B1` or `.%W0` after a variable: one of its bits, bytes or
// words, counted from the least significant (as in TIA Portal).
struct PartSelector {
    AccessSize size = AccessSize::Bit; // Bit, Byte or Word
    unsigned index = 0;
    SourceLocation location;
};

using Selector = std::variant<FieldSelector, IndexSelector, PartSelector>;

// The bits of a leaf that a part access reads and writes (set by the analyzer).
struct BitPart {
    unsigned shift = 0;                 // from the least significant bit
    unsigned width = 0;                 // 1, 8 or 16
    DataType whole = DataType::Unknown; // the type of the leaf it's part of
};

// One array index that's only known at run time (set by the analyzer).
struct IndexTerm {
    const Expression* index = nullptr; // points into the Access's selectors
    ArrayDimension bounds;
    std::size_t leafStride = 0; // leaves per step of this index
    std::size_t bitStride = 0;  // bits per step, for AT variables
};

// A variable, or a part of one: count, t1.Q, values[i], line[2].start.x.
//
// The parser fills in `name` and `selectors`; the analyzer resolves them to
// a variable plus an offset. Constant indices go into the fixed offsets;
// indices only known at run time become `dynamic` terms.
struct Access {
    std::string name;
    std::vector<Selector> selectors;

    // Set by the analyzer:
    std::size_t instanceSlot = kNoSlot; // fb.member: the instance variable; kNoSlot for the POU's own variables
    std::size_t slot = kNoSlot;         // the variable: in this POU, or in the function block when instanceSlot is set
    std::size_t leafOffset = 0;         // fixed part of the offset inside the variable, in leaves
    std::size_t bitOffset = 0;          // the same, in bits (for AT variables)
    std::vector<IndexTerm> dynamic;     // indices computed at run time
    const Type* type = nullptr;         // the type of what is accessed
    // The leaf to use directly, when it's fixed: no run-time index and not
    // an AT variable (the common case, which both engines make fast).
    std::size_t directLeaf = kNoSlot;
    // For `.%X3` and the like: which bits of the leaf. `type` is then the
    // part's type (BOOL, BYTE or WORD), and there is no direct leaf.
    std::optional<BitPart> part;
    // For s[i] on a STRING or WSTRING: the index (counting from 1). `type` is
    // then CHAR or WCHAR, and there is no direct leaf.
    const Expression* charIndex = nullptr;

    bool isPlainVariable() const { return instanceSlot == kNoSlot && selectors.empty(); }
};

// One argument of a call: `NAME := value`, or just `value` in a function
// call that gives its arguments in order.
struct CallArgument {
    std::string name; // empty for an argument given by position
    ExpressionPtr value;
    SourceLocation location;
    std::size_t slot = kNoSlot; // set by the analyzer: the input inside the instance or function
};

// The standard functions, built into the language (see src/st/functions.hpp).
enum class BuiltinFunction {
    None,
    Abs, Sqrt, Ln, Log, Exp, Sin, Cos, Tan, Asin, Acos, Atan, Expt,
    Trunc, Round, Ceil, Floor,
    Min, Max, Limit, Sel, Mux,
    Shl, Shr, Rol, Ror, // shift and rotate a bit string
    Len, Left, Right, Mid, Concat, Insert, Delete, Replace, Find, // texts (see string_functions.hpp)
    ToString, FromString,    // INT_TO_STRING, STRING_TO_INT, ...: a value as text, and back
    TextToText,              // STRING_TO_WSTRING, WSTRING_TO_STRING
    CharToText, TextToChar,  // CHAR_TO_STRING, STRING_TO_CHAR (and the W versions)
    Convert, // BOOL_TO_INT, DINT_TO_INT, REAL_TO_DINT, ...: from argumentTypes[0] to the result type
    DWordToReal, RealToDWord, // copy the 32 bits unchanged, e.g. to decode a float received as a DWORD
};

// A function call inside an expression: Scale(raw := r, low := 0.0) or MAX(a, b).
struct CallExpression {
    std::string name;
    std::vector<CallArgument> arguments;

    // Set by the analyzer: either a FUNCTION from the source file...
    std::size_t functionIndex = kNoSlot; // index into CompilationUnit::functions
    // ...or a standard function, with its arguments put in order and the
    // type each one is converted to before the call.
    BuiltinFunction builtin = BuiltinFunction::None;
    std::vector<DataType> argumentTypes;
    DataType resultType = DataType::Unknown; // for both kinds
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
    std::variant<Literal, Access, CallExpression, UnaryExpression, BinaryExpression> node;
    SourceLocation location;
    DataType type = DataType::Unknown; // set by the analyzer: the type of the result
    // Set by the analyzer for expressions that make a text (a literal, a
    // call giving a STRING): the scratch buffer it's made in, an index into
    // its POU's stringTemps.
    std::size_t stringTemp = kNoSlot;
};

// ---------------------------------------------------------------- Statements

struct Statement;
using StatementList = std::vector<Statement>;

struct Assignment {
    Access target;
    ExpressionPtr value;
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

// ---------------------------------------------------------------- Initial values

struct Initializer;
using InitializerPtr = std::unique_ptr<Initializer>;

// One item of an array initializer: a value, or `3(0)` for three times 0.
struct ArrayInitItem {
    std::int64_t repeat = 1;
    InitializerPtr value; // null for `3()`: three default values
};

// [1, 2, 3]  or  [10(0)]  or  [(x := 1.0), (x := 2.0)]
struct ArrayInitializer {
    std::vector<ArrayInitItem> items;
};

struct StructInitField {
    std::string name;
    InitializerPtr value;
    SourceLocation location;
};

// (x := 1.0, y := 2.0): fields not listed keep their default.
struct StructInitializer {
    std::vector<StructInitField> fields;
};

struct Initializer {
    std::variant<ExpressionPtr, ArrayInitializer, StructInitializer> node;
    SourceLocation location;
};

InitializerPtr cloneInitializer(const Initializer& initializer);
std::string formatInitializer(const Initializer& initializer); // as it would be written

// ---------------------------------------------------------------- POUs

enum class VarSection {
    Local,  // VAR: internal memory
    Input,  // VAR_INPUT: set by the caller
    Output, // VAR_OUTPUT: read by the caller
};

struct VariableDeclaration {
    std::string name;
    TypeSpec typeSpec;                    // as written
    DataType type = DataType::Unknown;    // set by the parser for BOOL..TIME, by the analyzer for the rest
    VarSection section = VarSection::Local;
    std::optional<Address> address;       // set for `name AT %QX0.0 : BOOL`
    InitializerPtr initialValue;          // null when there's no `:= value`
    SourceLocation location;
    // Set by the analyzer:
    const Type* typeInfo = nullptr;     // for everything but function block instances
    const Pou* functionBlock = nullptr; // when type is FunctionBlock
    std::size_t leaf = kNoSlot;         // where its leaves start in the instance; none for AT and FB variables
};

enum class PouKind { Program, FunctionBlock, Function };

// A Program Organization Unit: a PROGRAM or a FUNCTION_BLOCK. Both are
// variables plus a body; a function block can have many instances, each
// with its own copy of the variables.
struct Pou {
    PouKind kind = PouKind::Program;
    std::string name;
    std::vector<VariableDeclaration> variables; // `a, b : INT` is stored as two declarations
    StatementList body;
    SourceLocation location;
    bool builtin = false;      // a standard block like TON, implemented in C++ (its body is empty)
    std::size_t leafCount = 0; // set by the analyzer: the leaves of all its variables
    // Set by the analyzer: one scratch buffer for each expression in the POU
    // that makes a text (see Expression::stringTemp). A literal's holds its
    // text from the start; a call's holds its latest result.
    struct StringTemp {
        std::size_t capacity = 0;
        std::u16string text;
    };
    std::vector<StringTemp> stringTemps;
    // FUNCTION only: the type it returns, and the variable that holds the
    // result (named like the function, added by the parser as the last variable).
    TypeSpec returnType;
    std::size_t returnSlot = kNoSlot;
};

// A type declared in a TYPE block: a STRUCT, or another name for a type.
//
//   TYPE
//       Point : STRUCT x : REAL; y : REAL; END_STRUCT;
//       Row : ARRAY[1..4] OF INT;
//   END_TYPE
struct TypeDeclaration {
    std::string name;
    bool isStruct = false;
    std::vector<VariableDeclaration> fields; // STRUCT: its fields
    TypeSpec alias;                          // otherwise: the type it names
    SourceLocation location;
};

// Everything in one source file: types, functions, function blocks and one program.
//
// The analyzer stores pointers into it (to function blocks, types and
// initial values), so a CompilationUnit must stay where it is or be moved as
// a whole, never copied piece by piece. (It can't be copied anyway.)
struct CompilationUnit {
    std::vector<TypeDeclaration> types;
    std::vector<Pou> functions;
    std::vector<Pou> functionBlocks;
    Pou program;
    std::vector<std::unique_ptr<Type>> analyzedTypes; // owned here, created by the analyzer
};

// ---------------------------------------------------------------- Helpers

const char* operatorSymbol(UnaryOperator op);  // "-", "NOT"
const char* operatorSymbol(BinaryOperator op); // "+", "<=", "AND", ...

// A deep copy of an expression tree (unique_ptr can't be copied directly).
ExpressionPtr cloneExpression(const Expression& expression);
Access cloneAccess(const Access& access);

// a, t1.Q, values[(i + 1)].x
std::string formatAccess(const Access& access);
std::string partText(const PartSelector& part); // ".%X3"

// One line with every operation in parentheses, showing exactly how the
// parser grouped things: a + b * c  ->  (a + (b * c))
std::string formatExpression(const Expression& expression);

// Indented trees, one node per line, like the diagram at the top of this file.
std::string dumpTree(const Statement& statement);
std::string dumpTree(const Pou& pou);
std::string dumpTree(const CompilationUnit& unit);

} // namespace softplc::st
