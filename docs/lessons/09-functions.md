# Lesson 9: Functions and the standard functions

Milestone 9 adds **FUNCTIONs**: pieces of logic that take inputs and give back one value, with no memory, called inside expressions (`x := Scale(raw, 0.0, 10.0) + 1.0;`). It also adds the **standard functions** every PLC programmer expects: `ABS`, `SQRT`, `MIN`, `MAX`, `LIMIT`, `SEL`, `MUX`, `TRUNC`, `ROUND`, and conversions like `DINT_TO_INT` and `REAL_TO_INT`.

```
FUNCTION Scale : REAL ... END_FUNCTION      a POU like the others, plus a return type
x := Scale(raw := r) + MAX(a, b);           a new kind of expression: CallExpression
        │                    │
        │                    └── standard function: built into the analyzer and runtime
        └── user function: one frame of memory, reset at every call
```

Try it:
```bash
./build/soft_plc --monitor examples/analog.st     # then FORCE rawPressure 13824
./build/st_parse --bytecode examples/analog.st
```

---

## Part 1: PLC concepts

### 1.1 Function or function block?
| | FUNCTION (FC) | FUNCTION_BLOCK (FB) |
|---|---|---|
| Memory between calls | none | yes, per instance |
| Result | one value, in an expression | outputs, read after the call |
| Typical use | formulas, scaling, conversions | timers, sequences, anything that waits or counts |

A function with no memory is easier to trust: the same inputs always give the same result, whatever happened before. If something needs to remember (an edge, a timer, a state), it's a function block.

### 1.2 Analog values, the Siemens way
`examples/analog.st` does what `NORM_X` and `SCALE_X` do in TIA Portal. Siemens analog cards give **0 … 27648** for their nominal range (4 … 20 mA, 0 … 10 V). The values above 27648 (up to 32511) are the *overrange*, and below 0 is the *underrange*, which for a 4 … 20 mA input usually means a broken wire. A good scaling function **limits** the value, and a separate check raises a fault. Otherwise a broken wire turns into a pressure of −5 bar that some other part of the program happily acts on.

### 1.3 Conversions that lose information
Converting to a smaller type has to decide what happens to values that don't fit:
- `DINT_TO_INT` keeps the **low 16 bits**: 70000 becomes 4464. It's what the bits do on a PLC, and it's surprising, so display code usually reduces the value first (`counter MOD 10000`).
- `REAL_TO_INT` rounds and then **limits**: 1.0E9 becomes 32767. A REAL has no "low bits" to keep.
- Rounding of exact halves: `ROUND(2.5)` is 2 and `ROUND(3.5)` is 4 (halves go to the even number, IEEE 754's default). It avoids a bias when many values are rounded, but it surprises people who expect 3. Check your CPU's documentation before you rely on how it rounds halves.

TIA Portal's conversions also set `ENO` to FALSE when the value doesn't fit; soft_plc has no `ENO`.

---

## Part 2: Compiler concepts

### 2.1 A call is an expression
**Code:** `CallExpression` (`src/st/ast.hpp`), `Parser::callExpression`

`NAME(` inside an expression is a call. The parser doesn't know yet whether `NAME` is a FUNCTION, a standard function or a mistake (a function block instance, an unknown name); that's the analyzer's job, which gives each case its own message. A statement that starts with `NAME(` is still a function block call: a FUNCTION whose result is thrown away is an error.

### 2.2 Overloading by type: why standard functions are built in
`MAX(i, j)` with INTs gives an INT; `MAX(x, y)` with REALs gives a REAL; `MAX(t1, t2)` with TIMEs gives a TIME. A FUNCTION written in ST has one fixed type for each input, so it can't do that. The standard functions are therefore **intrinsics**: the analyzer knows their rules (`builtinCall` in `analyzer.cpp`). For each call it decides the type every argument is brought to (`argumentTypes`) and the type of the result (`resultType`), exactly as it does for `+` and `<`. At run time a single function, `applyBuiltin` (`src/st/functions.cpp`), computes the result for both engines.

### 2.3 Calls by name and in order
`Scale(raw := r, high := 10.0)` names its arguments; inputs left out keep their default. `Scale(r, 0.0, 10.0)` gives them in declaration order and must give all of them. After the analyzer, both forms look the same to the engines: every argument knows the input slot it goes to (`CallArgument::slot`). For standard functions, named arguments are **sorted into order** (`orderBuiltinArguments`), so `LIMIT(MX := 9, IN := x, MN := 0)` runs exactly like `LIMIT(0, x, 9)`.

### 2.4 No recursion: a call graph
**Code:** `Analyzer::checkNoFunctionCallsItself`

While checking each function's body, the analyzer writes down which functions it calls (`calls_`). That's a **call graph**: functions are nodes, calls are arrows. A function that calls itself, directly (`F → F`) or through others (`A → B → A`), is a cycle in that graph, found with the same depth-first search that finds a function block containing itself, or a STRUCT containing itself. IEC 61131-3 forbids recursion because a PLC must know at compile time how much memory a program needs and how deep its calls go.

### 2.5 One frame per function
**Code:** `Interpreter::FunctionFrame`, `invokeFunction`

Because there's no recursion, a function can never be running twice at the same time, so **one** block of memory per function is enough: no allocation per call. At each call the frame is reset to its initial values (`instance.values = frame.initial;`, a copy into memory that's already there), the inputs are filled in, the body runs, and the result is read from the variable named like the function.

