# Lesson 4: Function blocks and the standard library

Milestone 4 adds the building block of every real PLC program: the **function block** (FB). It also adds the **TIME** type and the IEC standard blocks: timers, counters, edge detection and flip-flops.

```
FUNCTION_BLOCK Blinker              ← the definition: written once
VAR_INPUT  enable : BOOL; ...       ← set by the caller
VAR_OUTPUT out : BOOL;              ← read by the caller
VAR        onTimer : TON; ...       ← internal memory, invisible outside
...
END_FUNCTION_BLOCK

PROGRAM Main
VAR fast, slow : Blinker; END_VAR   ← two instances: two independent copies of the memory
    fast(enable := TRUE, onTime := T#250ms);
    slowLamp := slow.out;
END_PROGRAM
```

Try it: `./build/soft_plc examples/function_blocks.st`

---

## Part 1: PLC concepts

### 1.1 Definition vs. instance
**Code:** `examples/function_blocks.st`

A function block **definition** is like a blueprint. Each **instance** (`fast : Blinker;`) is a separate copy of all its variables, like an instance DB in TIA Portal. `fast` and `slow` run the same code but have completely separate timers and outputs.

This is what makes function blocks different from plain code: they have **memory**. A TON has to remember when IN went on; a CTU has to remember its count. That memory lives in the instance and survives from one cycle to the next.

### 1.2 The interface
| Section | Who writes it | Who reads it |
|---|---|---|
| `VAR_INPUT` | the caller, in the call | the block |
| `VAR_OUTPUT` | the block | the caller, as `instance.output` |
| `VAR` | the block | only the block |

The analyzer enforces this. `t1.Q := TRUE;` is rejected (outputs are read-only from outside), and so is reading `c.calls` when `calls` is internal.

### 1.3 Calling
```
t1(IN := start, PT := T#2s);     (* set the inputs, then run the block once *)
lamp := t1.Q;                    (* read an output afterwards *)
```

- An input you **don't** pass keeps its value from the **previous call** (or its initial value). That's IEC behaviour, and it's tested in `tests/st/function_block_test.cpp`.
- A block only runs when it's **called**. A TON that isn't called doesn't time. A common bug in real projects is calling a timer inside an `IF`, so it only updates some of the time.
- Read outputs **after** the call to get this cycle's values. Read before the call, and you get last cycle's.

### 1.4 TIME
`T#500ms`, `T#1m30s`, `TIME#2h`. Internally it's milliseconds in 32 bits, like Siemens: the maximum is about 24 days. The rules make sense physically:
- `TIME + TIME` and `TIME - TIME` give a `TIME`
- `TIME * 3` and `TIME / 2` give a `TIME`
- `TIME + 5` is rejected: 5 what? Write `T#5s`.

### 1.5 The standard blocks
**Code:** `src/stdlib/`

| Block | Inputs → Outputs | Does |
|---|---|---|
| `TON` | IN, PT → Q, ET | Q on after IN has been on for PT (on-delay) |
| `TOF` | IN, PT → Q, ET | Q stays on for PT after IN goes off (off-delay) |
| `TP` | IN, PT → Q, ET | a pulse of exactly PT on each rising edge of IN |
| `CTU` | CU, R, PV → Q, CV | counts rising edges of CU up; Q when CV ≥ PV |
| `CTD` | CD, LD, PV → Q, CV | LD loads PV; counts down; Q when CV ≤ 0 |
| `R_TRIG` | CLK → Q | Q on for **one cycle** when CLK goes on |
| `F_TRIG` | CLK → Q | Q on for one cycle when CLK goes off |
| `SR` | S1, R → Q1 | flip-flop, **set** wins if both are on |
| `RS` | S, R1 → Q1 | flip-flop, **reset** wins if both are on |

Two details worth remembering:
- **Counters count edges, not cycles.** A sensor that stays on for 50 cycles counts once.
- **RS is the safe choice for motors**: if start and stop are pressed together, stop wins.

