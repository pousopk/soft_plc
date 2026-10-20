# 6. Functions

A **function** takes some inputs and gives back **one value**. Unlike a function block it has **no memory**: called twice with the same inputs, it always gives the same result. Use it for calculations: scaling, conversions, limits, formulas.

| | FUNCTION | FUNCTION_BLOCK |
|---|---|---|
| Gives back | one value, used in an expression: `x := F(a);` | outputs, read after the call: `fb(IN := a); x := fb.Q;` |
| Memory between calls | none | yes, per instance |
| Instances | none: just call it | declared like variables: `t : TON;` |
| Can contain | other function calls | function calls and FB instances |
| Like in TIA Portal | an FC with a return value | an FB with multi-instances |

## Writing a function

```st
FUNCTION Scale : REAL                   (* the type it gives back *)
VAR_INPUT
    raw : INT;
    low : REAL := 0.0;                  (* inputs can have defaults *)
    high : REAL := 100.0;
END_VAR
VAR
    fraction : REAL;                    (* temporary: starts again every call *)
END_VAR
    fraction := raw / 27648.0;
    Scale := low + (high - low) * fraction;   (* the result: assign to the function's name *)
END_FUNCTION

PROGRAM UseScale
VAR rawValue AT %IW0 : INT; pressure, level, temperature : REAL; END_VAR
    pressure := Scale(raw := rawValue, high := 10.0);        (* by name: low keeps its default *)
    level := Scale(rawValue, 0.0, 2500.0);                   (* in order: every input *)
    temperature := Scale(raw := rawValue, low := -40.0, high := 120.0);
END_PROGRAM
```

- `FUNCTION Name : TYPE` … `END_FUNCTION`. The type is BOOL, INT, DINT, REAL or TIME.
- The result is set by assigning to the function's name: `Scale := …;`. It can be read back inside the function too. If the function never assigns it, the result is 0 (or FALSE).
- `VAR_INPUT` are the inputs. `VAR` are temporary variables: they start from their initial value at **every** call.
- `RETURN;` ends the function early, with the result as it is.

Calling it:
- **By name:** `Scale(raw := x, high := 10.0)`. Inputs you leave out get their default (or 0).
- **In order:** `Scale(x, 0.0, 10.0)`. Then every input must be given, in the order they're declared.
- Don't mix the two in one call.
- A function call is an expression: it can be used anywhere a value can, including inside another call: `Scale(Scale(x))`.

### What functions can't do

```st
(* error: a FUNCTION has no memory between calls, so it can't contain function block instances like 'delay'; use a FUNCTION_BLOCK *)
FUNCTION Delayed : BOOL
VAR_INPUT x : BOOL; END_VAR
VAR delay : TON; END_VAR
    delay(IN := x, PT := T#1s);
    Delayed := delay.Q;
END_FUNCTION
PROGRAM P END_PROGRAM
```

- No function block instances inside, and no `VAR_OUTPUT`: one result only.
- No `AT`: a function only sees its inputs.
- **No recursion:** a function can call other functions, but never itself, directly or through others. That's an IEC rule, and the compiler checks it.
- It can't return an array or struct. It can **take** them as inputs (they're copied in); to give back several values, use a function block with outputs.
- A call whose result isn't used is an error: `Scale(raw := x);` on its own does nothing, so write `y := Scale(raw := x);`.

## The standard functions

They're built in, and most of them work with several types, like in TIA Portal. The names are case-insensitive.

### Numbers

| Function | Gives | Notes |
|---|---|---|
| `ABS(x)` | the same type as `x` | `ABS(-5)` is 5. For an INT, `ABS(-32768)` wraps to −32768, as on a PLC |
| `SQRT(x)` `LN(x)` `LOG(x)` `EXP(x)` | REAL | `LOG` is base 10. `SQRT(-1.0)` gives NaN, not an error |
| `SIN(x)` `COS(x)` `TAN(x)` `ASIN(x)` `ACOS(x)` `ATAN(x)` | REAL | angles in radians |
| `EXPT(x, y)` | REAL | x to the power y, the same as `x ** y` |
| `TRUNC(x)` | DINT | drops the decimals: `TRUNC(-2.7)` is −2 |
| `ROUND(x)` | DINT | nearest whole number; exact halves go to the **even** one: `ROUND(2.5)` is 2, `ROUND(3.5)` is 4 |
| `CEIL(x)`, `FLOOR(x)` | DINT | up and down: `CEIL(2.1)` is 3, `FLOOR(-2.1)` is −3 |

`TRUNC`, `ROUND`, `CEIL` and `FLOOR` limit their result to the DINT range.

### Choosing

| Function | Gives | |
|---|---|---|
| `MIN(a, b, …)`, `MAX(a, b, …)` | the smallest / largest | two or more inputs |
| `LIMIT(MN, IN, MX)` | `IN`, kept between `MN` and `MX` | `LIMIT(0, x, 100)` |
| `SEL(G, IN0, IN1)` | `IN0` if `G` is FALSE, `IN1` if TRUE | `G` is a BOOL |
| `MUX(K, IN0, IN1, …)` | input number `K`, counting from 0 | `K` outside the inputs **stops the PLC** |

The inputs (except `G` and `K`) must be of one kind: all numbers, all TIMEs, all bit strings, or (for `SEL` and `MUX`) all BOOLs. Numbers of different types are widened like in any expression: `MAX(anInt, aReal)` is a REAL.

