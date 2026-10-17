# 7. Memory, S7 and going online

## The memory areas

| Area | Address | Size in soft_plc | Meaning |
|---|---|---|---|
| Inputs | `%I` | 16 bytes | copied from the inputs at the start of every cycle |
| Outputs | `%Q` | 16 bytes | copied to the outputs at the end of every cycle |
| Markers | `%M` | 256 bytes | memory that's kept between cycles |
| Data blocks | `%DBn` | as needed | numbered blocks of memory, Siemens style |

How to write an address:

| Size | Inputs, outputs, markers | Data blocks |
|---|---|---|
| bit | `%IX0.3` or `%I0.3`, `%QX1.0`, `%MX10.7` | `%DB1.DBX0.3` |
| byte | `%IB0`, `%QB1`, `%MB10` | `%DB1.DBB2` |
| word (16 bits) | `%IW0`, `%MW10` | `%DB1.DBW4` |
| double word (32 bits) | `%ID0`, `%MD20` | `%DB1.DBD8` |

Words and double words are **big-endian**, as on an S7: in `%MW10`, byte 10 is the high byte. Areas overlap the same way too: `%MW10` is `%MB10` and `%MB11`, and `%MX10.0` is the lowest bit of `%MB10`.

Data blocks don't need to be declared: every DB used with `AT` is created when the program starts, just big enough. To make one bigger or to create one the program doesn't use, start soft_plc with `--db NUMBER:SIZE`, e.g. `--db 20:256`.

## Arrays and structs: the S7 layout

An array or struct placed with `AT` follows Siemens' rules for standard access:

- A **BOOL** takes the next free bit. Consecutive BOOLs share a byte.
- **INT, DINT, REAL and TIME** start at the next **even** byte.
- A **struct** starts at an even byte and its size is rounded up to an even number of bytes.
- An **array** starts at an even byte; its elements follow each other with no gaps, and its size is rounded up to an even number of bytes. In an `ARRAY OF BOOL`, the elements are packed bit after bit.
- In a multi-dimensional array, the **last** index changes fastest: `[1,1]`, `[1,2]`, … `[2,1]`.

For example (`examples/inspection.st`):

```text
TYPE Result : STRUCT              offset in the struct
    ok : BOOL;                    +0.0
    nok : BOOL;                   +0.1
    sequence : INT;               +2      (the next even byte)
    score : REAL;                 +4
    posX : REAL;                  +8
    posY : REAL;                  +12
END_STRUCT;                       16 bytes in total

cameras AT %DB10.DBB0 : ARRAY[1..4] OF Result;
    cameras[1] at DBB0, cameras[2] at DBB16, cameras[3] at DBB32, cameras[4] at DBB48
    cameras[2].score is %DB10.DBD20
```

Without `AT`, arrays and structs live in the program's own memory, where none of this applies.

## Running and options

```bash
./build/soft_plc program.st                    # run it
./build/soft_plc --monitor program.st          # ... and accept monitoring connections on 127.0.0.1:5020
./build/soft_plc --s7 program.st               # ... and be an S7 server on 127.0.0.1:102 (rack 0, slot 1)
./build/soft_plc --s7=1102 --db 20:256 program.st
./build/soft_plc --engine=tree program.st      # use the tree-walking interpreter instead of the bytecode VM
```

The cycle time is 10 ms. Port 102 needs extra rights on Linux (see the README); any port above 1023 doesn't.

## STOP

These stop the PLC while it runs:
- a division by zero (integers),
- an array index outside the bounds,
- the watchdog (more than 1 000 000 loop rounds in one cycle),
- a `FOR` with `BY 0`,
- an `AT` address outside its area or data block.

In STOP, the program no longer runs and **all outputs are switched off** (the safe state). The reason is printed with its line and column. Without `--monitor` or `--s7`, soft_plc then exits. With them, it stays up in STOP so connected tools can see what happened; S7 clients see the CPU in STOP.

## Going online

With `--monitor`, soft_plc answers requests between cycles. Use the GUI:

```bash
./build/soft_plc --monitor examples/inspection.st
./build/soft_plc_monitor                                  # a table of every variable, write, force, trend plot
./build/soft_plc_monitor --plot 'cameras[1].score'        # start with a variable in the trend (quote the brackets)
```

or type the requests yourself, one per line (`nc 127.0.0.1 5020`):

| Request | Answer | |
|---|---|---|
| `LIST` | `OK count:INT lamp:BOOL t1.ET:TIME …` | every variable, by path |
| `READ count t1.ET` | `OK 42 T#1s200ms` | `?` for a name that doesn't exist |
| `WRITE count 100` | `OK` | once: the program may overwrite it next cycle |
| `FORCE lamp TRUE` | `OK` | held at that value every cycle, until `UNFORCE` |
| `UNFORCE lamp` | `OK` | |
| `FORCES` | `OK lamp=TRUE` | |
| `STATUS` | `OK RUN cycles=1234 cycle_ms=10 avg_us=12.5 max_us=80.1 overruns=0` | in STOP: `OK STOP … error=…` |

Paths are case-insensitive: `fast.offTimer.ET`, `cameras[2].score`, `grid[1,3]`. Values are written as in a program: `TRUE`, `-5`, `16#FF`, `2.5`, `T#2s`.

A forced variable is written just before the program runs (so the program reads the forced value) and again just after (so the program's own writes don't reach the outputs). Forcing an input is the usual way to test a program without hardware.

## S7 clients

With `--s7`, S7 clients (plc_gui, Snap7, an HMI) can read and write the inputs, outputs, markers and data blocks, as with a real S7 using **PUT/GET** with standard access. `examples/plc_gui_s7_demo.yaml` and `examples/plc_gui_inspection.yaml` are ready-made `plc_gui` configurations.

Clients work on a copy of the memory that is exchanged with the PLC **between cycles**, so the program never sees a value change in the middle of a cycle. A client's write to one bit doesn't disturb other bits in the same byte. As on a real S7, only data read in **one request** is guaranteed to be from the same cycle; a large read split into several requests may not be. That's why `inspection.st` increments each result's `sequence` **last**: a client that sees a new sequence number knows the rest of that result is new too.
