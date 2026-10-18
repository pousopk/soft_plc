# 5. One thread runs the PLC

soft_plc uses several threads: one accepting monitor connections, one per monitor client, Snap7's own threads for S7 clients. But **one** thread does everything that touches the program and its memory: the scan cycle's. The interpreter has no mutex, no atomic, no lock anywhere, and that's the point of the design.

```
                         ┌──────────────── scan cycle thread ─────────────────┐
                         │ read inputs → program → write outputs → idle task: │
 monitor clients ──► queue ──────────────────────────────► answer requests   │
 (their own threads)     │                                  S7: exchange copy │
 S7 clients ──► Snap7's copy of the memory ◄──────────────► (synchronize)     │
 (Snap7's threads)       │                                  sleep until 10 ms │
                         └────────────────────────────────────────────────────┘
```

---

## Why not let other threads touch the program?

The obvious design is "protect the interpreter with a mutex, and let the monitor thread lock it to read a value". Three problems:

1. **Consistency.** A monitor thread reading `count` and `t1.ET` with a lock per read can see `count` from one cycle and `t1.ET` from the next. A PLC promises that the program sees its inputs frozen for a whole cycle, and that the outside sees a consistent state between cycles. Locks per access don't give that; a lock held for the whole cycle would, but then nobody else can do anything during the cycle anyway.
2. **Cost on the hot path.** A lock on every variable access, every cycle, for something that happens a few times a second.
3. **Reasoning.** Code that's touched by several threads has to be reasoned about with every interleaving in mind. The interpreter is the most complicated code in the project; keeping it single-threaded keeps it reasoned about one way.

So the rule is: **other threads never touch PLC memory. They hand over work, and the scan thread does it between cycles.** That's also exactly how a real PLC does it: communication is served by the operating system between OB1 cycles (or in its own time slice), never in the middle of the user program.

---

## How work gets handed over

### Monitor requests: a queue and a promise

A client thread reads a request line from its socket, puts it in a queue with a `std::promise` for the reply, and waits on the matching `std::future`. The scan thread, in its idle time, takes the **whole** queue at once (swapping it with an empty one under the mutex), answers each request, and fulfils each promise (`MonitorServer::processPending`). The mutex protects only the queue, and is held only for the swap.

The client thread waits for its reply, so a request takes up to one cycle to be answered: 10 ms at most. That's the price, and it's small. The benefit: every reply describes the memory **between two cycles**, never in the middle of one, and the handler (`MonitorService`) is ordinary single-threaded code.

### S7 clients: a copy, exchanged between cycles

Snap7 serves S7 clients from its own threads, at any moment, and can't be made to wait for the scan cycle. So it works on **its own copy** of the memory, and `S7Server::synchronize()`, called in the idle task, exchanges changes between that copy and the process image. The program never sees memory change mid-cycle; a client sees the state as of the last exchange.

The guide's chapter 8 explains a consequence for HMIs: one request is consistent, but a large read split into several requests may mix two cycles. That's true of real S7s as well, and it's why `inspection.st` writes a sequence number last.

### The online change: the same road

`LOAD` is just another monitor request, so the program is replaced by the scan thread, between two cycles, without any extra synchronization. The new design feature came for free from the existing rule.

---

## Time is a parameter

The scan thread reads the clock **once per cycle** and passes it down:

```cpp
void ScanCycle::runOnce(TimePoint now);        // the cycle's timestamp
void Interpreter::runCycle(ProcessImage&, TimePoint now);
void Ton::update(bool in, Duration preset, TimePoint now);
```

Nothing the program's behaviour depends on calls `Clock::now()` itself (the console I/O driver reads it, but only to timestamp what it prints). Two reasons:

- **Like a PLC:** every timer in one cycle sees the same instant. Two TONs started in the same cycle finish in the same cycle.
- **Testable:** a test calls `runOnce(t)` with `t` advancing 10 ms per call, and a 5-second timer test takes microseconds, with exactly the same result every run. A timer that read the real clock would make tests slow and flaky.

This is called **dependency injection** of time, and it's the single most useful testability decision in the project.

---

## Keeping the rhythm

`ScanCycle::run` sleeps until the next deadline (`sleep_until`), not for a fixed time (`sleep_for`): the time spent working doesn't make the cycle drift. When a cycle runs over, it doesn't try to catch up with a burst of quick cycles; it counts an overrun and re-aligns the rhythm from there. A burst of catch-up cycles would make timers and edges behave strangely (several cycles a few microseconds apart).

The stop signal is the only thing shared with another thread without a mutex: a `std::atomic<bool>` set by the Ctrl+C handler and read at the top of each loop. An atomic is enough for one flag: there's nothing else to keep consistent with it.

---

## Review questions

**1.** A monitor client sends `READ count t1.ET`. Can the two values come from different cycles? Why not?
<details><summary>Answer</summary>

No. The request is answered by the scan thread in its idle time, between two cycles, all at once. Both values are read while no cycle is running.
</details>

**2.** Why does the monitor server take the whole queue with a swap, instead of popping one request at a time under the lock?
<details><summary>Answer</summary>

So the lock is held only for an instant. While the scan thread answers the requests (which can take a while), client threads can keep queueing new ones without waiting. Popping under the lock one by one would make them wait for each answer.
</details>

**3.** Why do the timers get `now` as a parameter instead of calling `Clock::now()`?
<details><summary>Answer</summary>

So every timer in a cycle sees the same instant (like a PLC's cycle timestamp), and so tests can drive time themselves: a test advances `now` by 10 ms per cycle and checks a 5-second timer in microseconds, the same every run.
</details>

**4.** A cycle takes 25 ms with a 10 ms cycle time. What does `run` do next, and why not run two quick cycles to catch up?
<details><summary>Answer</summary>

It counts an overrun, starts the next cycle right away, and sets the next deadline from now. Catch-up cycles a few microseconds apart would distort edge detection and timers and achieve nothing useful: the time is lost either way.
</details>
