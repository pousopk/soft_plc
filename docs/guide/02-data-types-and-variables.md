# 2. Data types and variables

## The elementary types

| Type | Holds | Range | Size in memory |
|---|---|---|---|
| `BOOL` | `TRUE` or `FALSE` | | 1 bit |
| `SINT` | whole numbers | −128 … 127 | 8 bits |
| `USINT` | whole numbers, never negative | 0 … 255 | 8 bits |
| `INT` | whole numbers | −32 768 … 32 767 | 16 bits |
| `UINT` | whole numbers, never negative | 0 … 65 535 | 16 bits |
| `DINT` | whole numbers | −2 147 483 648 … 2 147 483 647 | 32 bits |
| `UDINT` | whole numbers, never negative | 0 … 4 294 967 295 | 32 bits |
| `LINT` | whole numbers | about ±9.2 × 10¹⁸ | 64 bits |
| `BYTE` | 8 bits | `16#00` … `16#FF` | 8 bits |
| `WORD` | 16 bits | `16#0000` … `16#FFFF` | 16 bits |
| `DWORD` | 32 bits | `16#0000_0000` … `16#FFFF_FFFF` | 32 bits |
| `REAL` | numbers with decimals | about ±3.4 × 10³⁸, 7 significant digits | 32 bits |
| `TIME` | a duration | about ±24.8 days, in milliseconds | 32 bits |
| `STRING[n]` | text, at most n characters of 1 byte | n from 1 to 254; `STRING` alone is `STRING[254]` | n + 2 bytes |
| `WSTRING[n]` | text, at most n characters of 2 bytes | n from 1 to 16 382; `WSTRING` alone is `WSTRING[254]` | 2n + 4 bytes |
| `CHAR` | one 1-byte character | codes 0 … 255 | 8 bits |
| `WCHAR` | one 2-byte character | codes 0 … 65 535 | 16 bits |

These are the same sizes as on a Siemens S7. There are also arrays and structs, built from these ([chapter 7](07-arrays-and-structs.md)).

Strings have a fixed size, like on a real PLC: a `STRING[20]` always takes 22 bytes, whatever it holds. A text that doesn't fit is cut to the length (see [Text](06-functions.md#text) for the string functions). A STRING's characters are bytes (Latin-1, so `'é'` is one character); a WSTRING's are 2 bytes each.

