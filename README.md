# soft_plc

A software PLC written from scratch: a fixed-time scan cycle, a process image, and a Structured Text (IEC 61131-3) compiler and interpreter.

**To write programs for it, start with the [programming guide](docs/guide/README.md).**

## Decisions

- **Style:** IEC 61131-3 Structured Text syntax, with some Siemens touches such as DB-style memory.
- **Role:** a headless engine first; the monitoring GUI is a separate part that connects to it.
- **Execution:** a tree-walking interpreter first; a bytecode compiler and VM come later.
- **Arrays and structs:** IEC syntax (`ARRAY[1..10] OF INT`, `TYPE … STRUCT`). Every value inside them is a separate "leaf", so indexing is arithmetic. Placed with `AT`, they use Siemens' standard-access layout, so S7 clients find fields at the offsets TIA Portal shows.

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
./build/soft_plc examples/inspection.st       # arrays and structs: four cameras' results in a DB
./build/soft_plc --monitor examples/analog.st # functions: scaling analog inputs, then force rawPressure
```

When a program stops (Ctrl+C or a runtime error), `soft_plc` prints the final value of every variable.

### Online monitoring

```bash
./build/soft_plc --monitor examples/function_blocks.st   # listens on 127.0.0.1:5020 (--monitor=PORT to change)
./build/soft_plc_monitor                                  # the GUI: watch, write, force and plot variables
./build/soft_plc_monitor --plot fast.out --plot slow.out  # start with variables in the trend plot
```

The GUI needs GLFW and OpenGL development files (`libglfw3-dev`, `libgl-dev`); without them the rest still builds. The protocol is one line of text per request, so it can also be used by hand, e.g. `nc 127.0.0.1 5020`, then `READ fast.out`, `FORCE doneLamp TRUE`, `STATUS`. See `src/monitor/monitor_service.hpp` for every command. With `--monitor`, a runtime error leaves the PLC in STOP (instead of exiting) so the monitor can show why.

### S7 server

```bash
./build/soft_plc --s7 examples/s7_demo.st          # an S7 server on 127.0.0.1:102, rack 0, slot 1
./build/soft_plc --s7=1102 examples/s7_demo.st     # on another port (no extra rights needed)
./build/soft_plc --s7 --db 10:256 program.st       # also create DB10 with 256 bytes
```

S7 clients (`plc_gui`, an HMI, Snap7, `factory_sim` later) can then read and write the inputs, outputs, markers and data blocks. Data blocks used with `AT %DBn...` are created automatically, as big as the program needs. `examples/plc_gui_s7_demo.yaml` is a `plc_gui` config for `examples/s7_demo.st`.

S7 always uses port 102, and Linux only lets privileged programs listen on ports below 1024. Either allow it for everyone (`sudo sysctl net.ipv4.ip_unprivileged_port_start=102`) or for this binary only (`sudo setcap cap_net_bind_service=+ep ./build/soft_plc`, needed again after every rebuild).

Snap7 is fetched and patched automatically (`cmake/patch_snap7.cmake`): it fixes a C++17 compile error, two memory bugs, and a missing `return` in Snap7's server code that crashed it as soon as a client asked for the CPU state.

### Two engines

Programs run on a **bytecode VM** by default: each POU is compiled once to instructions for a stack machine. The original **tree-walking interpreter** is still there (`--engine=tree`); both give exactly the same results, and the test suite runs on both (`ctest` runs it twice).

```bash
./build/st_parse --bytecode examples/benchmark.st        # show the compiled bytecode
./build-release/st_bench examples/benchmark.st           # compare the engines' speed
```

Measured on this machine at milestone 7 (Release build, µs per cycle). Arrays and structs (milestone 8) added about 4% more instructions to `benchmark.st`, almost all of them in the tree-walker; the VM is unchanged.

| Program | Tree-walker | Bytecode VM | Speed-up |
|---|---|---|---|
| `examples/benchmark.st` | 82.2 | 30.3 | 2.7× |
| `examples/loops.st` | 8.2 | 3.7 | 2.2× |
| `examples/function_blocks.st` | 0.63 | 0.28 | 2.2× |
| `examples/traffic_light.st` | 0.12 | 0.07 | 1.8× |

For an optimized build without sanitizers: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`.

### Using soft_plc from another project

`add_subdirectory(path/to/soft_plc soft_plc EXCLUDE_FROM_ALL)` gives two libraries: `soft_plc_language` (Structured Text: lexer, parser, analyzer, both engines, the standard library; no networking) and `soft_plc_core` (plus the scan cycle, monitoring and the S7 server). Included like that, soft_plc doesn't build its programs or tests (`SOFT_PLC_BUILD_PROGRAMS` is off unless soft_plc is the top-level project). `ladder_editor` uses `soft_plc_language` to colour ST and show compile errors as you type.

## Structured Text supported so far

