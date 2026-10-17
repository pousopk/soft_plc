# C++ 7: How the tests are built

soft_plc has about 160 test cases, run twice (once per engine) under AddressSanitizer and UBSan. This lesson shows the C++ techniques that make code testable and the tests short.

**Code:** `tests/`, mirroring `src/` (`tests/core/scan_cycle_test.cpp` tests `src/core/scan_cycle.cpp`, and so on)

---

## 1. doctest in one minute

```cpp
// tests/test_main.cpp — the whole file
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
```
That macro makes doctest supply `main()`. Every other test file just includes the header and writes test cases:

```cpp
TEST_CASE("runOnce reads inputs, executes the program, then writes outputs") {
    ...
    CHECK(calls == std::vector<std::string>{"read", "execute", "write"});
    CHECK(io.lastOutput);
}
```

| Macro | On failure |
|---|---|
| `CHECK(x)` | reports and **continues** the test |
| `REQUIRE(x)` | reports and **stops** this test case (use it when the next lines would crash, e.g. before `*result`) |
| `CHECK_FALSE`, `REQUIRE_FALSE` | the same, expecting false |
| `CHECK_THROWS_AS(expr, Type)` | expects an exception of that type |
| `CAPTURE(x)` | prints `x` if a later check in this scope fails |
| `FAIL("message" << value)` | fails right away |

`TEST_CASE` is a macro that defines a function and registers it in a global list before `main` runs. That's why there's no list of tests to maintain: add a `TEST_CASE` anywhere and it runs.

`ctest` runs the test executable (`ctest --test-dir build`). Run it directly for more control: `./build/tests/soft_plc_tests -tc="*TON*"` runs only test cases whose name contains "TON".

---

## 2. Fakes: testing through the interfaces

The scan cycle depends on an `IoDriver` and a `Program` (lesson C2). The tests pass **fakes**, small classes made for the test that record what happened:

```cpp
// tests/core/scan_cycle_test.cpp
class FakeIo : public IoDriver {
public:
    explicit FakeIo(std::vector<std::string>& calls) : calls_(calls) {}

    void readInputs(ProcessImage& image) override {
        calls_.push_back("read");
        image.writeBit(Area::Input, 0, 0, inputValue);     // pretend a sensor is on
    }
    void writeOutputs(const ProcessImage& image) override {
        calls_.push_back("write");
        lastOutput = image.readBit(Area::Output, 0, 0);    // capture what would go to the real output
    }

    bool inputValue = false;
    bool lastOutput = false;
private:
    std::vector<std::string>& calls_;
};
```

`FakeIo` and `CopyProgram` share one `calls` vector, so the test can check the **order** across both: `read`, `execute`, `write`. No hardware, no threads, no sleeping. This works only because `ScanCycle` takes `IoDriver&` instead of creating a specific driver itself. **Code that receives its collaborators (dependency injection) can be tested with fakes; code that creates them can't.**

---

## 3. Controlling time

```cpp
// ScanCycle
void runOnce(TimePoint now);                              // one cycle, at the time I give you
void run(const std::atomic<bool>& stopRequested);         // the real loop, with the real clock
```

`runOnce` takes the time as a **parameter** instead of reading the clock. The timer tests use made-up timestamps:

```cpp
const auto t0 = TimePoint{} + 1s;
plc.runOnce(t0);
plc.runOnce(t0 + 10ms);
plc.runOnce(t0 + 20ms);
```

`TimePoint{}` is the clock's zero point; adding durations gives any moment you like. A test of "TON switches on after 5 seconds" runs in microseconds and gives the exact same result every time. The test helper `Runner` in `interpreter_test.cpp` advances its own `now_` by 10 ms per cycle, exactly like the real scan cycle.

Only a few tests use the real clock (`run keeps the cycle time and stops when asked`), and they check generous bounds like `elapsed >= 20ms`, never exact values, because a busy computer can always be late.

---

## 4. Helpers that make tests read like the spec

