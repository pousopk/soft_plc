# C++ 2: Classes, memory as bytes, interfaces and templates

How the PLC's core types are built: small value types (`Address`), a class that owns memory (`ProcessImage`), interfaces (`Program`, `IoDriver`, `NativeBlock`) and templates that turn the C++ timers into ST function blocks.

---

## 1. `struct` for data, `class` for behaviour

```cpp
// src/core/address.hpp
struct Address {
    Area area = Area::Input;
    AccessSize size = AccessSize::Bit;
    std::size_t byteOffset = 0;
    unsigned bit = 0;
    std::uint16_t dbNumber = 0;

    bool operator==(const Address&) const = default;
    std::string toString() const;
};
```

In C++, `struct` and `class` are the same thing with one difference: members are public by default in a `struct` and private in a `class`. The convention used here:
- **`struct`:** plain data, all fields public, no rules to protect (`Address`, `CycleStats`, `Token`, `Instruction`).
- **`class`:** has an *invariant*, a rule that must always hold, so the data is private and only the member functions touch it. `ProcessImage` promises "areas never change size"; if `inputs_` were public, anyone could `resize()` it and break the S7 server, which holds pointers into it.

Every field has a **default value** (`= 0`, `= Area::Input`), so `Address{}` is never garbage. It also allows short initialisation: `Address{Area::Output, AccessSize::Bit, byte, bit}` fills the first four fields in order and leaves `dbNumber` at 0.

### `enum class`
```cpp
enum class Area { Input, Output, Marker, DataBlock };
```
Unlike a plain `enum`, the names are **scoped** (`Area::Input`, not just `Input`) and there's **no silent conversion to int**. `if (area == 2)` doesn't compile.

Look at how every `switch` over an enum is written:

```cpp
// src/core/process_image.cpp
std::size_t widthOf(AccessSize size) {
    switch (size) {
    case AccessSize::Bit:
    case AccessSize::Byte: return 1;
    case AccessSize::Word: return 2;
    case AccessSize::DWord: return 4;
    }
    return 1;
}
```

There's **no `default:`** on purpose. With `-Wall`, GCC warns when a `switch` over an enum misses a value. Add `AccessSize::LWord` one day and the compiler lists every switch that needs updating. A `default:` would hide them all. The `return 1;` after the switch is never reached; it's there because the compiler can't be sure an enum variable only holds valid values.

---

## 2. `ProcessImage`: owning bytes, lending views

```cpp
// src/core/process_image.hpp
std::vector<std::uint8_t> inputs_;
std::vector<std::uint8_t> outputs_;
std::vector<std::uint8_t> markers_;
std::map<std::uint16_t, std::vector<std::uint8_t>> dataBlocks_;
```

- **`std::vector<std::uint8_t>`:** a growable array of bytes on the heap. It's created once, with a size, in the constructor's **initialiser list**:
  ```cpp
  ProcessImage::ProcessImage(std::size_t inputBytes, std::size_t outputBytes, std::size_t markerBytes)
      : inputs_(inputBytes), outputs_(outputBytes), markers_(markerBytes) {}
  ```
  `inputs_(16)` means "16 elements, all zero". (Careful: `inputs_{16}` with braces would mean "one element with the value 16". Classic trap.)
- **`std::map<number, bytes>`:** data blocks by number, kept sorted. A `map` never moves its elements when others are added, so a DB's bytes stay where they are once created. The S7 server relies on that.
- **`std::uint8_t`, `std::uint16_t`…** from `<cstdint>`: integers with an exact width. A PLC byte is exactly 8 bits; `int` or `char` could be anything.

### `std::span`: a window into someone else's memory
```cpp
std::span<std::uint8_t> bytes(Area area);
std::span<const std::uint8_t> bytes(Area area) const;
```
A `span` is just **a pointer plus a length**. It doesn't own anything; it's cheap to copy (16 bytes); and it works with any contiguous memory (a vector, an array, a raw buffer). The I/O driver, the S7 server and `mergeClientWrites()` all take spans, so they don't care how the bytes are stored.

