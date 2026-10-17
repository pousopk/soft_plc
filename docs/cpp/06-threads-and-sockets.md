# C++ 6: Threads, sockets and move-only objects

soft_plc runs several things at once: the scan cycle, the monitor server's network threads, Snap7's threads, and in the GUI a polling thread next to the drawing loop. This lesson shows how they share data without stepping on each other.

**Code:** `src/monitor/monitor_server.*`, `src/monitor/monitor_client.*`, `tools/monitor_gui/poller.*`, `src/s7/s7_server.*`

---

## 1. Who runs on which thread

```
soft_plc process
 ├── main thread ─────── the scan cycle: read inputs → program → write outputs → idle task → sleep
 │                          idle task: answer monitor requests, synchronize S7
 ├── accept thread ───── MonitorServer::acceptLoop(): waits for new connections
 ├── client thread ×N ── MonitorServer::serveClient(): one per connected monitor
 └── Snap7 threads ───── inside the Snap7 library: S7 clients (plc_gui, HMI)

soft_plc_monitor process
 ├── main thread ─────── GLFW + ImGui: draws ~60 frames per second
 └── poller thread ───── Poller::run(): talks to the PLC ~10 times per second
```

The **design rule** that keeps this simple: *only the scan cycle thread ever touches the program and the process image.* The other threads never read or write PLC memory; they hand requests over and wait for answers. That's how a real PLC handles communication too: requests are served at a fixed point in the cycle, so the program never sees half a change. (The PLC side is in `docs/lessons/05-online-monitoring.md` and `06-s7-server.md`.)

---

## 2. A data race, and the tool against it: `std::mutex`

If two threads access the same memory at the same time and at least one writes, that's a **data race**, and in C++ a data race is undefined behaviour. In practice you get torn values, lost updates, or crashes that happen once a week.

The monitor server's request queue is shared by the client threads (which add requests) and the scan cycle (which takes them):

```cpp
// src/monitor/monitor_server.hpp
std::mutex queueMutex_;
std::deque<Request> queue_;
```

```cpp
std::string MonitorServer::ask(std::string line) {       // on a client thread
    std::future<std::string> reply;
    {
        std::lock_guard lock(queueMutex_);                // lock...
        if (stopping_) {
            return "ERR server stopping";
        }
        queue_.push_back(Request{std::move(line), {}});
        reply = queue_.back().reply.get_future();
    }                                                     // ...unlock here
    return reply.get();                                   // wait, without holding the lock
}
```

A `std::mutex` lets only one thread at a time past `lock()`; the others wait. `std::lock_guard` is RAII for it (lesson C3): it locks in its constructor and unlocks in its destructor. The extra `{ ... }` braces are there **only to end the lock_guard's life early**, so the lock is released before the long wait.

`std::lock_guard lock(queueMutex_);` without `<std::mutex>` works thanks to C++17 **class template argument deduction** (CTAD): the compiler works out the template argument from the constructor argument.

### Hold the lock briefly: swap the whole queue
```cpp
void MonitorServer::processPending(const Handler& handler) {   // on the scan cycle thread
    std::deque<Request> requests;
    {
        std::lock_guard lock(queueMutex_);
        requests.swap(queue_);            // take everything, leave an empty queue: O(1)
    }
    for (auto& request : requests) {
        request.reply.set_value(handler(request.line));   // the slow part, with no lock held
    }
}
```
`swap` exchanges the two deques' insides (a few pointers) without copying elements. The scan cycle holds the lock for nanoseconds, so client threads are never blocked long, and the handler runs without any lock.

The Poller in the GUI does the same with its command queue (`commands.swap(commands_)`).

---

## 3. Waiting for an answer from another thread: `promise` and `future`

```cpp
struct Request {
    std::string line;
    std::promise<std::string> reply;
};
```

A `std::promise<T>` and its `std::future<T>` are the two ends of a one-shot channel:
- The **future** end: `reply.get()` blocks until a value arrives, then returns it.
- The **promise** end: `set_value(x)` delivers the value and wakes the waiting thread.

```
client thread                         scan cycle thread
─────────────                         ─────────────────
push {line, promise} ──► queue
future.get()  ... waiting ...          processPending(): swap queue
                                       reply = handler(line)
            ◄───────────────────────── promise.set_value(reply)
send reply to the socket
```

It's simpler and safer than a hand-made "flag + condition variable" for each request.

**Every promise must be answered.** If the server shuts down while a client thread is waiting on `get()`, that thread would wait forever and `join()` would hang. The destructor answers them all:

```cpp
// ~MonitorServer
{
    std::lock_guard lock(queueMutex_);
    for (auto& request : queue_) {
        request.reply.set_value("ERR server stopping");
    }
    queue_.clear();
}
```

---

## 4. `std::atomic<bool>`: a flag without a mutex

```cpp
std::atomic<bool> stopping_{false};
...
while (!stopping_) { ... }          // read on the accept and client threads
...
stopping_ = true;                   // written in the destructor, on the main thread
```

