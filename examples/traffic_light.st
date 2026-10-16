(* A traffic light as a state machine.
   Run it with:  ./build/soft_plc examples/traffic_light.st

   There are no timers yet (they come in milestone 4), so time is measured
   by counting cycles: the cycle time is 10 ms, so 100 ticks = 1 second. *)
PROGRAM TrafficLight
VAR
    red    AT %QX0.0 : BOOL;
    yellow AT %QX0.1 : BOOL;
    green  AT %QX0.2 : BOOL;

    state : INT := 0;   (* 0 = red, 1 = red + yellow, 2 = green, 3 = yellow *)
    ticks : INT := 0;   (* cycles spent in the current state *)
END_VAR

    ticks := ticks + 1;

    CASE state OF
        0:  red := TRUE;  yellow := FALSE; green := FALSE;
            IF ticks >= 200 THEN state := 1; ticks := 0; END_IF;

        1:  red := TRUE;  yellow := TRUE;  green := FALSE;
            IF ticks >= 50 THEN state := 2; ticks := 0; END_IF;

        2:  red := FALSE; yellow := FALSE; green := TRUE;
            IF ticks >= 200 THEN state := 3; ticks := 0; END_IF;

        3:  red := FALSE; yellow := TRUE;  green := FALSE;
            IF ticks >= 50 THEN state := 0; ticks := 0; END_IF;
    ELSE
        state := 0;  (* should never happen: recover to red *)
    END_CASE;
END_PROGRAM
