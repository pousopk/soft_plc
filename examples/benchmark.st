(* A deliberately busy program for comparing the two engines:
     ./build-release/st_bench examples/benchmark.st
   Each cycle runs a loop of 200 iterations with integer and REAL arithmetic,
   a CASE, and calls to function blocks. *)

FUNCTION_BLOCK Filter
VAR_INPUT
    in : REAL;
    k  : REAL := 0.1;
END_VAR
VAR_OUTPUT
    out : REAL;
END_VAR
    out := out + k * (in - out);
END_FUNCTION_BLOCK

FUNCTION_BLOCK Counter
VAR_INPUT step : INT := 1; END_VAR
VAR_OUTPUT value : DINT; END_VAR
    value := value + step;
    IF value > 1000000 THEN value := 0; END_IF;
END_FUNCTION_BLOCK

PROGRAM Benchmark
VAR
    i : INT;
    sum : DINT;
    x : REAL;
    mode : INT;
    fast, slow : Filter;
    events : Counter;
    timer : TON;
    output AT %QX0.0 : BOOL;
END_VAR
    FOR i := 1 TO 200 DO
        sum := sum + i * 3 MOD 7;
        x := x * 0.999 + i;
        fast(in := x, k := 0.5);
        slow(in := fast.out);
        mode := i MOD 4;
        CASE mode OF
            0: events(step := 1);
            1, 2: events(step := 2);
        ELSE
            events();
        END_CASE;
        IF sum > 100000 THEN
            sum := 0;
        END_IF;
    END_FOR;
    timer(IN := NOT timer.Q, PT := T#1s);
    output := slow.out > 100.0 AND timer.Q;
END_PROGRAM