`span<const uint8_t>` is a read-only window. The `const` version of `bytes()` returns one, so a `const ProcessImage&` can only be looked at.

### Two versions of the same function: `const` and non-`const`
```cpp
std::span<std::uint8_t> ProcessImage::dataBlock(std::uint16_t number) {
    const auto view = std::as_const(*this).dataBlock(number);
    return {const_cast<std::uint8_t*>(view.data()), view.size()}; // safe: *this isn't const here
}
```
Both versions do the same lookup, so the non-`const` one **calls the `const` one** and removes the `const` from the result:
1. `std::as_const(*this)` treats this object as `const` for a moment, so the `const` overload is chosen.
2. `const_cast` removes `const` from the pointer.

`const_cast` is normally a red flag, but this is the one accepted use: the object really *isn't* const (we're inside the non-`const` function), so writing through the pointer is fine. The alternative is to copy the code twice.

### Bit manipulation, and why there are so many `static_cast`s
```cpp
void setBit(std::span<std::uint8_t> mem, std::size_t offset, unsigned bit, bool value) {
    const auto mask = static_cast<std::uint8_t>(1u << bit);
    mem[offset] = value ? static_cast<std::uint8_t>(mem[offset] | mask)
                        : static_cast<std::uint8_t>(mem[offset] & ~mask);
}
```
- `1u << bit`: a 1 moved left `bit` places, so bit 3 gives `0b0000'1000`.
- `| mask` sets that bit; `& ~mask` clears it (`~` flips every bit of the mask).

The casts exist because of **integer promotion**: in C++, any arithmetic on types smaller than `int` first converts them to `int`. `mem[offset] | mask` is an `int`, not a `uint8_t`. Storing an `int` back into a byte could lose data, and `-Wconversion` warns about it. The `static_cast` says "yes, I know, only the low 8 bits matter".

`~mask` is the sneaky one: `mask` is promoted to the int `0x00000008`, and `~` gives `0xFFFFFFF7`. The `&` with the (also promoted) byte clears those high bits again, and the cast stores the low 8.

```cpp
std::uint16_t getWord(std::span<const std::uint8_t> mem, std::size_t offset) {
    return static_cast<std::uint16_t>((mem[offset] << 8) | mem[offset + 1]);
}
```
Big-endian (S7 order): the byte at the lower address is the **high** byte, so it's moved up 8 bits.

### Bounds checks that can't overflow
```cpp
if (byteOffset > mem.size() || width > mem.size() - byteOffset) {
    throw std::out_of_range(...);
}
```
Why not simply `byteOffset + width > mem.size()`? Because `std::size_t` is unsigned. If `byteOffset` were huge, `byteOffset + width` would **wrap around** to a small number and pass the check. Written this way, the subtraction only happens once we know `byteOffset <= mem.size()`, so it can't wrap.

---

## 3. Interfaces: `virtual`

```cpp
// src/core/program.hpp
class Program {
public:
    virtual ~Program() = default;
    virtual void execute(ProcessImage& image, const CycleContext& context) = 0;
};
```

```cpp
class BlinkProgram : public Program {                            // src/demo/blink_program.hpp
    void execute(ProcessImage& image, const CycleContext& context) override;
};
class StProgram : public Program {                               // src/st/st_program.hpp
    void execute(ProcessImage& image, const CycleContext& context) override;
};
```

`ScanCycle` stores a `Program&` and calls `program_.execute(...)`. Which `execute` runs is decided **at run time**, by the real type of the object. Behind the scenes, every object of a class with virtual functions carries a hidden pointer to its class's table of functions (the *vtable*), and the call goes through that table.

