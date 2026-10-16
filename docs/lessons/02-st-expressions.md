# Lesson 2: Reading Structured Text

Milestone 2 teaches the PLC to **read** Structured Text: it turns text like `a := b + 3 * c;` into a tree the program can work with. It doesn't **run** the code yet; that's milestone 3.

```
"a := b + 3 * c;"
        │
        ▼  lexer (src/st/lexer.cpp)
[a] [:=] [b] [+] [3] [*] [c] [;] [EOF]          tokens
        │
        ▼  parser (src/st/parser.cpp)
Assignment a                                     syntax tree (AST)
  Binary +
    Variable b
    Binary *
      Integer 3
      Variable c
```

Try it: `./build/st_parse -e "a := b + 3 * c;"`

---

## Part 1: Structured Text and compiler concepts

### 1.1 Why two steps: lexer, then parser
Reading code character by character and understanding its structure at the same time is messy. Splitting it in two makes each step simple:

- The **lexer** only cares about *words*: "`:=` is one symbol", "`16#FF` is the number 255", "`(* … *)` is a comment, skip it". It knows nothing about grammar.
- The **parser** only cares about *structure*: "an assignment is a name, `:=`, an expression and `;`". It never sees spaces or comments.

This is how practically every compiler works, including the one in TIA Portal.

### 1.2 Tokens
**Code:** `src/st/token.hpp`

A token is one "word" of the language: its **kind** (identifier, integer, `:=`, `AND`…), its **text** as written, its **location** (line and column, for error messages), and for numbers, the **value**.

### 1.3 Things the lexer handles
**Code:** `src/st/lexer.cpp`

| Input | Becomes |
|---|---|
| `TRUE`, `true`, `True` | the same `True` token: keywords are **case-insensitive** in IEC 61131-3 |
| `1_000` | the integer 1000: underscores are separators for readability |
| `16#FF`, `2#1010`, `8#17` | 255, 10, 15: **based literals** (hex, binary, octal) |
| `3.14`, `1.5E3` | real numbers |
| `(* … *)`, `// …` | nothing: comments are skipped |
| `&` | the same token as `AND` |

### 1.4 The syntax tree (AST)
**Code:** `src/st/ast.hpp`

The tree keeps the **meaning** and drops the **spelling**. `(a + b) * c` and `( a+b )*c` give the same tree, and the parentheses disappear: the tree's shape already says "add first, then multiply". Each node is one of:

- **Literal**: a fixed value (`42`, `3.14`, `TRUE`)
- **VariableRef**: a name (`speed`)
- **UnaryExpression**: one operator and one operand (`-x`, `NOT done`)
- **BinaryExpression**: one operator and two operands (`a + b`)

Milestone 3 will walk this tree to compute values.

### 1.5 Operator precedence
**Code:** the comment at the top of the `Parser` class

Precedence decides which operator "grabs" its operands first. IEC 61131-3, tightest first:

| Level | Operators | Example | Grouped as |
|---|---|---|---|
| 1 | `( )` | `(a + b) * c` | `((a + b) * c)` |
| 2 | unary `-`, `NOT` | `-a * b` | `((-a) * b)` |
| 3 | `**` | `-2 ** 2` | `((-2) ** 2)` = 4 |
| 4 | `*`, `/`, `MOD` | `a + b * c` | `(a + (b * c))` |
| 5 | `+`, `-` | | |
| 6 | `<`, `>`, `<=`, `>=` | `a + 1 > b` | `((a + 1) > b)` |
| 7 | `=`, `<>` | | |
| 8 | `AND`, `&` | | |
| 9 | `XOR` | | |
| 10 | `OR` | `a OR b AND c` | `(a OR (b AND c))` |

Operators of the **same** level group left to right: `a - b - c` is `((a - b) - c)`, which is what you expect from maths.

### 1.6 Recursive descent parsing
**Code:** `src/st/parser.cpp`

The parser has **one function per precedence level**, and each one parses its operands by calling the next, tighter level:

```
orExpression → xorExpression → andExpression → equality → comparison
→ additive → multiplicative → power → unary → primary
```

