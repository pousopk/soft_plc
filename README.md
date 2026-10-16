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

Measured on this machine (Release build, µs per cycle):

| Program | Tree-walker | Bytecode VM | Speed-up |
|---|---|---|---|
| `examples/benchmark.st` | 82.2 | 30.3 | 2.7× |
| `examples/loops.st` | 8.2 | 3.7 | 2.2× |
| `examples/function_blocks.st` | 0.63 | 0.28 | 2.2× |
| `examples/traffic_light.st` | 0.12 | 0.07 | 1.8× |

For an optimized build without sanitizers: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`.

## Structured Text supported so far

- `PROGRAM … END_PROGRAM` with `VAR … END_VAR` blocks
- `FUNCTION_BLOCK … END_FUNCTION_BLOCK` with `VAR_INPUT`, `VAR_OUTPUT` and `VAR`; instances (`t1 : TON;`), calls (`t1(IN := x, PT := T#2s);`) and outputs (`t1.Q`)
- Standard blocks: `TON`, `TOF`, `TP`, `CTU`, `CTD`, `R_TRIG`, `F_TRIG`, `SR`, `RS`
- Types `BOOL`, `INT`, `DINT`, `REAL`, `TIME`; initial values (`count : INT := 5;`); `AT %QX0.0` or `AT %DB1.DBW2` to place a variable in the process image or a data block
- Statements: `:=`, `IF/ELSIF/ELSE`, `CASE` (lists and ranges), `FOR … BY`, `WHILE`, `REPEAT … UNTIL`, `EXIT`, `RETURN`
- Operators: `+ - * / MOD **`, `= <> < <= > >=`, `AND & OR XOR NOT`
- Literals: `42`, `1_000`, `16#FF`, `2#1010`, `3.14`, `1.5E3`, `TRUE`, `FALSE`, `T#1m30s`; comments `(* … *)` and `//`

## Layout

| Path | Contents |
|---|---|
| `src/core/` | Addresses, process image, program and I/O interfaces, scan cycle |
| `src/stdlib/` | Standard function blocks in C++: timers, counters, edges, flip-flops |
| `src/st/` | Structured Text: lexer, syntax tree, parser, analyzer (type checks), interpreter, bytecode compiler and VM, standard block bridge |
| `src/monitor/` | Online monitoring: request handling, TCP server and client |
| `src/s7/` | S7 server (Snap7) sharing the process image with S7 clients |
| `cmake/` | Snap7 build recipe and source patches |
| `src/demo/` | Demo program and console I/O driver used by `main.cpp` |
| `src/util/` | Small general-purpose helpers |
| `tools/` | `st_parse` and `st_bench` (command line), and `monitor_gui/` (the `soft_plc_monitor` GUI) |
| `examples/` | Example Structured Text programs |
| `tests/` | doctest unit tests, mirroring `src/` |
| `docs/lessons/` | What each milestone teaches about PLCs and C++, with review questions |

## Components

- **Scan cycle engine:** read inputs → run program → write outputs, fixed cycle time, watchdog, cycle statistics.
- **Memory model:** `%I`, `%Q`, `%M` areas and data blocks with bit, byte and word access.
- **Data types:** BOOL, INT, DINT, REAL, TIME, STRING, arrays, structs.
- **Structured Text:** lexer → parser → AST → type checking → tree-walking interpreter or bytecode compiler + VM.
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
