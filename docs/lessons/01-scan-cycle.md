# Lesson 1: Scan cycle and process image

Milestone 1 builds the skeleton of a PLC: memory, a fixed-time cycle, and one timer. The PLC program is still plain C++ (a blinker); Structured Text comes in milestone 2.

```
 ┌──────────────── every 10 ms ────────────────┐
 │ read inputs  →  execute program  →  write outputs │
 └──────────────────────────────────────────────┘
       IoDriver        Program            IoDriver
            ╲             │              ╱
             └──── ProcessImage (%I %Q %M) ┘
```

---

## Part 1: PLC concepts

### 1.1 The process image
**Code:** `src/core/process_image.hpp`

The program never reads sensors or writes actuators directly. At the start of each cycle, the I/O driver copies all inputs into the **input area** (`%I`). The program reads and writes only this memory, and at the end of the cycle the **output area** (`%Q`) is copied to the real outputs. **Markers** (`%M`) are internal memory that keeps its value between cycles.

**Why:** every line of the program sees the same input values for the whole cycle, even if a sensor changes halfway through. Without this, two lines checking the same sensor could disagree.

### 1.2 Addresses
**Code:** `src/core/address.hpp`, `parseAddress()`

| Address | Meaning |
|---|---|
| `%IX0.3` or `%I0.3` | Input, bit 3 of byte 0 |
| `%QB1` | Output byte 1 (8 bits) |
| `%MW10` | Marker word at byte 10 (16 bits: bytes 10 and 11) |
| `%MD20` | Marker double word at byte 20 (32 bits: bytes 20–23) |

### 1.3 Big-endian storage
**Code:** `ProcessImage::writeWord()`

A word has two bytes. Siemens S7 stores the **most significant byte first** (big-endian): `0x1234` in `%MW10` puts `0x12` in byte 10 and `0x34` in byte 11. A PC's own memory does the opposite (little-endian), so we build words byte by byte instead of copying memory. Matching S7 keeps us compatible with Snap7 later.

### 1.4 The scan cycle
**Code:** `src/core/scan_cycle.cpp`

`runOnce()` does one cycle: read inputs → execute → write outputs, and measures how long that took. `run()` repeats it at a fixed rhythm. If a cycle takes longer than the cycle time, it's an **overrun**: it's counted, and the next cycle starts immediately.

### 1.5 The TON timer
**Code:** `src/stdlib/ton.cpp`

An on-delay timer: Q switches on once IN has been true for the preset time (PT). ET (elapsed time) counts up to PT. When IN goes false, everything resets.

```
IN  ___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\______
Q   _________/‾‾‾‾‾‾‾‾‾\______
       |<-- PT -->|
```

Each `Ton` object is one timer instance with its own memory, like an instance DB in TIA Portal. `running_` remembers whether IN was already true on the previous call; that's how the **rising edge** (false → true, when timing starts) is detected.

### 1.6 Execution order matters
**Code:** `src/demo/blink_program.cpp`

Code runs top to bottom within a cycle. A value written by a later line is only seen by earlier lines on the **next** cycle. That's why the blinker toggles about every 510 ms instead of 500 ms: each timer reacts to the other one cycle (10 ms) late. Real PLCs behave exactly the same with rung order.

---

## Part 2: C++ concepts

### 2.1 Interfaces with virtual functions
**Code:** `src/core/program.hpp`, `src/core/io_driver.hpp`

```cpp
class Program {
public:
    virtual ~Program() = default;
    virtual void execute(ProcessImage& image, const CycleContext& context) = 0;
};
```

`= 0` makes the function **pure virtual**: `Program` can't be created on its own, only classes that implement `execute` (like `BlinkProgram`). `ScanCycle` only knows `Program&`, so any program works with it: today the C++ blinker, later the Structured Text interpreter. The **virtual destructor** makes sure the right destructor runs when an object is deleted through a `Program*`.

### 2.2 Time with `std::chrono`
**Code:** `src/core/plc_time.hpp`

- A **time point** is a moment (`TimePoint`); a **duration** is a length of time (`Duration`). Subtracting two time points gives a duration: `now - start_`.
- `steady_clock` never jumps backwards, even if someone changes the system clock. Timers need that.
- `using namespace std::chrono_literals;` lets you write `500ms`, `10s`.
- Durations of different units convert automatically when no precision is lost: `500ms` can be passed where nanoseconds are expected.

### 2.3 `sleep_until` vs `sleep_for`: avoiding drift
**Code:** `ScanCycle::run()`

If each cycle's work takes 2 ms:
- `sleep_for(10ms)` after each cycle → cycles start at 0, 12, 24, 36 ms… The error grows forever.
- `sleep_until(deadline)` with `deadline += 10ms` → cycles start at 0, 10, 20, 30 ms. It sleeps only what's left.

Like a metronome versus "wait 10 seconds after finishing each task."

### 2.4 Two ways to report errors
- **`tl::expected<Address, std::string>`** (`parseAddress`): holds *either* a result *or* an error. Use it when failure is a normal situation, such as a user typing a bad address. The caller must check:
  ```cpp
  auto result = parseAddress("%MW10");
  if (result) { use(*result); } else { print(result.error()); }
  ```
  (`std::expected` is the same thing in C++23.)
- **Exceptions** (`ProcessImage` throws `std::out_of_range`): for programming errors that shouldn't happen, like a real PLC stopping with an area error.

### 2.5 Compiler-generated comparison (C++20)
**Code:** `Address` in `src/core/address.hpp`

```cpp
bool operator==(const Address&) const = default;
```

The compiler writes `==` for you by comparing every member in order. In C++17 you'd write `area == other.area && size == other.size && …` by hand. Used in the tests: `CHECK(parsed == Address{Area::Input, AccessSize::Bit, 0, 3});`.

