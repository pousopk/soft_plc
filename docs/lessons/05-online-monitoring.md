# Lesson 5: Online monitoring

Milestone 5 lets you look inside the running PLC from another program, the way TIA Portal "goes online": watch every variable live, write values, **force** them, and plot them over time.

```
soft_plc --monitor program.st                     soft_plc_monitor (GUI)
┌──────────────────────────────┐   TCP, text     ┌─────────────────────────┐
│ scan cycle ─ program         │   one line per  │ background thread polls │
│     │                        │   request       │ STATUS / READ / FORCES  │
│  idle time: answer requests ◄├─────────────────┤ table, write, force,    │
│  (MonitorService)            │  127.0.0.1:5020 │ trend plot              │
└──────────────────────────────┘                 └─────────────────────────┘
```

Try it in two terminals:
```bash
./build/soft_plc --monitor examples/function_blocks.st
./build/soft_plc_monitor --plot fast.out --plot fast.offTimer.ET
```

---

## Part 1: PLC concepts

### 1.1 Write vs. force
**Code:** `src/st/st_program.hpp`

| | What it does | When the program writes the variable |
|---|---|---|
| **Write** (TIA: "modify") | sets the value **once** | the program's value wins from the next cycle on |
| **Force** | holds the value **every cycle** | the forced value always wins |

Try `WRITE blinks.PV 99` in the example: a `READ` straight after shows 3 again, because the program passes `PV := 3` in every call. A force is the only way to override that.