For one simple value that several threads read and write, `std::atomic` is enough: every read and write happens as one indivisible step, and other threads are guaranteed to see the change. The same type is used for Ctrl+C (`stopRequested` in `main.cpp`), set from a signal handler.

**Atomic is not a replacement for a mutex** when two values must change together, or for read-check-write sequences. The queue needs a real mutex.

---

## 5. Starting and stopping threads safely

### `std::thread` and `join()`
```cpp
acceptThread_ = std::thread([this] { acceptLoop(); });
...
clientThreads_.emplace_back([this, fd] { serveClient(fd); });
```
A thread runs a callable. Here the lambdas capture `this` (to call member functions) and, for clients, the socket number **by value** (`fd`), so each thread has its own copy.

A `std::thread` must be **joined** (wait for it to finish) before it's destroyed, or the program calls `std::terminate`. `~MonitorServer` does it in a careful order:

```cpp
MonitorServer::~MonitorServer() {
    stopping_ = true;                       // 1. tell everyone
    ::shutdown(listenFd_, SHUT_RDWR);       // 2. wake the accept thread out of accept()
    acceptThread_.join();                   // 3. wait for it, so no new client threads appear
    ::close(listenFd_);
    { lock; shutdown every client socket }  // 4. wake the client threads out of recv()
    { lock; answer every pending promise }  // 5. wake the ones waiting for a reply
    for (auto& thread : clientThreads_) {   // 6. now they can all finish
        thread.join();
    }
}
```
A thread blocked in `accept()` or `recv()` doesn't look at `stopping_`. `shutdown()` on its socket makes the blocking call return, so the thread gets to check the flag and exit.

### Member order matters
```cpp
// src/monitor/monitor_server.hpp
std::mutex queueMutex_;
std::deque<Request> queue_;
std::mutex clientsMutex_;
std::vector<int> clientFds_;
std::vector<std::thread> clientThreads_;

std::thread acceptThread_; // last, so it starts after everything above exists
```
Members are constructed **in the order they're declared** (not the order in the initialiser list). The accept thread uses the other members as soon as it starts, so they must exist first.

### `std::jthread` and `std::stop_token` (C++20)
```cpp
// tools/monitor_gui/poller.cpp
Poller::Poller() : thread_([this](std::stop_token stop) { run(stop); }) {}
Poller::~Poller() = default; // ~jthread: request_stop(), then join()

void Poller::run(std::stop_token stop) {
    while (!stop.stop_requested()) { ... }
}
```
`std::jthread` ("joining thread") fixes `std::thread`'s two annoyances: its destructor **joins automatically**, and it carries a built-in stop flag. If the function takes a `std::stop_token`, the jthread passes one in; the destructor first calls `request_stop()`, then joins. So `~Poller` has nothing to write.

`sleepFor` sleeps in 10 ms steps and checks the token between them, so closing the window never waits for a full one-second reconnect pause.

### A snapshot instead of shared state
```cpp
Snapshot Poller::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;                // a copy
}
```
The GUI draws 60 times a second and must never wait for the network. So the poller builds a complete new `Snapshot` on its own thread, **without** the lock, and only takes the lock to swap it in (`snapshot_ = std::move(next);`). The GUI takes the lock just long enough to copy it. Each side then works on its own copy, at its own speed.

`mutable std::mutex mutex_;`: `snapshot()` is a `const` function but must lock the mutex, which changes the mutex. `mutable` allows that one member to change in `const` functions. That's the standard use of `mutable`.

---

## 6. Sockets: a C API wrapped in C++

The networking uses the operating system's POSIX socket functions directly: `::socket`, `::bind`, `::listen`, `::accept`, `::recv`, `::send`, `::close`. The leading `::` means "the global one, from the C library", not something in our namespaces.

Three details that every socket program needs and are easy to miss:

```cpp
// send() may send only part of the data; keep going until it's all out.
bool sendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}
```
1. **Partial sends:** `send()` may send fewer bytes than asked. Loop until done.
2. **`MSG_NOSIGNAL`:** writing to a socket whose other end has gone normally sends the process `SIGPIPE`, which **kills it**. A monitor closing its window must not kill the PLC.
3. **TCP is a stream, not messages:** one `recv()` may return half a line or three lines. `serveClient` appends to a `buffer` and only processes complete lines up to `'\n'`.

Errors from C functions come back as `-1` with the reason in `errno`. `MonitorServer` turns that into a C++ exception: `throw std::system_error(errno, std::generic_category(), "bind/listen")`, so `main` can print "Address already in use".

---

## 7. Move-only objects: `MonitorClient`

A socket is a number (`int fd_`) that must be closed exactly once. `MonitorClient` owns one:

```cpp
// src/monitor/monitor_client.hpp
class MonitorClient {
public:
    static tl::expected<MonitorClient, std::string> connect(const std::string& host, std::uint16_t port, ...);
    ~MonitorClient();                                        // closes the socket
    MonitorClient(MonitorClient&& other) noexcept;           // move: allowed
    MonitorClient& operator=(MonitorClient&& other) noexcept;
    MonitorClient(const MonitorClient&) = delete;            // copy: forbidden
    MonitorClient& operator=(const MonitorClient&) = delete;
private:
    explicit MonitorClient(int fd) : fd_(fd) {}
    int fd_ = -1;
};
```

