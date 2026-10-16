# Lesson 7: A bytecode compiler and VM

Milestone 7 adds a second way to run programs: **compile** each POU once into a flat list of simple instructions (**bytecode**), and run those on a small **virtual machine** (VM). It's how Python, Java and many PLC runtimes work. It gives the same results as the tree-walking interpreter from lesson 3, and is about 2–3 times faster.

```
syntax tree ──► compiler (src/st/compiler.cpp) ──► bytecode ──► VM (Interpreter::runBytecode)
                         once, at start-up                        every cycle
```

Try it:
```bash
./build/st_parse --bytecode examples/benchmark.st
./build-release/st_bench examples/benchmark.st
```

---

## Part 1: Compiler and VM concepts

### 1.1 Why the tree-walker is slow
To compute `b + 3 * c`, the tree-walker visits the `+` node, which calls `evaluate()` on its children, which visits the `*` node, and so on. Every node costs a function call, a `std::visit` to find out what kind of node it is, a check of `operandType`, and building and taking apart `Value` variants. That's a lot of work just to know what to do, every cycle, again and again, for code that never changes.

### 1.2 Bytecode: decide once, then just do
The compiler makes all those decisions **once**, at start-up, and writes down the result as a list of instructions:

```
a := b + 3 * c;        LoadInt    b        ; push b
                       PushInt    3        ; push 3
                       LoadInt    c        ; push c
                       MulI                ; pop 3 and c, push 3*c
                       Wrap16              ; INT: wrap to 16 bits
                       AddI                ; pop b and 3*c, push the sum
                       Wrap16
                       StoreInt16 a        ; pop the result into a
```

`MulI` already says "these are integers"; `StoreInt16` already says "wrap to 16 bits". The VM doesn't have to look anything up.

### 1.3 A stack machine
Most instructions take their operands from the top of a **stack** and push their result back. There are no registers to allocate, so the compiler stays simple: compile the left operand, then the right one, then emit the operator. It's the same bottom-up order as the tree-walker, written out as a list.

### 1.4 Control flow becomes jumps
There's no `IF` instruction. `IF`, `CASE` and the loops become **conditional jumps** to other positions in the list:

```
IF a > 10 THEN b := 0;        LoadInt a; PushInt 10; GtI
ELSE b := b + 1; END_IF;      JumpIfFalse ELSE
                              PushInt 0; StoreInt16 b
                              Jump END
                       ELSE:  LoadInt b; PushInt 1; AddI; Wrap16; StoreInt16 b
                       END:
```

The compiler doesn't know where `ELSE` will be when it writes the `JumpIfFalse`, so it writes the jump first and **patches** the target in later (`patchJump()`). `EXIT` works the same way: each loop keeps a list of its `EXIT` jumps and patches them all when the loop ends.

### 1.5 The VM loop
**Code:** `Interpreter::runBytecode()` in `src/st/interpreter.cpp`

```cpp
for (;;) {
    const Instruction& in = code[pc++];
    switch (in.op) {
    case Op::AddI: --sp; sp[-1].i = add(sp[-1].i, sp->i); break;
    case Op::JumpIfFalse: if ((--sp)->i == 0) pc = in.a; break;
    ...
    }
}
```

Fetch an instruction, do it, repeat. `pc` (program counter) is where we are in the code; `sp` (stack pointer) points just past the top of the stack. A `switch` on a small enum compiles to a jump table, so dispatching one instruction is very cheap.

### 1.6 Same memory, same behaviour
The VM uses the same `Instance` memory as the tree-walker, so monitoring, forcing, the S7 server and function block instances work identically with either engine. The arithmetic helpers (`wrapInteger`, `roundToReal`, `divide`...) moved to `src/st/runtime_values.hpp`, so both engines share them.

### 1.7 Where the time still goes
2–3× is a typical gain for a first VM. The remaining cost is mostly in `Instance::values`, which still holds `std::variant`s (every load and store checks which type is inside), plus the function call for every FB call and the watchdog check in every loop. A next step would be to store variables in untyped slots too, the way the operand stack already does.

---

## Part 2: Proving the two engines agree

### 2.1 The same tests, twice
**Code:** `tests/support/engine.hpp`, `tests/CMakeLists.txt`

`ctest` runs the whole test suite twice: once with `SOFT_PLC_TEST_ENGINE=tree` and once with `bytecode`. Every test written since milestone 3 now checks both engines.

