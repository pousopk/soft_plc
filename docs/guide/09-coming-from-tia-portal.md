# 9. Coming from TIA Portal

soft_plc's Structured Text is IEC 61131-3, and SCL in TIA Portal is Siemens' version of the same language. Most code reads the same; this chapter lists what's different.

## The same

- `IF … THEN … ELSIF … ELSE … END_IF;`, `CASE … OF … END_CASE;`, `FOR … TO … BY … DO … END_FOR;`, `WHILE`, `REPEAT … UNTIL … END_REPEAT;`, `EXIT`, `RETURN`.
- Operators and their precedence, `MOD`, `**`, `AND`/`&`, `XOR`, `NOT`.
- `INT`, `DINT`, `REAL`, `TIME`, `BOOL`, with the same sizes and ranges; integer overflow wraps the same way.
- Literals: `16#FF`, `2#1010`, `T#1s500ms`, `1.5E3`.
- `Array[1..10] of Int` and multi-dimensional arrays; structs like UDTs, with the standard-access memory layout when placed in a DB.
- IEC timers and counters with the same names and pins: `TON`, `TOF`, `TP` (`IN`, `PT`, `Q`, `ET`), `CTU`, `CTD`, `R_TRIG`, `F_TRIG`, `SR`, `RS`.
- Functions with a return value, like FCs: `FUNCTION Scale : REAL … Scale := …; END_FUNCTION`, called by name or in order.
- The common standard functions with the same names: `ABS`, `SQRT`, `LN`, `EXP`, `SIN`…, `MIN`, `MAX`, `LIMIT`, `SEL`, `MUX`, `TRUNC`, `ROUND`, `CEIL`, `FLOOR`, and conversions like `INT_TO_REAL`, `DINT_TO_INT`, `REAL_TO_DINT`.
- Names are case-insensitive.

## Written differently

| TIA Portal (SCL) | soft_plc |
|---|---|
| `#count := #count + 1;` (local variables with `#`) | `count := count + 1;` (no `#`) |
| `"Motor_Start"` (a PLC tag) or `"DB_Data".speed` (a global DB) | declare a variable with `AT`: `motorStart AT %IX0.0 : BOOL;`, `speed AT %DB1.DBD4 : REAL;` |
| a UDT: `TYPE "Point" VERSION : 0.1 STRUCT … END_STRUCT; END_TYPE` | `TYPE Point : STRUCT … END_STRUCT; END_TYPE` (no quotes, no version; several types per block) |
| an FB with its own instance DB: `"Timer_DB"(IN := …);` | only multi-instances: declare `t : TON;` in `VAR` and call `t(IN := …);` |
| OB1 | the `PROGRAM`, run every 10 ms |
| reading outputs in the call: `t(IN := x, Q => done);` | read after the call: `t(IN := x); done := t.Q;` |
| `IF count THEN` would be an error in SCL too | the same here: conditions must be BOOL |

## Not there yet

- **Functions without a return value** (`FUNCTION F : VOID`), `VAR_OUTPUT` and `VAR_IN_OUT` in functions, and functions that return arrays or structs.
- **More standard functions:** `NORM_X`, `SCALE_X`, `MOVE_BLK`, `FILL_BLK`, the string functions, and Siemens' generic `CONVERT`.
- **Types:** `ULINT`, `LWORD`, `LREAL`, `STRING`, `CHAR`, `DATE`, `TIME_OF_DAY`, `DTL`, enums.
- **Variable sections:** `VAR_IN_OUT`, `VAR_TEMP`, `VAR CONSTANT`, retentive variables. All variables keep their values between cycles (like static variables in an FB).
- **Statements:** `CONTINUE`, `GOTO`, `REGION`.
- **Arrays of FB instances** (`ARRAY[1..4] OF TON`) and FB instances inside structs.
- **Several tasks:** no startup OB (OB100), no cyclic interrupts (OB30-OB38), no hardware interrupts or error OBs.

## Behaves differently

- **Array index out of range:** a real S7 reports an access error, and depending on the CPU and its settings, goes to STOP or calls an error OB. soft_plc always goes to STOP and tells you the line and column.
- **Variables without `AT`** are like *optimized* access in TIA: they have no byte address and can only be reached by name (from the program or a monitoring tool). Variables with `AT` are like *standard* access: fixed addresses that S7 clients use.
- **Assigning arrays:** both sides must have the same bounds and element type; there's no `MOVE_BLK` for copying part of an array (yet).
- **Bit strings and integers don't mix**, as in TIA Portal with *IEC check* on: `myInt := myWord` needs `WORD_TO_INT`, and a WORD can't be added to. With IEC check off, TIA Portal converts these on its own.
- **Integers to REAL:** any integer converts to REAL on its own, also DINT, UDINT and LINT, which can lose precision there (a REAL has 7 digits). Strict IEC only allows the conversions that can't.
- **Bits of a variable** (`myWord.%X3`) need a constant number, as in TIA Portal. `.%D` isn't supported.
- **Narrowing conversions:** `DINT_TO_INT` keeps the low 16 bits and `REAL_TO_INT` limits to the INT range. TIA Portal's conversions set `ENO` to FALSE when a value doesn't fit; soft_plc has no `ENO`.
- **Calls in order** must give every input; calls by name may leave inputs out (they get their default).
- **Compile errors** are reported one at a time: the first error stops compilation.
- **Download in RUN:** soft_plc keeps every variable whose name and type didn't change, in every block, with no memory reserve to set up; TIA Portal can keep values when variables are added to optimized blocks with a memory reserve, and reinitializes a block when its interface changes in ways the reserve can't absorb. In both, a new initial value doesn't change a variable that keeps its value (chapter 8, *Changing a running program*).
