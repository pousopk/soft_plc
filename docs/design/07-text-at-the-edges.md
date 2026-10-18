# 7. Text at the edges

Inside, soft_plc is all numbers: slots, leaves, bytecode. At its **edges**, where it meets people and other programs, it's text whenever it can be: the programs, the error messages, the monitoring protocol, the guide. That's a choice, and it has costs; this note explains why it's worth them.

---

## The monitoring protocol: one line of text

```
READ count t1.ET          →  OK 42 T#1s200ms
FORCE lamp TRUE           →  OK
WRITE count abc           →  ERR 'abc' is not an integer
```

A binary protocol (fixed-size messages, values as raw bytes) would be smaller and faster to parse. Text was chosen because:

- **You can use it by hand.** `nc 127.0.0.1 5020`, type `READ count`, read the answer. No client to write, no tool to install. During development this was how every monitoring feature was first tried, and the guide teaches it.
- **Any language can talk to it.** The GUI is C++, `ladder_editor`'s session is C++, the tests' scripts and the screenshots for these lessons used Python with a plain socket. A binary protocol needs the same struct layout and byte order on every side.
- **Values look like they do in programs.** `T#1s200ms`, `16#FF`, `TRUE`: the same parser that reads literals in ST reads them in `WRITE` (`parseValue`), and the same formatting writes them (`formatValue`). One definition of what a value looks like, everywhere.
- **The speed doesn't matter here.** Monitoring reads tens of values ten times a second. Parsing a line of text is microseconds; the network round trip is far more.

The protocol's one awkward case is `LOAD`, which carries a whole program on one line: line ends become `\n` and backslashes `\\`. It's a small escaping rule instead of a change of protocol (length-prefixed messages, or multi-line requests), so every existing client and the server's line reader stayed as they were.

Where soft_plc must talk **binary** (S7, with its fixed telegrams), it's because the other side is defined by Siemens; there it uses Snap7 rather than inventing its own.

---

## Errors a person can act on

A compile error carries a message and a location, and the command line prints it like a compiler does:

```
line 1, column 10: expected an expression, found '*'
  a := b + * c;
           ^
```

Three decisions behind that:

- **Every message names what was expected and what was found**, in the program's own words (`'+' needs numbers, but got BOOL and integer literal`), not the implementation's ("unexpected token 17").
- **Locations count from 1** (like an editor) and **columns count bytes** (like the lexer). `ladder_editor` relies on both to put its red underline in the right place, and has to step over UTF-8 characters itself (its lesson 6).
- **Run-time faults have locations too.** A division by zero says which line and column. The tree carries `SourceLocation`s on every node into run time, at a small memory cost, because "STOP: division by zero" without a line is almost useless on a big program.

The same messages appear in the command line, the monitor's `STATUS`, and `ladder_editor`'s underline and problem list, because they all come from the same `Diagnostic`.

---

## Documentation that can't drift

The programming guide (`docs/guide/`) is full of programs, and programs in documentation go stale: someone changes the language, the guide still shows the old syntax, and nobody notices.

`tests/st/guide_test.cpp` reads every Markdown file in the guide, takes every `` ```st `` block that holds a whole program, and compiles and runs it. Blocks that start with `(* error: … *)` must fail with exactly that message. So:

- every example in the guide is known to work, today;
- when the language changes, the guide's test fails until the guide is updated;
- a message changed in the analyzer shows up in the guide's error examples at once.

The online change section's example goes one step further: a separate test (`tests/st/online_change_test.cpp`) runs exactly that example and checks the numbers the text claims (count from 400, `kept=6`, the lamp at 20 s).

Writing docs as text that the build reads costs a small parser in a test. It buys documentation you can trust, which is the only kind worth reading.

---

## Programs are files

A soft_plc program is a `.st` file, given on the command line. There's no project database, no binary format, no IDE required: any editor works, `git diff` shows exactly what changed, and `examples/` is a folder of programs you can read in a browser. `ladder_editor` keeps that property: its projects are folders of readable files (KOP networks are saved in their one-line notation, ST blocks as `.st`), and **Export ST** writes the whole project as one `.st` file that soft_plc runs.

---

## Review questions

**1.** The monitor protocol is text. What would a binary protocol gain, and why wasn't it worth it here?
<details><summary>Answer</summary>

Smaller messages and faster parsing. But monitoring moves tens of values ten times a second, so parsing time is negligible next to the network round trip; and text can be used by hand with `nc`, from any language, with values written exactly as in ST.
</details>

**2.** Why do run-time faults carry a line and column, when that means keeping locations in the tree while the program runs?
<details><summary>Answer</summary>

Because "STOP: division by zero" doesn't tell you which of a hundred divisions failed. The memory cost (a location per node) is small and fixed; the time saved finding the fault is large.
</details>

**3.** The analyzer's message for an unknown variable changes. What fails, and why is that good?
<details><summary>Answer</summary>

The guide's test, if the guide has an error example with the old message (and the analyzer's own tests). It's good because the guide would otherwise keep showing a message soft_plc no longer prints; the failing test forces the guide to be updated in the same change.
</details>
