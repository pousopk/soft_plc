# C++ 5: Inside the interpreter and the VM

How the PLC's memory for variables is laid out, how values are computed without undefined behaviour, and how the bytecode VM loop is written for speed.

**Code:** `src/st/interpreter.hpp/.cpp`, `src/st/runtime_values.hpp`, `src/st/bytecode.hpp`, `src/st/compiler.cpp`

---

## 1. Instances: a tree of memory

```cpp
// src/st/interpreter.cpp
struct Instance {
    const Pou* type = nullptr;                          // which PROGRAM/FUNCTION_BLOCK this is
    std::vector<Value> values;                          // one per variable, in declaration order
    std::vector<std::unique_ptr<Instance>> children;    // one per variable; set only for FB instances
    std::unique_ptr<NativeBlock> native;                // set only for TON, CTU, ...
};
```

For this program:

```
PROGRAM Main
VAR  count : INT;  fast : Blinker;  t1 : TON;  END_VAR
```

the interpreter builds:

```
root (Main)
 ├── values:   [count=0, -, -]
 └── children: [null, ─────┐, ──────┐]
                           │        │
              Instance(Blinker)   Instance(TON)
               values: [...]       values: [IN, PT, Q, ET]
               children: [...]     native: TimerBlock<Ton>
```

This mirrors a Siemens PLC exactly: each FB instance has its own **instance memory**, and multi-instances live inside their parent's.

### Slots instead of names
The analyzer replaces every name with an **index**:

```cpp
struct VariableRef {
    std::string name;
    std::size_t slot = kNoSlot; // set by the analyzer: index into the POU's variables
};
constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();
```

At run time, reading `count` is `self.values[slot]`, a direct array access, with no string comparison and no map lookup. Doing the slow work (finding names) **once, before running** is the theme of both engines. `kNoSlot` is the largest possible `size_t`, a value that can never be a real index, so "not resolved yet" is visible in a debugger.

### Pointers into the tree, and why `CompilationUnit` is on the heap
`Instance::type` and `VariableDeclaration::functionBlock` are raw `const Pou*` pointers into the `CompilationUnit`. Raw pointers here mean **"I use it, I don't own it"**. The danger is the thing they point to moving:

```cpp
// src/st/interpreter.hpp
// On the heap, so that the instances' pointers into it stay valid when
// the Interpreter itself is moved.
std::unique_ptr<CompilationUnit> unit_;
```

If `unit_` were a plain member, moving an `Interpreter` would move the `CompilationUnit` (and its `Pou`s) to a new address, and every `const Pou*` would point at the old, dead one. On the heap, moving the `Interpreter` just hands over the `unique_ptr`; the `CompilationUnit` itself stays put.

---

## 2. Hiding a type: incomplete types and the out-of-line destructor

The header only says `struct Instance;`. Its contents are in the `.cpp`:

```cpp
// interpreter.hpp
struct Instance;                         // "exists, but you can't see inside"
class Interpreter {
public:
    ~Interpreter();                      // declared here...
    Interpreter(Interpreter&&) noexcept;
    Interpreter& operator=(Interpreter&&) noexcept;
private:
    std::unique_ptr<Instance> root_;
};

// interpreter.cpp
struct Instance { ... };                 // the full definition
Interpreter::~Interpreter() = default;   // ...defined here, where Instance is complete
Interpreter::Interpreter(Interpreter&&) noexcept = default;
Interpreter& Interpreter::operator=(Interpreter&&) noexcept = default;
```

`unique_ptr<Instance>` can be a member of a type that's only declared. But **destroying** it calls `delete` on an `Instance`, and the compiler must know `Instance` fully to do that. If the destructor were left implicit, the compiler would generate it inline in every file that destroys an `Interpreter` (including `main.cpp`), where `Instance` is unknown: compile error. Declaring `~Interpreter()` in the header and writing `= default` in the `.cpp` puts the generated code where `Instance` is complete.

Why bother? `main.cpp`, the monitor and the tests include `interpreter.hpp` and never need to know how instances are stored. Change `Instance` and only `interpreter.cpp` is recompiled. (This is close to the "pimpl" idiom.)