### 2.2 Differential testing
**Code:** `tests/st/engine_equivalence_test.cpp`

Specific tests only check what someone thought of. **Differential testing** compares two implementations on many inputs instead: every example program, plus a stress program that uses every instruction, runs on both engines side by side for hundreds of cycles, with the **same pseudo-random inputs** every cycle. After each cycle, every variable and every byte of memory must be identical, about half a million checks in all. A fixed random seed (`std::mt19937 random(12345)`) makes the "random" inputs the same on every run, so any failure can be reproduced.

### 2.3 Benchmarking honestly
**Code:** `tools/st_bench.cpp`

- **Release build.** The Debug build's sanitizers slow everything down several times and would distort the comparison.
- **Warm-up first.** The first cycles pay for cache misses and memory allocation; they're run but not timed.
- **Check the result.** A benchmark that gets faster by computing the wrong thing is worthless, so `st_bench` also compares the final variables of both engines.

---

## Part 3: C++ concepts

### 3.1 `union`: one slot, two meanings
**Code:** `VmSlot` in `src/st/interpreter.hpp`

```cpp
union VmSlot {
    std::int64_t i;
    double r;
};
```

A `union` stores **one** of its members in the same memory. Unlike `std::variant`, it doesn't remember which one, so it's smaller and faster, but reading the wrong member is a bug. That's safe here only because the compiler knows every value's type: `AddI` always finds integers, `AddR` always finds REALs.

### 3.2 `std::bit_cast` again: a double inside an int64
`PushReal` keeps its constant in the instruction's `std::int64_t imm` field: `std::bit_cast<std::int64_t>(3.14)` stores the double's 64 bits unchanged, and `std::bit_cast<double>(in.imm)` gets it back. One field serves both kinds of constant.

### 3.3 Raw pointers into a vector, made safe
`vmStack_` is a `std::vector<VmSlot>` sized once (16 384 slots) and never resized. So `sp` and `temps` can be raw pointers into it, the fastest way to access it. If it could grow, a reallocation would leave them dangling. Nested function block calls use the part of the stack above their caller's, and a small RAII guard (`FrameGuard`) gives that space back even when an exception is thrown.

### 3.4 `std::unordered_map` keyed by pointer
`chunks_` maps `const Pou*` to its compiled `Chunk`. The address of a POU is a perfect key: unique, cheap to hash, and stable, because the `CompilationUnit` lives on the heap (lesson 4).

### 3.5 Unsigned operands and `-Wsign-conversion`
The first version stored operands as `std::int32_t`, and the compiler warned on every `values[in.a]`: a negative number converted to a `size_t` index becomes huge. Since operands are always slots or jump targets, they're now `std::uint32_t`, and the warning (and the theoretical bug) is gone. That's the kind of mistake strict warnings catch early.

---

## Review questions

**1.** What does the compiler do once that the tree-walker does every cycle?
<details><summary>Answer</summary>

It works out what each node is and which type each operation uses, and writes the answer down as typed instructions (`AddI`, `StoreInt16`...). The tree-walker rediscovers all of that every time it evaluates a node. See 1.1 and 1.2.
</details>

**2.** In a stack machine, what's on the stack after `PushInt 2; PushInt 3; MulI`?
<details><summary>Answer</summary>

A single value, 6. `MulI` pops 3 and 2 and pushes their product. See 1.3.
</details>

**3.** Why does the compiler emit a jump before it knows where it should go?
<details><summary>Answer</summary>

The target (e.g. the start of the `ELSE` part) comes later in the code. The jump is written with a placeholder and patched once the target's position is known. See 1.4.
</details>

**4.** Why can the VM use a `union` for its stack instead of a `std::variant`?
<details><summary>Answer</summary>

Because the instructions are typed: the compiler guarantees `AddI` only ever finds integers and `AddR` only REALs, so the VM never needs to ask what's inside. A union is smaller and needs no checks. See 3.1.
</details>

**5.** What is differential testing, and why does it use a fixed random seed?
<details><summary>Answer</summary>

Running two implementations on the same many inputs and checking they agree. The fixed seed makes the "random" inputs the same on every run, so a failure can be reproduced. See 2.2.
</details>

**6.** Why must benchmarks run on a Release build?
<details><summary>Answer</summary>

The Debug build includes sanitizers and no optimisation, which slow code down by different amounts and would distort the comparison. See 2.3.
</details>