### 1.6 Timers depend on the cycle
Timers only update when they're called, once per cycle. With a 10 ms cycle, a `T#100ms` TON switches on in the cycle where at least 100 ms have passed, so the output reacts in steps of 10 ms. That's why the test for nested blinkers expects a 50 ms blink to take 120 ms per period: each phase adds one cycle of reaction delay. The same thing happens on a real PLC.

---

## Part 2: Compiler and runtime concepts

### 2.1 POUs and scopes
**Code:** `Pou` in `src/st/ast.hpp`

IEC calls programs and function blocks **POUs**: Program Organization Units. Both are "variables + body". The analyzer checks each one separately, with its own **scope**: names declared in `Blinker` are invisible in `Main`, and the other way round. That's why each POU has its own symbol table.

### 2.2 Instances at run time
**Code:** `Instance` in `src/st/interpreter.cpp`

```
root (Main)
├── values: fastLamp, slowLamp, ...
├── fast (Blinker) ── values: enable, onTime, offTime, out
│   ├── offTimer (TON) ── values: IN, PT, Q, ET  + C++ timer state
│   └── onTimer  (TON) ── ...
└── slow (Blinker) ── ...
```

`createInstance()` builds this tree once, at start-up. A call stores the arguments into the instance's inputs and then runs its body with **that instance as `self`**. That's how the same `Blinker` code works on `fast`'s memory in one call and on `slow`'s in the next.

### 2.3 Standard blocks: interface in ST, behaviour in C++
**Code:** `src/st/builtins.cpp`

The interfaces of TON, CTU and the others are written **in Structured Text** inside the C++ file, and parsed at start-up:

```
FUNCTION_BLOCK TON
VAR_INPUT IN : BOOL; PT : TIME; END_VAR
VAR_OUTPUT Q : BOOL; ET : TIME; END_VAR
END_FUNCTION_BLOCK
```

So the analyzer checks `t1(IN := ..., PT := ...)` exactly like a call to your own blocks, with no special cases. Only the behaviour is different: the instance gets a `NativeBlock` that runs C++ instead of an ST body.

### 2.4 Catching an FB that contains itself
**Code:** `checkNoFunctionBlockContainsItself()` in `src/st/analyzer.cpp`

If `A` contains a `B` and `B` contains an `A`, creating one instance would need infinitely many. The analyzer walks the "contains" relationships and keeps the current path. If it meets a block that's already on the path, it reports the loop: `function block 'A' contains itself: A -> B -> A`. This is the standard way to find a cycle in a graph: depth-first search with a path stack.

---

## Part 3: C++ concepts

### 3.1 Templates to share code between similar classes
**Code:** `TimerBlock<Timer>`, `CounterBlock<Counter>` in `src/st/builtins.cpp`

TON, TOF and TP have the same interface (IN, PT → Q, ET), so one class template adapts all three:

```cpp
template <class Timer> class TimerBlock : public NativeBlock {
    void execute(std::vector<Value>& v, TimePoint now) override {
        timer_.update(boolAt(v, 0), toDuration(intAt(v, 1)), now);
        ...
    }
    Timer timer_;
};
...
if (upper == "TON") return std::make_unique<TimerBlock<Ton>>();
if (upper == "TOF") return std::make_unique<TimerBlock<Tof>>();
```

The compiler writes a separate class for each `Timer` type. The only requirement is that the type has `update()`, `q()` and `et()`. This "if it has the right functions, it works" approach is sometimes called duck typing at compile time.

### 3.2 Templates and virtual functions together
`NativeBlock` is an **interface** with a virtual `execute()` (run-time choice: the interpreter doesn't know which block it's calling). `TimerBlock<Ton>` is a **template** (compile-time code generation). Each tool does a different job: templates avoid writing the same class three times, and virtual functions let one piece of code call any of them.

### 3.3 Factory functions
**Code:** `createBuiltin()`

A **factory** is a function that creates objects of different classes from some input, here a name, and returns them through the common interface: `std::unique_ptr<NativeBlock>`. The caller never needs to know about `TimerBlock` or `CounterBlock`.

### 3.4 Function-local statics: "build it once, on first use"
**Code:** `builtinFunctionBlocks()`