---

## 3. `resolvePath`: one template for `const` and non-`const`

```cpp
template <class InstanceType>
std::optional<Resolved<InstanceType>> resolvePath(InstanceType& root, std::string_view path) {
    InstanceType* instance = &root;
    ...
    instance = instance->children[*slot].get();
    ...
}

std::optional<Value> Interpreter::valueOf(std::string_view path, const ProcessImage& image) const {
    const auto found = resolvePath(std::as_const(*root_), path);    // InstanceType = const Instance
    ...
}
bool Interpreter::setValue(std::string_view path, const Value& value, ProcessImage& image) {
    const auto found = resolvePath(*root_, path);                   // InstanceType = Instance
    ...
}
```

Walking `"fast.offTimer.ET"` down the tree is the same code for reading and writing. The only difference is whether the result may be modified. Making the instance type a template parameter gives both from one function: reading gets `const Instance*`, writing gets `Instance*`. The lesson C2 trick (`as_const` + `const_cast`) is another way to solve the same problem.

---

## 4. Arithmetic without undefined behaviour

**Code:** `src/st/runtime_values.hpp`

In C++, **signed integer overflow is undefined behaviour** (UB). `INT64_MAX + 1` isn't "wraps to negative"; it's "the compiler may assume this never happens", and optimisations built on that assumption can produce nonsense. A PLC, on the other hand, *defines* overflow: INT 32767 + 1 = −32768. So the runtime does arithmetic in a way C++ guarantees:

```cpp
inline std::int64_t add(std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
}
```
**Unsigned** overflow is well defined (it wraps modulo 2⁶⁴). Convert to unsigned, add, convert back; since C++20, the conversion back is also guaranteed to wrap.

Then the result is brought into the ST type's range:
```cpp
inline std::int64_t wrapInteger(std::int64_t value, DataType type) {
    switch (type) {
    case DataType::Int: return static_cast<std::int16_t>(value);     // keeps the low 16 bits
    case DataType::DInt:
    case DataType::Time: return static_cast<std::int32_t>(value);
    default: return value;
    }
}
```

The one remaining trap is division: `INT64_MIN / -1` overflows (the answer would be `INT64_MAX + 1`), and it's UB that crashes on x86. Hence:
```cpp
inline std::int64_t divide(std::int64_t a, std::int64_t b) { return b == -1 ? subtract(0, a) : a / b; }
```

The Debug build's **UBSan** (`-fsanitize=undefined`) would report any signed overflow that slipped through. The tests run under it.

### REAL is a 32-bit float
```cpp
inline double roundToReal(double value) { return static_cast<double>(static_cast<float>(value)); }
```
Values are stored as `double` (the `Value` variant has one floating type), but after every operation they're rounded to `float` precision, so `0.1 + 0.2` gives the same bits as on an S7.

### `std::bit_cast`: same bits, different type (C++20)
```cpp
// Interpreter::load, for `x AT %MD0 : REAL`
case DataType::Real:
    return static_cast<double>(std::bit_cast<float>(raw));   // raw: the 32 bits in memory
```
The 4 bytes in `%MD0` **are** an IEEE float. `static_cast<float>(raw)` would convert the *number* (0x3F800000 → 1065353216.0). `bit_cast` reinterprets the *bits* (0x3F800000 → 1.0f). Before C++20 you'd use `std::memcpy`; casting pointers (`*(float*)&raw`) breaks C++'s aliasing rules and is UB.

---

## 5. The bytecode VM

### An instruction is a small struct
```cpp
// src/st/bytecode.hpp
enum class Op : std::uint8_t { PushInt, PushReal, LoadBool, LoadInt, ..., Call, Return };

struct Instruction {
    Op op;
    std::uint32_t a = 0;   // a slot, temp or jump target
    std::uint32_t b = 0;
    std::int64_t imm = 0;  // an immediate constant
};

struct Chunk {
    std::vector<Instruction> code;
    std::vector<SourceLocation> locations;   // one per instruction, for error messages
    std::size_t temps = 0;
    std::size_t maxStack = 0;
};
```
`enum class Op : std::uint8_t` chooses the enum's underlying type: one byte. A program is a `std::vector<Instruction>`: contiguous memory, so the CPU reads instructions one after another from cache. The source locations are kept in a **separate** vector, because they're only needed when something goes wrong, and keeping them out of `Instruction` keeps the hot data small.