One subtlety: in `Scale(Scale(x))`, the inner call uses the same frame as the outer one. So **all arguments are evaluated first**, and only then is the frame reset and filled. Both engines follow that order.

### 2.6 Calls in the bytecode
```
a := Twice(a) + MAX(i, 5);      LoadInt      0          ; push a
                                CallFunction 0 ; Twice  ; pop 1 argument, push the result
                                LoadInt      1          ; push i
                                PushInt      5
                                CallBuiltin  1 ; MAX    ; pop 2 arguments, push the result
                                AddI
                                Wrap32
                                StoreInt32   0
```
Arguments go onto the stack like any operands, already converted to the type the function wants; the call instruction pops them and pushes the result. Arrays and structs passed to a function aren't put on the stack: their **place** is, and the call copies them into the frame. A call's effect on the stack depends on how many arguments it has, so the compiler adjusts its stack-depth count by hand (`depth_ += 1 - argumentCount`).

---

## Part 3: C++ concepts

### 3.1 `std::array` and `std::span`: no allocation for arguments
**Code:** `Interpreter::callBuiltin`, `Op::CallBuiltin`

```cpp
std::array<Value, kMaxCallArguments> values;      // on the stack: no heap allocation
...
return applyBuiltin(call.builtin, call.resultType, std::span(values.data(), count), where);
```

A `std::vector` would allocate memory at every call, every cycle. A `std::array` of a fixed maximum size lives on the C++ stack and costs nothing to create. `std::span(values.data(), count)` then passes "the first `count` of them" to `applyBuiltin`, which doesn't need to know where they're stored. That's the reason for the limit of 32 arguments per call (`kMaxCallArguments`).

### 3.2 A table in `constexpr` and a name built at run time
**Code:** `findBuiltinFunction` (`src/st/functions.cpp`)

The function names are a `constexpr` table of `{name, enum}` pairs. The conversions are generated instead: the table lists the pairs of types that can be converted (`{Int, Real}`, `{DInt, Int}`, …), and the lookup builds each name with `fmt::format("{}_TO_{}", …)`. Twelve conversions without twelve hand-written entries, and adding one is adding one pair.

### 3.3 Floating-point details
- `std::nearbyint` and friends follow the current rounding mode; `roundHalfEven` makes ROUND's rule explicit instead, so it can't change by accident.
- `std::clamp(value, low, high)` limits REAL_TO_INT's result. Converting a double that's out of range straight to an integer type is **undefined behaviour** in C++, so the clamp is required for correctness, not only for the PLC's behaviour.
- `std::isnan` catches NaN first: NaN compares false with everything, so `std::clamp` can't be trusted with it.

