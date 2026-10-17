(* Analog inputs, the Siemens way: an analog input card gives 0..27648 for
   its full range (for example 4..20 mA), and the program scales that to
   engineering units. Written with FUNCTIONs and standard functions.

   Run it:    ./build/soft_plc --monitor examples/analog.st   (then soft_plc_monitor)
   Try:       FORCE rawPressure 13824     (half range: 5.0 bar)
              FORCE rawPressure 32000     (overflow: the card's "above range" area)
   There's no real input card, so the raw values stay 0 until you force them. *)

(* Like NORM_X then SCALE_X in TIA Portal: 0..27648 -> low..high. Values
   outside the card's range are limited, so a broken wire can't produce
   nonsense further on. *)
FUNCTION ScaleAnalog : REAL
VAR_INPUT
    raw : INT;
    low : REAL;
    high : REAL;
END_VAR
    ScaleAnalog := low + (high - low) * LIMIT(0, raw, 27648) / 27648.0;
END_FUNCTION

(* TRUE when the card reports a value outside its normal range: above
   27648 is overrange, below 0 is underrange (e.g. a broken 4..20 mA wire). *)
FUNCTION OutOfRange : BOOL
VAR_INPUT raw : INT; END_VAR
    OutOfRange := raw > 27648 OR raw < 0;
END_FUNCTION

(* The average of the last values, as a function of an array. *)
FUNCTION Average : REAL
VAR_INPUT values : ARRAY[0..9] OF REAL; END_VAR
VAR i : INT; sum : REAL; END_VAR
    FOR i := 0 TO 9 DO
        sum := sum + values[i];
    END_FOR;
    Average := sum / 10.0;
END_FUNCTION

PROGRAM Analog
VAR
    rawPressure AT %IW0 : INT;      (* 0..10 bar *)
    rawLevel    AT %IW2 : INT;      (* 0..2500 mm *)
    pump        AT %QX0.0 : BOOL;
    fault       AT %QX0.1 : BOOL;
    levelPercent AT %QW2 : INT;     (* to a display, as a whole number *)

    pressure : REAL;                (* bar *)
    level : REAL;                   (* mm *)
    history : ARRAY[0..9] OF REAL;  (* the last 10 pressures, one every 100 ms *)
    next : INT;
    smoothPressure : REAL;
    sample : TON;
END_VAR
    pressure := ScaleAnalog(rawPressure, 0.0, 10.0);
    level := ScaleAnalog(raw := rawLevel, low := 0.0, high := 2500.0);
    fault := OutOfRange(rawPressure) OR OutOfRange(rawLevel);

    (* A rolling average over the last second. *)
    sample(IN := NOT sample.Q, PT := T#100ms);
    IF sample.Q THEN
        history[next] := pressure;
        next := (next + 1) MOD 10;
    END_IF;
    smoothPressure := Average(history);

    (* Pump on below 2 bar, off above 4 bar (in between, it stays as it is). *)
    IF smoothPressure < 2.0 AND NOT fault THEN
        pump := TRUE;
    ELSIF smoothPressure > 4.0 OR fault THEN
        pump := FALSE;
    END_IF;

    levelPercent := REAL_TO_INT(level / 25.0);
END_PROGRAM
