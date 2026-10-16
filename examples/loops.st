(* The three loop statements. Every cycle recalculates the same results;
   press Ctrl+C and the final variable values are printed.
   Run it with:  ./build/soft_plc examples/loops.st *)
PROGRAM Loops
VAR
    i : INT;
    sum : DINT;          (* 1 + 2 + ... + 100 *)
    factorial : DINT;    (* 10! *)
    firstSquareOver1000 : INT;
    halvings : INT;      (* how many times 1000.0 can be halved before dropping below 1 *)
    value : REAL;
    countdown : INT;
END_VAR

    sum := 0;
    FOR i := 1 TO 100 DO
        sum := sum + i;
    END_FOR;

    factorial := 1;
    FOR i := 10 TO 1 BY -1 DO
        factorial := factorial * i;
    END_FOR;

    i := 1;
    WHILE TRUE DO
        IF i * i > 1000 THEN
            firstSquareOver1000 := i * i;
            EXIT;
        END_IF;
        i := i + 1;
    END_WHILE;

    value := 1000.0;
    halvings := 0;
    REPEAT
        value := value / 2.0;
        halvings := halvings + 1;
    UNTIL value < 1.0
    END_REPEAT;

    countdown := 5;
    WHILE countdown > 0 DO
        countdown := countdown - 1;
    END_WHILE;
END_PROGRAM
