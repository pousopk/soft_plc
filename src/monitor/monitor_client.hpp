#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <tl/expected.hpp>

namespace softplc::monitor {

// The other end of MonitorServer: sends one request line, waits for one reply
// line. Blocking, so use it from a background thread in a GUI.
class MonitorClient {
public:
    // Connects to host:port. `host` is a name or an IPv4 address.
    static tl::expected<MonitorClient, std::string> connect(const std::string& host, std::uint16_t port,
                                                            std::chrono::milliseconds timeout = std::chrono::seconds(2));

    ~MonitorClient();
    MonitorClient(MonitorClient&& other) noexcept;
    MonitorClient& operator=(MonitorClient&& other) noexcept;
    MonitorClient(const MonitorClient&) = delete;
    MonitorClient& operator=(const MonitorClient&) = delete;

    // Sends `request` and returns the server's reply line (without the newline).
    tl::expected<std::string, std::string> request(std::string_view request);

private:
    explicit MonitorClient(int fd) : fd_(fd) {}

    int fd_ = -1;
    std::string buffer_; // bytes received after the last complete line
};

} // namespace softplc::monitor
