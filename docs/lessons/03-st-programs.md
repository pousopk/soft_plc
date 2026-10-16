# Lesson 3: Running Structured Text programs

Milestone 3 makes the PLC **run** Structured Text. A program now goes through four stages:

```
source text
    │  lexer        (lesson 2)  text → tokens
    ▼
    │  parser       (lesson 2)  tokens → syntax tree
    ▼
    │  analyzer     NEW         checks names and types, fills in what's needed to run
    ▼
    │  interpreter  NEW         walks the tree every scan cycle
    ▼
process image (%I %Q %M)
```

Try it:
```bash
./build/soft_plc examples/traffic_light.st
./build/soft_plc examples/stop_demo.st
```

---

## Part 1: PLC concepts

### 1.1 Program structure
```
PROGRAM TrafficLight
VAR
    red   AT %QX0.0 : BOOL;     (* linked to an output bit *)
    state : INT := 0;           (* internal variable with an initial value *)
END_VAR
    ... statements ...
END_PROGRAM
```

Variables in a `PROGRAM` keep their values from one cycle to the next. That's what lets `ticks := ticks + 1;` count cycles.

### 1.2 Data types and their limits
**Code:** `DataType` in `src/st/ast.hpp`

| Type | Size | Range |
|---|---|---|
| `BOOL` | 1 bit | `TRUE` / `FALSE` |
| `INT` | 16 bits | −32 768 … 32 767 |
| `DINT` | 32 bits | about ±2.1 billion |
| `REAL` | 32 bits | about 7 significant digits |

**Overflow wraps around**, like a real PLC: `INT 32767 + 1` gives `−32768`. The PLC doesn't stop; the value just rolls over, like a car's odometer. That's why choosing the type matters.

**Integer division drops the remainder**, rounding towards zero: `7 / 2 = 3` and `−7 / 2 = −3`. Use `MOD` for the remainder, or a `REAL` for decimals: `7 / 2.0 = 3.5`.

**REAL is not exact**: `0.1` is stored as `0.100000001…`, and above 16 777 216 a REAL can't even represent every whole number. Never compare REALs with `=`.

### 1.3 Located variables (`AT`)
**Code:** `Interpreter::load()` and `Interpreter::store()` in `src/st/interpreter.cpp`

`lamp AT %QX0.0 : BOOL;` gives the output bit `%QX0.0` a name. The variable has **no memory of its own**: reading it reads the process image, writing it writes the process image. It's the same as a tag in TIA Portal that points to an absolute address.

The size must match: a `BOOL` needs a bit (`%QX0.0`), an `INT` a word (`%MW0`), a `DINT` or `REAL` a double word (`%MD0`).

### 1.4 Statements
| Statement | Use it for |
|---|---|
| `IF … ELSIF … ELSE … END_IF;` | choosing between conditions |
| `CASE state OF 0: … 1, 2: … 3..5: … ELSE … END_CASE;` | choosing by a number; **state machines** |
| `FOR i := 1 TO 10 BY 2 DO … END_FOR;` | a known number of repetitions |
| `WHILE cond DO … END_WHILE;` | repeat while true (checks first, may run 0 times) |
| `REPEAT … UNTIL cond END_REPEAT;` | repeat until true (checks last, runs at least once) |
| `EXIT;` | leave the innermost loop |
| `RETURN;` | end the program for this cycle |

### 1.5 The most important PLC rule: never wait inside a loop
A loop runs **completely within one scan cycle**. While it runs, inputs aren't read and outputs aren't written. Code like this would freeze the PLC:

```
WHILE NOT sensor DO END_WHILE;     (* WRONG: sensor never changes during the loop *)
```

The input can't change while the loop runs, because inputs are only read at the start of the cycle. To wait for something, remember where you are in a variable and check again **next cycle**. That's a **state machine**, like `examples/traffic_light.st`:

```
CASE state OF
    0: red := TRUE;
       IF ticks >= 200 THEN state := 1; ticks := 0; END_IF;   (* wait 2 s, across 200 cycles *)
    1: ...
END_CASE;
```

### 1.6 Watchdog and STOP
**Code:** `Interpreter::countLoopIteration()`, `src/st/st_program.cpp`

If a loop never ends, the PLC would hang. Like a real CPU's cycle watchdog, the interpreter counts loop iterations in each cycle and raises an error past a limit.

