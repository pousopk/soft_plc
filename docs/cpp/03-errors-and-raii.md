# C++ 3: Errors and cleanup (exceptions, `expected`, RAII)

Things go wrong in a PLC all the time: a typo in the ST file, a division by zero while running, a client disconnecting. This lesson shows the three ways the code reports problems, and the one way it cleans up after them.

---

## 1. Three tools for "it didn't work"

| Tool | Means | Used for | Example |
|---|---|---|---|
| `std::optional<T>` | "a T, or nothing" | Not finding something is normal and needs no explanation | `readFile()`, `Interpreter::valueOf(path)`, `parseOptions()` |
| `tl::expected<T, E>` | "a T, or an error E explaining why" | Failure is normal and the caller must say why | `parseAddress()`, `compileProgram()`, `MonitorClient::connect()` |
| exceptions (`throw`) | "this can't continue; unwind to someone who can handle it" | Errors deep inside, far from where they're handled | `RuntimeError` in the interpreter, `std::out_of_range` in `ProcessImage` |

`tl::expected` is a library version of C++23's `std::expected`. GCC 11 doesn't have it, so the project fetches it.

### Using `expected`
```cpp
// src/main.cpp
auto compiled = softplc::st::compileProgram(source);
if (!compiled) {                                     // false when it holds an error
    fmt::print(stderr, "error: {}\n", formatDiagnostic(compiled.error(), source));
    return 1;
}
auto st = std::make_unique<StProgram>(std::move(*compiled), ...);  // *compiled: the value
```
- `if (!x)` / `x.has_value()`: did it work?
- `*x` or `x->...`: the value (only if it worked!)
- `x.error()`: the error (only if it didn't)
- `x.value()`: the value, or **throws** if there isn't one. Handy in tests and tools where failure means "stop everything": `st::compileProgram(source).value()` in `st_bench.cpp`.

Returning one:
```cpp
// src/core/address.cpp
tl::unexpected<std::string> fail(std::string_view text, std::string_view reason) {
    return tl::unexpected(fmt::format("invalid address '{}': {}", text, reason));
}
...
if (rest.empty()) {
    return fail(text, "missing area (I, Q, M or DB)");   // the error
}
...
return address;                                          // the value
```
`tl::unexpected(...)` wraps an error so it can't be confused with a value. A plain `return address;` converts to "success".

For a function that has no result but can still fail: `tl::expected<void, Diagnostic> analyze(...)`. Success is `return {};`.

---

## 2. "Throw inside, `expected` outside"

The lexer, parser and analyzer all use the same trick:

```cpp
// src/st/parser.cpp
struct ParseError {
    Diagnostic diagnostic;
};

[[noreturn]] static void fail(const Token& at, std::string message) {
    throw ParseError{Diagnostic{std::move(message), at.location}};
}

const Token& expect(TokenKind kind, const std::string& what) {
    if (!check(kind)) {
        fail(peek(), fmt::format("expected {}, found {}", what, describe(peek())));
    }
    return advance();
}
```

and at the single public entry point:

```cpp
template <class Rule>
auto parseWith(std::string_view source, Rule rule) -> tl::expected<...> {
    ...
    try {
        Parser parser(std::move(*tokens));
        return rule(parser);
    } catch (const ParseError& error) {
        return tl::unexpected(error.diagnostic);
    }
}
```

**Why not `expected` everywhere?** The parser is deeply recursive: `ifStatement → statementList → statement → assignment → expression → orExpression → … → primary`. An error in `primary()` would have to be checked and passed up by **every** function in that chain, about 15 levels. With an exception, `fail()` jumps straight back to `parseWith` in one go. The functions in between stay clean and look like the grammar.

**Why not exceptions everywhere?** Because the caller (`main`, the tests, `st_parse`) should see clearly, in the function's type, that parsing can fail. `ParseError` never leaves `parser.cpp`; it's in an anonymous namespace, so nobody outside could even catch it.

### `[[noreturn]]`
Tells the compiler "this function never returns normally (it always throws)". Without it, code like this would warn:

```cpp
Statement statement() {
    switch (peek().kind) {
    case TokenKind::If: return ifStatement();
    ...
    default: fail(peek(), "expected a statement ...");
    }
}   // without [[noreturn]] on fail: "control reaches end of non-void function"
```

### Throwing your own types
`ParseError`, `LexError`, `AnalysisError` and `RuntimeError` are plain structs, not derived from `std::exception`. That's allowed: you can throw any type. The usual advice is to derive from `std::exception` so a generic `catch (const std::exception&)` catches them too. Here they're caught by their exact type, very close to where they're thrown, so it doesn't matter. `RuntimeError` travels further (interpreter → `StProgram`), and that's a place where deriving from `std::runtime_error` would be a reasonable improvement.

**Catch by `const&`:** `catch (const ParseError& error)`. Catching by value would copy the exception and, for class hierarchies, "slice" it into the base type.

---

## 3. Runtime errors → STOP

```cpp
// src/st/st_program.cpp
void StProgram::execute(ProcessImage& image, const CycleContext& context) {
    if (stopped()) {
        return;
    }
    try {
        applyForces(image);
        interpreter_.runCycle(image, context.now);
        applyForces(image);
    } catch (const RuntimeError& error) {
        stop(image, error);
    } catch (const std::out_of_range& error) {
        stop(image, RuntimeError{error.what(), {}});
    }
}
```

A division by zero is found deep inside the VM loop (`throw RuntimeError{"division by zero", where()}`). The exception unwinds all the way out of the interpreter, through any nested function block calls, to here, where it becomes **PLC STOP**: outputs off, error remembered, `onStop` called. This is the one place in the scan cycle that decides what an error means.

`std::out_of_range` comes from `ProcessImage` (an `AT %DB9.DBW0` when DB9 doesn't exist). On a Siemens PLC that's an "area length error", and it's also turned into STOP here.

---

## 4. RAII: the destructor does the cleanup

**R**esource **A**cquisition **I**s **I**nitialisation: get the resource in a constructor, release it in the destructor. A local object's destructor runs **whenever the scope ends**, whichever way it ends: the normal end, `return`, `break`, or an exception passing through. That last one is called **stack unwinding**: as an exception travels up, every local object in every function it leaves is destroyed in order.

That's the answer to the last quiz question:

```cpp
// src/s7/s7_server.cpp
void S7Server::synchronize() {
    for (const auto& area : areas_) {
        AreaLock lock(server_, area->code, area->index);   // constructor: Srv_LockArea
        mergeClientWrites(area->plc, area->shared, area->published);
    }                                                       // destructor: Srv_UnlockArea
}
```

If `mergeClientWrites` throws, `lock` is still destroyed during unwinding, so the area **is unlocked**. With a manual `Srv_LockArea(...); merge(...); Srv_UnlockArea(...);`, the exception would skip the unlock, and Snap7's threads would wait for that area forever.

### Every RAII class in soft_plc

| Class | Acquires | Releases | Where |
|---|---|---|---|
| `std::unique_ptr<T>` | a heap object | `delete` | everywhere |
| `std::vector`, `std::string`, `std::map` | heap memory | frees it | everywhere |
| `std::lock_guard` | a mutex | unlocks it | monitor server, poller |
| `AreaLock` | a Snap7 area lock | `Srv_UnlockArea` | `s7_server.cpp` |
| `ImageBinding` | points the interpreter at an image | sets the pointer back to `nullptr` | `interpreter.cpp` |
| `FrameGuard` | VM stack space | gives it back | `Interpreter::runBytecode` |
| `MonitorClient` | a socket | `close()` | `monitor_client.cpp` |
| `S7Server` | a Snap7 server | `Srv_Stop` + `Srv_Destroy` | `s7_server.cpp` |
| `std::jthread` | a thread | asks it to stop, then joins | `Poller` |

Search the project for `delete`, `free(`, `unlock(`: you'll find none in our code. All of it is in destructors.

### A tiny RAII class: `ImageBinding`
```cpp
// src/st/interpreter.cpp
class ImageBinding {
public:
    ImageBinding(ProcessImage*& slot, ProcessImage& image) : slot_(slot) { slot_ = &image; }
    ~ImageBinding() { slot_ = nullptr; }
    ImageBinding(const ImageBinding&) = delete;
    ImageBinding& operator=(const ImageBinding&) = delete;
private:
    ProcessImage*& slot_;      // a reference to a pointer: lets us change the interpreter's own pointer
};

void Interpreter::runCycle(ProcessImage& image, TimePoint now) {
    ImageBinding binding(image_, image);
    ...
    run(*root_);               // may throw RuntimeError
}
```
The interpreter needs the image only *during* a cycle. If a stale `image_` pointer stayed around after the cycle, a later bug could write to an image that no longer exists. The binding guarantees `image_` is `nullptr` outside a cycle, **even if the cycle ended with an exception**.

`ProcessImage*& slot` reads right to left: "a reference to a pointer to ProcessImage". The binding changes the interpreter's member `image_` itself, not a copy.

### An even tinier one: `FrameGuard`
```cpp
// Interpreter::runBytecode
struct FrameGuard {
    std::size_t& top;
    std::size_t saved;
    ~FrameGuard() { top = saved; }
} guard{vmTop_, vmTop_};
vmTop_ += frameSize;
```
A struct defined **inside a function**, used once. It remembers where the VM stack top was and puts it back on the way out. If a nested function block throws a division by zero, every frame on the way out restores its own `vmTop_`.

### Why copying is deleted
```cpp
AreaLock(const AreaLock&) = delete;
AreaLock& operator=(const AreaLock&) = delete;
```
If an `AreaLock` were copied, there would be two objects and **two destructors**, so the area would be unlocked twice. A resource owned by two objects is released twice. Deleting the copy operations turns that bug into a compile error. Every RAII class above either forbids copying or, like `unique_ptr` and `MonitorClient`, allows **moving** only (lesson C6).

---

## Review questions

**1.** (The quiz question.) If `mergeClientWrites` throws inside `synchronize()`, is the area unlocked? Why?
<details><summary>Answer</summary>

Yes. `lock` is a local object, and during stack unwinding the destructors of all locals in the scopes being left are run, so `~AreaLock` calls `Srv_UnlockArea`. That's the point of RAII: cleanup that can't be skipped.
</details>

**2.** `readFile()` returns `std::optional<std::string>`, but `parseAddress()` returns `tl::expected<Address, std::string>`. Why the difference?
<details><summary>Answer</summary>

For `readFile`, "it didn't work" needs no explanation for the caller (`main` prints "cannot open 'file'"). For `parseAddress`, the user needs to know *what* is wrong with the address ("bit number must be 0-7"), so the error carries a message.
</details>

**3.** Why does the parser throw `ParseError` internally instead of returning `tl::expected` from every function?
<details><summary>Answer</summary>

The parser is recursive and an error can be ~15 calls deep. With `expected`, every function in between would have to check and forward the error. One exception jumps straight to the single `catch` in `parseWith`, which turns it into an `expected` for callers.
</details>

**4.** What does `[[noreturn]]` on `fail()` do?
<details><summary>Answer</summary>

It tells the compiler that `fail()` never returns (it always throws). Then the compiler doesn't warn about "missing return" in functions whose last statement is a call to `fail()`, and it can optimise knowing that code after the call is never reached.
</details>

**5.** Why does `ImageBinding` set `image_` back to `nullptr` in a destructor, instead of `runCycle` doing it on its last line?
<details><summary>Answer</summary>

Because `run(*root_)` can throw (division by zero, watchdog). A last line would be skipped by the exception, leaving `image_` pointing at an image the interpreter no longer owns. The destructor runs on every exit.
</details>

**6.** What would go wrong if `AreaLock` could be copied?
<details><summary>Answer</summary>

Two objects would each think they own the same lock, and both destructors would call `Srv_UnlockArea`: one unlock too many. Deleting the copy constructor and copy assignment makes that a compile error.
</details>

**7.** Find a place in soft_plc where an error is turned from an exception into a value, and one where a value is turned into an exception.
<details><summary>Answer</summary>

Exception → value: `parseWith` (`ParseError` → `tl::unexpected`), `tokenize`, `analyze`, and `StProgram::execute` (`RuntimeError` → the stored `stopError_`). Value → exception: `.value()` on an `expected`, e.g. `compileProgram(source).value()` in `st_bench.cpp`, which throws if compiling failed. Also `builtinFunctionBlocks()`, which turns a parse error into a `std::logic_error`.
</details>