- **`= 0`:** "pure virtual", there's no body here. You can't create a plain `Program`, only subclasses that implement it.
- **`override`:** "this replaces a virtual function of the base". If the signature doesn't match exactly (a missing `const`, say), it's a compile error instead of a silent new function that's never called.
- **`virtual ~Program() = default;`:** `main` deletes the program through a `unique_ptr<Program>`. Without a virtual destructor, only `~Program` would run, never `~StProgram`, and the interpreter inside would leak. **Rule: a class meant to be used through a base pointer needs a virtual destructor.**

The same pattern appears three times in the project:

| Interface | Implementations | Called by |
|---|---|---|
| `Program` | `BlinkProgram`, `StProgram`, test fakes | `ScanCycle` |
| `IoDriver` | `ConsoleIo`, test fakes (`FakeIo`) | `ScanCycle` |
| `NativeBlock` | `TimerBlock<Ton>`, `CounterBlock<Ctu>`, … | `Interpreter` |

This is how the tests work without hardware: they pass a `FakeIo` that pretends to be the inputs (`tests/core/scan_cycle_test.cpp`). The same seam is where a real I/O driver for factory_sim would plug in.

---

## 4. Templates: the same code for different types

The C++ timers (`Ton`, `Tof`, `Tp` in `src/stdlib/timers.hpp`) know nothing about Structured Text. The interpreter wants a `NativeBlock` that reads inputs from a `std::vector<Value>` and writes outputs back. One small adapter class does that, written **once** for all three timers:

```cpp
// src/st/builtins.cpp
template <class Timer> class TimerBlock : public NativeBlock {
public:
    void execute(std::vector<Value>& v, TimePoint now) override {
        timer_.update(boolAt(v, 0), toDuration(intAt(v, 1)), now);   // IN, PT
        v[2] = timer_.q();                                            // Q
        v[3] = toMilliseconds(timer_.et());                           // ET
    }
private:
    Timer timer_;
};
```

`template <class Timer>` means: "this is a recipe; `Timer` is a blank to fill in". Writing `TimerBlock<Ton>` makes the compiler stamp out a real class with `Ton` in place of `Timer`; `TimerBlock<Tof>` stamps out another one. The only requirement is that whatever you put in has `update(bool, Duration, TimePoint)`, `q()` and `et()`. That's checked when the class is stamped out, at compile time.

**Templates and virtual functions together:** each stamped class is a normal subclass of `NativeBlock`. The template removes the copy-paste; `virtual` lets the interpreter call any of them through one pointer type.

### The factory
```cpp
std::unique_ptr<NativeBlock> createBuiltin(std::string_view name) {
    const auto upper = toUpper(name);
    if (upper == "TON") return std::make_unique<TimerBlock<Ton>>();
    if (upper == "CTU") return std::make_unique<CounterBlock<Ctu>>();
    ...
    return nullptr;
}
```
A **factory function** turns a name (known only at run time, read from the ST file) into an object of the right class. The caller gets a `unique_ptr<NativeBlock>` and never sees the concrete type.

### A function template: `compare`
```cpp
// src/st/interpreter.cpp
template <class T> bool compare(BinaryOperator op, T a, T b) {
    switch (op) {
    case BinaryOperator::Equal: return a == b;
    case BinaryOperator::Less: return a < b;
    ...
    }
}
...
if (type == DataType::Bool) return compare(binary.op, std::get<bool>(left), std::get<bool>(right));
if (type == DataType::Real) return compare(binary.op, toReal(left), toReal(right));
return compare(binary.op, toInteger(left), toInteger(right));
```
You don't write `compare<double>(...)`: the compiler **deduces** `T` from the arguments.

### Templates vs `std::function`
Both mean "some code to call", chosen in different ways:
- `ScanCycle::setIdleTask(std::function<void()> task)`: **any** callable with that signature, chosen at run time and stored in a member. It costs a little (possibly a heap allocation, and an indirect call).
- `parseWith(std::string_view source, Rule rule)` in `parser.cpp` is a template: the exact lambda type is known at compile time and can be inlined. Nothing is stored.

