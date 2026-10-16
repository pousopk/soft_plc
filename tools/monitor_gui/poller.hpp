#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace softplc::monitor {

struct VariableRow {
    std::string path;
    std::string type;  // "BOOL", "INT", ...
    std::string value; // as the PLC formats it: TRUE, 42, T#1s
    bool forced = false;
};

// Everything the window shows, as one consistent copy.
struct Snapshot {
    bool connected = false;
    std::string connectionError; // why we're not connected, if we aren't
    std::string state;           // "RUN" or "STOP"
    std::string statistics;      // "cycles=... avg_us=..."
    std::string stopError;       // the runtime error, in STOP
    std::vector<VariableRow> variables;
    std::string lastReply;     // the PLC's answer to the last WRITE/FORCE/UNFORCE
    std::uint64_t sequence = 0; // goes up by one with every completed poll
};

// Talks to the PLC on a background thread, so the GUI never waits for the
// network: about ten times a second it asks for the status, every value and
// the forces, and publishes the result as a Snapshot. Commands from the GUI
// (WRITE, FORCE, ...) are queued and sent before the next poll.
class Poller {
public:
    Poller();
    ~Poller(); // stops the thread (std::jthread asks it to stop and joins)

    void connect(std::string host, std::uint16_t port);
    void disconnect();
    void send(std::string command);

    Snapshot snapshot() const; // a copy, safe to use while the poller keeps working

private:
    void run(std::stop_token stop);

    mutable std::mutex mutex_;
    Snapshot snapshot_;
    std::deque<std::string> commands_;
    std::string host_;
    std::uint16_t port_ = 0;
    bool wanted_ = false;          // should we be connected?
    std::uint64_t generation_ = 0; // changes on every connect()/disconnect()

    std::jthread thread_; // last: starts after the members above exist
};

} // namespace softplc::monitor
