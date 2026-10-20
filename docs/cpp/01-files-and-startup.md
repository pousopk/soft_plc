# C++ 1: Files, the build, and how `main()` starts the PLC

This lesson follows what happens when you type `./build/soft_plc --s7 examples/s7_demo.st`: which files are involved, how they're compiled together, and which object owns which.

---

## 1. Headers and source files

Every part of the code comes as a pair:

| File | Contains | Who reads it |
|---|---|---|
| `src/core/process_image.hpp` | The **declaration**: the class, its members, and a comment for each function | Everyone who uses `ProcessImage` |
| `src/core/process_image.cpp` | The **definitions**: the bodies of the functions | Only the compiler, once |

When `main.cpp` writes `#include "core/process_image.hpp"`, the preprocessor pastes that file's text in place. So `main.cpp` knows the class's *shape* (its members, its function signatures) but not the code inside the functions. The **linker** joins the pieces at the end.

Why split them?
- **Build speed:** change the inside of `readWord()` and only `process_image.cpp` is compiled again. Change the header and every file that includes it is compiled again.
- **Hiding details:** the header is the public face. Helpers like `getWord()` and `checkRange()` live only in the `.cpp`.

### `#pragma once`
Every header starts with it. If a header gets included twice into the same `.cpp` (directly, and through another header), the second copy is skipped. Without it, the class would be defined twice, which is an error.

### Small functions in headers
Some functions are written directly in the header:

```cpp
// src/core/scan_cycle.hpp
const CycleStats& stats() const { return stats_; }
```

A function defined inside the class body is automatically `inline`: it may appear in many `.cpp` files without a "defined twice" error, and the compiler can paste its body where it's called. That's good for tiny getters. Free functions in headers need the keyword written out, like `inline std::string toUpper(...)` in `src/util/text.hpp`.

---

## 2. Namespaces

```cpp
namespace softplc { ... }            // the whole project
namespace softplc::st { ... }        // Structured Text (C++17 nested syntax)
namespace softplc::monitor { ... }
namespace softplc::s7 { ... }
namespace softplc::st::runtime { ... }
```

A namespace is a surname for names. `softplc::st::Value` can't clash with something called `Value` in Snap7 or ImGui.

### The anonymous namespace
Almost every `.cpp` has one:

```cpp
// src/core/process_image.cpp
namespace {
void checkBit(unsigned bit) { ... }
std::uint16_t getWord(std::span<const std::uint8_t> mem, std::size_t offset) { ... }
}
```

A namespace without a name means **"only visible in this file"**. Two `.cpp` files can both have a helper called `findVariable` (the analyzer and the interpreter do), and the linker never confuses them. In C you'd write `static` for the same thing.

Rule of thumb in this codebase: if a function isn't in the header, it's in an anonymous namespace.

---

## 3. CMake: one library, many programs

**Code:** `CMakeLists.txt`

```cmake
add_library(soft_plc_core STATIC
  src/core/address.cpp
  src/core/process_image.cpp
  ...
)
add_executable(soft_plc   src/main.cpp src/demo/blink_program.cpp src/demo/console_io.cpp)
add_executable(st_parse   tools/st_parse.cpp)
add_executable(st_bench   tools/st_bench.cpp)
add_executable(soft_plc_monitor tools/monitor_gui/...)
add_executable(soft_plc_tests   tests/...)
```

Almost all the code goes into a **static library**, `soft_plc_core`: a `.a` file, an archive of compiled code. Each program is just a small `main()` plus that library. The tests link the same library, so **the tests check exactly the code the PLC runs**, compiled once.

```cmake
target_link_libraries(soft_plc_core
  PUBLIC  soft_plc_options tl::expected Threads::Threads
  PRIVATE snap7_static
  PRIVATE fmt::fmt
)
```

