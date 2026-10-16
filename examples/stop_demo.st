(* Shows what happens on a runtime error: the PLC goes to STOP and switches
   every output off. The lamp is on for about 2 seconds, then `divisor`
   reaches 0 and the division fails.
   Run it with:  ./build/soft_plc examples/stop_demo.st *)
PROGRAM StopDemo
VAR
    lamp AT %QX0.0 : BOOL;
    divisor : INT := 200;
    result : INT;
END_VAR

    lamp := TRUE;
    divisor := divisor - 1;
    result := 1000 / divisor;
END_PROGRAM