```cpp
// tests/st/analyzer_test.cpp
std::string program(std::string_view body) {             // wrap a body in a PROGRAM with standard variables
    return std::string("PROGRAM Test\nVAR\n    flag, other : BOOL;\n ...") + std::string(body) + "\nEND_PROGRAM\n";
}
std::string errorOf(const std::string& source) {         // "" if fine, else the error message
    auto result = compileProgram(source);
    return result ? std::string() : result.error().message;
}
```

so a test is one line of input and one line of expectation. And in `interpreter_test.cpp`, a `Runner` that compiles a program, runs N cycles and reads variables by name:

```cpp
TEST_CASE("Variables keep their values between cycles") {
    Runner runner("PROGRAM Counter VAR count : INT := 100; END_VAR count := count + 1; END_PROGRAM");
    CHECK(runner.integer("count") == 100); // the initial value, before the first cycle
    runner.cycles(5);
    CHECK(runner.integer("count") == 105);
}
```

`cycles()` returns `Runner&` (`return *this;`), so calls can also be chained: `runner.cycles(5).integer("count")`.

---

## 5. Table-driven tests

When the same check applies to many inputs, write the inputs as a table:

```cpp
// tests/st/lexer_test.cpp
TEST_CASE("Lexical errors report what went wrong and where") {
    struct Case {
        const char* source;
        const char* messagePart;
        SourceLocation location;
    };
    const Case cases[] = {
        {"a := $;",      "unexpected character '$'", {1, 6}},
        {"x AT %IX0.9",  "bit number must be 0-7",   {1, 6}},
        {"x := 16#;",    "needs base-16 digits",     {1, 6}},
        ...
    };
    for (const auto& c : cases) {
        CAPTURE(c.source);
        auto result = tokenize(c.source);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find(c.messagePart) != std::string::npos);
        CHECK(result.error().location == c.location);
    }
}
```

- A **local struct** defined inside the test, just for this table.
- **Aggregate initialisation**: `{"a := $;", "...", {1, 6}}` fills the fields in order; the inner `{1, 6}` builds a `SourceLocation`.
- `CAPTURE(c.source)` makes a failure say *which row* failed.
- Adding a case is one line. Checking `message.find(part)` instead of the whole message keeps tests from breaking when a message is reworded.

---

## 6. Testing the tree through its text form

Comparing syntax trees node by node would be long and fragile. Instead, the parser tests compare **printed** trees:

```cpp
// tests/st/parser_test.cpp
std::string grouped(std::string_view source) {
    auto expression = parseExpression(source);
    if (!expression) {
        FAIL("parse error: " << expression.error().message);
    }
    return formatExpression(**expression);   // * for the expected, * again for the unique_ptr
}
...
CHECK(grouped("a + b * c") == "(a + (b * c))");
CHECK(grouped("a * b + c") == "((a * b) + c)");
```

`formatExpression` puts parentheses around every operation, so precedence and associativity are visible in one string. When it fails, doctest prints both strings and the difference is obvious.

The same trick is in the differential test, which compares values as text (`formatValue(*a, type) == formatValue(*b, type)`) so a mismatch reads `T#1s200ms` vs `T#1s190ms` instead of two raw numbers.

---

## 7. One suite, two engines: an environment variable

```cpp
// tests/support/engine.hpp
inline softplc::st::Engine testEngine() {
    const char* value = std::getenv("SOFT_PLC_TEST_ENGINE");
    return value != nullptr && std::string_view(value) == "tree" ? Engine::TreeWalker : Engine::Bytecode;
}
```
```cmake
# tests/CMakeLists.txt
add_test(NAME soft_plc_tests_bytecode COMMAND soft_plc_tests)
set_tests_properties(soft_plc_tests_bytecode PROPERTIES ENVIRONMENT "SOFT_PLC_TEST_ENGINE=bytecode")
add_test(NAME soft_plc_tests_tree COMMAND soft_plc_tests)
set_tests_properties(soft_plc_tests_tree PROPERTIES ENVIRONMENT "SOFT_PLC_TEST_ENGINE=tree")
```
Every test that runs ST code creates its interpreter with `testEngine()`. `ctest` runs the same binary twice with a different environment, so **every behaviour test checks both engines** without writing anything twice.