On any **runtime error** (division by zero, watchdog), the PLC goes to **STOP**: the program stops running, and **every output is switched off**, which is the safe state for most machines. `examples/stop_demo.st` shows this.

Division by zero is an error for integers. For `REAL`, it gives infinity, following the IEEE 754 floating-point rules, as most PLCs do.

---

## Part 2: Compiler concepts

### 2.1 Compile-time vs. run-time errors
**Code:** `src/st/analyzer.cpp`

The **analyzer** checks the program *before* it runs, like pressing "Compile" in TIA Portal:

```
error: line 6, column 8: the IF condition must be BOOL, not INT
      IF speed THEN
         ^
```

Catching mistakes here means they can never happen on the machine. Only problems that depend on values, like dividing by a variable that happens to be 0, are left for run time.

### 2.2 The symbol table
`slots_` in the analyzer maps every variable name (in upper case, since ST names are case-insensitive) to a number: its **slot**. The analyzer writes the slot into every `VariableRef`. The interpreter then uses `values_[slot]` directly, with no name lookup while running, which is much faster.

### 2.3 Type checking
For every expression, the analyzer works out the type of the result and stores it in `Expression::type`. The rules:
- `+ - * /` need numbers. Mixed numbers use the "widest" type: `INT + REAL` → `REAL`.
- `AND OR XOR NOT` need `BOOL`s.
- Comparisons give `BOOL`.
- An assignment may **widen** (`INT` into `DINT`, anything into `REAL`) but not **narrow** (`DINT` into `INT` could lose data).
- An integer literal like `42` adapts to the other side, but must fit: `small := 40000;` is rejected for an `INT`.

A tree with this extra information filled in is called a **decorated** (or annotated) syntax tree.

### 2.4 The tree-walking interpreter
**Code:** `Interpreter::evaluate()` and `Interpreter::execute()`

