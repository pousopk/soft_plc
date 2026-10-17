(* A vision station with four cameras, with the PLC interface laid out the
   way it often is in practice: one result UDT per camera, in a DB.

   Run it:     ./build/soft_plc --s7 examples/inspection.st
   or watch:   ./build/soft_plc --monitor examples/inspection.st   (then soft_plc_monitor)
   plc_gui:    examples/plc_gui_inspection.yaml

   There are no real cameras: every 500 ms a "trigger" makes up a score for
   each camera, judges it and publishes the result.

   DB10, standard (non-optimized) layout, the same offsets TIA Portal shows:
     cameras[1]   DBB0    Result (16 bytes)
       .ok        +0.0    BOOL
       .nok       +0.1    BOOL
       .sequence  +2      INT    goes up by one after every new result
       .score     +4      REAL   0..100
       .posX      +8      REAL   position in pixels
       .posY      +12     REAL
     cameras[2]   DBB16
     cameras[3]   DBB32
     cameras[4]   DBB48
     stats        DBB64   Stats (8 bytes)
       .inspected +0      INT
       .rejected  +2      INT
       .lastTen   +4.0    ARRAY[0..9] OF BOOL, packed into bits 4.0 .. 5.1
       .next      +6      INT    where the next result goes in lastTen *)

TYPE
    Result : STRUCT
        ok : BOOL;
        nok : BOOL;
        sequence : INT;
        score : REAL;
        posX : REAL;
        posY : REAL;
    END_STRUCT;

    Stats : STRUCT
        inspected : INT;
        rejected : INT;
        lastTen : ARRAY[0..9] OF BOOL;  (* TRUE = that cycle had a reject *)
        next : INT;
    END_STRUCT;
END_TYPE

(* Judges one score against two limits: below limits[1] is a reject,
   below limits[2] is still good but gives a warning. *)
FUNCTION_BLOCK Judge
VAR_INPUT
    score : REAL;
    limits : ARRAY[1..2] OF REAL;
END_VAR
VAR_OUTPUT
    ok : BOOL;
    warning : BOOL;
END_VAR
    ok := score >= limits[1];
    warning := ok AND score < limits[2];
END_FUNCTION_BLOCK

PROGRAM Inspection
VAR
    cameras AT %DB10.DBB0 : ARRAY[1..4] OF Result;
    stats AT %DB10.DBB64 : Stats;
    rejectGate AT %QX0.0 : BOOL;
    warningLamp AT %QX0.1 : BOOL;

    limits : ARRAY[1..2] OF REAL := [60.0, 75.0];
    judge : Judge;
    trigger : TON;
    seed : DINT := 12345;
    cam : INT;
    anyReject, anyWarning : BOOL;
END_VAR
    trigger(IN := NOT trigger.Q, PT := T#500ms);
    IF trigger.Q THEN
        anyReject := FALSE;
        anyWarning := FALSE;
        FOR cam := 1 TO 4 DO
            (* Stand-in for a camera: a pseudo-random score from 40 to 100. *)
            seed := seed * 1103515245 + 12345;
            cameras[cam].score := 40.0 + ((seed / 65536) MOD 61 + 61) MOD 61;
            cameras[cam].posX := 320 + seed MOD 20;
            cameras[cam].posY := 240 + (seed / 256) MOD 20;

            judge(score := cameras[cam].score, limits := limits);
            cameras[cam].ok := judge.ok;
            cameras[cam].nok := NOT judge.ok;
            (* Last: a client that sees the new sequence number knows the
               rest of the result is new too. *)
            cameras[cam].sequence := cameras[cam].sequence + 1;

            anyReject := anyReject OR cameras[cam].nok;
            anyWarning := anyWarning OR judge.warning;
        END_FOR;

        stats.inspected := stats.inspected + 1;
        IF anyReject THEN
            stats.rejected := stats.rejected + 1;
        END_IF;
        stats.lastTen[stats.next] := anyReject;
        stats.next := (stats.next + 1) MOD 10;

        rejectGate := anyReject;
        warningLamp := anyWarning;
    END_IF;
END_PROGRAM
