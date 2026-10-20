#include "poller.hpp"
#include "monitor/monitor_client.hpp"
#include "util/text.hpp"
#include <chrono>
#include <map>
#include <optional>

namespace softplc::monitor {

namespace {

using namespace std::chrono_literals;

// Replies are words separated by spaces; a quoted text ('Hello World') is one word.
std::vector<std::string> words(const std::string& text) { return splitQuotedWords(text); }

// Strips the leading "OK", or returns nothing for an "ERR" reply.
std::optional<std::string> okPayload(const std::string& reply) {
    if (reply.rfind("OK", 0) != 0) {
        return std::nullopt;
    }
    return reply.size() > 3 ? reply.substr(3) : std::string();
}

// Sleeps in small steps so a stop request is noticed quickly.
void sleepFor(std::chrono::milliseconds duration, const std::stop_token& stop) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(10ms);
    }
}

} // namespace

Poller::Poller() : thread_([this](std::stop_token stop) { run(stop); }) {}

Poller::~Poller() = default; // ~jthread: request_stop(), then join()

void Poller::connect(std::string host, std::uint16_t port) {
    std::lock_guard lock(mutex_);
    host_ = std::move(host);
    port_ = port;
    wanted_ = true;
    ++generation_;
}

void Poller::disconnect() {
    std::lock_guard lock(mutex_);
    wanted_ = false;
    ++generation_;
}

void Poller::send(std::string command) {
    std::lock_guard lock(mutex_);
    commands_.push_back(std::move(command));
}

Snapshot Poller::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void Poller::run(std::stop_token stop) {
    std::optional<MonitorClient> client;
    std::uint64_t clientGeneration = 0;
    std::vector<std::pair<std::string, std::string>> variables; // path, type (from LIST)

    const auto setDisconnected = [&](std::string why) {
        client.reset();
        std::lock_guard lock(mutex_);
        snapshot_.connected = false;
        snapshot_.connectionError = std::move(why);
    };

    while (!stop.stop_requested()) {
        std::string host;
        std::uint16_t port = 0;
        bool wanted = false;
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(mutex_);
            host = host_;
            port = port_;
            wanted = wanted_;
            generation = generation_;
        }

        if (!wanted) {
            if (client) {
                setDisconnected("disconnected");
            }
            sleepFor(100ms, stop);
            continue;
        }

        // Connect, or reconnect if the user picked a different address.
        if (!client || clientGeneration != generation) {
            auto connected = MonitorClient::connect(host, port, 1s);
            if (!connected) {
                setDisconnected(connected.error());
                sleepFor(1s, stop);
                continue;
            }
            client = std::move(*connected);
            clientGeneration = generation;

            const auto list = client->request("LIST");
            const auto payload = list ? okPayload(*list) : std::nullopt;
            if (!payload) {
                setDisconnected(list ? *list : list.error());
                sleepFor(1s, stop);
                continue;
            }
            variables.clear();
            for (const auto& entry : words(*payload)) {
                const auto colon = entry.rfind(':');
                variables.emplace_back(entry.substr(0, colon), entry.substr(colon + 1));
            }
        }

        // One poll: queued commands first, then the status, the values and the forces.
        std::deque<std::string> commands;
        {
            std::lock_guard lock(mutex_);
            commands.swap(commands_);
        }
        std::string lastReply;
        bool failed = false;
        std::string failure;
        for (const auto& command : commands) {
            auto reply = client->request(command);
            if (!reply) {
                failed = true;
                failure = reply.error();
                break;
            }
            lastReply = command + "  ->  " + *reply;
        }

        Snapshot next;
        if (!failed) {
            const auto status = client->request("STATUS");
            std::string readRequest = "READ";
            for (const auto& [path, type] : variables) {
                readRequest += " " + path;
            }
            const auto values = variables.empty() ? tl::expected<std::string, std::string>("OK")
                                                  : client->request(readRequest);
            const auto forces = client->request("FORCES");
            if (!status || !values || !forces) {
                failed = true;
                failure = !status ? status.error() : !values ? values.error() : forces.error();
            } else {
                // STATUS: "OK RUN cycles=... [error=...]"
                const auto statusText = okPayload(*status).value_or("");
                const auto space = statusText.find(' ');
                next.state = statusText.substr(0, space);
                auto rest = space == std::string::npos ? std::string() : statusText.substr(space + 1);
                if (const auto errorAt = rest.find("error="); errorAt != std::string::npos) {
                    next.stopError = rest.substr(errorAt + 6);
                    rest = rest.substr(0, errorAt);
                }
                next.statistics = rest;

                // FORCES: "OK path=value path=value"
                std::map<std::string, bool> forced;
                for (const auto& entry : words(okPayload(*forces).value_or(""))) {
                    forced[entry.substr(0, entry.find('='))] = true;
                }

                const auto valueWords = words(okPayload(*values).value_or(""));
                for (std::size_t i = 0; i < variables.size(); ++i) {
                    VariableRow row;
                    row.path = variables[i].first;
                    row.type = variables[i].second;
                    row.value = i < valueWords.size() ? valueWords[i] : "?";
                    row.forced = forced.count(row.path) > 0;
                    next.variables.push_back(std::move(row));
                }
            }
        }

        if (failed) {
            setDisconnected(failure);
            sleepFor(1s, stop);
            continue;
        }

        {
            std::lock_guard lock(mutex_);
            next.connected = true;
            next.sequence = snapshot_.sequence + 1;
            next.lastReply = lastReply.empty() ? snapshot_.lastReply : lastReply;
            snapshot_ = std::move(next);
        }
        sleepFor(100ms, stop);
    }
}

} // namespace softplc::monitor