- **Copy is deleted:** two objects with the same `fd_` would close it twice. Worse, the second `close` might close a *different* socket that got the same number in the meantime.
- **Move is allowed:** ownership can be handed over. `connect()` returns a client inside an `expected`, and the Poller moves it into a `std::optional<MonitorClient>`. Without move, neither would be possible.
- **The constructor is private**, and the only way to get a client is the `static` factory `connect()`, which returns an error instead of a half-built object if the connection fails. No "not connected yet" state to worry about.

```cpp
MonitorClient::MonitorClient(MonitorClient&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), buffer_(std::move(other.buffer_)) {}
```
`std::exchange(other.fd_, -1)` sets `other.fd_` to −1 and **returns the old value**: take the socket, and leave the other object owning nothing, so its destructor does nothing. That's the standard way to write a move constructor.

**`noexcept`** promises the move never throws. When a `std::vector` grows, it moves its elements to the new memory only if their move constructor is `noexcept` (or they can't be copied at all); otherwise it copies them, so that an exception halfway through can't leave it half-moved. Always mark moves `noexcept` when they can't fail.

The move assignment also guards against **self-assignment** (`if (this != &other)`): `a = std::move(a)` must not close `a`'s own socket.

---

## 8. A C library with threads: Snap7

The S7 server is the same problem with someone else's threads. Snap7 serves clients from its own threads, at any time, reading and writing buffers we registered with `Srv_RegisterArea(server_, code, index, area->shared.data(), size)`.

- Snap7 keeps a **raw pointer** to each buffer, so they must never move. Each `SharedArea` is allocated on its own with `std::make_unique` and kept in a `std::vector<std::unique_ptr<SharedArea>>`. The vector may move its *pointers* when it grows, but the `SharedArea` objects (and their buffers) stay where they are.
- Snap7's locking is `Srv_LockArea` / `Srv_UnlockArea`, a C pair, so it gets wrapped in the RAII `AreaLock` (lesson C3).
- Snap7's handle is a `uintptr_t` "object reference". `S7Server`'s destructor calls `Srv_Stop` and `Srv_Destroy`, so the handle can't leak.

---

## Review questions

**1.** In `MonitorServer::ask`, why is the `lock_guard` inside its own `{ }` block?
<details><summary>Answer</summary>

So the lock is released before `reply.get()`. Waiting for the reply while holding `queueMutex_` would block the scan cycle's `processPending()` from taking the queue, which is the one thing that would produce the reply: a deadlock.
</details>

**2.** What does `requests.swap(queue_)` gain over processing the requests while holding the lock?
<details><summary>Answer</summary>

The lock is held only for the swap (a few pointer exchanges), not while every request is handled. Client threads can keep adding requests meanwhile and are never blocked for long.
</details>

**3.** Why does `~MonitorServer` call `set_value("ERR server stopping")` on every queued request?
<details><summary>Answer</summary>

Client threads may be blocked in `future.get()` waiting for those replies. If nobody ever answered, they'd wait forever and `thread.join()` in the destructor would never return.
</details>

**4.** `stopping_` is a `std::atomic<bool>`, but the queue needs a `std::mutex`. Why not make the queue atomic too?
<details><summary>Answer</summary>

`std::atomic` only works for single values whose reads and writes are indivisible on their own (like a bool or an int). A deque is a whole data structure; `push_back` is many steps that must not interleave with another thread's `swap`. That needs a mutex.
</details>

**5.** In `MonitorServer`, why is `acceptThread_` declared last?
<details><summary>Answer</summary>

Members are constructed in declaration order. The accept thread starts running immediately and uses `clientsMutex_`, `clientFds_` and `clientThreads_`; they must already exist when it starts. (They're also destroyed in reverse order, after the destructor body has joined the thread.)
</details>

**6.** Why is `MonitorClient`'s copy constructor deleted but its move constructor allowed?
<details><summary>Answer</summary>

A copy would make two owners of one socket, and both destructors would close it (possibly closing an unrelated socket that reused the number). A move hands ownership over and leaves the source with `fd_ = -1`, so exactly one object closes the socket.
</details>

**7.** What would happen if a monitor client disconnects in the middle of a reply and `send` were called without `MSG_NOSIGNAL`?
<details><summary>Answer</summary>

The OS would send `SIGPIPE` to the soft_plc process, whose default action is to terminate it. The PLC would stop because someone closed a monitor window. With `MSG_NOSIGNAL`, `send` just returns an error and only that client's thread ends.
</details>

**8.** `Poller::snapshot()` is `const`, yet it locks `mutex_`. How does that compile?
<details><summary>Answer</summary>

`mutex_` is declared `mutable`, which allows it to be changed inside `const` member functions. Locking a mutex doesn't change the object's visible state, so this is the textbook use of `mutable`.
</details>
