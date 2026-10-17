# C++ 4: The syntax tree (`variant`, `visit`, `unique_ptr`, recursion)

The parser turns ST text into a tree of C++ objects, and the analyzer, the interpreter, the compiler and `st_parse` all walk that tree. This lesson shows how a tree with many kinds of nodes is built in modern C++ without class hierarchies.

**Code:** `src/st/ast.hpp` (the tree), `src/st/parser.cpp` (builds it), `src/st/ast.cpp` (prints and copies it)

---

## 1. One node, many kinds: `std::variant`

An expression can be a literal, an access to a variable (`count`, `t1.Q`, `values[i].x`), a unary operation or a binary operation:

```cpp
// src/st/ast.hpp
struct Literal          { Value value; bool isTime = false; };
struct Access           { std::string name; std::vector<Selector> selectors; ... };
struct UnaryExpression  { UnaryOperator op; ExpressionPtr operand; };
struct BinaryExpression { BinaryOperator op; ExpressionPtr left; ExpressionPtr right; DataType operandType; };

struct Expression {
    std::variant<Literal, Access, UnaryExpression, BinaryExpression> node;
    SourceLocation location;
    DataType type = DataType::Unknown;
};
```

A `std::variant<A, B, C>` holds **exactly one** of A, B or C at a time, and remembers which. Think of a type-safe `union`. Its size is the size of the biggest alternative plus a small tag.

The same idea is used three times:

| Variant | Alternatives |
|---|---|
| `Value` | `bool`, `std::int64_t`, `double` (a runtime value) |
| `Expression::node` | the 4 expression kinds above |
| `Selector` | `FieldSelector` (`.x`), `IndexSelector` (`[i, j]`): the steps after a name in an `Access` |
| `Initializer::node` | `ExpressionPtr`, `ArrayInitializer` (`[1, 2, 3(0)]`), `StructInitializer` (`(x := 1.0)`) |
| `Statement::node` | `Assignment`, `IfStatement`, `CaseStatement`, `ForStatement`, `WhileStatement`, `RepeatStatement`, `ExitStatement`, `ReturnStatement`, `CallStatement` |
| `Token::value` | `std::monostate` (nothing), `std::int64_t`, `double`, `Address` |

`std::monostate` is an empty type, used as "this token carries no value".

### The classic alternative, and why not
The textbook way is a base class `Expression` with virtual functions and subclasses `Literal`, `Binary`, … Each new operation on the tree (type check, evaluate, compile, print, clone) then needs a new virtual function in **every** subclass, or the more complicated "visitor pattern". With a variant, the node types are just data, and each operation is written in **one place**: the analyzer in `analyzer.cpp`, the evaluator in `interpreter.cpp`, the compiler in `compiler.cpp`. This project has 5 operations over the tree, so that matters.

The trade-off: adding a new *kind* of node means updating every `visit`. The compiler helps there, because a `visit` that doesn't handle every alternative doesn't compile.

---

## 2. Asking a variant what it holds

```cpp
std::holds_alternative<bool>(literal.value)        // is it a bool? -> true/false
std::get<std::int64_t>(token.value)                // give me the int64; throws std::bad_variant_access if it isn't one
std::get_if<Literal>(&expression.node)             // pointer to the Literal, or nullptr if it isn't one
```

`get_if` is the safe "try" version. From the analyzer:

```cpp
// src/st/analyzer.cpp
std::optional<std::int64_t> integerConstant(const Expression& expression) {
    if (const auto* literal = std::get_if<Literal>(&expression.node)) {
        if (const auto* value = std::get_if<std::int64_t>(&literal->value); value && !literal->isTime) {
            return *value;
        }
    }
    ...
}
```

`std::get` without a check is used where the analyzer has already proven the type. The interpreter does `std::get<bool>(evaluate(*branch.condition, self))` because an IF condition is guaranteed to be BOOL. If that assumption were ever wrong, it would throw instead of silently reading garbage.

---

## 3. Doing something for every kind: `std::visit` and `Overloaded`

```cpp
// src/st/ast.cpp
std::string formatExpression(const Expression& expression) {
    return std::visit(Overloaded{
                          [](const Literal& literal) { return formatLiteral(literal); },
                          [](const Access& access) { return formatAccess(access); },
                          [](const UnaryExpression& unary) { ... },
                          [](const BinaryExpression& binary) {
                              return fmt::format("({} {} {})", formatExpression(*binary.left),
                                                 operatorSymbol(binary.op), formatExpression(*binary.right));
                          },
                      },
                      expression.node);
}
```

`std::visit(f, v)` looks at which alternative `v` holds and calls `f` with it. `f` must accept **every** alternative, so you need one object with four `operator()`s. That's what `Overloaded` builds out of four lambdas:

