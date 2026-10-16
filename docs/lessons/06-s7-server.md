# Lesson 6: Data blocks and the S7 server

Milestone 6 makes the soft-PLC look like a Siemens S7 from the outside. It now has **data blocks** (`%DB1.DBW2`), and an **S7 server** (through the Snap7 library), so any S7 client can read and write its memory: `plc_gui`, an HMI, another PLC, and later `factory_sim`.

```
 S7 clients (plc_gui, HMI, Snap7, ...)
        │  TCP port 102, S7 protocol (binary)
        ▼
 Snap7's threads ──► shared copy (I, Q, M, DB1, DB2 ...)
                            ▲ │   synchronize(), between cycles:
                            │ ▼   bit-by-bit merge of client writes
 scan cycle ───────► process image ◄── program
```

Try it: `./build/soft_plc --s7=1102 examples/s7_demo.st`

---

## Part 1: PLC concepts

### 1.1 Data blocks
**Code:** `src/core/process_image.hpp`

A data block (DB) is a numbered block of memory. It's how Siemens programs organise data, and how other systems usually exchange data with a PLC: an "interface DB" with an agreed layout, like DB1 in `examples/s7_demo.st`:

| Byte | Type | Variable | Written by |
|---|---|---|---|
| 0.0 | BOOL | start | client |
| 0.1 | BOOL | stop | client |
| 2 | REAL | setpoint | client |
| 6 | REAL | speed | PLC |
| 10.0 | BOOL | running | PLC |

Addresses use the Siemens syntax: `%DB1.DBX0.0` (bit), `%DB1.DBB4` (byte), `%DB1.DBW4` (word), `%DB1.DBD6` (double word).

### 1.2 Standard vs. optimized access
A real S7-1200/1500 has two kinds of DBs:
- **Standard** (non-optimized): every variable has a fixed byte offset, exactly like ours. S7 clients (Snap7, `plc_gui`) can only read these, because they address memory by DB number and offset.
- **Optimized**: TIA Portal arranges the memory itself and there are no fixed offsets. Faster inside the PLC, but invisible to offset-based clients.

That's why an interface DB for vision systems, HMIs or other PLCs must have "Optimized block access" **unchecked** in TIA Portal. Our soft-PLC only has standard DBs.

### 1.3 The S7 memory areas
| S7 name | Area | Our address |
|---|---|---|
| PE (Prozess-Eingänge) | inputs | `%I` |
| PA (Prozess-Ausgänge) | outputs | `%Q` |
| MK (Merker) | markers | `%M` |
| DB | data blocks | `%DBn.DB…` |

The German abbreviations are why Siemens uses E/A for inputs/outputs in German-language TIA Portal.

### 1.4 CPU state
Clients can ask the CPU for its state. Our server reports RUN normally and STOP after a runtime error, just like a real S7 when TIA Portal shows the CPU's status LED.

---

## Part 2: Concurrency: a copy, exchanged between cycles

### 2.1 The problem
Snap7 answers clients from **its own threads**, at any moment, also in the middle of our cycle. If clients read and wrote the process image directly:
- a client could read a REAL while the program is half-way through writing it, and get two bytes old and two bytes new: a **torn read**;
- a client's write could change a variable between two lines of the program, which a real PLC never allows.

### 2.2 The solution: double buffering
**Code:** `S7Server::synchronize()`

The server gets its **own copy** of every area (`shared`). Snap7 only ever touches that copy. Between cycles, in the idle task, `synchronize()` locks each area so Snap7 waits, and then:
1. takes what clients changed in `shared` into the process image;
2. copies the process image back into `shared` for clients to read.

Inside a cycle, the program and the clients never touch the same memory.

### 2.3 Merging bit by bit
**Code:** `mergeClientWrites()` in `src/s7/s7_server.cpp`

How do we know what a client changed? We keep a third copy, `published`: what we gave the clients last time. Any bit that differs between `shared` and `published` was changed by a client:

```
changedByClient = shared XOR published
plc = (plc AND NOT changedByClient) OR (shared AND changedByClient)
```

Bit by bit, not byte by byte: if the program sets bit 0 of a byte in the same cycle a client sets bit 7 of it, **both** changes survive. The tests in `tests/s7/s7_server_test.cpp` check exactly that.

---

## Part 3: Working with a C library

### 3.1 Opaque handles
**Code:** `src/s7/s7_server.cpp`