Store it for later: `std::function`. Call it right now: a template parameter.

---

## 5. Build once, on first use: function-local statics

```cpp
// src/st/builtins.cpp
const std::vector<Pou>& builtinFunctionBlocks() {
    static const std::vector<Pou> blocks = [] {
        auto parsed = parseFunctionBlocks(kInterfaces);
        if (!parsed) {
            throw std::logic_error("built-in function block interfaces don't parse: " + parsed.error().message);
        }
        for (auto& block : *parsed) {
            block.builtin = true;
        }
        return std::move(*parsed);
    }();
    return blocks;
}
```
- A **`static` local** is created the first time the function runs and lives until the program ends. Every later call returns the same object.
- Since C++11, that first initialisation is **thread-safe**: if two threads call it at once, one builds and the other waits.
- `[] { ... }()` is a lambda **called immediately**. It lets a multi-step initialisation still produce a `const` variable.

The standard blocks' interfaces are written in Structured Text (`kInterfaces`, a **raw string literal** `R"( ... )"`, where newlines and quotes need no escaping) and parsed by the project's own parser. So TON is checked by the analyzer exactly like a user's function block.

---

## Review questions

**1.** Why does `Program` have a virtual destructor? What would happen without it?
<details><summary>Answer</summary>

`main` owns the program as `unique_ptr<Program>` and deletes it through that base type. Without a virtual destructor, deleting a `StProgram` through a `Program*` only runs `~Program` (formally it's undefined behaviour), so the `Interpreter` inside and everything it owns would leak.
</details>

**2.** Why is there no `default:` in the switches over `Area` and `AccessSize`?
<details><summary>Answer</summary>

So the compiler warns (`-Wswitch`, part of `-Wall`) when a new enum value is added and a switch doesn't handle it. A `default:` would silently catch the new value and hide every place that needs updating.
</details>

**3.** `std::vector<std::uint8_t> v(16);` and `std::vector<std::uint8_t> v{16};`: what's in each?
<details><summary>Answer</summary>

Parentheses: 16 elements, all zero. Braces: one element with the value 16. Braces prefer the "list of elements" constructor.
</details>

**4.** In `setBit`, why is `static_cast<std::uint8_t>(mem[offset] | mask)` needed? Isn't a byte OR a byte still a byte?
<details><summary>Answer</summary>

Integer promotion: both bytes are converted to `int` before the `|`, so the result is an `int`. Storing it in a byte narrows it, which `-Wconversion` warns about. The cast says the narrowing is intended.
</details>

**5.** The check is `width > mem.size() - byteOffset`, not `byteOffset + width > mem.size()`. Why?
<details><summary>Answer</summary>

`std::size_t` is unsigned and wraps around on overflow. A huge `byteOffset` could make `byteOffset + width` wrap to a small number and pass the check. Testing `byteOffset > mem.size()` first and then subtracting can't wrap.
</details>

**6.** You write a new timer class `Tonr` (retentive on-delay) with `update(bool, Duration, TimePoint)`, `q()` and `et()`. What do you need to change to make it usable from ST?
<details><summary>Answer</summary>

Add its interface to `kInterfaces` (`FUNCTION_BLOCK TONR ... END_FUNCTION_BLOCK`) and one line in `createBuiltin`: `if (upper == "TONR") return std::make_unique<TimerBlock<Tonr>>();`. The `TimerBlock` template already adapts anything with that shape. (Plus tests, of course.)
</details>

**7.** `ScanCycle::setIdleTask` takes a `std::function<void()>`, but `parseWith` takes a template parameter `Rule`. Why the difference?
<details><summary>Answer</summary>

The idle task is **stored** in a member and called later, so it needs one fixed type that can hold any callable: `std::function`. `parseWith` calls its rule immediately and doesn't store it, so a template is simpler and lets the compiler inline the lambda.
</details>
