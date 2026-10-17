# 3. Expressions

## Operators

Tightest first (higher in the table = done first):

| Operators | Meaning | Works on |
|---|---|---|
| `( )` | grouping | |
| `-x`, `NOT x` | negation | numbers and TIME; BOOL |
| `**` | power | numbers; the result is always REAL |
| `*`, `/`, `MOD` | multiply, divide, remainder | numbers (`MOD`: integers only) |
| `+`, `-` | add, subtract | numbers, TIME |
| `<`, `>`, `<=`, `>=` | compare | numbers, TIME |
| `=`, `<>` | equal, not equal | numbers, TIME, BOOL |
| `AND`, `&` | and | BOOL |
| `XOR` | exclusive or | BOOL |
| `OR` | or | BOOL |

So `a + b * c` is `a + (b * c)`, and `x > 0 AND y > 0` is `(x > 0) AND (y > 0)`. Operators of the same level go left to right: `10 - 4 - 3` is `3`. When in doubt, add parentheses.

There are no bitwise operations on integers (`AND` only takes BOOLs).

## Mixing types

When two numbers of different types meet, the narrower one is widened first: INT with DINT gives DINT; anything with REAL gives REAL.

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
- **Overflow wraps around**, as on an S7: an INT at `32767` plus 1 is `-32768`. Nothing stops; the value just wraps. If that matters, use DINT, or check before adding.
- **Division by zero stops the PLC** (STOP, outputs off) with the line where it happened. Check first: `IF b <> 0 THEN q := a / b; END_IF;`

## REAL precision

A REAL is a 32-bit float, like on the PLC: about 7 significant digits. Two consequences:

- `0.1` can't be stored exactly. Don't compare REALs with `=`; compare with a tolerance: `(x - y) < 0.001 AND (y - x) < 0.001` (there's no `ABS` function yet).
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