`BYTE`, `WORD` and `DWORD` are **bit strings**: a row of bits, like 16 input signals in one word or a status word from a drive, not a number. You combine them with `AND`, `OR`, `XOR` and `NOT`, shift them, and pick out single bits ([chapter 3](03-expressions.md#bit-strings)), but you can't add or multiply them. To do arithmetic on one, convert it to an integer first.

## Writing values (literals)

| Type | Examples |
|---|---|
| BOOL | `TRUE`, `FALSE` |
| integers | `42`, `-7`, `1_000_000` (underscores are ignored), `16#FF` (hex), `2#1010_0101` (binary), `8#17` (octal) |
| REAL | `3.14`, `-0.5`, `1.5E3`, `2.0E-6`. A REAL always has a decimal point: `1.0`, not `1.` or `.5` |
| TIME | `T#500ms`, `T#2s`, `T#1m30s`, `T#1h`, `T#1d2h`, also `TIME#5s`. Units: `d`, `h`, `m`, `s`, `ms` (biggest first) |
| STRING, CHAR | `'Hello'`, `''` (empty), `'a'` |
| WSTRING, WCHAR | `"Hello"`, `"$20AC"` (€) |
| any type, written out | `INT#5`, `UINT#65535`, `LINT#-5`, `WORD#16#FF00`, `BYTE#2#1010_0101`, `REAL#2`, `BOOL#1`, `STRING#'a'`, `CHAR#'a'`, `WCHAR#"a"` |

In a string, `$` starts an escape: `$'` is a quote in single quotes, `$"` one in double quotes, `$$` a dollar sign, `$N` or `$L` a new line, `$R` a carriage return, `$T` a tab, `$P` a form feed, and `$hh` a character by its hex code (`$41` is `A`); in a WSTRING, `$hhhh` (`$20AC` is €). A one-character string like `'a'` is also a CHAR where one is expected.

An integer literal like `42` has no fixed type: it becomes whatever integer, bit string or REAL it's used with. But it must fit: `x := 40000;` is an error when `x` is an INT, and so is `b := 256;` when `b` is a BYTE.

A **typed literal** like `INT#5` or `WORD#16#FF00` has exactly that type, as if it were a variable of it. It must fit its type too: `USINT#-1` is an error.

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

- Without an initial value, a variable starts at `FALSE`, `0`, `0.0`, `16#0` or `T#0ms`.
- Initial values must be **constants**: `a : INT := b;` is an error. A calculation of constants is fine: `a : INT := 60 * 60;`.
- The initial value is set **once**, when the PLC starts. After that, the variable keeps whatever the program last wrote, cycle after cycle.

A string variable copies the text it's given, and a text too long is cut to its length:

```st
PROGRAM Texts
VAR
    name : STRING[10] := 'pump';
    copy, numbered : STRING;
    label : STRING[4];
    last : CHAR;
    euro : WSTRING := "$20AC 5";
END_VAR
    copy := name;                     (* 'pump': its own copy of the text *)
    numbered := CONCAT(name, ' 2');   (* 'pump 2'; name stays 'pump' *)
    label := numbered;                (* 'pump': cut to 4 *)
    last := numbered[6];              (* '2': characters count from 1 *)
END_PROGRAM
```

```st
(* error: 'too long' is 8 characters, too long for STRING[4] *)
PROGRAM TooLong
VAR label : STRING[4]; END_VAR
    label := 'too long';
END_PROGRAM
```

## Converting between types

soft_plc only converts **automatically when nothing can be lost**:

| From → to | Allowed? |
|---|---|
| an integer → a bigger one that holds all its values: SINT → INT → DINT → LINT, USINT → UINT → UDINT → LINT, USINT → INT, UINT → DINT, UDINT → LINT | yes |
| any integer → REAL | yes (but DINT, UDINT and LINT can lose precision: REAL has 7 digits) |
| DINT → INT, INT → UINT, UINT → INT | **no**: the value might not fit |
| REAL → any integer | **no**: the decimals would be lost |
| BYTE → WORD → DWORD, and BOOL → any bit string | yes |
| bit strings ↔ integers | **no**: a WORD is bits, not a number (see below) |
| BOOL ↔ numbers | **no** |
| TIME ↔ numbers | **no**: write times as `T#…` |
| STRING ↔ WSTRING, CHAR ↔ WCHAR, texts ↔ anything else | **no**: use a conversion like `STRING_TO_WSTRING` or `INT_TO_STRING` |

When two different integers meet in a calculation, both become the smallest type that holds all values of both: INT with UINT gives DINT, DINT with UDINT gives LINT.

```st
PROGRAM Conversions
VAR small : INT := 5; big : DINT; x : REAL; count : UINT := 7; status : WORD; b : BYTE := 16#0F; END_VAR
    big := small;           (* fine: INT fits in DINT *)
    x := big * 2;           (* fine: becomes REAL *)
    big := small + count;   (* fine: INT + UINT is a DINT *)
    status := b;            (* fine: BYTE fits in WORD *)
END_PROGRAM
```

Bit strings and integers never convert into each other on their own, as in TIA Portal with IEC checking on. Say which one you mean with a conversion function, which keeps the bits:

```st
(* error: can't assign WORD to 'value', which is INT (convert with WORD_TO_INT) *)
PROGRAM Mixing
VAR value : INT; status : WORD := 16#00FF; END_VAR
    value := status;
END_PROGRAM
```

```st
(* error: can't assign DINT to 'small', which is INT (values might not fit) *)
PROGRAM Narrowing
VAR small : INT; big : DINT := 5; END_VAR
    small := big;
END_PROGRAM
```

To go the other way, say so with a conversion function, which also decides what happens to values that don't fit ([chapter 6](06-functions.md#conversions)):

```st
PROGRAM Explicit
VAR small : INT; big : DINT := 5; x : REAL := 2.7; END_VAR
    small := DINT_TO_INT(big);     (* keeps the low 16 bits *)
    small := REAL_TO_INT(x);       (* rounds: 3 *)
END_PROGRAM
```

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

The size of the address must match the type: a bit (`X`) for BOOL, a byte (`B`) for SINT, USINT and BYTE, a word (`W`) for INT, UINT and WORD, a double word (`D`) for DINT, UDINT, DWORD, REAL and TIME. A LINT has no address of its size, so it can't be placed with `AT` on its own; put it in a struct in a data block instead. CHAR takes a byte and WCHAR a word. Arrays, structs and strings take a byte address at an even byte (`%MB10`, `%DB1.DBB0`), see [chapter 7](07-arrays-and-structs.md) and, for the layout of a string, [chapter 8](08-memory-s7-and-online.md#arrays-and-structs-the-s7-layout). [Chapter 8](08-memory-s7-and-online.md) explains the memory areas.

- `AT` is only allowed in the PROGRAM, not inside function blocks.
- An `AT` variable can't have an initial value: its value comes from the memory.
- `%IX0.0` can also be written `%I0.0`. The `%` is required.
