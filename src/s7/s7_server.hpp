#pragma once
#include "core/process_image.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <tl/expected.hpp>
#include <vector>

namespace softplc::s7 {

// Merges what S7 clients changed into the PLC's memory, bit by bit, then
// publishes the PLC's memory back to the clients.
//
//   plc        the PLC's memory (the process image)
//   shared     the copy the S7 server reads and writes for its clients
//   published  what we last copied into `shared`
//
// A bit that differs between `shared` and `published` was written by a
// client, so it goes into `plc`; every other bit keeps the PLC's value. Then
// `shared` and `published` become copies of `plc`. Bit by bit (not byte by
// byte), so a client writing one bit doesn't undo the program's change to a
// neighbouring bit in the same byte.
void mergeClientWrites(std::span<std::uint8_t> plc, std::span<std::uint8_t> shared,
                       std::span<std::uint8_t> published);

// An S7 server (via Snap7), so S7 clients (plc_gui, an HMI, another PLC,
// factory_sim) can read and write the soft-PLC's memory like a Siemens S7:
// inputs (PE), outputs (PA), markers (MK) and every data block (DB).
//
// Snap7 serves clients from its own threads, at any moment. So that the
// program never sees memory change in the middle of a cycle, the server
// works on its own copy, and synchronize() exchanges changes between that
// copy and the process image, between cycles (from the scan cycle's idle
// task), with the server's areas locked.
class S7Server {
public:
    // Shares the process image's areas and its data blocks, including ones
    // added later (e.g. by a download), from the next synchronize() on.
    explicit S7Server(ProcessImage& image);
    ~S7Server();

    S7Server(const S7Server&) = delete;
    S7Server& operator=(const S7Server&) = delete;

    // Starts listening. S7 uses TCP port 102, which needs extra rights on
    // Linux (see the README); tests use a high port instead.
    tl::expected<void, std::string> start(std::uint16_t port = 102, const std::string& address = "127.0.0.1");

    // Shares any new data blocks, then exchanges changes with the clients.
    // Call between cycles.
    void synchronize();

    // The CPU state clients see when they ask (e.g. plc_gui, TIA Portal): RUN or STOP.
    void setRunning(bool running);

    int clientCount() const;

private:
    struct SharedArea {
        int code;            // Snap7 area code: srvAreaPE, srvAreaPA, srvAreaMK or srvAreaDB
        std::uint16_t index; // the DB number for data blocks, 0 otherwise
        std::span<std::uint8_t> plc;
        std::vector<std::uint8_t> shared; // the server reads and writes this, from its threads
        std::vector<std::uint8_t> published;
    };

    void share(int code, std::uint16_t index, std::span<std::uint8_t> plc);
    void shareNewDataBlocks();

    ProcessImage& image_;
    std::uintptr_t server_ = 0; // Snap7's S7Object handle
    // unique_ptr: the server keeps a pointer to each `shared` buffer, so the
    // SharedArea objects must never move in memory.
    std::vector<std::unique_ptr<SharedArea>> areas_;
};

} // namespace softplc::s7
