# 3. Who owns what

soft_plc has no garbage collector and no `std::shared_ptr` at all. Every object has **exactly one owner**, and everything else that uses it borrows it, through a reference or a raw pointer. That only works if every borrower is sure to die before the owner, so the design is mostly about arranging lifetimes so that's obviously true.

The rule of thumb used everywhere:

| To say | Use |
|---|---|
| "I own this, and it's mine alone" | a member by value, or `std::unique_ptr` when it must be on the heap |
| "I use this; someone else owns it, and it outlives me" | a reference member (`Program&`), or a raw pointer when it can be null or change |
| "I look at these values for a moment" | `std::span`, `std::string_view` |

`std::shared_ptr` ("whoever is last turns off the light") was never needed: there's no object here whose lifetime nobody can say in advance.

---

## Trees own downwards

The syntax tree is a tree of `std::unique_ptr`: a statement owns its expressions, a binary expression owns its two sides (`ExpressionPtr` in `src/st/ast.hpp`). The instance tree is the same: an `Instance` owns its child instances (`std::vector<std::unique_ptr<Instance>> children`). Deleting the root deletes everything, in the right order, with no code at all.

Trees are the easy case. The interesting part is everything that points **sideways**: an `Access` points at the `Type` it reads, an instance at the `Pou` it's an instance of, the VM at the code of a POU. Those are raw pointers (`const Pou*`, `const Type*`), and each one is only safe because what it points at **never moves**.

---

## Making things that never move

### The analyzer's types

The analyzer creates a `Type` for every array and struct it meets, and everything else points at them. They're kept like this (`CompilationUnit`, `src/st/ast.hpp`):

```cpp
std::vector<std::unique_ptr<Type>> analyzedTypes; // owned here, created by the analyzer
```

Why not `std::vector<Type>`? Because a vector **moves its elements** when it grows past its capacity. Every `const Type*` taken before would then point at freed memory. With `unique_ptr`s in the vector, growing moves the *pointers*; the `Type`s stay where they were allocated. One heap allocation per type is the price, paid once at compile time.

### The interpreter's compilation unit

```cpp
std::unique_ptr<CompilationUnit> unit_; // src/st/interpreter.hpp
```

The `Interpreter` owns the program it runs, and its instances point into it (`instance->type` is a `const Pou*` into `unit_`). The `Interpreter` is movable: the online change builds a new one and move-assigns it into `StProgram` (`StProgram::replace`). If the `CompilationUnit` were a plain member, moving the `Interpreter` would move it too, and `unit.program` (a `Pou` held directly, not through a pointer) would end up at a new address, leaving every instance of the program pointing at the old one. On the heap, behind a `unique_ptr`, moving the `Interpreter` just hands over the pointer, and the unit stays put. The comment in the header says exactly that, because it's the kind of reason that's invisible in the code.

### The VM's stack

```cpp
vmStack_.resize(16 * 1024); // once, in the constructor
```

The VM keeps raw pointers into its stack while it runs (`VmSlot* const temps = vmStack_.data() + vmTop_`), including across nested function block calls. A vector that grew on demand could reallocate under them. So it's sized **once**, and a call that would need more is a runtime error ("VM stack overflow: function blocks nested too deeply", so the PLC goes to STOP) instead of a reallocation. A fixed limit in exchange for pointers that are always valid: the same trade a real PLC makes with its local data stack.

---

## References for dependencies

```cpp
class ScanCycle {                        // src/core/scan_cycle.hpp
    ScanCycle(ProcessImage& image, IoDriver& io, Program& program, Duration cycleTime);
    ...
    ProcessImage& image_;
    IoDriver& io_;
    Program& program_;
};
```

The scan cycle *uses* the process image, the I/O driver and the program; it doesn't own them. Reference members say three things at once: it can't be null, it can't be re-pointed, and someone else owns it. `main()` then creates them in order (image, I/O, program, scan cycle) as local variables, and C++ destroys locals in the **reverse** order, so the scan cycle is always gone before the things it refers to.

