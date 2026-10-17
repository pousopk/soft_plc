# 4. Statements

## Assignment

```st
PROGRAM Assignment
VAR a, b : INT; ready : BOOL; END_VAR
    a := 5;
    b := a * 2 + 1;
    ready := a > 3 AND b < 20;
END_PROGRAM
```

The value on the right must fit the variable on the left ([chapter 2](02-data-types-and-variables.md#converting-between-types)).

## IF

```st
PROGRAM Thermostat
VAR temperature : REAL; heater, alarm : BOOL; END_VAR
    IF temperature < 18.0 THEN
        heater := TRUE;
    ELSIF temperature > 22.0 THEN
        heater := FALSE;
    ELSE
        ;  (* between 18 and 22: keep the heater as it is *)
    END_IF;
    alarm := temperature > 40.0;   (* a condition can simply be assigned *)
END_PROGRAM
```

Any number of `ELSIF`s; `ELSE` is optional. The conditions must be BOOL: `IF count THEN` is an error, write `IF count <> 0 THEN`. A lone `;` is an empty statement.

## CASE

```st
PROGRAM Sequence
VAR step : INT; valve, pump, done : BOOL; END_VAR
    CASE step OF
        0:      valve := FALSE; pump := FALSE;
        1, 2:   valve := TRUE;
        3..9:   pump := TRUE;
        -1:     done := TRUE;
    ELSE
        step := 0;   (* unknown step: start again *)
    END_CASE;
END_PROGRAM
```

- The selector must be an integer (INT or DINT).
- Labels are integer constants: single values, lists (`1, 2`) and ranges (`3..9`). A value may only appear once.
- Only the matching branch runs; there's no "fall-through" to the next one.
- `ELSE` runs when no label matches.

`CASE` is the usual way to write a **state machine**: `step` remembers where the sequence is between cycles, and each branch decides when to move on. See `examples/traffic_light.st`.

## FOR

```st
PROGRAM Sums
VAR i, total, countdown : INT; END_VAR
    total := 0;
    FOR i := 1 TO 10 DO
        total := total + i;          (* 55 *)
    END_FOR;
    FOR i := 10 TO 0 BY -2 DO        (* 10, 8, 6, 4, 2, 0 *)
        countdown := countdown + 1;
    END_FOR;
END_PROGRAM
```

- The loop variable must be an INT or DINT, and the loop can't change it.
- `TO` and `BY` are worked out **once**, before the first round. With `BY` negative, the loop counts down while the variable is `>=` the end.
- `BY 0` would never end, so it stops the PLC.

## WHILE and REPEAT

```st
PROGRAM Loops
VAR n, steps : DINT := 27; END_VAR
    steps := 0;
    WHILE n <> 1 DO             (* checks first: may run 0 times *)
        IF n MOD 2 = 0 THEN n := n / 2; ELSE n := 3 * n + 1; END_IF;
        steps := steps + 1;
    END_WHILE;
    REPEAT                      (* checks last: runs at least once *)
        n := n + 1;
    UNTIL n >= 10
    END_REPEAT;
END_PROGRAM
```

Remember [chapter 1](01-first-program.md#what-every-cycle-means): a loop runs **inside one cycle**. Use loops to process data (arrays, calculations), never to wait for something to happen.

## EXIT and RETURN

- `EXIT;` leaves the innermost loop at once. It's only allowed inside `FOR`, `WHILE` and `REPEAT`.
- `RETURN;` ends the program (or function block) for this cycle. The next cycle starts from the top again.

```st
PROGRAM Search
VAR values : ARRAY[1..5] OF INT := [4, 8, 15, 16, 23]; i, found : INT; END_VAR
    found := 0;
    FOR i := 1 TO 5 DO
        IF values[i] = 15 THEN
            found := i;
            EXIT;
        END_IF;
    END_FOR;
END_PROGRAM
```

## The watchdog

A program that never finishes its cycle would freeze the PLC. soft_plc counts loop rounds: more than **1 000 000 in one cycle** stops the PLC with "watchdog: more than 1000000 loop iterations in one cycle". A cycle that's just slow (longer than 10 ms) isn't stopped; it's counted as an **overrun** and the next cycle starts late (see `STATUS` in [chapter 7](07-memory-s7-and-online.md)).

A classic way to trip the watchdog: `FOR i := 0 TO 32767 DO` with an INT `i`. After 32767, `i + 1` wraps to −32768, which is again `<= 32767`, so the loop never ends. Use a DINT for loops that go to the top of the INT range.