`std::getenv` returns a C string pointer, or `nullptr` if the variable isn't set, which must be checked before comparing.

---

## 8. Differential testing with random inputs

**Code:** `tests/st/engine_equivalence_test.cpp`

```cpp
std::mt19937 random(12345); // fixed seed: the same "random" inputs every run
for (int cycle = 0; cycle < cycles; ++cycle) {
    CAPTURE(cycle);
    for (std::size_t i = 0; i < 4; ++i) {
        const auto byte = static_cast<std::uint8_t>(random());
        tree.image.writeByte(Area::Input, i, byte);
        bytecode.image.writeByte(Area::Input, i, byte);
    }
    tree.cycle(now);
    bytecode.cycle(now);
    REQUIRE(tree.error == bytecode.error);
    ... every variable and every byte of memory must be equal ...
}
```

Two independent implementations of the same thing run side by side on the same random inputs; any difference is a bug in one of them. The tests don't need to know the right answer, only that both agree.

`std::mt19937` is the standard Mersenne Twister generator. With a **fixed seed** the "random" sequence is the same on every run, so a failure can always be reproduced; `CAPTURE(cycle)` tells you exactly where.

---

## 9. Sanitizers: tests that find bugs you didn't test for

The Debug build compiles everything with `-fsanitize=address,undefined` (see `CMakeLists.txt`). While the tests run:
- **AddressSanitizer** catches reading or writing outside an array, use after free, and memory leaks.
- **UBSan** catches signed overflow, invalid shifts, misaligned access and other undefined behaviour.

A test that "passes" but touches freed memory still fails under ASan, with a stack trace showing where. This is why the runtime's careful unsigned arithmetic (lesson C5) can be trusted: a slip would show up as a UBSan report in the INT-overflow tests.

---

## Review questions

**1.** What's the difference between `CHECK` and `REQUIRE`? When must you use `REQUIRE`?
<details><summary>Answer</summary>

`CHECK` records a failure and continues; `REQUIRE` records it and stops the test case. Use `REQUIRE` when the following lines depend on it: e.g. `REQUIRE(result.has_value())` before `*result`, which would otherwise be undefined behaviour.
</details>

**2.** How can `scan_cycle_test.cpp` test the cycle without any I/O hardware?
<details><summary>Answer</summary>

`ScanCycle` takes an `IoDriver&` (dependency injection), so the test passes `FakeIo`, a subclass that simulates an input and records the output and the order of calls.
</details>

**3.** Why does `ScanCycle::runOnce` take `TimePoint now` instead of calling `Clock::now()` itself?
<details><summary>Answer</summary>

So tests can choose the time. Timer tests can then jump seconds ahead instantly and get exact, repeatable results instead of sleeping and depending on how busy the machine is.
</details>

**4.** In a table-driven test, what does `CAPTURE(c.source)` add?
<details><summary>Answer</summary>

If a check in that loop iteration fails, doctest prints the value of `c.source`, so you know which row of the table failed.
</details>

**5.** How does the same test binary test both engines?
<details><summary>Answer</summary>

Tests create interpreters with `testEngine()`, which reads the `SOFT_PLC_TEST_ENGINE` environment variable. CMake registers the binary twice with `ctest`, once with `tree` and once with `bytecode`.
</details>

**6.** The differential test uses random inputs. Why is the seed fixed (`12345`)?
<details><summary>Answer</summary>

So every run uses the same sequence. If it finds a difference, rerunning reproduces it exactly, and `CAPTURE(cycle)` says at which cycle. A random seed could find a bug once and never again.
</details>

**7.** A test passes on your machine but fails under AddressSanitizer with "heap-use-after-free". Is the test wrong?
<details><summary>Answer</summary>

No, the code is. It read memory that had been freed; without ASan it just happened to still contain the old values. Memory bugs often "work" until they don't, which is why the tests run under sanitizers.
</details>