```cpp
// src/util/overloaded.hpp
template <class... Lambdas> struct Overloaded : Lambdas... {
    using Lambdas::operator()...;
};
template <class... Lambdas> Overloaded(Lambdas...) -> Overloaded<Lambdas...>;
```

These three lines contain a lot of C++:
- **Every lambda is an object of its own, unnamed class** with an `operator()`.
- `template <class... Lambdas>` is a **variadic template**: it takes any number of types. `Lambdas...` is a *pack*.
- `struct Overloaded : Lambdas...` **inherits from all the lambda classes at once**, so it gets all their `operator()`s.
- `using Lambdas::operator()...;` makes all of them visible side by side, so normal overload resolution picks the right one for each type.
- The last line is a **deduction guide**: it lets you write `Overloaded{...}` without spelling out the lambda types (which have no names you could write anyway). C++20 can deduce this on its own; it's kept for clarity.

You don't need to write things like this often, but it's a well-known idiom and worth recognising.

### Generic lambda instead of `Overloaded`
When every alternative should go to a function with the same name, a lambda with `auto` is shorter:

```cpp
// src/st/analyzer.cpp
void checkStatements(StatementList& list) {
    for (auto& statement : list) {
        std::visit([&](auto& node) { check(node, statement.location); }, statement.node);
    }
}
void check(Assignment& s, SourceLocation where) { ... }
void check(IfStatement& s, SourceLocation where) { ... }
void check(ForStatement& s, SourceLocation where) { ... }
...
```

`[&](auto& node)` is a **generic lambda**: a small template. For each alternative, `auto` becomes that type, and the call `check(node, ...)` picks the matching overload. Forget one `check` overload and it doesn't compile. The compiler in `compiler.cpp` does the same with its `statement(...)` overloads.

---

## 4. Owning a tree: `std::unique_ptr`

```cpp
struct Expression;                                // declared, not yet defined
using ExpressionPtr = std::unique_ptr<Expression>;

struct BinaryExpression {
    BinaryOperator op;
    ExpressionPtr left;
    ExpressionPtr right;
};
```

Why not `Expression left;` directly? Because an `Expression` contains a variant that can contain a `BinaryExpression` that contains an `Expression`… An object can't contain itself; its size would be infinite. A **pointer** has a fixed size (8 bytes) whatever it points to, which breaks the loop.

`unique_ptr` means **one owner**: the parent owns its children. When the root of a tree is destroyed, it destroys its children, which destroy theirs, and so on, so the whole tree is freed with no code written for it.

Statements avoid the pointer where they can:

```cpp
struct Statement;
using StatementList = std::vector<Statement>;
struct IfBranch { ExpressionPtr condition; StatementList body; };
```

A `std::vector<Statement>` of a type that isn't defined yet is allowed since C++17, because the vector stores its elements on the heap anyway.

### Building nodes
```cpp
// src/st/parser.cpp
ExpressionPtr makeExpression(decltype(Expression::node) node, SourceLocation location) {
    return std::make_unique<Expression>(Expression{std::move(node), location});
}
...
left = makeExpression(BinaryExpression{found->second, std::move(left), std::move(right)}, opToken.location);
```
- `decltype(Expression::node)` means "the type of that member", so the long variant type isn't written twice.
- **`std::move(left)`**: a `unique_ptr` can't be copied (two owners would be wrong), only **moved**. Moving hands the pointer over: the new `BinaryExpression` now owns the old `left`, and the local `left` becomes empty. Then `left = ...` puts the new node in it. That's how `a - b - c` becomes `((a - b) - c)`: the tree grows upwards.

### Copying a tree by hand
Because `unique_ptr` can't be copied, neither can `Expression`. Where a copy is really needed (`a, b : INT := 5;` gives each variable its own initial value), there's a hand-written deep copy:

```cpp
// src/st/ast.cpp
ExpressionPtr cloneExpression(const Expression& expression) {
    using Node = decltype(Expression::node);
    auto node = std::visit(Overloaded{
                               [](const Literal& literal) -> Node { return literal; },
                               ...
                               [](const BinaryExpression& binary) -> Node {
                                   return BinaryExpression{binary.op, cloneExpression(*binary.left),
                                                           cloneExpression(*binary.right), binary.operandType};
                               },
                           },
                           expression.node);
    return std::make_unique<Expression>(Expression{std::move(node), expression.location, expression.type});
}
```
`-> Node` is a **trailing return type**: every lambda must return the same type for `std::visit`, so each one says "I return the whole variant".

---

## 5. A recursive descent parser in C++

**One function per grammar rule**, and the call structure *is* the grammar:

