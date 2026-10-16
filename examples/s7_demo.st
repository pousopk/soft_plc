(* A small "mixer" controlled over S7, from plc_gui or any S7 client.
   Run it with:  ./build/soft_plc --s7 examples/s7_demo.st
   (port 102 needs extra rights, see the README; or use --s7=1102 for tests)
   and point plc_gui at it with examples/plc_gui_s7_demo.yaml.

   DB1 is the interface, laid out like a non-optimized Siemens DB:
     byte 0.0   start        BOOL   written by the client
     byte 0.1   stop         BOOL   written by the client
     byte 2     setpoint     REAL   written by the client (rpm)
     byte 6     speed        REAL   ramps towards the setpoint while running
     byte 10.0  running      BOOL
     byte 10.1  atSpeed      BOOL
     byte 12    starts       INT    how many times it was started
     byte 14    runTime      TIME   total time spent running *)
PROGRAM Mixer
VAR
    start    AT %DB1.DBX0.0 : BOOL;
    stop     AT %DB1.DBX0.1 : BOOL;
    setpoint AT %DB1.DBD2   : REAL;
    speed    AT %DB1.DBD6   : REAL;
    running  AT %DB1.DBX10.0 : BOOL;
    atSpeed  AT %DB1.DBX10.1 : BOOL;
    starts   AT %DB1.DBW12  : INT;
    runTime  AT %DB1.DBD14  : TIME;

    motor    AT %QX0.0 : BOOL;

    latch      : RS;      (* reset wins: stop beats start *)
    startEdge  : R_TRIG;
END_VAR
    latch(S := start, R1 := stop);
    running := latch.Q1;
    motor := running;

    startEdge(CLK := running);
    IF startEdge.Q THEN
        starts := starts + 1;
    END_IF;

    (* Ramp: at most 20 rpm per 10 ms cycle, i.e. 2000 rpm per second. *)
    IF running THEN
        runTime := runTime + T#10ms;
        IF speed < setpoint - 20.0 THEN
            speed := speed + 20.0;
        ELSIF speed > setpoint + 20.0 THEN
            speed := speed - 20.0;
        ELSE
            speed := setpoint;
        END_IF;
    ELSIF speed > 20.0 THEN
        speed := speed - 20.0;
    ELSE
        speed := 0.0;
    END_IF;
    atSpeed := running AND speed = setpoint;
END_PROGRAM
