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
./build/soft_plc                    # demo: blinks %QX0.0 every 500 ms; Ctrl+C to stop
```

For an optimized build without sanitizers: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`.

## Layout

| Path | Contents |
|---|---|
| `src/core/` | Addresses, process image, program and I/O interfaces, scan cycle |
| `src/stdlib/` | Standard function blocks (TON so far) |
| `src/demo/` | Demo program and console I/O driver used by `main.cpp` |
| `tests/` | doctest unit tests, mirroring `src/` |
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
3. ST statements: `IF`, `CASE`, `FOR`, `WHILE`, `VAR` blocks, `PROGRAM`, `FUNCTION_BLOCK`
4. Standard function blocks callable from ST
5. Online monitoring GUI
6. Snap7 server
7. Bytecode compiler and VM, benchmarked against the tree-walking interpreter