```cpp
ExpressionPtr expression()     { return orExpression(); }
ExpressionPtr orExpression()   { return binaryLevel(&Parser::xorExpression, {{TokenKind::Or, BinaryOperator::Or}}); }
ExpressionPtr xorExpression()  { return binaryLevel(&Parser::andExpression, {{TokenKind::Xor, BinaryOperator::Xor}}); }
...
ExpressionPtr additive() {
    return binaryLevel(&Parser::multiplicative,
                       {{TokenKind::Plus, BinaryOperator::Add}, {TokenKind::Minus, BinaryOperator::Subtract}});
}
```

### Pointer to member function
```cpp
using Level = ExpressionPtr (Parser::*)();   // "pointer to a Parser member function taking nothing, returning ExpressionPtr"

ExpressionPtr binaryLevel(Level next, std::initializer_list<std::pair<TokenKind, BinaryOperator>> operators) {
    auto left = (this->*next)();             // call it on this object
    ...
}
```
All 8 binary precedence levels share one loop, `binaryLevel`. Each passes **which function parses the next tighter level** as `&Parser::multiplicative`. A member function needs an object to run on, so the call syntax is `(this->*next)()`. Unusual-looking, but the alternative would be eight copies of the same loop.

### `std::initializer_list`
`{{TokenKind::Plus, BinaryOperator::Add}, {TokenKind::Minus, BinaryOperator::Subtract}}` is a braced list passed as `std::initializer_list<std::pair<...>>`. It's a lightweight read-only array that only lives for the call. Perfect for small tables written in place.

### `constexpr` tables
```cpp
// src/st/lexer.cpp
constexpr std::pair<std::string_view, TokenKind> keywords[] = {
    {"TRUE", TokenKind::True}, {"FALSE", TokenKind::False}, ...
};
```
`constexpr` means it's built **at compile time** and placed in read-only memory: no work at startup and no heap. `std::string_view` can be `constexpr` (it's just a pointer + length into the string literal); `std::string` mostly can't.

---

## 6. `std::string_view`: reading text without copying

The lexer reads the source through a `std::string_view source_`. A `string_view` is to strings what `span` is to arrays: **a pointer plus a length into someone else's text**. `source_.substr(begin, pos_ - begin)` makes a new view, not a new string, so no memory is allocated.

**The danger:** a view must not outlive the text it points into. The lexer is safe because `tokenize(source)` finishes before the caller's string goes away, and each `Token` stores its own `std::string text` (a copy), not a view. Tokens outlive the lexer; views don't.

---

## Review questions

**1.** Why does `BinaryExpression` hold `ExpressionPtr left` instead of `Expression left`?
<details><summary>Answer</summary>

`Expression` contains a variant that can hold a `BinaryExpression`, which would contain an `Expression`, which… An object can't contain itself (its size would be infinite). A pointer has a fixed size and breaks the loop. `unique_ptr` also means the parent owns the child, so the whole tree is freed automatically.
</details>

**2.** What's the difference between `std::get<bool>(v)` and `std::get_if<bool>(&v)`?
<details><summary>Answer</summary>

`get` returns the bool, or throws `std::bad_variant_access` if `v` holds something else. `get_if` returns a pointer to the bool, or `nullptr`. Use `get_if` when you're asking ("is it a bool?"), and `get` when you already know.
</details>

**3.** You add a new expression kind, `FunctionCall`, to `Expression::node`. What will the compiler tell you?
<details><summary>Answer</summary>

Every `std::visit` over `Expression::node` that doesn't handle `FunctionCall` fails to compile: in `formatExpression`, `cloneExpression`, the `TreeWriter`, the analyzer's `checkExpression`, the interpreter's `evaluate` and the compiler's `expression`. That's the list of places to update, for free.
</details>

**4.** In `binaryLevel`, why is it `std::move(left)` and not just `left`?
<details><summary>Answer</summary>

`left` is a `unique_ptr`, which can't be copied (that would mean two owners). `std::move` transfers ownership into the new `BinaryExpression`; the local `left` becomes empty and is then given the new node.
</details>

**5.** What does `Overloaded{ lambda1, lambda2, lambda3 }` produce?
<details><summary>Answer</summary>

One object whose class inherits from all three lambdas' classes and brings all their `operator()`s into scope, so it can be called with any of their parameter types. `std::visit` needs exactly that: one callable that accepts every alternative.
</details>

**6.** The lexer works on a `std::string_view`, but `Token::text` is a `std::string`. Why not make `text` a `string_view` too?
<details><summary>Answer</summary>

It would save copies, but then every token would point into the caller's source string, and tokens (and error messages built from them) would break if that string were destroyed or changed first. A `std::string` owns its text, so a token is safe on its own.
</details>