### An untagged stack slot: `union`
```cpp
union VmSlot {
    std::int64_t i;
    double r;
};
```
A `union` puts all its members **in the same memory**: a `VmSlot` is 8 bytes, either an integer or a double. Unlike `std::variant`, it **doesn't remember which one** it holds, so reading the wrong member is a bug the language won't catch. That's acceptable here only because the compiler emits typed instructions (`AddI` vs `AddR`): the analyzer proved every type, so the VM never needs to check. `std::variant` would check the tag on every access, and that's exactly the cost the VM exists to avoid.

### The loop
```cpp
// Interpreter::runBytecode
VmSlot* const temps = vmStack_.data() + vmTop_;
VmSlot* sp = temps + chunk.temps;                     // stack pointer: just past the top
const Instruction* const code = chunk.code.data();
std::size_t pc = 0;                                   // program counter

for (;;) {
    const Instruction& in = code[pc++];
    switch (in.op) {
    case Op::PushInt: (sp++)->i = in.imm; break;      // write at sp, then move up
    case Op::LoadInt: (sp++)->i = std::get<std::int64_t>(values[in.a]); break;
    case Op::StoreInt16: values[in.a] = std::int64_t{static_cast<std::int16_t>((--sp)->i)}; break; // move down, then read
    case Op::AddI: --sp; sp[-1].i = add(sp[-1].i, sp->i); break;   // two operands → one result
    case Op::JumpIfFalse:
        if ((--sp)->i == 0) {
            pc = in.a;
        }
        break;
    ...
    case Op::Return: return;
    }
}
```

- **Raw pointers** (`sp`, `code`) and **pointer arithmetic** are used instead of `vector::push_back` / `pop_back` because they're as cheap as it gets: no size checks, no reallocation.
- `(sp++)->i = x` means "write `x` at `sp`, then move `sp` up one". `(--sp)->i` means "move down one, then read". That's push and pop.
- `sp[-1]` is the slot just below `sp`, the current top. `AddI` pops the right operand (`--sp`, now `sp` points at it) and writes the sum into the left operand's slot (`sp[-1]`).
- One `switch` on a one-byte opcode: GCC compiles it into a **jump table**, a single indexed jump per instruction.

**Why are raw pointers safe here?** `vmStack_` is resized once to 16 K slots in the constructor and never resized again, so `data()` never changes. Each call checks it has room (`vmTop_ + frameSize > vmStack_.size()` → "VM stack overflow" error) before reserving a frame. And the compiler computed `maxStack` for each chunk, so a frame can't grow past what was reserved.

### A lambda for an expensive thing that's rarely needed
```cpp
const auto where = [&] { return chunk.locations[pc - 1]; };
...
throw RuntimeError{"division by zero", where()};
```
The location is only looked up when an error is actually thrown. Naming it as a lambda keeps the error lines short.

---

## 6. The compiler: emitting and patching jumps

**Code:** `src/st/compiler.cpp`

```cpp
std::size_t emit(Op op, SourceLocation where, std::uint32_t a = 0, std::uint32_t b = 0, std::int64_t imm = 0) {
    chunk_.code.push_back(Instruction{op, a, b, imm});
    chunk_.locations.push_back(where);
    depth_ += stackEffect(op);
    maxDepth_ = std::max(maxDepth_, depth_);
    return chunk_.code.size() - 1;               // the index of the new instruction
}
```
- **Default arguments** (`a = 0, ...`): most instructions have no operands, so `emit(Op::AddI, where)` is enough.
- It tracks the **stack depth** as it goes, so `maxStack` is known exactly when compiling ends. The VM uses it to reserve the frame.

