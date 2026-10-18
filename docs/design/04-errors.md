# 4. Three kinds of errors

"Error handling" in soft_plc is really three different problems, and each gets a different tool. Telling them apart is most of the design.

| Kind | Example | Who should hear about it | Tool |
|---|---|---|---|
| A **mistake in the user's program**, found when compiling | `cuont := 1;` (unknown variable) | the person writing the program, at the right line and column | `tl::expected<…, Diagnostic>` returned; exceptions used only inside |
| A **fault while the program runs** | division by zero, an index outside the bounds, the watchdog | the PLC: it must go to a safe state | `RuntimeError` thrown, caught once per cycle, STOP |
| A **bug in soft_plc itself** | the compiler meets an operator it doesn't know | the developer, loudly | `std::logic_error`, tests, sanitizers |

[C++ lesson 3](../cpp/03-errors-and-raii.md) shows the mechanics. This note is about why each kind gets the tool it gets.

---

## 1. Mistakes in the user's program: values, not exceptions

Compile errors are **expected**: someone typing a program makes mistakes all the time, and `ladder_editor` compiles on every keystroke. So the public functions return them as values:

```cpp
tl::expected<CompilationUnit, Diagnostic> compileProgram(std::string_view source);
```

A caller can't forget that compiling can fail: the result has to be unpacked. And a `Diagnostic` carries what a person needs: a message, and a line and column.

**Inside**, though, the lexer, parser and analyzer throw (`LexError`, `ParseError`, `AnalysisError`). They're recursive: an error is found three levels deep, inside an expression inside an argument inside a call. Returning `expected` from every one of those functions and checking it at every level would roughly double the parser's code. Throwing jumps straight back to the top, where `tokenize()`, `parse…()` and `analyze()` catch it and turn it into the `expected`. Nothing outside those files ever sees one of these exceptions. "Throw inside, expected outside" keeps both sides simple.

### Only the first error

soft_plc stops at the first error. TIA Portal lists them all. Reporting all of them needs **error recovery**: after an error, the parser has to guess where the next good statement starts, and the analyzer has to invent a type for the broken expression so it can go on without reporting a cascade of follow-on errors that aren't real. It's a lot of careful code for a learning project, and an editor that checks on every keystroke shows the next error a moment after you fix the first. It's listed as a difference in the guide's chapter 9.

---

## 2. Faults at run time: exceptions, and STOP

At run time, almost every operation could fail in principle (any division, any array index, any loop round could hit the watchdog), but in a correct program **none** of them do. That's exactly the case exceptions are built for:

- On the path where nothing goes wrong, an exception that's never thrown costs nothing: there's no flag to check after each operation.
- When something does go wrong, the fault is deep inside: an index in an expression in a function block called from a loop. Throwing `RuntimeError` unwinds all of it at once, to one place:

```cpp
void StProgram::execute(ProcessImage& image, const CycleContext& context) {
    try {
        ...
        interpreter_.runCycle(image, context.now);
    } catch (const RuntimeError& error) {
        stop(image, error);                       // STOP: outputs off, remember why
    } catch (const std::out_of_range& error) {
        stop(image, RuntimeError{error.what(), {}}); // an AT address outside its area
    }
}
```

With `expected`, every evaluation in both engines would return a result to check: slower in the common case, and much longer code, for something that happens only in a broken program.

### Why STOP, and not "carry on"

What happens after a fault is a **PLC decision**, not a C++ one. A real S7 goes to STOP on an access error (unless the program has an error OB to handle it), and in STOP the outputs go to a safe state. A machine whose program hit a fault shouldn't keep driving its motors on half-computed values. So soft_plc does the same: STOP, all outputs off, the reason kept with its line and column, and (with `--monitor`) the process stays up so you can see why.

### What isn't a fault

Some things that are errors in other languages are **defined behaviour** on a PLC, and soft_plc follows the PLC:
- **Integer overflow wraps**: 32767 + 1 is −32768 in an INT, as on an S7. The arithmetic is done in 64 bits and then wrapped explicitly (`Wrap16`), which also avoids C++'s undefined behaviour for signed overflow.
- **Narrowing conversions don't fail**: `REAL_TO_INT` limits to the INT range, `DINT_TO_INT` keeps the low 16 bits.

Choosing which is which is part of the language design. A fault stops a machine, so only things that really mean the program is broken (an index outside an array can't mean anything) are faults.

---

## 3. Bugs in soft_plc: fail loudly

Some situations can only happen if soft_plc itself is wrong: the compiler meeting an operator the analyzer should have rejected, the standard blocks' interface text failing to parse. These throw `std::logic_error`, which nothing catches on purpose: the program ends with a clear message. Hiding a bug as a "user error" would make it much harder to find.

They're found long before a user meets them, by the things in [C++ lesson 7](../cpp/07-testing.md): the test suite on both engines, the differential test, and the Debug build's AddressSanitizer and UndefinedBehaviorSanitizer, which turn silent memory and arithmetic bugs into immediate, located crashes.

---

## 4. Across a process boundary: errors as text

The monitor protocol can't throw an exception into another process. Every reply starts with `OK` or `ERR`, and an error is a message a person can read (`ERR unknown variable 'cuont'`, `ERR line 2, column 5: …`). The client decides what to do with it: `ladder_editor` shows it in its header. It's the same idea as `expected`, in text.

---

## Review questions

**1.** Why does the parser throw internally but return `tl::expected` to its callers?
<details><summary>Answer</summary>

Inside, errors are found deep in recursive functions; throwing gets back to the top in one step without checking a result at every level. Outside, a compile error is an ordinary outcome (people make typos), so callers get a value they must unpack and can't ignore. The exception never leaves the parser's file.
</details>

**2.** Why are run-time faults exceptions, when compile errors aren't (from the caller's side)?
<details><summary>Answer</summary>

Faults are rare (only in broken programs) and happen deep inside the evaluation; an exception costs nothing until it's thrown and unwinds everything at once to `StProgram::execute`. Returning `expected` from every evaluation would slow down every correct program and double the code. Compile errors, by contrast, are common and are a normal result for the caller to handle.
</details>

**3.** `x := 32767; x := x + 1;` on an INT: fault or not, and why?
<details><summary>Answer</summary>

Not a fault: it wraps to −32768, as on an S7. Overflow is defined behaviour on a PLC. soft_plc computes in 64 bits and wraps explicitly, which also keeps it clear of C++'s undefined behaviour for signed overflow.
</details>

**4.** The compiler meets an operator it doesn't know and throws `std::logic_error`. Why not turn it into a compile error for the user?
<details><summary>Answer</summary>

It can only happen if soft_plc has a bug (the analyzer should have rejected anything the compiler can't handle). Reporting it as the user's mistake would send them looking for an error that isn't theirs, and would hide the bug. Failing loudly gets it fixed.
</details>
