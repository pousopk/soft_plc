#include "monitor/monitor_server.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

namespace softplc::monitor {

namespace {

[[noreturn]] void throwErrno(const char* what) { throw std::system_error(errno, std::generic_category(), what); }

// send() may send only part of the data; keep going until it's all out.
bool sendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        // MSG_NOSIGNAL: if the client has gone, return an error instead of
        // killing the whole process with SIGPIPE.
        const auto n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace

MonitorServer::MonitorServer(std::uint16_t port) {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        throwErrno("socket");
    }
    // Lets the server restart straight away on the same port, without waiting
    // for the operating system to release connections from the last run.
    const int yes = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || ::listen(listenFd_, 8) < 0) {
        const int error = errno;
        ::close(listenFd_);
        throw std::system_error(error, std::generic_category(), "bind/listen");
    }

    socklen_t length = sizeof(address);
    ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);

    acceptThread_ = std::thread([this] { acceptLoop(); });
}

MonitorServer::~MonitorServer() {
    stopping_ = true;

    // shutdown() wakes up threads blocked in accept() or recv() on these sockets.
    ::shutdown(listenFd_, SHUT_RDWR);
    acceptThread_.join();
    ::close(listenFd_);

    {
        std::lock_guard lock(clientsMutex_);
        for (const int fd : clientFds_) {
            ::shutdown(fd, SHUT_RDWR);
        }
    }
    // Client threads may be waiting for replies that will never come.
    {
        std::lock_guard lock(queueMutex_);
        for (auto& request : queue_) {
            request.reply.set_value("ERR server stopping");
        }
        queue_.clear();
    }
    for (auto& thread : clientThreads_) {
        thread.join();
    }
}

void MonitorServer::acceptLoop() {
    while (!stopping_) {
        const int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) {
            if (stopping_) {
                return;
            }
            continue; // e.g. interrupted by a signal: try again
        }
        std::lock_guard lock(clientsMutex_);
        if (stopping_) {
            ::close(fd);
            return;
        }
        clientFds_.push_back(fd);
        clientThreads_.emplace_back([this, fd] { serveClient(fd); });
    }
}

void MonitorServer::serveClient(int fd) {
    std::string buffer;
    char chunk[4096];
    while (!stopping_) {
        const auto n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            break; // the client closed the connection, or we're shutting down
        }
        buffer.append(chunk, static_cast<std::size_t>(n));

        // TCP is a stream, not messages: a recv() may hold half a line or
        // several lines. Answer every complete line and keep the rest.
        for (auto newline = buffer.find('\n'); newline != std::string::npos; newline = buffer.find('\n')) {
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back(); // tolerate Windows-style line ends
            }
            if (!sendAll(fd, ask(std::move(line)) + "\n")) {
                break;
            }
        }
    }

    std::lock_guard lock(clientsMutex_);
    clientFds_.erase(std::remove(clientFds_.begin(), clientFds_.end(), fd), clientFds_.end());
    ::close(fd);
}

std::string MonitorServer::ask(std::string line) {
    std::future<std::string> reply;
    {
        std::lock_guard lock(queueMutex_);
        if (stopping_) {
            return "ERR server stopping";
        }
        queue_.push_back(Request{std::move(line), {}});
        reply = queue_.back().reply.get_future();
    }
    return reply.get(); // blocks until processPending() (or the destructor) answers
}

void MonitorServer::processPending(const Handler& handler) {
    // Take the whole queue at once, then answer without holding the lock, so
    // clients can keep queueing while we work.
    std::deque<Request> requests;
    {
        std::lock_guard lock(queueMutex_);
        requests.swap(queue_);
    }
    for (auto& request : requests) {
        request.reply.set_value(handler(request.line));
    }
}

} // namespace softplc::monitor
