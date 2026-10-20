# soft_plc programming guide

How to write programs for soft_plc. The language is **Structured Text** (ST) from IEC 61131-3, the same family as SCL in TIA Portal, with a few Siemens touches such as data blocks (`%DB1.DBW2`) and the S7 memory layout.

If you know SCL, most of it will look familiar; [chapter 9](09-coming-from-tia-portal.md) lists the differences.

| # | Chapter | |
|---|---|---|
| 1 | [Your first program](01-first-program.md) | the shape of a program, running it, the scan cycle |
| 2 | [Data types and variables](02-data-types-and-variables.md) | BOOL, the integers, BYTE/WORD/DWORD, REAL, TIME, literals, conversions, initial values, `AT` |
| 3 | [Expressions](03-expressions.md) | operators, precedence, overflow, REAL precision, TIME arithmetic |
| 4 | [Statements](04-statements.md) | `IF`, `CASE`, `FOR`, `WHILE`, `REPEAT`, `EXIT`, `RETURN`, the watchdog |
| 5 | [Function blocks](05-function-blocks.md) | writing and calling FBs; TON, TOF, TP, CTU, CTD, R_TRIG, F_TRIG, SR, RS |
| 6 | [Functions](06-functions.md) | writing FUNCTIONs; ABS, SQRT, MIN, MAX, LIMIT, SEL, MUX, ROUND, the conversions |
| 7 | [Arrays and structs](07-arrays-and-structs.md) | `ARRAY`, `TYPE … STRUCT`, initial values, copying, function block interfaces |
| 8 | [Memory, S7 and going online](08-memory-s7-and-online.md) | the process image, data blocks, the S7 layout, monitoring, forcing, STOP, changing a running program |
| 9 | [Coming from TIA Portal](09-coming-from-tia-portal.md) | what's the same, what's different, what's missing |

Every complete program in this guide is compiled by the test suite (`tests/st/guide_test.cpp`), and the examples marked as errors are checked to fail with the message shown. If the language changes, the guide's tests fail until the guide is updated.

Ready-made programs to run are in [`examples/`](../../examples/).
