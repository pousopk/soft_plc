(* Function blocks: one definition, many instances.
   Run it with:  ./build/soft_plc examples/function_blocks.st

   Two Blinker instances run side by side, each with its own timers.
   The slow one blinks three times, counted by a CTU, then stops and
   switches on %QX0.2. *)

FUNCTION_BLOCK Blinker
VAR_INPUT
    enable  : BOOL;
    onTime  : TIME := T#500ms;
    offTime : TIME := T#500ms;
END_VAR
VAR_OUTPUT
    out : BOOL;
END_VAR
VAR
    offTimer : TON;   (* runs while the output is off *)
    onTimer  : TON;   (* runs while the output is on *)
END_VAR
    offTimer(IN := enable AND NOT onTimer.Q, PT := offTime);
    onTimer(IN := offTimer.Q, PT := onTime);
    out := offTimer.Q;
END_FUNCTION_BLOCK

PROGRAM Main
VAR
    fastLamp AT %QX0.0 : BOOL;
    slowLamp AT %QX0.1 : BOOL;
    doneLamp AT %QX0.2 : BOOL;

    fast : Blinker;
    slow : Blinker;
    slowRising : R_TRIG;
    blinks : CTU;
END_VAR
    fast(enable := TRUE, onTime := T#250ms, offTime := T#250ms);
    slow(enable := NOT blinks.Q, onTime := T#1s, offTime := T#1s);

    (* CTU only counts rising edges by itself; the R_TRIG is here to show how
       one works: its Q is on for exactly one cycle when slow.out turns on. *)
    slowRising(CLK := slow.out);
    blinks(CU := slowRising.Q, PV := 3);

    fastLamp := fast.out;
    slowLamp := slow.out;
    doneLamp := blinks.Q;
END_PROGRAM