With names, the inputs are called `IN1`, `IN2`, … for `MIN` and `MAX`; `MN`, `IN`, `MX` for `LIMIT`; `G`, `IN0`, `IN1` for `SEL`; `K`, `IN0`, `IN1`, … for `MUX`; and `IN` for the functions with one input.

```st
PROGRAM Choosing
VAR speed, setpoint : REAL; manual : BOOL; manualSpeed : REAL := 50.0; recipe : INT; delay : TIME; END_VAR
    setpoint := LIMIT(MN := 0.0, IN := setpoint, MX := 1500.0);
    speed := SEL(manual, setpoint, manualSpeed);
    delay := MUX(LIMIT(0, recipe, 2), T#2s, T#5s, T#10s);
    speed := MAX(speed, 10.0);
END_PROGRAM
```

### Conversions

| Function | What it does |
|---|---|
Every `<FROM>_TO_<TO>` between the integers, the bit strings and BOOL exists, and between the integers and REAL. Between REAL and the bit strings there are only `DWORD_TO_REAL` and `REAL_TO_DWORD` (see below), and BOOL doesn't convert to or from REAL: go through an integer, as in `UINT_TO_REAL(WORD_TO_UINT(w))`. TIME only converts to and from DINT.

| Function | What it does |
|---|---|
| `INT_TO_DINT`, `USINT_TO_UINT`, `INT_TO_REAL`, … | widen; never loses anything (except that a REAL keeps only about 7 digits) |
| `DINT_TO_INT`, `INT_TO_USINT`, … | keep the **low bits**, like a PLC: `DINT_TO_INT(70000)` is 4464, `INT_TO_USINT(-1)` is 255. Check the range first if that matters |
| `WORD_TO_INT`, `INT_TO_WORD`, `BYTE_TO_USINT`, … | keep the bits as they are: `WORD_TO_INT(16#FFFF)` is −1, `INT_TO_WORD(-1)` is `16#FFFF` |
| `BYTE_TO_WORD`, `DWORD_TO_WORD`, … | widen with zeros, or keep the low bits |
| `REAL_TO_INT`, `REAL_TO_UDINT`, … | round to the nearest (halves to even), and **limit** to the type's range: `REAL_TO_INT(1.0E9)` is 32767, `REAL_TO_USINT(-5.0)` is 0 |
| `DWORD_TO_REAL`, `REAL_TO_DWORD` | **copy the 32 bits unchanged**, not the number: see below |
| `BOOL_TO_INT`, `BOOL_TO_WORD`, … | FALSE → 0, TRUE → 1 |
| `INT_TO_BOOL`, `WORD_TO_BOOL`, … | 0 → FALSE, anything else → TRUE |
| `TIME_TO_DINT`, `DINT_TO_TIME` | a TIME as milliseconds, and back |

`DWORD_TO_REAL` is different from every other conversion: it doesn't turn the number 1 into 1.0, it takes the 32 bits as they are and reads them as a REAL, as in TIA Portal. That's what you need when a REAL arrives as two words, for example over Modbus: `DWORD_TO_REAL(16#3F80_0000)` is 1.0. To turn a number into a REAL, use `UDINT_TO_REAL`, `DINT_TO_REAL` and so on.

```st
PROGRAM Conversions
VAR counter : DINT := 123456; display : INT; seconds : REAL; runTime : TIME := T#1m30s; END_VAR
    display := DINT_TO_INT(counter MOD 10000);            (* the last 4 digits: always fits *)
    seconds := DINT_TO_REAL(TIME_TO_DINT(runTime)) / 1000.0;   (* 90.0 *)
END_PROGRAM
```

The rounding of `ROUND` and `REAL_TO_INT` (halves to the even number) is IEEE 754's default rounding. Check your CPU's documentation before relying on how a real PLC rounds exact halves.

### Bits

| Function | Gives | |
|---|---|---|
| `SHL(IN, N)` | `IN` shifted `N` bits to the left; zeros come in on the right | `SHL(BYTE#16#81, 1)` is `16#02` |
| `SHR(IN, N)` | `IN` shifted `N` bits to the right; zeros come in on the left | `SHR(WORD#16#8000, 15)` is `16#0001` |
| `ROL(IN, N)` | `IN` rotated `N` bits to the left: what leaves on one side comes back on the other | `ROL(BYTE#16#81, 1)` is `16#03` |
| `ROR(IN, N)` | `IN` rotated `N` bits to the right | `ROR(BYTE#16#01, 1)` is `16#80` |

`IN` must be a bit string (BYTE, WORD or DWORD), and the result has its type; `N` is any integer. Shifting by the width or more gives 0, rotating goes round (`ROR` of a BYTE by 9 is the same as by 1), and a negative `N` changes nothing. With names, the inputs are `IN` and `N`. An untyped literal isn't a bit string, so write `SHL(BYTE#16#81, 1)`, not `SHL(16#81, 1)`.

```st
PROGRAM Shifting
VAR
    received : ARRAY[1..2] OF WORD := [16#3F80, 16#0000];  (* a REAL, as two Modbus registers *)
    value : REAL;
    lamps : BYTE := 16#01;
END_VAR
    value := DWORD_TO_REAL(SHL(WORD_TO_DWORD(received[1]), 16) OR received[2]);  (* 1.0 *)
    lamps := ROL(lamps, 1);   (* a running light: one lamp further each cycle *)
END_PROGRAM
```