For `a + b * c`: `additive()` asks `multiplicative()` for the left operand and gets `a`. It sees `+`, then asks `multiplicative()` again for the right operand. That call reads `b`, sees `*`, and grabs `c` too, returning `b * c` as one piece. So the result is `a + (b * c)`. The precedence comes from the **order of the calls**, not from a table.

"Recursive" because `primary()` handles `( … )` by calling `expression()` again, starting from the top. That's how parentheses can nest without limit.

### 1.7 Good error messages
**Code:** `src/st/diagnostic.cpp`

Every token knows its line and column, so errors can point at the exact spot:

```
error: line 1, column 10: expected an expression, found '*'
  a := b + * c;
           ^
```

---

## Part 2: C++ concepts

### 2.1 `std::variant`: one of several types
**Code:** `Expression::node` in `src/st/ast.hpp`

```cpp
std::variant<Literal, VariableRef, UnaryExpression, BinaryExpression> node;
```

A variant holds **exactly one** of the listed types at a time, and remembers which. It's a type-safe version of a C `union`. Useful functions:
- `std::holds_alternative<Literal>(node)`: is it currently a `Literal`?
- `std::get<Literal>(node)`: get it (throws if it's something else)

The token value uses `std::variant<std::monostate, std::int64_t, double>`, where `std::monostate` means "no value" (for tokens that aren't numbers).

### 2.2 `std::visit` with `Overloaded`
**Code:** `src/util/overloaded.hpp`, `formatExpression()` in `src/st/ast.cpp`

`std::visit` calls a function with whatever the variant currently holds. `Overloaded` bundles one lambda per type, and the compiler picks the matching one:

```cpp
std::visit(Overloaded{
    [](const Literal& l)          { ... },
    [](const VariableRef& v)      { ... },
    [](const UnaryExpression& u)  { ... },
    [](const BinaryExpression& b) { ... },
}, expression.node);
```

If you forget a type, it **doesn't compile**. When milestone 3 adds new node types, the compiler will list every place that needs updating.

How `Overloaded` works: it's a struct that **inherits from all the lambdas** (a lambda is just an object with an `operator()`), and `using Lambdas::operator()...;` puts all their `operator()`s side by side so normal overloading can choose. The `...` is a **variadic template**: it works for any number of lambdas.

### 2.3 `std::unique_ptr`: owning a tree
**Code:** `ExpressionPtr` in `src/st/ast.hpp`

A `BinaryExpression` contains two child expressions. It can't contain them *directly*, because an `Expression` would then contain itself, and its size would be infinite. Instead, it holds **pointers** to children.

`std::unique_ptr` is a pointer that **owns** what it points to: when the pointer is destroyed, the object is deleted. So destroying the root of the tree automatically deletes the whole tree. No `delete` anywhere, and no memory leaks. It can be moved but not copied (one owner at a time), which is why the code uses `std::move` when building nodes.

`struct Expression;` before `using ExpressionPtr = …` is a **forward declaration**: "this type exists, its details come later", which is enough to declare a pointer to it.

### 2.4 Pointer to member function
**Code:** `Level` and `binaryLevel()` in `src/st/parser.cpp`

```cpp
using Level = ExpressionPtr (Parser::*)();
...
auto left = (this->*next)();
```

`Level` is a pointer to **one of the `Parser` functions** that take no arguments and return `ExpressionPtr`. It lets `binaryLevel()` hold the shared loop logic, while each level only says "my operators are these, and the next level is that function":

```cpp
ExpressionPtr additive() {
    return binaryLevel(&Parser::multiplicative, {{TokenKind::Plus, BinaryOperator::Add},
                                                 {TokenKind::Minus, BinaryOperator::Subtract}});
}
```

The `{{…}, {…}}` list is a `std::initializer_list` of pairs.

### 2.5 Exceptions inside, `expected` outside
**Code:** `LexError`, `ParseError`, and the public functions at the bottom of `lexer.cpp` and `parser.cpp`

The parser is about ten functions deep when it finds an error. Returning `tl::expected` from every one of them would mean checking and passing the error up at every level. Instead:
- **Inside**, `fail()` throws a private `ParseError`, which jumps straight out of all the nested calls.
- **At the boundary**, `parseStatements()` catches it and returns a `tl::expected` error.

Callers never see an exception. They get the "value or error" style from lesson 1, and the internals stay simple.

`[[noreturn]]` on `fail()` tells the compiler the function never returns normally (it always throws), so it doesn't warn about missing `return`s after it.

### 2.6 `std::from_chars`: fast, strict number parsing
**Code:** `parseInteger()` and `real()` in `src/st/lexer.cpp`

```cpp
const auto [ptr, ec] = std::from_chars(begin, end, value, base);
```

It converts text to a number without exceptions or locale surprises, supports bases (16 for hex), and reports overflow with `std::errc::result_out_of_range`. The `[ptr, ec]` syntax is a **structured binding**: it unpacks the returned struct into two named variables.

### 2.7 Small but important details
- **`std::isdigit(static_cast<unsigned char>(c))`**: passing a plain `char` with a negative value (e.g. a non-ASCII byte like `é`) to `isdigit` is undefined behaviour. Casting to `unsigned char` first is the safe pattern.
- **Nested namespaces**: `namespace softplc::st { … }` is C++17 shorthand for `namespace softplc { namespace st { … } }`.
- **Returning `const Token&`**: `peek()` and `advance()` return references into `tokens_`, with no copying. That's safe because the vector never changes while parsing.

---

## Part 3: Testing

### 3.1 Testing the tree through its text form
**Code:** `grouped()` in `tests/st/parser_test.cpp`

Comparing trees node by node in tests would be long and hard to read. Instead, `formatExpression()` prints a fully parenthesized form, and the tests compare strings:

```cpp
CHECK(grouped("a + b * c") == "(a + (b * c))");
```

One line per precedence rule, easy to read and easy to add.

### 3.2 Table-driven tests
**Code:** the error tests in `lexer_test.cpp` and `parser_test.cpp`

```cpp
const Case cases[] = {
    {"a := b + * c;", "expected an expression, found '*'", {1, 10}},
    {"a := ;",        "expected an expression, found ';'", {1, 6}},
    ...
};
for (const auto& c : cases) { CAPTURE(c.source); ... }
```

Each row is one scenario: input, expected message, expected location. Adding a case is one line. `CAPTURE` makes doctest print which input failed.

---

## Review questions

**1.** What's the difference between the lexer's job and the parser's job?
<details><summary>Answer</summary>

The lexer splits text into tokens (words, numbers, symbols) and skips spaces and comments. The parser takes those tokens and works out the structure (which operations apply to what), building the syntax tree. See 1.1.
</details>

**2.** How is `a - b * c` grouped? And `a - b - c`?
<details><summary>Answer</summary>

`(a - (b * c))`, because `*` binds tighter than `-`. And `((a - b) - c)`, because operators of the same level group left to right. See 1.5.
</details>

**3.** Why are parentheses not stored in the syntax tree?
<details><summary>Answer</summary>

The tree's shape already shows the grouping: in `(a + b) * c`, the `+` node is a child of the `*` node. The parentheses were only needed in the text to force that shape. See 1.4.
</details>

**4.** What does `std::variant<Literal, VariableRef, UnaryExpression, BinaryExpression>` hold?
<details><summary>Answer</summary>

Exactly one of those four types at a time, plus a record of which one it is. See 2.1.
</details>

**5.** Why do expression nodes hold their children through `std::unique_ptr` instead of directly?
<details><summary>Answer</summary>

A type can't contain itself directly (its size would be infinite), so children must be pointers. `unique_ptr` also owns the child, so destroying the root deletes the whole tree automatically. See 2.3.
</details>

**6.** The parser throws exceptions internally. Why don't callers of `parseStatements()` need a `try`/`catch`?
<details><summary>Answer</summary>

`parseStatements()` catches the internal `ParseError` itself and returns it as a `tl::expected` error. Exceptions never leave the parser. See 2.5.
</details>

**7.** Is `16#1F` valid? What's its value?
<details><summary>Answer</summary>

Yes: it's hexadecimal 1F, which is 31. See 1.3.
</details>