Our force writes the value twice per cycle: **before** the program runs (so the program reads the forced value) and **after** it runs (so the program's own writes are overridden before the outputs go out).

Forcing is powerful and dangerous: a forced output stays on whatever the safety logic says. That's why the GUI marks forced values clearly, with an **F** in orange.

### 1.2 Communication between cycles
**Code:** `ScanCycle::setIdleTask()`

The PLC answers monitoring requests **between cycles**, in the time left before the next one, never in the middle of a cycle. So a READ always sees a consistent state, and a WRITE never changes a variable while the program is half-way through using it. Real PLCs do the same, and that's why heavy communication load can make a PLC's cycle time longer.

### 1.3 STOP stays visible
Without `--monitor`, a runtime error ends `soft_plc`. With `--monitor`, the PLC stays up in STOP so you can connect and see why: `STATUS` returns `OK STOP ... error=line 14, column 20: division by zero`, and the GUI shows it in red.

---

## Part 2: Networking concepts

### 2.1 A text protocol
**Code:** `src/monitor/monitor_service.hpp`

One line of text per request, one line per reply, every reply starting with `OK` or `ERR`:
```
READ fast.out blinks.CV      →  OK TRUE 3
FORCE doneLamp TRUE          →  OK
WRITE blinks.PV abc          →  ERR 'abc' is not an integer
```
Text protocols are slower than binary ones, but you can read them, debug them with `nc`, and test them with plain string comparisons. The S7 protocol in milestone 6 is the binary kind.

### 2.2 TCP is a stream, not messages
**Code:** `MonitorServer::serveClient()`

TCP delivers bytes in order, but not in the pieces they were sent in: one `recv()` can return half a line, or three lines at once. The server collects bytes in a buffer and only processes complete lines (up to `\n`), keeping the rest for later. Every program that uses TCP must do this "framing".

Two other details in the same file:
- `send()` may send only part of the data, so `sendAll()` loops until everything is out.
- `MSG_NOSIGNAL` stops the operating system from killing the whole process with `SIGPIPE` when a client disconnects while we're writing to it.

### 2.3 Only localhost
The server binds to `127.0.0.1`, so only programs on the same computer can connect. An open port that can force outputs is a serious security risk on a machine network.

---

## Part 3: C++ concepts: threads

### 3.1 Who runs what
| Thread | Does |
|---|---|
| scan cycle (main) | runs the program; in its idle time, answers queued requests |
| accept thread | waits for new connections |
| one per client | reads request lines, queues them, waits for the reply, sends it |
| GUI poller (in the monitor) | talks to the PLC so the window never waits for the network |

### 3.2 `std::mutex` and `std::lock_guard`
**Code:** `queueMutex_` in `MonitorServer`

Several threads use the request queue. A `std::mutex` makes sure only one thread at a time touches it. `std::lock_guard lock(mutex)` locks it and unlocks it automatically at the end of the scope (RAII again, as in lesson 3), so you can never forget to unlock, even when an exception is thrown.

Keep locks short: `processPending()` swaps the whole queue out under the lock, then answers the requests **without** holding it, so clients can keep queueing meanwhile.

### 3.3 `std::promise` and `std::future`: waiting for an answer from another thread
**Code:** `MonitorServer::ask()` and `processPending()`

```cpp
// client thread                                    // scan cycle thread
queue_.push_back(Request{line, {}});                for (auto& request : requests)
reply = queue_.back().reply.get_future();               request.reply.set_value(handler(request.line));
return reply.get();   // sleeps until set_value()
```

A `promise` is the "write" end and a `future` the "read" end of a one-time channel. The client thread puts the promise in the queue and waits on the future; the scan cycle fulfils the promise; the client wakes up with the answer. It's the cleanest way to hand one result from one thread to another.

### 3.4 `std::jthread` and `std::stop_token` (C++20)
**Code:** `Poller` in `tools/monitor_gui/poller.cpp`

```cpp
std::jthread thread_([this](std::stop_token stop) { run(stop); });
...
while (!stop.stop_requested()) { ... }
```

A plain `std::thread` must be joined by hand, or the program crashes when it's destroyed. A `std::jthread` asks its thread to stop (through the `stop_token`) and joins it **in its destructor**. The poller's destructor is just `= default`.

### 3.5 `std::atomic<bool>`
`stopping_` in the server is read by several threads and written by one. A plain `bool` would be a **data race**: undefined behaviour. `std::atomic<bool>` makes every read and write indivisible and visible to other threads.

### 3.6 Shutting threads down cleanly
**Code:** `MonitorServer::~MonitorServer()`

Threads blocked in `accept()` or `recv()` won't notice a flag. The destructor calls `shutdown()` on the sockets, which wakes those calls up with an error, so the threads can see `stopping_` and finish. Then it answers any request still waiting with `ERR server stopping`, and finally joins every thread. The test "Destroying the server with a client still waiting doesn't hang" checks exactly this.

### 3.7 Templates for const and non-const
**Code:** `resolvePath()` in `src/st/interpreter.cpp`

Reading a variable needs a `const Instance`, writing needs a non-const one. Instead of writing the path-walking code twice, it's a template: `resolvePath(std::as_const(*root_), path)` for reading, `resolvePath(*root_, path)` for writing. The compiler generates one version for each.

### 3.8 Move-only resource owners
`MonitorClient` owns a socket. Copying it would mean two objects closing the same socket, so copying is deleted. Moving hands the socket over with `std::exchange(other.fd_, -1)`: take the value, and leave `-1` ("nothing") behind, so only the new owner closes it.

---

## Part 4: Testing

- **The service is tested without any network** (`MonitorService::handle()` is just "string in, string out"), so most tests are simple string comparisons.
- **Server tests use port 0**, which asks the operating system for any free port, so tests never clash with a running PLC or with each other.
- **`std::async` plays the client** while the test thread plays the scan cycle, calling `processPending()` until the reply arrives.
- **ThreadSanitizer** (`-fsanitize=thread`) finds data races. It can't be combined with AddressSanitizer, so it needs a separate build:
  ```bash
  cmake -S . -B build-tsan -DSOFT_PLC_SANITIZERS=OFF -DSOFT_PLC_BUILD_GUI=OFF \
        -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
  cmake --build build-tsan -j && ./build-tsan/tests/soft_plc_tests
  ```

---

## Review questions

**1.** You `WRITE` a variable and it changes back on the next cycle. Why, and what do you use instead?
<details><summary>Answer</summary>

The program writes that variable every cycle, so it overwrites your one-time write. A FORCE holds the value every cycle. See 1.1.
</details>

**2.** Why are monitoring requests answered between cycles instead of as soon as they arrive?
<details><summary>Answer</summary>

So they always see a consistent state, and a write never changes a variable while the program is part-way through using it. It also means only one thread ever touches the PLC's variables, so they need no locks. See 1.2.
</details>

**3.** A client sends `READ a\nREAD b\n` and the server's `recv()` returns `READ a\nRE`. What does the server do?
<details><summary>Answer</summary>

It answers the complete line `READ a`, and keeps `RE` in its buffer until the rest of the line arrives. TCP is a stream, so lines must be reassembled. See 2.2.
</details>

**4.** What is a `std::promise` / `std::future` pair used for in the server?
<details><summary>Answer</summary>

To hand the reply from the scan cycle thread (which calls `set_value`) to the waiting client thread (which calls `get`). See 3.3.
</details>

**5.** What's the advantage of `std::jthread` over `std::thread`?
<details><summary>Answer</summary>

It requests a stop and joins automatically in its destructor; a `std::thread` that's destroyed without being joined crashes the program. See 3.4.
</details>

**6.** Why does the server only listen on 127.0.0.1?
<details><summary>Answer</summary>

So only programs on the same computer can connect: a port that can force outputs must not be open to the whole network. See 2.3.
</details>
