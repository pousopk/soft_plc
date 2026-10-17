# 5. Function blocks

A **function block** (FB) is a piece of logic with its own memory. You write it once and use it many times: each **instance** has its own copy of the FB's variables, kept from cycle to cycle. Timers and counters are function blocks.

## Using one: a timer

```st
PROGRAM Delay
VAR
    start AT %IX0.0 : BOOL;
    motor AT %QX0.0 : BOOL;
    onDelay : TON;               (* an instance of the standard TON *)
END_VAR
    onDelay(IN := start, PT := T#3s);   (* call it: give the inputs *)
    motor := onDelay.Q;                 (* read an output *)
END_PROGRAM
```

- Declare an instance like a variable: `onDelay : TON;`.
- **Call** it with its inputs: `onDelay(IN := start, PT := T#3s);`. A call is a statement on its own; it can't be part of an expression.
- After the call, read its outputs (and inputs) with a dot: `onDelay.Q`, `onDelay.ET`.
- Inputs you leave out keep the value from the last call: `onDelay(IN := start);` still uses `T#3s`.

### Call it every cycle

A function block only runs when it's called. A timer called inside an `IF` stops counting while the `IF` is false: its outputs freeze, and it doesn't reset. Call timers and edge detectors **every cycle**, outside any `IF`, and put the condition into the input:

```st
PROGRAM CallEveryCycle
VAR enable, start, lamp : BOOL; t : TON; END_VAR
    t(IN := enable AND start, PT := T#5s);   (* not: IF enable THEN t(...); END_IF; *)
    lamp := t.Q;
END_PROGRAM
```

## The standard blocks

### Timers: TON, TOF, TP
All three have inputs `IN : BOOL`, `PT : TIME` and outputs `Q : BOOL`, `ET : TIME` (elapsed time, never more than `PT`).

```text
TON (on-delay):   IN  ___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾\_____     Q comes on PT after IN, goes off with IN
                  Q   _________/‾‾‾‾‾‾‾‾\_____
TOF (off-delay):  IN  ___/‾‾‾‾‾‾\______________   Q follows IN on, goes off PT after IN
                  Q   ___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾\______
TP (pulse):       IN  ___/‾\________/‾‾‾‾‾‾‾‾\__   a rising edge gives a pulse of exactly PT
                  Q   ___/‾‾‾‾‾\____/‾‾‾‾‾\_____
```

A timer is checked once per cycle, so it can switch up to one cycle (10 ms) late.

### Counters: CTU, CTD

| Block | Inputs | Outputs | Behaviour |
|---|---|---|---|
| `CTU` | `CU : BOOL`, `R : BOOL`, `PV : INT` | `Q : BOOL`, `CV : INT` | `CV` goes up by 1 on each **rising edge** of `CU`; `R` resets it to 0; `Q` is `CV >= PV` |
| `CTD` | `CD : BOOL`, `LD : BOOL`, `PV : INT` | `Q : BOOL`, `CV : INT` | `CV` goes down by 1 on each rising edge of `CD`; `LD` loads `PV`; `Q` is `CV <= 0` |

A box holding the sensor on for many cycles counts **once**. A pulse shorter than one cycle may not be seen at all (chapter 1: inputs are only read between cycles).

### Edges: R_TRIG, F_TRIG
Input `CLK : BOOL`, output `Q : BOOL`. `Q` is TRUE for exactly **one cycle** when `CLK` goes from FALSE to TRUE (`R_TRIG`) or from TRUE to FALSE (`F_TRIG`).

### Flip-flops: SR, RS

| Block | Inputs | Output | When both inputs are on |
|---|---|---|---|
| `SR` | `S1`, `R` | `Q1` | set wins: `Q1` on |
| `RS` | `S`, `R1` | `Q1` | reset wins: `Q1` off (the usual choice for start/stop, so stop always wins) |

```st
PROGRAM StartStop
VAR
    startButton AT %IX0.0 : BOOL;
    stopButton  AT %IX0.1 : BOOL;   (* a normally closed button: TRUE = not pressed *)
    motor       AT %QX0.0 : BOOL;
    latch : RS;
    starts : CTU;
    started : R_TRIG;
END_VAR
    latch(S := startButton, R1 := NOT stopButton);
    motor := latch.Q1;
    started(CLK := motor);
    starts(CU := started.Q, R := FALSE, PV := 1000);
END_PROGRAM
```

## Writing your own

```st
FUNCTION_BLOCK Blinker
VAR_INPUT
    enable : BOOL := TRUE;     (* inputs can have a default *)
    period : TIME := T#1s;
END_VAR
VAR_OUTPUT
    lamp : BOOL;
END_VAR
VAR
    half : TON;                (* internal: an FB inside an FB *)
END_VAR
    half(IN := enable AND NOT half.Q, PT := period / 2);
    IF half.Q THEN
        lamp := NOT lamp;
    END_IF;
    IF NOT enable THEN
        lamp := FALSE;
    END_IF;
END_FUNCTION_BLOCK

PROGRAM TwoBlinkers
VAR
    fast, slow : Blinker;      (* two instances, each with its own memory *)
    lampA AT %QX0.0 : BOOL;
    lampB AT %QX0.1 : BOOL;
END_VAR
    fast(period := T#200ms);
    slow();                    (* all defaults *)
    lampA := fast.lamp;
    lampB := slow.lamp;
END_PROGRAM
```

| Section | Who sets it | Who can read it |
|---|---|---|
| `VAR_INPUT` | the caller, in the call | the FB, and the caller as `inst.name` |
| `VAR_OUTPUT` | the FB | the FB, and the caller as `inst.name` |
| `VAR` | the FB | only the FB (and monitoring tools) |

Rules:
- The caller can't assign to an instance's variables outside the call: `fast.period := T#1s;` is an error.
- Inputs and outputs can be any type, including arrays and structs ([chapter 6](06-arrays-and-structs.md)), but not function block instances.
- A function block can contain instances of other blocks, but not of itself (directly or through others).
- `AT` isn't allowed inside function blocks: pass values in and out through inputs and outputs.
- The names of the standard blocks (TON, CTU, …) can't be reused.

```st
(* error: can't assign to 't.IN': set inputs in the call, like t(IN := ...); outputs are read-only *)
PROGRAM Wrong
VAR t : TON; END_VAR
    t.IN := TRUE;
END_PROGRAM
```