- `PROGRAM … END_PROGRAM` with `VAR … END_VAR` blocks
- `FUNCTION_BLOCK … END_FUNCTION_BLOCK` with `VAR_INPUT`, `VAR_OUTPUT` and `VAR`; instances (`t1 : TON;`), calls (`t1(IN := x, PT := T#2s);`) and outputs (`t1.Q`)
- Standard blocks: `TON`, `TOF`, `TP`, `CTU`, `CTD`, `R_TRIG`, `F_TRIG`, `SR`, `RS`
- `FUNCTION Name : TYPE … END_FUNCTION` with `VAR_INPUT` and temporary `VAR`, called in expressions by name (`Scale(raw := x, high := 10.0)`) or in order (`Scale(x, 0.0, 10.0)`); no memory between calls, no recursion
- Standard functions: `ABS`, `SQRT`, `LN`, `LOG`, `EXP`, `SIN`, `COS`, `TAN`, `ASIN`, `ACOS`, `ATAN`, `EXPT`, `TRUNC`, `ROUND`, `CEIL`, `FLOOR`, `MIN`, `MAX`, `LIMIT`, `SEL`, `MUX`, and conversions `INT_TO_DINT`, `INT_TO_REAL`, `DINT_TO_INT`, `DINT_TO_REAL`, `REAL_TO_INT`, `REAL_TO_DINT`, `BOOL_TO_INT`, `BOOL_TO_DINT`, `INT_TO_BOOL`, `DINT_TO_BOOL`, `TIME_TO_DINT`, `DINT_TO_TIME`
- Types `BOOL`, `INT`, `DINT`, `REAL`, `TIME`; initial values (`count : INT := 5;`); `AT %QX0.0` or `AT %DB1.DBW2` to place a variable in the process image or a data block
- Arrays `ARRAY[1..10] OF INT`, also multi-dimensional `ARRAY[1..3, 0..4] OF REAL`; structs in `TYPE … STRUCT … END_STRUCT; END_TYPE`, nested in any combination; access like `line[i].start.x`; whole-array and whole-struct assignment; initial values `[1, 2, 3(0)]` and `(x := 1.0, y := 2.0)`; arrays and structs as function block inputs and outputs, and in data blocks with the S7 layout
- Statements: `:=`, `IF/ELSIF/ELSE`, `CASE` (lists and ranges), `FOR … BY`, `WHILE`, `REPEAT … UNTIL`, `EXIT`, `RETURN`
- Operators: `+ - * / MOD **`, `= <> < <= > >=`, `AND & OR XOR NOT`
- Literals: `42`, `1_000`, `16#FF`, `2#1010`, `3.14`, `1.5E3`, `TRUE`, `FALSE`, `T#1m30s`; comments `(* … *)` and `//`

## Layout

| Path | Contents |
|---|---|
| `src/core/` | Addresses, process image, program and I/O interfaces, scan cycle |
| `src/stdlib/` | Standard function blocks in C++: timers, counters, edges, flip-flops |
| `src/st/` | Structured Text: lexer, syntax tree, parser, analyzer (type checks), interpreter, bytecode compiler and VM, standard blocks and standard functions |
| `src/monitor/` | Online monitoring: request handling, TCP server and client |
| `src/s7/` | S7 server (Snap7) sharing the process image with S7 clients |
| `cmake/` | Snap7 build recipe and source patches |
| `src/demo/` | Demo program and console I/O driver used by `main.cpp` |
| `src/util/` | Small general-purpose helpers |
| `tools/` | `st_parse` and `st_bench` (command line), and `monitor_gui/` (the `soft_plc_monitor` GUI) |
| `examples/` | Example Structured Text programs |
| `tests/` | doctest unit tests, mirroring `src/` |
| `docs/lessons/` | What each milestone teaches about PLCs and C++, with review questions |
| `docs/cpp/` | The C++ in the finished code, one topic per lesson, with review questions |
| `docs/guide/` | The programming guide: how to write programs for soft_plc (every example is compiled by the tests) |

## Components

- **Scan cycle engine:** read inputs → run program → write outputs, fixed cycle time, watchdog, cycle statistics.
- **Memory model:** `%I`, `%Q`, `%M` areas and data blocks with bit, byte and word access.
- **Data types:** BOOL, INT, DINT, REAL, TIME, arrays and structs (STRING is not supported yet).
- **Structured Text:** lexer → parser → AST → type checking → tree-walking interpreter or bytecode compiler + VM.
- **Standard library:** TON, TOF, TP, CTU, CTD, R_TRIG, F_TRIG, SR, RS; standard functions (ABS, SQRT, MIN, MAX, LIMIT, SEL, MUX, ROUND, conversions, …).
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
8. Arrays and structs, with the S7 memory layout for data blocks, and the programming guide
9. FUNCTIONs and the standard functions (math, MIN/MAX/LIMIT/SEL/MUX, rounding, conversions)
