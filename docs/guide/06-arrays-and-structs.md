# 6. Arrays and structs

## Arrays

```st
PROGRAM Arrays
VAR
    temperatures : ARRAY[1..8] OF REAL;           (* 8 REALs: temperatures[1] .. temperatures[8] *)
    offsets : ARRAY[-2..2] OF INT;                (* bounds can be any integers, also negative *)
    grid : ARRAY[1..3, 1..4] OF DINT;             (* two dimensions: 3 rows of 4 *)
    i, row, col : INT;
    total : REAL;
END_VAR
    temperatures[1] := 21.5;                      (* a constant index *)
    FOR i := 1 TO 8 DO                            (* a computed index *)
        total := total + temperatures[i];
    END_FOR;
    FOR row := 1 TO 3 DO
        FOR col := 1 TO 4 DO
            grid[row, col] := row * 10 + col;
        END_FOR;
    END_FOR;
END_PROGRAM
```

- `ARRAY[low..high] OF type`, with integer bounds, `low <= high`. Several dimensions are separated by commas: `ARRAY[1..3, 1..4]`. An array of arrays (`ARRAY[...] OF ARRAY[...]`) isn't allowed; use dimensions, or a [TYPE](#naming-types-type) for the inner array.
- An element is used like any variable: `temperatures[i] := 0.0;`, `IF grid[2, 3] > 0 THEN …`.
- The index must be an integer.

### Indices outside the array
A **constant** index outside the bounds is a compile error:

```st
(* error: index 9 is outside the bounds of 'temperatures' (1..8) *)
PROGRAM OutOfBounds
VAR temperatures : ARRAY[1..8] OF REAL; END_VAR
    temperatures[9] := 0.0;
END_PROGRAM
```

A **computed** index is checked every time it's used. If it's outside, the PLC goes to STOP with `array index 9 is outside the bounds 1..8` and the line, the same as a real PLC with an area error. When an index comes from outside (an HMI, a sensor, a counter), check it first: `IF i >= 1 AND i <= 8 THEN …`.

## Structs

A struct groups related values under one name, like a UDT in TIA Portal. Struct types are declared in a `TYPE` block, outside the PROGRAM:

```st
TYPE
    Point : STRUCT
        x : REAL;
        y : REAL;
    END_STRUCT;

    Part : STRUCT
        id : DINT;
        position : Point;           (* a struct inside a struct *)
        good : BOOL := TRUE;        (* fields can have defaults *)
        measurements : ARRAY[1..3] OF REAL;
    END_STRUCT;
END_TYPE

PROGRAM Parts
VAR
    current : Part;
    lastTen : ARRAY[1..10] OF Part; (* an array of structs *)
    next : INT := 1;
END_VAR
    current.id := current.id + 1;
    current.position.x := 12.5;
    current.measurements[2] := 0.98;
    lastTen[next] := current;       (* copies the whole struct *)
    next := next MOD 10 + 1;
END_PROGRAM
```

- Fields are reached with a dot: `current.position.x`. Dots and indices combine freely: `lastTen[next].measurements[2]`.
- A field can be of any type except a function block: elementary, an array, another struct.
- A struct can't contain itself, directly or through another struct.

## Naming types: TYPE

A `TYPE` block can also give a name to an array type, which is handy when the same array appears in several places:

```st
TYPE
    Recipe : ARRAY[1..5] OF REAL;
END_TYPE

PROGRAM Recipes
VAR current, saved : Recipe; END_VAR
    saved := current;
END_PROGRAM
```

A file can have several `TYPE` blocks, before or after the function blocks and the program. Types can use each other in any order.

## Copying whole arrays and structs

`a := b;` copies **every value** from `b` to `a`. After that, the two are independent: changing `b[1]` doesn't change `a[1]`.

Both sides must have the **same type**:
- two structs: the same declared `TYPE`. Two struct types with identical fields are still different types.
- two arrays: the same bounds and the same element type. A named array type (`Recipe` above) matches any array with the same bounds and element type.