- **PUBLIC:** whoever uses `soft_plc_core` also needs these. `tl::expected` appears in our headers, so everyone who includes them needs its include path too.
- **PRIVATE:** only `soft_plc_core`'s own `.cpp` files use it. Snap7 is hidden inside `s7_server.cpp`. No header includes `snap7.h`: `s7_server.hpp` stores Snap7's handle as a plain `std::uintptr_t` (which is what Snap7's `S7Object` really is) precisely so it doesn't have to.

`soft_plc_options` is an `INTERFACE` library: it has no code, only settings (warnings, sanitizers). Linking it is how a target "inherits" those settings.

**FetchContent** downloads {fmt}, tl::expected, doctest, Snap7, ImGui and ImPlot at configure time, at a fixed version (`GIT_TAG 11.1.4`). There's nothing to install by hand, and everyone gets the same versions.

---

## 4. `main()`, step by step

**Code:** `src/main.cpp`

### 4.1 Reading the options
```cpp
struct Options {
    std::string programPath; // empty: start with an empty program
    std::uint16_t monitorPort = kDefaultMonitorPort;
    std::optional<std::uint16_t> s7Port;
    std::map<std::uint16_t, std::size_t> dataBlocks; // --db N:SIZE
    softplc::st::Engine engine = softplc::st::Engine::Bytecode;
};

std::optional<Options> parseOptions(int argc, char** argv);
```

- **`std::optional<T>`** holds a `T` or nothing. `s7Port` empty means "no `--s7`". That's clearer than a magic value like port `0` or `-1`. Monitoring is always on, so `monitorPort` is a plain number with a default.
- `parseOptions` itself returns `std::optional<Options>`: nothing means "bad command line, show the usage".
- **Default member initialisers** (`engine = Engine::Bytecode`) mean an `Options{}` is valid straight away.

```cpp
if (auto s7 = portOption(arg, "--s7", kDefaultS7Port, matched, valid); matched) {
```
This is an **if with an initialiser** (C++17): `s7` exists only inside the `if`/`else`. It keeps a temporary close to where it's used.

### 4.2 Stopping cleanly with Ctrl+C
```cpp
std::atomic<bool> stopRequested{false};
extern "C" void onSignal(int) { stopRequested = true; }
...
std::signal(SIGINT, onSignal);
```
A signal handler may run at any moment, in the middle of anything. About the only safe thing it can do is set a flag. The scan loop checks `stopRequested` once per cycle. `std::atomic` makes the write and the read safe across that interruption (more in lesson C6).

### 4.3 Compiling the ST program
```cpp
auto compiled = softplc::st::compileProgram(source);
if (!compiled) {
    fmt::print(stderr, "error: {}\n", softplc::st::formatDiagnostic(compiled.error(), source));
    return 1;
}
```
`compileProgram` returns `tl::expected<CompilationUnit, Diagnostic>`: either the checked program or the first error with its line and column. See lesson C3. Without a program file, `source` is `st::kEmptyProgram` (`PROGRAM Main END_PROGRAM`), so there's always an ST program for a download to replace.

### 4.4 Creating the objects

```cpp
softplc::ProcessImage image(16, 16, 256);
softplc::ConsoleIo io(softplc::Clock::now());
softplc::st::StProgram program(std::move(*compiled), onStop, 1'000'000, options->engine);
softplc::ScanCycle plc(image, io, program, 10ms);
std::unique_ptr<softplc::monitor::MonitorServer> server;
softplc::monitor::MonitorService service(program, image, plc);
std::unique_ptr<softplc::s7::S7Server> s7;
```

This is the most important picture in the program: **who owns what**.

```
main() owns (local variables, destroyed in reverse order)
 ├── image            ProcessImage          the memory
 ├── io               ConsoleIo
 ├── program          StProgram             └─ Interpreter → instances, bytecode
 ├── plc              ScanCycle             holds references: image&, io&, program&
 ├── server           unique_ptr<MonitorServer>
 ├── service          MonitorService        holds references: program&, image&, plc&
 └── s7               unique_ptr<S7Server>  (null without --s7) holds a reference to image
```

Two ways of "having" another object:
- **Owning:** a value member, or a `std::unique_ptr`. When the owner dies, the owned object dies too.
- **Referring:** a reference (`ProcessImage& image_`) or a span or raw pointer. The object lives somewhere else; you just use it. `ScanCycle` doesn't own the image; it borrows it.

The rule that makes borrowing safe: **the borrower must die before the owner.** C++ destroys local variables in the **reverse order** they were created: `s7` first, then `service`, `server`, `plc`, `program`, `io`, and `image` last. Everything that points into `image` is gone before `image` is. Declaring `image` after `plc` would be a bug, because `plc` would hold a reference to an object that was destroyed first (a "dangling reference").

### 4.5 Why `unique_ptr` for `s7` and `server`?
`s7` should exist only with `--s7`. A `std::unique_ptr` can be empty (`nullptr`), so "create it only if needed" is natural:

```cpp
if (options->s7Port) {
    s7 = std::make_unique<softplc::s7::S7Server>(image);
}
```

`std::optional<S7Server>` would also work here. `unique_ptr` was chosen because `S7Server` can't be copied or moved (it owns the Snap7 server and the buffers Snap7 points into), and a heap object never moves.

`server` always exists, but its constructor throws when the port is taken, and the error must be caught to print a helpful message. An object declared inside `try { }` dies at its `}`, so it's declared outside, empty, and created inside:

```cpp
std::unique_ptr<softplc::monitor::MonitorServer> server;
try {
    server = std::make_unique<softplc::monitor::MonitorServer>(options->monitorPort);
} catch (const std::system_error& error) {
    ...
    return 1;
}
```

### 4.6 Wiring it together with a lambda
```cpp
plc.setIdleTask([&] {
    server->processPending([&](std::string_view request) { return service.handle(request); });
    if (s7) {
        s7->synchronize();
        s7->setRunning(!program.stopped());
    }
});
plc.run(stopRequested);
```

The `ScanCycle` knows nothing about monitoring or S7. It just calls "whatever function I was given" between cycles. The lambda captures `[&]`, **everything by reference**, so it uses `main`'s own `server`, `s7`, `service` and `program`. That's safe because the lambda never outlives `main`'s variables: `plc.run()` returns before any of them are destroyed.

### 4.7 The end
After Ctrl+C, `run()` returns, the statistics and variables are printed, and `main` returns. Then the destructors run in reverse order: the S7 server stops its threads, the monitor server joins its threads, and so on. **No `delete` and no "cleanup" function is written anywhere.** The destructors are the cleanup. That's RAII, lesson C3.

---

## 5. `const`, everywhere

You'll see `const` a lot. Three places:

```cpp
const auto& stats = plc.stats();                  // 1. a variable I won't change
void writeOutputs(const ProcessImage& image)      // 2. a parameter the function won't change
Duration cycleTime() const { return cycleTime_; } // 3. a member function that doesn't change the object
```

(3) is the interesting one: a `const` member function can be called on a `const` object, and the compiler refuses if it tries to change a member. `IoDriver::writeOutputs` receives a `const ProcessImage&`: the driver can read the outputs but can't write to the image. The type system enforces "outputs flow out only".

---

## Review questions

**1.** You change one line inside `ProcessImage::readWord()` in `process_image.cpp`. Which files get compiled again? And if you add a member to the class in `process_image.hpp`?
<details><summary>Answer</summary>

Changing the `.cpp`: only `process_image.cpp` is compiled again, then everything is linked. Changing the header: every `.cpp` that includes it, directly or through another header, is compiled again. That's almost the whole project, since `ProcessImage` is used everywhere.
</details>

**2.** Why do `src/st/analyzer.cpp` and `src/st/interpreter.cpp` both have a function `findVariable` without a linker error?
<details><summary>Answer</summary>

Each one is inside an anonymous namespace (`namespace { ... }`), so it's visible only inside its own file. The linker sees them as two different functions.
</details>

**3.** In `main()`, what would go wrong if `ProcessImage image(...)` were declared *after* `ScanCycle plc(...)`?
<details><summary>Answer</summary>

It wouldn't compile as written, because `plc` needs `image` to exist when it's constructed. But in general the problem is lifetime: locals are destroyed in reverse order, so whatever is declared later dies first. The borrower (`plc`, which keeps a reference to `image`) must be declared *after* the owner (`image`), so that it dies *before* it.
</details>

**4.** `IoDriver::writeOutputs` takes `const ProcessImage&`, but `readInputs` takes `ProcessImage&` (no `const`). Why the difference?
<details><summary>Answer</summary>

`readInputs` must write the input area, so it needs a modifiable image. `writeOutputs` only needs to read the output area; the `const` makes the compiler stop it from changing anything, by accident or on purpose.
</details>

**5.** Why is `fmt::fmt` linked `PRIVATE` to `soft_plc_core`, but `tl::expected` `PUBLIC`?
<details><summary>Answer</summary>

Our headers mention `tl::expected` (functions return it), so anyone including those headers needs tl::expected's include path: PUBLIC. {fmt} is only used inside `.cpp` files, so users of the library don't need it: PRIVATE. (The executables use {fmt} themselves, so they link it on their own.)
</details>

**6.** The idle-task lambda captures `[&]`. When would capturing by reference be dangerous?
<details><summary>Answer</summary>

When the lambda is called after the variables it refers to are gone. For example, if it were stored somewhere that lives longer than `main`'s locals, or handed to a thread that keeps running after the function returns. Here `plc.run()` finishes before any of those variables are destroyed, so it's safe.
</details>
