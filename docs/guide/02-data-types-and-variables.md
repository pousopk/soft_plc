# 2. Data types and variables

## The elementary types

| Type | Holds | Range | Size in memory |
|---|---|---|---|
| `BOOL` | `TRUE` or `FALSE` | | 1 bit |
| `INT` | whole numbers | −32 768 … 32 767 | 16 bits |
| `DINT` | whole numbers | −2 147 483 648 … 2 147 483 647 | 32 bits |
| `REAL` | numbers with decimals | about ±3.4 × 10³⁸, 7 significant digits | 32 bits |
| `TIME` | a duration | about ±24.8 days, in milliseconds | 32 bits |

These are the same sizes as on a Siemens S7. There are also arrays and structs, built from these ([chapter 6](06-arrays-and-structs.md)).

## Writing values (literals)

| Type | Examples |
|---|---|
| BOOL | `TRUE`, `FALSE` |
| integers | `42`, `-7`, `1_000_000` (underscores are ignored), `16#FF` (hex), `2#1010_0101` (binary), `8#17` (octal) |
| REAL | `3.14`, `-0.5`, `1.5E3`, `2.0E-6`. A REAL always has a decimal point: `1.0`, not `1.` or `.5` |
| TIME | `T#500ms`, `T#2s`, `T#1m30s`, `T#1h`, `T#1d2h`, also `TIME#5s`. Units: `d`, `h`, `m`, `s`, `ms` (biggest first) |

An integer literal like `42` has no fixed type: it becomes INT, DINT or REAL to match what it's used with. But it must fit: `x := 40000;` is an error when `x` is an INT.

## Declaring variables

```st
PROGRAM Variables
VAR
    running : BOOL;                 (* starts FALSE *)
    count : INT := 10;              (* starts at 10 *)
    total, average : DINT;          (* two variables, both 0 *)
    setpoint : REAL := 1500.0;
    delay : TIME := T#2s;
END_VAR
    count := count + 1;
END_PROGRAM
```

- Without an initial value, a variable starts at `FALSE`, `0`, `0.0` or `T#0ms`.
- Initial values must be **constants**: `a : INT := b;` is an error. A calculation of constants is fine: `a : INT := 60 * 60;`.
- The initial value is set **once**, when the PLC starts. After that, the variable keeps whatever the program last wrote, cycle after cycle.

## Converting between types

soft_plc only converts **automatically when nothing can be lost**:

| From → to | Allowed? |
|---|---|
| INT → DINT | yes |
| INT, DINT → REAL | yes |
| DINT → INT | **no**: the value might not fit |
| REAL → INT or DINT | **no**: the decimals would be lost |
| BOOL ↔ numbers | **no** |
| TIME ↔ numbers | **no**: write times as `T#…` |

```st
PROGRAM Conversions
VAR small : INT := 5; big : DINT; x : REAL; END_VAR
    big := small;       (* fine: INT fits in DINT *)
    x := big * 2;       (* fine: becomes REAL *)
END_PROGRAM
```

```st
(* error: can't assign DINT to 'small', which is INT (values might not fit) *)
PROGRAM Narrowing
VAR small : INT; big : DINT := 5; END_VAR
    small := big;
END_PROGRAM
```

Conversion functions like `DINT_TO_INT` and `REAL_TO_INT` don't exist yet. Until they do, keep values in the bigger type (`DINT`, `REAL`).

## Variables in the PLC's memory: `AT`

A variable declared with `AT` doesn't have memory of its own: it **is** a place in the process image.

```st
PROGRAM Wiring
VAR
    startButton AT %IX0.0 : BOOL;    (* input bit 0.0 *)
    motor       AT %QX0.0 : BOOL;    (* output bit 0.0 *)
    speedRaw    AT %IW2   : INT;     (* input word at bytes 2-3 *)
    counter     AT %MD10  : DINT;    (* marker double word at bytes 10-13 *)
    setpoint    AT %DB1.DBD4 : REAL; (* data block 1, bytes 4-7 *)
END_VAR
    motor := startButton;
END_PROGRAM
```

The size of the address must match the type: a bit (`X`) for BOOL, a word (`W`) for INT, a double word (`D`) for DINT, REAL and TIME. Arrays and structs take a byte address (`%MB10`, `%DB1.DBB0`), see [chapter 6](06-arrays-and-structs.md). [Chapter 7](07-memory-s7-and-online.md) explains the memory areas.

- `AT` is only allowed in the PROGRAM, not inside function blocks.
- An `AT` variable can't have an initial value: its value comes from the memory.
- `%IX0.0` can also be written `%I0.0`. The `%` is required.