To compute an expression, `evaluate()` first computes the children, then applies the operator: the bottom of the tree first, the root last (the answer to lesson 2's last question). `execute()` does the same for statements: an `IF` evaluates its condition, then executes the chosen branch's statements.

### 2.5 `EXIT` and `RETURN` without exceptions
**Code:** `Interpreter::Flow`

Every `execute()` returns a small `Flow` value: `Normal`, `Exit` or `Return`. A statement list stops as soon as one statement returns something other than `Normal`, and passes it outwards. A loop that receives `Exit` stops looping and returns `Normal`, because the `EXIT` has been handled. `Return` passes through everything up to the top. It's simple, fast, and easy to follow.

---

## Part 3: C++ concepts

### 3.1 `std::bit_cast` (C++20): same bits, different type
**Code:** `Interpreter::load()` for `REAL`

```cpp
std::bit_cast<float>(image.readDWord(area, offset))
```

A `REAL` in `%MD4` is 32 bits in IEEE 754 float format. `static_cast<float>(0x41480000)` would convert the *number* 1095237632 to a float. `std::bit_cast` instead **reinterprets the same 32 bits** as a float, giving 12.5. Before C++20 this needed `memcpy`.

### 3.2 Avoiding undefined behaviour in integer overflow
**Code:** `add()`, `subtract()`, `multiply()` in `interpreter.cpp`

In C++, overflowing a **signed** integer is **undefined behaviour**: the compiler may assume it never happens, and the program can do anything. Unsigned integers, on the other hand, are defined to wrap. So the interpreter does the arithmetic on `uint64_t` and converts back. Since C++20, converting to a smaller signed type (`static_cast<std::int16_t>(value)`) is guaranteed to wrap, which gives exactly the PLC behaviour for `INT`. The UBSan sanitizer in Debug builds would report it if this were done wrong.

### 3.3 RAII: cleanup that can't be forgotten
**Code:** `ImageBinding` in `interpreter.cpp`

```cpp
void Interpreter::runCycle(ProcessImage& image) {
    ImageBinding binding(image_, image);   // constructor: image_ = &image
    ...
}                                          // destructor: image_ = nullptr
```

The destructor runs whenever `binding` goes out of scope, **including when an exception is thrown**. So `image_` never keeps pointing at an image after the cycle. RAII stands for "Resource Acquisition Is Initialization": tie cleanup to an object's lifetime. `std::unique_ptr`, `std::vector` and file streams all work this way.

### 3.4 `std::function`: storing "something to call"
**Code:** `StProgram::StopHandler`

```cpp
using StopHandler = std::function<void(const RuntimeError&)>;
```

It can hold any function, lambda or callable object with that signature. `main.cpp` passes a lambda that prints the error and stops the scan cycle; the tests pass one that counts calls. `StProgram` doesn't need to know which.

### 3.5 Move semantics: handing over ownership
**Code:** `Interpreter(ProgramUnit program, …) : program_(std::move(program))`

A `ProgramUnit` holds a whole tree of `unique_ptr`s, so it can't be copied, only **moved**. Moving transfers the contents to the new owner and leaves the old object empty, with no copying of the tree. Taking the parameter **by value** and then moving it is the usual way to say "give me this; I'm keeping it".

When a tree really must be duplicated (`a, b : INT := 5;` gives each variable its own initial value), `cloneExpression()` copies it explicitly, node by node.

### 3.6 More `std::variant` tools
- **`std::get_if<Literal>(&node)`** returns a pointer if the variant holds a `Literal`, or `nullptr` if not. That's handy in an `if`, without exceptions (`integerConstant()` in the analyzer).
- **A generic lambda as visitor**: `std::visit([&](auto& node) { check(node, location); }, statement.node);` The `auto` parameter makes the lambda a template, so the compiler calls the matching `check()` overload for each statement type.

### 3.7 Small things worth knowing
- **`std::optional<T>`**: a value that may be missing (`VariableDeclaration::address`, `Interpreter::valueOf()`).
- **`unordered_map::emplace`** returns `[iterator, inserted]`: if `inserted` is false, the key was already there. That's how duplicate variable names are found in one step.
- **Function templates**: `compare<T>()` is written once and works for `bool`, `int64_t` and `double`. `parseWith(source, rule)` takes any lambda and works out its return type with `decltype`.
- **`constexpr` tables**: the keyword list in the lexer is a fixed array built at compile time.

---

## Part 4: Testing

- **A helper class for tests** (`Runner` in `tests/st/interpreter_test.cpp`) compiles a program, runs cycles, and reads variables by name, so each test is one readable line: `CHECK(runOnce("r := 7 / 2;").integer("r") == 3);`
- **Teaching doctest about our exception**: `RuntimeError` doesn't derive from `std::exception`, so `REGISTER_EXCEPTION_TRANSLATOR` tells doctest how to print it.
- **Testing each stage separately:** lexer, parser, analyzer and interpreter each have their own tests. When something breaks, the failing test points at the right stage.

---

## Review questions

**1.** An `INT` variable holds 32767. What's its value after `x := x + 1;`?
<details><summary>Answer</summary>

−32768. INT overflow wraps around, like a real PLC. See 1.2.
</details>

**2.** What's `−7 / 2` with integers? And `7 / 2.0`?
<details><summary>Answer</summary>

−3 (integer division rounds towards zero) and 3.5 (a REAL is involved, so the result is a REAL). See 1.2.
</details>

**3.** Why is `WHILE NOT sensor DO END_WHILE;` a bad idea in a PLC?
<details><summary>Answer</summary>

The loop runs inside one scan cycle, and inputs are only read at the start of a cycle, so `sensor` can never change while the loop runs. The loop never ends; the watchdog stops the PLC. Wait across cycles with a state machine instead. See 1.5.
</details>

**4.** `lamp AT %QX0.0 : BOOL;` — where is `lamp`'s value stored?
<details><summary>Answer</summary>

In the process image, in output bit `%QX0.0`. The variable is only a name for that bit. See 1.3.
</details>

**5.** `small : INT; big : DINT;` — which of these does the analyzer reject: `big := small;` or `small := big;`?
<details><summary>Answer</summary>

`small := big;`, because a DINT value might not fit in an INT. The other direction always fits. See 2.3.
</details>

**6.** What happens to the outputs when the PLC goes to STOP?
<details><summary>Answer</summary>

They're all switched off (set to 0), the safe state. See 1.6.
</details>

**7.** What's the difference between `static_cast<float>(bits)` and `std::bit_cast<float>(bits)`?
<details><summary>Answer</summary>

`static_cast` converts the *number* to a float. `bit_cast` keeps the same bits and reads them as a float. A REAL stored in PLC memory needs `bit_cast`. See 3.1.
</details>