Snap7 has a C interface: `S7Object server = Srv_Create();` returns a **handle**, just a number (`uintptr_t`), and every function takes it: `Srv_StartTo(server, ...)`, `Srv_Destroy(&server)`. C has no classes, so the handle plays the role of `this`. Our `S7Server` class wraps it: the constructor creates it, the destructor destroys it, and copying is deleted so it can't be destroyed twice. That's RAII again, the standard way to use C resources from C++.

`snap7.h` is only included in the `.cpp` file. It defines global names like `byte` and `word` that we don't want spreading into our own code.

### 3.2 Error codes instead of exceptions
C functions return an `int`: 0 for success, an error code otherwise. `Srv_ErrorText()` turns a code into a message. `S7Server::start()` turns that into a `tl::expected` with a helpful message. For port 102, it even includes the exact commands to fix the permission problem.

### 3.3 Memory that must not move
**Code:** `std::vector<std::unique_ptr<SharedArea>> areas_`

`Srv_RegisterArea()` gives Snap7 a **raw pointer** into our buffer, and Snap7 keeps it. If that buffer ever moved (say a `std::vector<SharedArea>` grew and reallocated), Snap7 would write into freed memory. Holding each area through a `unique_ptr` means the `SharedArea` objects stay where they are, whatever happens to the vector. The process image's own buffers never move either: they're created once and never resized (see the comment in `process_image.hpp`).

### 3.4 Fixing a library you don't own
**Code:** `cmake/patch_snap7.cmake`

Snap7 has bugs. `plc_gui` already patches a C++17 compile error and two memory errors found with AddressSanitizer. This milestone found another one: `TS7Worker::SZLSubFuncRead()` is declared `bool` but has no `return` on its normal path. In C++ that's **undefined behaviour**, and GCC 15 turns it into a trap instruction, so the server died with `SIGILL` as soon as a client asked for the CPU state. How it was found:
1. A test crashed with SIGILL.
2. `gdb` showed the exact line in Snap7.
3. Compiling every Snap7 file with `-Wreturn-type` (without `-w`) showed it was the only such function.

The patch is applied automatically by CMake when Snap7 is downloaded, and checks a marker so it's never applied twice. The lesson: **compiler warnings about missing returns are real bugs**, and hiding a library's warnings with `-w` hides them too.

### 3.5 `std::span` as a view into memory owned elsewhere
`SharedArea::plc` is a `std::span<std::uint8_t>`: a pointer plus a size, pointing into the process image. It doesn't own anything, so it costs nothing to copy. The data must simply outlive it, which it does: the process image lives for the whole program.

---

## Part 4: Testing

- **Port 102 needs rights; tests don't use it.** `freePort()` asks the operating system for a free port (bind to port 0, read which one was given), and the server is started there.
- **The test client is Snap7's own client**, the same code `plc_gui` uses, so the tests show real interoperability.
- **The full path is tested:** an ST program in a running scan cycle, with an S7 client writing the setpoint and the start bit, then reading back the speed and the running bit (the last test in `s7_server_test.cpp`).

---

## Review questions

**1.** Why must an interface DB have "Optimized block access" switched off in TIA Portal?
<details><summary>Answer</summary>

S7 clients address memory by DB number and byte offset. Optimized DBs have no fixed offsets, so those clients can't read them. See 1.2.
</details>

**2.** What could go wrong if Snap7's threads read the process image directly?
<details><summary>Answer</summary>

A client could read a value while the program is half-way through writing it (a torn read), or change a variable between two lines of the program. See 2.1.
</details>

**3.** A byte is `0000 0000`. During a cycle, the program sets bit 0 and a client sets bit 7. What's the byte after `synchronize()`?
<details><summary>Answer</summary>

`1000 0001`: the merge is bit by bit, so both changes survive. See 2.3.
</details>

**4.** Why are the server's buffers held through `std::unique_ptr` instead of directly in a `std::vector`?
<details><summary>Answer</summary>

Snap7 keeps raw pointers to them. A vector that grows can move its elements, which would leave Snap7 writing into freed memory; `unique_ptr`s keep the objects in place. See 3.3.
</details>

**5.** A function declared to return `bool` reaches its closing brace without a `return`. What happens in C++?
<details><summary>Answer</summary>

Undefined behaviour: anything can happen. With GCC 15 it became a trap instruction, so the program crashed with SIGILL. See 3.4.
</details>

**6.** Why does the S7 server need extra rights to start on its normal port?
<details><summary>Answer</summary>

S7 uses port 102, and Linux only lets privileged programs listen on ports below 1024. See the README for the two ways to allow it.
</details>
