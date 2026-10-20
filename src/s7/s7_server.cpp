#include "s7/s7_server.hpp"
#include <algorithm>
#include <fmt/format.h>
#include <snap7.h>
#include <stdexcept>

namespace softplc::s7 {

namespace {

std::string errorText(int code) {
    char text[256] = {};
    Srv_ErrorText(code, text, sizeof(text));
    return text;
}

// Locks one of the server's areas for as long as the object lives (RAII), so
// Snap7's threads can't read or write it meanwhile.
class AreaLock {
public:
    AreaLock(S7Object server, int code, std::uint16_t index) : server_(server), code_(code), index_(index) {
        Srv_LockArea(server_, code_, index_);
    }
    ~AreaLock() { Srv_UnlockArea(server_, code_, index_); }
    AreaLock(const AreaLock&) = delete;
    AreaLock& operator=(const AreaLock&) = delete;

private:
    S7Object server_;
    int code_;
    std::uint16_t index_;
};

} // namespace

void mergeClientWrites(std::span<std::uint8_t> plc, std::span<std::uint8_t> shared,
                       std::span<std::uint8_t> published) {
    if (plc.size() != shared.size() || plc.size() != published.size()) {
        throw std::invalid_argument("mergeClientWrites: the three buffers must have the same size");
    }
    for (std::size_t i = 0; i < plc.size(); ++i) {
        const auto changedByClient = static_cast<std::uint8_t>(shared[i] ^ published[i]);
        plc[i] = static_cast<std::uint8_t>((plc[i] & ~changedByClient) | (shared[i] & changedByClient));
        shared[i] = plc[i];
        published[i] = plc[i];
    }
}

S7Server::S7Server(ProcessImage& image) : image_(image), server_(Srv_Create()) {
    share(srvAreaPE, 0, image.bytes(Area::Input));
    share(srvAreaPA, 0, image.bytes(Area::Output));
    share(srvAreaMK, 0, image.bytes(Area::Marker));
    shareNewDataBlocks();
}

S7Server::~S7Server() {
    Srv_Stop(server_);
    S7Object server = server_;
    Srv_Destroy(&server);
}

void S7Server::share(int code, std::uint16_t index, std::span<std::uint8_t> plc) {
    if (plc.empty()) {
        return; // S7 has nothing to show for an empty area
    }
    auto area = std::make_unique<SharedArea>();
    area->code = code;
    area->index = index;
    area->plc = plc;
    area->shared.assign(plc.begin(), plc.end());
    area->published = area->shared;
    Srv_RegisterArea(server_, code, index, area->shared.data(), static_cast<int>(area->shared.size()));
    areas_.push_back(std::move(area));
}

// Snap7 allows registering an area while it serves clients: it builds the
// area completely, then stores one pointer to it.
void S7Server::shareNewDataBlocks() {
    for (const auto& [number, bytes] : image_.dataBlocks()) {
        const bool shared = std::any_of(areas_.begin(), areas_.end(), [number = number](const auto& area) {
            return area->code == srvAreaDB && area->index == number;
        });
        if (!shared) {
            share(srvAreaDB, number, image_.dataBlock(number));
        }
    }
}

tl::expected<void, std::string> S7Server::start(std::uint16_t port, const std::string& address) {
    Srv_SetParam(server_, p_u16_LocalPort, &port);
    if (const int result = Srv_StartTo(server_, address.c_str()); result != 0) {
        auto message = fmt::format("can't start the S7 server on {}:{}: {}", address, port, errorText(result));
        if (port < 1024) {
            message += fmt::format(
                "\nPorts below 1024 need extra rights on Linux. Either allow them for everyone:\n"
                "    sudo sysctl net.ipv4.ip_unprivileged_port_start={}\n"
                "or give this program the right to use them:\n"
                "    sudo setcap cap_net_bind_service=+ep ./build/soft_plc",
                port);
        }
        return tl::unexpected(message);
    }
    setRunning(true);
    return {};
}

void S7Server::synchronize() {
    shareNewDataBlocks();
    for (const auto& area : areas_) {
        AreaLock lock(server_, area->code, area->index);
        mergeClientWrites(area->plc, area->shared, area->published);
    }
}

void S7Server::setRunning(bool running) { Srv_SetCpuStatus(server_, running ? S7CpuStatusRun : S7CpuStatusStop); }

int S7Server::clientCount() const {
    int serverStatus = 0;
    int cpuStatus = 0;
    int clients = 0;
    Srv_GetStatus(server_, &serverStatus, &cpuStatus, &clients);
    return clients;
}

} // namespace softplc::s7