```st
(* error: can't assign ARRAY[0..4] OF REAL to 'current', which is Recipe *)
TYPE Recipe : ARRAY[1..5] OF REAL; END_TYPE
PROGRAM Mismatch
VAR current : Recipe; other : ARRAY[0..4] OF REAL; END_VAR
    current := other;
END_PROGRAM
```

Whole arrays and structs can only be copied. They can't be compared (`a = b`), added, or used in conditions; work field by field or element by element.

## Initial values

```st
TYPE
    Point : STRUCT x : REAL; y : REAL := 1.0; END_STRUCT;
END_TYPE

PROGRAM InitialValues
VAR
    primes : ARRAY[1..5] OF INT := [2, 3, 5, 7, 11];
    zeros : ARRAY[1..100] OF INT;                  (* everything 0 *)
    filled : ARRAY[1..6] OF INT := [3(7), 1];      (* 7, 7, 7, 1, then 0, 0 *)
    origin : Point;                                (* x = 0.0, y = 1.0 (the type's default) *)
    corner : Point := (x := 5.0);                  (* x = 5.0, y = 1.0 *)
    path : ARRAY[1..3] OF Point := [(x := 1.0), (x := 2.0, y := 2.0)];
END_VAR
END_PROGRAM
```

- An array takes a list in brackets: `[a, b, c]`. `n(value)` repeats a value `n` times; `n()` gives `n` default values. A shorter list leaves the rest at their defaults; a longer one is an error.
- A struct takes `(field := value, …)` in parentheses. Fields you leave out keep the default from the `TYPE` (or 0 / FALSE).
- They nest: an array of structs takes a list of `( … )`.
- As always, initial values must be constants.

## Function blocks with arrays and structs

Inputs and outputs can be arrays and structs. Passing one copies it in; reading an output reads the instance's copy.

```st
TYPE
    Summary : STRUCT total : DINT; highest : INT; END_STRUCT;
END_TYPE

FUNCTION_BLOCK Summarize
VAR_INPUT values : ARRAY[1..4] OF INT; END_VAR
VAR_OUTPUT result : Summary; END_VAR
VAR i : INT; END_VAR
    result.total := 0;
    result.highest := values[1];
    FOR i := 1 TO 4 DO
        result.total := result.total + values[i];
        IF values[i] > result.highest THEN
            result.highest := values[i];
        END_IF;
    END_FOR;
END_FUNCTION_BLOCK

PROGRAM UseSummarize
VAR
    readings : ARRAY[1..4] OF INT := [3, 9, -2, 5];
    summarize : Summarize;
    best : INT;
    copy : Summary;
END_VAR
    summarize(values := readings);
    best := summarize.result.highest;      (* 9 *)
    copy := summarize.result;
END_PROGRAM
```

Arrays of function block instances (`ARRAY[1..4] OF TON`) and function blocks inside structs aren't supported yet. Declare the instances one by one.

## Arrays and structs in the PLC's memory

With `AT`, an array or struct is placed in the process image or a data block, laid out exactly as Siemens does for **standard (non-optimized) access**. An S7 client, an HMI or `plc_gui` then finds every field at the byte offset TIA Portal would show. The rules, and a full example, are in [chapter 7](07-memory-s7-and-online.md#arrays-and-structs-the-s7-layout).

```st
TYPE
    Result : STRUCT ok : BOOL; nok : BOOL; count : INT; score : REAL; END_STRUCT;
END_TYPE

PROGRAM Placed
VAR
    results AT %DB10.DBB0 : ARRAY[1..4] OF Result;   (* DB10, 8 bytes per result *)
    flags AT %MB20 : ARRAY[0..15] OF BOOL;           (* %M20.0 .. %M21.7 *)
END_VAR
    results[1].ok := flags[0];
END_PROGRAM
```

An array or struct with `AT` needs a byte address at an **even** byte: `%MB20`, `%DB10.DBB0` (or `%DB10.DBX0.0`).

## Watching them online

Monitoring sees every element and field as its own variable, by path: `results[1].score`, `grid[2,3]` (no space after the comma), `summarize.result.total`. You can READ, WRITE and FORCE each of them ([chapter 7](07-memory-s7-and-online.md#going-online)). A whole array or struct isn't a single value, so `READ results` gives `?`.
