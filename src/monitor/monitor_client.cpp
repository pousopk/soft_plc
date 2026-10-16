#include "monitor/monitor_client.hpp"
#include <cerrno>
#include <cstring>
#include <fmt/format.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

namespace softplc::monitor {

tl::expected<MonitorClient, std::string> MonitorClient::connect(const std::string& host, std::uint16_t port,
                                                                std::chrono::milliseconds timeout) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    const auto service = std::to_string(port);
    if (const int error = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &results); error != 0) {
        return tl::unexpected(fmt::format("can't find host '{}': {}", host, ::gai_strerror(error)));
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        ::freeaddrinfo(results);
        return tl::unexpected(fmt::format("socket: {}", std::strerror(errno)));
    }
    // A reply should come within one cycle; never hang forever if the PLC is gone.
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    const int result = ::connect(fd, results->ai_addr, results->ai_addrlen);
    ::freeaddrinfo(results);
    if (result < 0) {
        const auto message = fmt::format("can't connect to {}:{}: {}", host, port, std::strerror(errno));
        ::close(fd);
        return tl::unexpected(message);
    }
    return MonitorClient(fd);
}

MonitorClient::~MonitorClient() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

// Moving hands the socket over; the moved-from object no longer owns it.
MonitorClient::MonitorClient(MonitorClient&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), buffer_(std::move(other.buffer_)) {}

MonitorClient& MonitorClient::operator=(MonitorClient&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = std::exchange(other.fd_, -1);
        buffer_ = std::move(other.buffer_);
    }
    return *this;
}

tl::expected<std::string, std::string> MonitorClient::request(std::string_view request) {
    if (fd_ < 0) {
        return tl::unexpected(std::string("not connected"));
    }
    std::string line(request);
    line += '\n';
    for (std::size_t sent = 0; sent < line.size();) {
        const auto n = ::send(fd_, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return tl::unexpected(fmt::format("connection lost while sending: {}", std::strerror(errno)));
        }
        sent += static_cast<std::size_t>(n);
    }

    char chunk[4096];
    for (;;) {
        if (const auto newline = buffer_.find('\n'); newline != std::string::npos) {
            std::string reply = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            return reply;
        }
        const auto n = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (n == 0) {
            return tl::unexpected(std::string("the PLC closed the connection"));
        }
        if (n < 0) {
            return tl::unexpected(errno == EAGAIN || errno == EWOULDBLOCK
                                      ? std::string("no reply from the PLC (timeout)")
                                      : fmt::format("connection lost: {}", std::strerror(errno)));
        }
        buffer_.append(chunk, static_cast<std::size_t>(n));
    }
}

} // namespace softplc::monitor
