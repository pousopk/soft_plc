# 1. Your first program

## The shape of a program

```st
PROGRAM Blink
VAR
    lamp AT %QX0.0 : BOOL;   (* an output *)
    ticks : INT;             (* internal memory *)
END_VAR
    ticks := ticks + 1;
    IF ticks >= 50 THEN      // 50 cycles of 10 ms = half a second
        lamp := NOT lamp;
        ticks := 0;
    END_IF;
END_PROGRAM
```

- A file has exactly one **`PROGRAM`**: the code the PLC runs every cycle. It can also have any number of **`FUNCTION_BLOCK`s** ([chapter 5](05-function-blocks.md)), **`FUNCTION`s** ([chapter 6](06-functions.md)) and **`TYPE`** blocks ([chapter 7](07-arrays-and-structs.md)), in any order.
- Variables are declared in **`VAR … END_VAR`**: `name : TYPE;`. Several with the same type: `a, b, c : INT;`.
- `AT %QX0.0` places a variable in the PLC's memory, here output bit 0.0. Variables without `AT` are internal memory.
- Every statement ends with **`;`**, including `END_IF;`.
- Comments: `(* like this *)`, over several lines if you want, or `// to the end of the line`.
- Names and keywords are **case-insensitive**: `lamp`, `Lamp` and `LAMP` are the same variable, `if` and `IF` the same keyword. Names are letters, digits and `_`, and don't start with a digit.

## Running it

```bash
./build/soft_plc examples/traffic_light.st      # run until Ctrl+C
./build/st_parse examples/traffic_light.st      # only check it, and show how it was understood
```

`soft_plc` prints every change of an output bit with a timestamp. When you stop it with Ctrl+C, it prints the cycle statistics and the final value of every variable.

If the program has a mistake, nothing runs; you get the line, the column and what's wrong:

```text
error: line 3, column 14: can't assign REAL to 'count', which is INT (values might not fit)
      count := 2.5;
               ^
```

## What "every cycle" means

soft_plc works like any PLC. Every **10 ms** it:

1. copies the inputs into memory (`%I`),
2. runs your program once, from top to bottom,
3. copies the outputs from memory (`%Q`) to the real outputs.

Then it waits for the next 10 ms. That has consequences you'll meet all the time:

- **Variables keep their value from one cycle to the next.** `ticks := ticks + 1;` counts cycles. (All variables are "static"; there are no temporary variables.)
- **Inputs don't change during a cycle.** If a sensor changes halfway through your program, you see it next cycle.
- **Outputs only change at the end of the cycle.** If your program writes `lamp := TRUE;` and later `lamp := FALSE;` in the same cycle, the lamp never turns on.
- **Never wait in a loop.** A `WHILE NOT sensor DO END_WHILE;` never ends, because the sensor can't change while your program runs (step 1 only happens between cycles). Wait by remembering state between cycles instead, as the blinker above does with `ticks`, or with a timer ([chapter 5](05-function-blocks.md)).

## Next

[Chapter 2](02-data-types-and-variables.md) covers the data types and how to write values.
