#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace softplc::monitor {

// A TCP server for monitoring requests: one line of text per request, one
// line per reply.
//
// Networking happens on background threads (one to accept connections, one
// per client), but requests are *answered* on whichever thread calls
// processPending(): the scan cycle's thread, between cycles. A client thread
// puts each request in a queue together with a std::promise, and waits on the
// matching std::future until the scan cycle has filled in the reply.
//
//   client thread                     scan cycle thread
//   ─────────────                     ─────────────────
//   read "READ x" ──push──► queue ──► processPending(): reply = handler(line)
//   wait on future ◄──────────────────  promise.set_value(reply)
//   send reply
//
// It only listens on 127.0.0.1, so it can't be reached from other computers.
class MonitorServer {
public:
    using Handler = std::function<std::string(std::string_view request)>;

    // Starts listening. Port 0 picks any free port (see port()).
    // Throws std::system_error if the port can't be opened.
    explicit MonitorServer(std::uint16_t port);
    ~MonitorServer(); // stops listening, disconnects clients and joins every thread

    MonitorServer(const MonitorServer&) = delete;
    MonitorServer& operator=(const MonitorServer&) = delete;

    std::uint16_t port() const { return port_; }

    // Answers every request that arrived since the last call, by calling
    // `handler` on the calling thread.
    void processPending(const Handler& handler);

private:
    struct Request {
        std::string line;
        std::promise<std::string> reply;
    };

    void acceptLoop();
    void serveClient(int fd);
    std::string ask(std::string line); // queue a request and wait for its reply

    int listenFd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};

    std::mutex queueMutex_;
    std::deque<Request> queue_;

    std::mutex clientsMutex_;
    std::vector<int> clientFds_;
    std::vector<std::thread> clientThreads_;

    std::thread acceptThread_; // last, so it starts after everything above exists
};

} // namespace softplc::monitor