**Backpatching:** when compiling `IF c THEN x; END_IF;`, the `JumpIfFalse` must jump past `x`, but `x` hasn't been compiled yet, so its end isn't known. The compiler emits the jump with a dummy target, remembers its index, compiles `x`, then fixes it up:

```cpp
expression(*branch.condition);
const auto skip = emit(Op::JumpIfFalse, where);   // target unknown yet
statements(branch.body);
...
patchJump(skip, here());                          // now we know: jump to here
```

`EXIT` works the same way, with a **stack of lists**, one list per loop being compiled:
```cpp
std::vector<std::vector<std::size_t>> exits_;     // per enclosing loop: its EXIT jumps

void statement(const ExitStatement&, SourceLocation where) {
    exits_.back().push_back(emit(Op::Jump, where));   // innermost loop
}
void closeLoop() {
    for (const auto jump : exits_.back()) {
        patchJump(jump, here());
    }
    exits_.pop_back();
}
```
Nested loops push and pop, so an `EXIT` always belongs to the innermost loop, exactly as ST requires.

### Keyed by pointer: `unordered_map<const Pou*, Chunk>`
```cpp
std::unordered_map<const Pou*, Chunk> chunks_;
...
runBytecode(chunks_.at(instance.type), instance);
```
Each POU is compiled once, and its chunk is found through the POU's **address**, a unique key that's cheap to hash. It works because the `Pou`s never move (section 1).

---

## Review questions

**1.** Why does reading a variable at run time use `self.values[slot]` instead of looking the name up in a `std::map<std::string, Value>`?
<details><summary>Answer</summary>

Speed. The analyzer resolves every name to an index once, before the program runs. At run time a vector index is one memory access; a map lookup means comparing strings at every step, every cycle.
</details>

**2.** `Interpreter` has `~Interpreter();` in the header and `Interpreter::~Interpreter() = default;` in the `.cpp`. Why not just leave the destructor out?
<details><summary>Answer</summary>

`root_` is a `unique_ptr<Instance>`, and `Instance` is only declared in the header. An implicit destructor would be generated in every file that destroys an `Interpreter`, where `Instance` is incomplete, and `unique_ptr` can't delete an incomplete type. Defining it in the `.cpp`, where `Instance` is complete, fixes that.
</details>

**3.** Why does `add()` convert to `std::uint64_t` and back instead of just writing `a + b`?
<details><summary>Answer</summary>

Signed overflow is undefined behaviour in C++, while ST defines it (it wraps). Unsigned overflow is defined to wrap, and the conversion back to signed is defined to wrap since C++20. So the result is the PLC's result, guaranteed.
</details>

**4.** `x AT %MD4 : REAL;` and the 4 bytes hold `0x40490FDB`. What do `static_cast<float>(raw)` and `std::bit_cast<float>(raw)` give?
<details><summary>Answer</summary>

`static_cast` converts the integer value: about 1078530011.0. `bit_cast` reinterprets the bits as an IEEE float: about 3.1415927 (π). The PLC means the second.
</details>

**5.** The VM uses `union VmSlot` but the AST uses `std::variant` for values. Why not a variant in the VM too?
<details><summary>Answer</summary>

A variant checks its tag on every access, and an untagged union doesn't. In the VM, the compiler already knows the type of every slot (instructions are typed: `AddI`, `AddR`), so the check would only cost time. The union is only safe because of that guarantee; reading the wrong member would be a silent bug.
</details>

**6.** In the VM loop, what do `(sp++)->i = 5;` and `(--sp)->i` do?
<details><summary>Answer</summary>

`(sp++)->i = 5;` writes 5 into the slot `sp` points at, then moves `sp` up one: a push. `(--sp)->i` moves `sp` down one, then reads that slot: a pop.
</details>

**7.** When the compiler emits a `JumpIfFalse` for an `IF`, it doesn't know the target yet. How does it get fixed?
<details><summary>Answer</summary>

`emit` returns the instruction's index. The compiler keeps it, compiles the body, and then calls `patchJump(index, here())` to write the now-known target into that instruction (backpatching). `EXIT` jumps are collected per loop in `exits_` and patched when the loop is closed.
</details>