```cpp
const std::vector<Pou>& builtinFunctionBlocks() {
    static const std::vector<Pou> blocks = [] { ... parse ... }();
    return blocks;
}
```

The `static` variable is created the first time the function runs, and never again. C++ guarantees this is thread-safe. The `[] { ... }()` is a lambda that's called immediately, a neat way to run several statements to initialise one `const` variable.

### 3.5 Raw string literals
`R"( ... )"` is a string where backslashes and quotes need no escaping, and line breaks are kept. It's perfect for embedding the ST interface text in C++.

### 3.6 Incomplete types, `unique_ptr` and the out-of-line destructor
**Code:** `Interpreter` in `src/st/interpreter.hpp`

The header only says `struct Instance;` (a forward declaration); the real definition is in the `.cpp`. But `std::unique_ptr<Instance>` must know the complete type to delete it. That's why `~Interpreter()` and the move operations are **declared** in the header and **defined** in the `.cpp` (as `= default`), where `Instance` is complete. Code that includes the header never needs to know what an `Instance` looks like. This is the core of the **pimpl** ("pointer to implementation") idiom.

### 3.7 Keeping pointers valid when moving
**Code:** `std::unique_ptr<CompilationUnit> unit_` in the `Interpreter`

Instances point to their POU definitions (`instance->type`). If the `Interpreter` held the `CompilationUnit` directly and was moved, the POUs would move to a new address and those pointers would dangle. Putting the unit on the heap behind a `unique_ptr` fixes it: moving the `Interpreter` only moves the pointer, and the POUs stay where they are. When you store pointers to objects, always ask: can that object move?

---

## Part 4: Testing

- **The C++ blocks are tested on their own** (`tests/stdlib/`), with made-up timestamps like the TON tests from lesson 1, so timing edge cases are exact.
- **The same blocks are tested again from ST** (`tests/st/function_block_test.cpp`) with a fake clock that advances 10 ms per cycle, to check the bridge between the two.
- **Arrays of inputs and expected outputs** (`tests/stdlib/edges_test.cpp`) describe a signal over time compactly: `clk = {false, true, true, false}`, `expected = {false, true, false, false}`.

---

## Review questions

**1.** Two instances, `a, b : Counter;`. You call `a()` three times and `b()` once. How many times has `b`'s body run?
<details><summary>Answer</summary>

Once. Each instance has its own memory and only runs when *it* is called. See 1.1.
</details>

**2.** A TON is called inside `IF start THEN t1(IN := TRUE, PT := T#5s); END_IF;`. What goes wrong when `start` goes off?
<details><summary>Answer</summary>

The timer is no longer called, so it never sees IN go off. It keeps its old state: Q may stay on, and it won't reset. Call timers every cycle and put the condition in `IN` instead: `t1(IN := start, PT := T#5s);`. See 1.3.
</details>

**3.** What's the difference between `TON` and `TOF`?
<details><summary>Answer</summary>

TON delays switching **on** (Q goes on PT after IN). TOF delays switching **off** (Q stays on for PT after IN goes off). See 1.5.
</details>

**4.** A sensor stays on for 50 cycles while a box passes. How much does a `CTU` count?
<details><summary>Answer</summary>

One. Counters count rising edges, not cycles. See 1.5.
</details>

**5.** Why is `T#5s + 3` rejected?
<details><summary>Answer</summary>

`3` has no unit, so adding it to a duration is meaningless. Write `T#5s + T#3s`. Multiplying is fine: `T#5s * 3` is 15 seconds. See 1.4.
</details>

**6.** Why are `~Interpreter()` and its move operations defined in the `.cpp` file?
<details><summary>Answer</summary>

`unique_ptr<Instance>` needs the complete `Instance` type to delete it, and `Instance` is only defined in the `.cpp`. See 3.6.
</details>

**7.** What does `TimerBlock<Tof>` need from `Tof` to compile?
<details><summary>Answer</summary>

Functions `update(bool, Duration, TimePoint)`, `q()` and `et()`. Any type with those works. See 3.1.
</details>