### 2.6 `enum class`
**Code:** `Area`, `AccessSize`

Unlike a plain `enum`, values must be written with their type (`Area::Input`), they don't convert silently to `int`, and two enums can both have a value called `Input` without clashing.

### 2.7 Views: `std::string_view` and `std::span`
- `std::string_view` (`parseAddress`): a read-only window into text that someone else owns. No copy. `remove_prefix(1)` moves the window forward one character, which makes it easy to parse step by step.
- `std::span` (`ProcessImage::bytes()`, C++20): the same idea for arrays. It gives an I/O driver direct access to an area's bytes without copying the vector.

### 2.8 Explicit conversions with `static_cast`
**Code:** `ProcessImage::writeWord()`

```cpp
mem[byteOffset] = static_cast<std::uint8_t>(value >> 8);
```

Putting a 16-bit value into an 8-bit byte loses bits. With `-Wconversion` enabled, the compiler warns about it unless you say explicitly that it's intended. The cast documents "yes, I mean to keep only the low 8 bits."

### 2.9 Code organisation
- **`namespace softplc`** groups all our names so they can't clash with other libraries.
- **Anonymous namespace** (`namespace { … }` in `.cpp` files): helpers visible only inside that file.
- **`const` and non-const overloads** (`ProcessImage::storage()`): the const version has the real code; the non-const one reuses it with `const_cast`. That's safe because the object itself isn't const when the non-const version is called.

### 2.10 Thread-safe stop flag
**Code:** `src/main.cpp`

Ctrl+C triggers a **signal handler**, which can interrupt the program at any moment. Inside it, you may only do very simple things. Setting a `std::atomic<bool>` is one of them. The scan cycle checks that flag at the start of every loop.

---

## Part 3: Testing

### 3.1 Fakes
**Code:** `tests/core/scan_cycle_test.cpp`

`FakeIo` and `CopyProgram` are tiny implementations of the interfaces that record what happened (`"read"`, `"execute"`, `"write"`). The test then checks the order. This works only because `ScanCycle` depends on interfaces, not on concrete classes (2.1).

### 3.2 Controlling time in tests (dependency injection)
**Code:** `tests/stdlib/ton_test.cpp`

`Ton::update()` receives `now` as a parameter instead of reading the clock itself. Tests can then pass made-up times (`t0 + 499ms`, `t0 + 500ms`):
- **Fast:** no real waiting.
- **Exact:** the real clock never wakes up precisely on time.
- **Repeatable:** same result on every run.

Handing a function what it needs, instead of letting it fetch it, is called **dependency injection**.

---

## Part 4: Build setup

**Code:** `CMakeLists.txt`

- **`FetchContent`** downloads dependencies ({fmt}, tl::expected, doctest) at configure time, so nothing has to be installed by hand.
- **`soft_plc_options`** is an `INTERFACE` library: it compiles nothing, it just carries settings (warnings, sanitizers) to every target that links it.
- **Strict warnings** (`-Wall -Wextra -Wconversion …`) catch mistakes early. Third-party headers are marked as *system* headers so their warnings don't flood our output.
- **Sanitizers** in Debug builds: AddressSanitizer catches invalid memory access; UBSan catches undefined behaviour such as signed overflow. They make the program slower, which is why the demo shows execution times of tens of microseconds.
- **A library + executables:** the code lives in `soft_plc_core`; the demo and the tests both link it, so nothing is compiled twice.

---

## Review questions

**1.** The cycle time is 10 ms, but the program takes 15 ms per cycle. What happens, and where would you see it?
<details><summary>Answer</summary>

Every cycle is an overrun. `run()` notices the deadline has passed, increments `stats_.overruns`, and starts the next cycle immediately without sleeping. The real cycle time becomes about 15 ms, and the statistics printed at the end show `Overruns:` equal to roughly the number of cycles.
</details>

**2.** Why does `run()` use `sleep_until(deadline)` instead of `sleep_for(cycleTime_)`?
<details><summary>Answer</summary>

`sleep_for` waits a full cycle time *after* the work, so every cycle is longer than intended and the error accumulates (drift). `sleep_until` waits until fixed points in time, so it sleeps only for what's left of the cycle and the rhythm stays exact. See 2.3.
</details>

**3.** Why do the TON tests use made-up timestamps instead of the real clock?
<details><summary>Answer</summary>

So they're fast (no waiting), exact (the real clock is never precisely on time) and repeatable. See 3.2.
</details>

**4.** In `BlinkProgram::execute`, what changes if `offTimer_` is updated before `onTimer_`?
<details><summary>Answer</summary>

`offTimer_` would see `onTimer_`'s output from the previous cycle instead of the current one, so it starts one cycle (10 ms) later and the output stays on slightly longer. Execution order within a cycle matters. See 1.6.
</details>

**5.** What does `bool operator==(const Address&) const = default;` do?
<details><summary>Answer</summary>

It asks the compiler to generate `==` by comparing every member of `Address` in order. It's a C++20 feature; in C++17 you write the comparison by hand. See 2.5.
</details>

**6.** When should a function return `tl::expected` and when should it throw?
<details><summary>Answer</summary>

Return `expected` when failure is a normal, expected situation the caller should handle (bad user input). Throw for programming errors that shouldn't happen (accessing memory outside an area). See 2.4.
</details>

**7.** `%MW10` holds `0x1234`. What's in byte 10 and byte 11?
<details><summary>Answer</summary>

Byte 10 holds `0x12` and byte 11 holds `0x34`: big-endian, most significant byte first, like Siemens S7. See 1.3.
</details>
