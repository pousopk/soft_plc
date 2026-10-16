(* Milestone 2 example: assignments with expressions.
   Try: ./build/st_parse examples/expressions.st *)

speed := baseSpeed + 3 * offset;        // * binds tighter than +
level := (inflow - outflow) * 16#0A;    // hexadecimal literal
average := (a + b + c) / 3.0;
alarm := level > 900 AND NOT acknowledged OR emergencyStop;
power := -2 ** 2;                       // unary minus binds tighter than **: (-2) ** 2