The price of reference members: the class can't be copy-assigned (a reference can't be re-pointed). For a scan cycle that's a feature: there's no meaningful "make this scan cycle a copy of that one".

---

## Holding something "for now": RAII

The interpreter needs the process image while a cycle runs, to read and write `AT` variables, but doesn't own it and shouldn't keep a pointer to it after the cycle. `ImageBinding` (`src/st/interpreter.cpp`) is a tiny class that sets `image_` in its constructor and clears it in its destructor:

```cpp
{
    ImageBinding binding(image_, image); // image_ = &image
    ...                                   // run the cycle; may throw
}                                         // image_ = nullptr, even after an exception
```

Clearing it by hand at the end of `runCycle` would be forgotten on the path where a runtime error throws out of the middle. A destructor runs on every path. That's what RAII means in practice: tie the "undo" to a scope, and it can't be skipped.

---

## Hiding what's inside: incomplete types

`Instance` is declared in the header (`struct Instance;`) but defined only in `interpreter.cpp`. Code outside the interpreter can hold `std::unique_ptr<Instance>` without knowing what's in it, so changing an instance's layout recompiles one file instead of everything that includes the header.

There's one catch, and the header has a comment about it: `unique_ptr`'s destructor needs the full type, so the `Interpreter`'s destructor and move operations are **declared** in the header and **defined** in the `.cpp` (`Interpreter::~Interpreter() = default;` there), where `Instance` is complete.

---

## Moved-from objects

The online change moves each standard block's state out of the old program (`now.native = std::move(before.native)`), leaving the old `Interpreter` with empty pointers where its timers' state was. That's fine, because the old one is destroyed right after. But a moved-from object that *looks* normal is a classic trap, so `takeOver`'s comment says plainly: `old` is left without its standard blocks' state, throw it away.

---

## Threads are members too

A `std::thread` member that runs a function using other members must start **after** those members exist and stop **before** they're destroyed. `MonitorServer` declares its accept thread **last** (`std::thread acceptThread_; // last, so it starts after everything above exists`) and its destructor joins every thread before any member goes away. Members are constructed in declaration order and destroyed in reverse, so where a line sits in the class is part of the design.

---

## Review questions

**1.** `analyzedTypes` is a `std::vector<std::unique_ptr<Type>>`. What would go wrong with `std::vector<Type>`, and when?
<details><summary>Answer</summary>

When the vector grows past its capacity, it moves its `Type`s to new memory. Every `const Type*` already stored (in `Access` nodes, in fields, in other types) would point at the old, freed memory. With `unique_ptr`s, only the pointers move; the `Type`s stay where they are.
</details>

**2.** Why is `Interpreter::unit_` behind a `unique_ptr`, when the `Interpreter` is its only owner?
<details><summary>Answer</summary>

So the `CompilationUnit` doesn't move when the `Interpreter` is moved (the online change moves a new interpreter into `StProgram`). Instances point into the unit (`instance->type`); if the unit moved, `unit.program`'s address would change and those pointers would dangle. Behind a `unique_ptr`, a move only transfers the pointer.
</details>

**3.** In `main()`, the scan cycle is created after the process image and the program. Why does that order matter at the end of `main()`?
<details><summary>Answer</summary>

Locals are destroyed in reverse order, so the scan cycle (which holds references to the image and the program) is destroyed first, while what it refers to still exists. Created the other way round, its references would outlive their targets.
</details>

**4.** What would happen if `runCycle` set `image_ = &image` at its start and `image_ = nullptr` at its end, without `ImageBinding`, and a cycle hit a division by zero?
<details><summary>Answer</summary>

The `RuntimeError` exception would leave `runCycle` before the last line, so `image_` would keep pointing at the process image after the cycle: the interpreter would hold a pointer it doesn't own past the moment it was meant for, and if that process image were ever destroyed first (in a test, or a restart), the pointer would dangle. The RAII class clears it on every way out of the scope, exceptions included.
</details>