### 3.4 A private nested struct that both engines share
`Interpreter::Argument` and `Interpreter::FunctionFrame` are declared inside the class, in its `private:` part. Only the interpreter's own member functions (the tree-walker's `callFunction` and the VM's `CallFunction` case) can use them. That's how the two engines are forced through the same `invokeFunction`.

---

## Part 4: Testing

- `tests/st/functions_test.cpp` has a table with one row per compile error (about 40), and runs user functions and every standard function on both engines. The awkward cases are tested explicitly: `ABS` of the smallest INT, `SQRT(-1.0)` giving NaN, rounding of exact halves, clamping, `DINT_TO_INT` keeping the low bits, a REAL losing digits in `DINT_TO_REAL(16777217)`, and the MUX error with its line and column.
- A new differential test runs a program that calls every standard function and three user functions (with array and struct inputs, nested calls and an early RETURN) on 2000 random inputs, on both engines, comparing every value after every cycle.
- The guide's new chapter 6 has three programs and one error example, all checked by `guide_test.cpp`, like the rest of the guide (26 programs and 5 error examples in total).

---

## Review questions

**1.** Why can't a FUNCTION contain a `TON`?
<details><summary>Answer</summary>

A TON has to remember when its input turned on, from one cycle to the next. A function has no memory between calls: its frame is reset every time. So the timer would start again at every call. Anything that remembers belongs in a function block.
</details>

**2.** What do `DINT_TO_INT(70000)` and `REAL_TO_INT(70000.0)` give, and why are they different?
<details><summary>Answer</summary>

`DINT_TO_INT(70000)` gives 4464: it keeps the low 16 bits (70000 − 65536), like the bits on a PLC. `REAL_TO_INT(70000.0)` gives 32767: a REAL has no low bits to keep, so the value is limited to the INT range.
</details>

**3.** Why is `MAX` built into the compiler instead of written in ST as a FUNCTION?
<details><summary>Answer</summary>

A FUNCTION written in ST has fixed input types, but `MAX` works for INTs, DINTs, REALs and TIMEs and gives back the same type. Only the compiler can choose the types per call, like it does for operators.
</details>

**4.** Why does soft_plc need only one frame of memory per function, even if it's called from many places?
<details><summary>Answer</summary>

Recursion isn't allowed, so a function is never running twice at the same moment: each call finishes before the next one starts. One frame, reset at each call, is enough.
</details>

**5.** In `Scale(Scale(x))`, what would go wrong if the outer call reset its frame before evaluating its argument?
<details><summary>Answer</summary>

The inner call uses the same frame. It would overwrite the inputs the outer call had already set up. So all arguments are evaluated first, and the frame is reset and filled only after that.
</details>

**6.** `FUNCTION A` calls `B`, and `B` calls `A`. How does the analyzer find that, and why does IEC forbid it?
<details><summary>Answer</summary>

It records which functions each function calls (a call graph) and searches it depth-first; reaching the starting function again is a cycle, `A -> B -> A`. IEC forbids recursion so that the memory and the call depth a program needs are known before it runs.
</details>

**7.** Why does `callBuiltin` use `std::array<Value, 32>` and not `std::vector<Value>`?
<details><summary>Answer</summary>

A vector allocates heap memory every time it's created, which would happen at every call, every cycle. A fixed-size `std::array` lives on the C++ stack and costs nothing to create; a `std::span` then passes just the part that's used.
</details>

**8.** Why must `REAL_TO_INT` clamp the value before converting the double to an integer in C++?
<details><summary>Answer</summary>

Converting a floating-point value that doesn't fit into an integer type is undefined behaviour in C++. Clamping first makes it well defined, and also gives the PLC behaviour we want.
</details>
