# 3. Expressions

## Operators

Tightest first (higher in the table = done first):

| Operators | Meaning | Works on |
|---|---|---|
| `( )` | grouping | |
| `-x`, `NOT x` | negation | numbers and TIME; BOOL and bit strings |
| `**` | power | numbers; the result is always REAL |
| `*`, `/`, `MOD` | multiply, divide, remainder | numbers (`MOD`: integers only) |
| `+`, `-` | add, subtract | numbers, TIME |
| `<`, `>`, `<=`, `>=` | compare | numbers, TIME, bit strings |
| `=`, `<>` | equal, not equal | numbers, TIME, BOOL, bit strings |
| `AND`, `&` | and | BOOL; bit strings, bit by bit |
| `XOR` | exclusive or | BOOL; bit strings, bit by bit |
| `OR` | or | BOOL; bit strings, bit by bit |

So `a + b * c` is `a + (b * c)`, and `x > 0 AND y > 0` is `(x > 0) AND (y > 0)`. Operators of the same level go left to right: `10 - 4 - 3` is `3`. When in doubt, add parentheses.

`AND`, `OR`, `XOR` and `NOT` work bit by bit on bit strings (BYTE, WORD, DWORD), not on integers ([Bit strings](#bit-strings) below). For `ABS`, `SQRT`, `MIN`, `MAX`, `LIMIT`, the shifts and the rest, see [chapter 6](06-functions.md).

## Mixing types

When two numbers of different types meet, both become the smallest type that holds every value of both: INT with DINT gives DINT, INT with UINT gives DINT, DINT with UDINT gives LINT; anything with REAL gives REAL.

```st
PROGRAM Mixing
VAR i : INT := 7; d : DINT := 100000; r : REAL; f : BOOL; END_VAR
    d := d + i;          (* DINT *)
    r := i / 2.0;        (* REAL: 3.5 *)
    f := i > 2.5;        (* compare as REAL *)
END_PROGRAM
```

## Integer arithmetic works like the PLC

- **Division rounds towards zero:** `7 / 2` is `3`, `-7 / 2` is `-3`. `MOD` has the sign of the left side: `-7 MOD 2` is `-1`.
- **Overflow wraps around**, as on an S7: an INT at `32767` plus 1 is `-32768`, a USINT at `255` plus 1 is `0`, a UINT at `0` minus 1 is `65535`. Nothing stops; the value just wraps. If that matters, use a bigger type, or check before adding.
- **Division by zero stops the PLC** (STOP, outputs off) with the line where it happened. Check first: `IF b <> 0 THEN q := a / b; END_IF;`

## Bit strings

A BYTE, WORD or DWORD is a row of bits: 16 input signals read as one word, a status word from a drive, a set of alarm flags. `AND`, `OR`, `XOR` and `NOT` work on every bit at once, and a BOOL mixed in counts as `16#0001`. Bit strings compare as unsigned numbers (`16#FF00 > 16#00FF`).

Pick out one bit, byte or word with `.%X`, `.%B` or `.%W` and its number, counted from the least significant, as in TIA Portal. It works on integers too, and you can write to it:

```st
PROGRAM Bits
VAR
    inputs AT %IW0 : WORD;       (* 16 sensors *)
    alarms : WORD;
    mask : WORD := 16#00F0;
    door, anyAlarm : BOOL;
    lowByte : BYTE;
END_VAR
    alarms := inputs AND mask;   (* keep bits 4 to 7 *)
    alarms := alarms OR door;    (* a BOOL counts as bit 0 *)
    alarms := alarms XOR 16#0100;
    anyAlarm := alarms <> 0;
    door := inputs.%X3;          (* bit 3 *)
    lowByte := inputs.%B0;       (* bits 0 to 7 *)
    alarms.%X15 := NOT door;     (* write one bit; the others stay *)
END_PROGRAM
```

A bit string is not a number: `+`, `-`, `*`, `/` and `MOD` don't take one. Convert it first, with `WORD_TO_UINT` or the like ([chapter 6](06-functions.md#conversions)):

```st
(* error: '+' needs numbers, but got WORD and integer literal *)
PROGRAM NoArithmetic
VAR status : WORD; END_VAR
    status := status + 1;
END_PROGRAM
```

The shifts `SHL`, `SHR`, `ROL` and `ROR` are standard functions ([chapter 6](06-functions.md#bits)).

## REAL precision

A REAL is a 32-bit float, like on the PLC: about 7 significant digits. Two consequences:

- `0.1` can't be stored exactly. Don't compare REALs with `=`; compare with a tolerance: `ABS(x - y) < 0.001`.
- Big values lose small steps: `16777216.0 + 1.0` is still `16777216.0`. Don't use a REAL as a counter.

Dividing a REAL by `0.0` doesn't stop the PLC; the result is infinity, as on an S7.

## TIME arithmetic

| Expression | Result |
|---|---|
| `TIME + TIME`, `TIME - TIME` | TIME |
| `TIME * integer`, `integer * TIME` | TIME |
| `TIME / integer` | TIME |
| `TIME < TIME` (any comparison) | BOOL |

```st
PROGRAM Times
VAR total : TIME; laps : INT := 3; average : TIME; late : BOOL; END_VAR
    total := T#1m + T#30s;
    average := total / laps;      (* T#30s *)
    late := average > T#25s;
END_PROGRAM
```

TIME and numbers don't mix: `total + 5` is an error (write `total + T#5ms`).
