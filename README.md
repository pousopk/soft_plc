# soft_plc

A software PLC written from scratch: a fixed-time scan cycle, a process image, and a Structured Text (IEC 61131-3) compiler and interpreter.

## Decisions

- **Style:** IEC 61131-3 Structured Text syntax, with some Siemens touches such as DB-style memory.
- **Role:** a headless engine first; the monitoring GUI is a separate part that connects to it.
- **Execution:** a tree-walking interpreter first; a bytecode compiler and VM come later.

## Building and running

Requires a C++20 compiler (GCC 11 or newer) and CMake ≥ 3.21. {fmt}, tl::expected and doctest are fetched automatically.

```bash
cmake -S . -B build                 # Debug by default, with AddressSanitizer + UBSan
cmake --build build -j
ctest --test-dir build --output-on-failure

./build/soft_plc                              # built-in C++ blink demo; Ctrl+C to stop
./build/soft_plc examples/traffic_light.st    # run a Structured Text program
./build/soft_plc examples/function_blocks.st  # function blocks, timers and counters
./build/st_parse examples/function_blocks.st  # show how a program is parsed (and check it)
./build/st_parse -e "a := b + 3 * c;"
```

When a program stops (Ctrl+C or a runtime error), `soft_plc` prints the final value of every variable.

For an optimized build without sanitizers: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`.

## Structured Text supported so far

- `PROGRAM … END_PROGRAM` with `VAR … END_VAR` blocks
- `FUNCTION_BLOCK … END_FUNCTION_BLOCK` with `VAR_INPUT`, `VAR_OUTPUT` and `VAR`; instances (`t1 : TON;`), calls (`t1(IN := x, PT := T#2s);`) and outputs (`t1.Q`)
- Standard blocks: `TON`, `TOF`, `TP`, `CTU`, `CTD`, `R_TRIG`, `F_TRIG`, `SR`, `RS`
- Types `BOOL`, `INT`, `DINT`, `REAL`, `TIME`; initial values (`count : INT := 5;`); `AT %QX0.0` to place a variable in the process image
- Statements: `:=`, `IF/ELSIF/ELSE`, `CASE` (lists and ranges), `FOR … BY`, `WHILE`, `REPEAT … UNTIL`, `EXIT`, `RETURN`
- Operators: `+ - * / MOD **`, `= <> < <= > >=`, `AND & OR XOR NOT`
- Literals: `42`, `1_000`, `16#FF`, `2#1010`, `3.14`, `1.5E3`, `TRUE`, `FALSE`, `T#1m30s`; comments `(* … *)` and `//`

## Layout

| Path | Contents |
|---|---|
| `src/core/` | Addresses, process image, program and I/O interfaces, scan cycle |
| `src/stdlib/` | Standard function blocks in C++: timers, counters, edges, flip-flops |
| `src/st/` | Structured Text: lexer, syntax tree, parser, analyzer (type checks), interpreter, standard block bridge |
| `src/demo/` | Demo program and console I/O driver used by `main.cpp` |
| `src/util/` | Small general-purpose helpers |
| `tools/` | Command-line tools (`st_parse`) |
| `examples/` | Example Structured Text programs |
| `tests/` | doctest unit tests, mirroring `src/` |
| `docs/lessons/` | What each milestone teaches about PLCs and C++, with review questions |

## Components

- **Scan cycle engine:** read inputs → run program → write outputs, fixed cycle time, watchdog, cycle statistics.
- **Memory model:** `%I`, `%Q`, `%M` areas and data blocks with bit, byte and word access.
- **Data types:** BOOL, INT, DINT, REAL, TIME, STRING, arrays, structs.
- **Structured Text:** lexer → parser → AST → type checking → interpreter (later bytecode and a VM).
- **Standard library:** TON, TOF, TP, CTU, CTD, R_TRIG, F_TRIG, SR, RS.
- **Online monitoring:** watch and force variables while running.
- **S7 server:** expose memory through Snap7 so `plc_gui` and `factory_sim` can connect.

## Milestones

1. Scan cycle and process image with the program written in C++ (blink, TON)
2. ST expressions: lexer and parser for `a := b + 3 * c;`
3. ST programs: `PROGRAM`, `VAR` blocks, `IF`, `CASE`, `FOR`, `WHILE`, `REPEAT`, type checking, and an interpreter running inside the scan cycle
4. `FUNCTION_BLOCK` and standard function blocks (TON, TOF, CTU, R_TRIG, …) callable from ST
5. Online monitoring GUI
6. Snap7 server
7. Bytecode compiler and VM, benchmarked against the tree-walking interpreter
