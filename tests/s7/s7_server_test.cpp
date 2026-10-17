#include <doctest/doctest.h>
#include "core/scan_cycle.hpp"
#include "s7/s7_server.hpp"
#include "st/analyzer.hpp"
#include "st/st_program.hpp"
#include "support/engine.hpp"
#include "util/file.hpp"
#include <arpa/inet.h>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <snap7.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace softplc;
using namespace std::chrono_literals;

namespace {

// Asks the operating system for a free TCP port (bind to port 0, see which
// one we got, release it). Tests use it instead of the privileged port 102.
std::uint16_t freePort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    ::close(fd);
    return ntohs(address.sin_port);
}

// A Snap7 client, the same library plc_gui uses, connected to our server.
class Client {
public:
    explicit Client(std::uint16_t port) : client_(Cli_Create()) {
        Cli_SetParam(client_, p_u16_RemotePort, &port);
        REQUIRE(Cli_ConnectTo(client_, "127.0.0.1", 0, 1) == 0);
    }
    ~Client() {
        Cli_Disconnect(client_);
        Cli_Destroy(&client_);
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    std::vector<std::uint8_t> readDb(int db, int start, int size) {
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
        REQUIRE(Cli_DBRead(client_, db, start, size, data.data()) == 0);
        return data;
    }
    int writeDb(int db, int start, std::vector<std::uint8_t> data) {
        return Cli_DBWrite(client_, db, start, static_cast<int>(data.size()), data.data());
    }
    std::uint8_t readMarkerByte(int start) {
        std::uint8_t value = 0;
        REQUIRE(Cli_MBRead(client_, start, 1, &value) == 0);
        return value;
    }
    void writeMarkerByte(int start, std::uint8_t value) { REQUIRE(Cli_MBWrite(client_, start, 1, &value) == 0); }
    void writeInputByte(int start, std::uint8_t value) { REQUIRE(Cli_EBWrite(client_, start, 1, &value) == 0); }
    std::uint8_t readOutputByte(int start) {
        std::uint8_t value = 0;
        REQUIRE(Cli_ABRead(client_, start, 1, &value) == 0);
        return value;
    }
    int plcStatus() {
        int status = 0;
        Cli_GetPlcStatus(client_, &status);
        return status;
    }

private:
    S7Object client_;
};

std::array<std::uint8_t, 4> realBytes(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    return {static_cast<std::uint8_t>(bits >> 24), static_cast<std::uint8_t>(bits >> 16),
            static_cast<std::uint8_t>(bits >> 8), static_cast<std::uint8_t>(bits)};
}

float realFrom(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    const auto bits = (std::uint32_t{bytes[at]} << 24) | (std::uint32_t{bytes[at + 1]} << 16) |
                      (std::uint32_t{bytes[at + 2]} << 8) | bytes[at + 3];
    return std::bit_cast<float>(bits);
}

} // namespace

// ---------------------------------------------------------------- Addresses and data blocks

TEST_CASE("Data block addresses parse and format Siemens style") {
    CHECK(parseAddress("%DB1.DBX0.3").value() == Address{Area::DataBlock, AccessSize::Bit, 0, 3, 1});
    CHECK(parseAddress("DB10.DBW4").value() == Address{Area::DataBlock, AccessSize::Word, 4, 0, 10});
    CHECK(parseAddress("%DB2.DBB7").value().size == AccessSize::Byte);
    CHECK(parseAddress("%DB65535.DBD8").value().dbNumber == 65535);
    CHECK(parseAddress("%DB1.DBX0.3").value().toString() == "%DB1.DBX0.3");
    CHECK(parseAddress("%DB10.DBD8").value().toString() == "%DB10.DBD8");

    for (const char* bad : {"%DB0.DBW0", "%DB.DBW0", "%DB1.W0", "%DB1.DB0", "%DB1.DBX0", "%DB1.DBW", "%DB70000.DBW0"}) {
        CAPTURE(bad);
        CHECK_FALSE(parseAddress(bad).has_value());
    }
}

TEST_CASE("The process image holds numbered data blocks") {
    ProcessImage image(0, 0, 0);
    image.addDataBlock(5, 8);
    CHECK(image.hasDataBlock(5));
    CHECK_FALSE(image.hasDataBlock(6));
    CHECK(image.dataBlock(5).size() == 8);

    image.write(parseAddress("%DB5.DBW2").value(), 0x1234);
    CHECK(image.dataBlock(5)[2] == 0x12); // big-endian, like the other areas
    CHECK(image.read(parseAddress("%DB5.DBX3.2").value()) == 1); // 0x34 = 0011'0100

    CHECK_THROWS_AS(image.addDataBlock(5, 4), std::invalid_argument); // already exists
    CHECK_THROWS_AS(image.read(parseAddress("%DB6.DBW0").value()), std::out_of_range);
    CHECK_THROWS_AS(image.read(parseAddress("%DB5.DBD6").value()), std::out_of_range); // past the end
}

TEST_CASE("dataBlocksUsedBy works out which blocks a program needs, and how big") {
    const auto unit = st::compileProgram("PROGRAM P VAR a AT %DB1.DBX0.0 : BOOL; b AT %DB1.DBD8 : REAL;\n"
                                         "c AT %DB7.DBW2 : INT; d AT %MW0 : INT; END_VAR END_PROGRAM")
                          .value();
    CHECK(st::dataBlocksUsedBy(unit) == std::map<std::uint16_t, std::size_t>{{1, 12}, {7, 4}});
}

TEST_CASE("ST variables can live in data blocks") {
    auto unit = st::compileProgram("PROGRAM P VAR input AT %DB2.DBW0 : INT; output AT %DB2.DBD2 : REAL; END_VAR\n"
                                   "output := input * 1.5; END_PROGRAM")
                    .value();
    ProcessImage image(0, 0, 0);
    image.addDataBlock(2, 6);
    st::Interpreter interpreter(std::move(unit), 1'000'000, testEngine());
    image.write(parseAddress("%DB2.DBW0").value(), 10);
    interpreter.runCycle(image, TimePoint{});
    CHECK(std::bit_cast<float>(image.read(parseAddress("%DB2.DBD2").value())) == 15.0F);
}

TEST_CASE("A missing data block puts the program in STOP instead of crashing") {
    ProcessImage image(0, 1, 0); // no DB3
    st::StProgram program(st::compileProgram("PROGRAM P VAR x AT %DB3.DBW0 : INT; END_VAR x := 1; END_PROGRAM").value(),
                          {}, 1'000'000, testEngine());
    program.execute(image, CycleContext{TimePoint{}, 0});
    REQUIRE(program.stopped());
    CHECK(program.stopError()->message == "DB3 doesn't exist");
}

// ---------------------------------------------------------------- Merging client writes

TEST_CASE("mergeClientWrites takes the bits a client changed and keeps the PLC's other bits") {
    std::vector<std::uint8_t> plc{0b0000'0001};       // the program set bit 0 this cycle
    std::vector<std::uint8_t> published{0b0000'0000}; // what clients saw after the last cycle
    std::vector<std::uint8_t> shared{0b1000'0000};    // a client set bit 7 meanwhile

    s7::mergeClientWrites(plc, shared, published);

    CHECK(plc[0] == 0b1000'0001);       // both changes survive
    CHECK(shared[0] == 0b1000'0001);    // and clients see the result
    CHECK(published[0] == 0b1000'0001);
}

TEST_CASE("mergeClientWrites: a client clearing a bit wins over the PLC's old value") {
    std::vector<std::uint8_t> plc{0xFF};
    std::vector<std::uint8_t> published{0xFF};
    std::vector<std::uint8_t> shared{0xF0}; // the client cleared the low four bits
    s7::mergeClientWrites(plc, shared, published);
    CHECK(plc[0] == 0xF0);
}

TEST_CASE("mergeClientWrites with no client writes just publishes the PLC's memory") {
    std::vector<std::uint8_t> plc{1, 2, 3};
    std::vector<std::uint8_t> published{0, 0, 0};
    std::vector<std::uint8_t> shared{0, 0, 0};
    s7::mergeClientWrites(plc, shared, published);
    CHECK(plc == std::vector<std::uint8_t>{1, 2, 3});
    CHECK(shared == plc);
}

// ---------------------------------------------------------------- The server, with a real S7 client

TEST_CASE("S7 clients read and write markers, inputs, outputs and data blocks") {
    ProcessImage image(4, 4, 4);
    image.addDataBlock(1, 8);
    s7::S7Server server(image);
    const auto port = freePort();
    REQUIRE(server.start(port).has_value());
    Client client(port);

    // PLC -> client: values appear after synchronize()
    image.writeByte(Area::Marker, 0, 0x5A);
    image.writeByte(Area::Output, 1, 0x0F);
    image.write(parseAddress("%DB1.DBW2").value(), 0xBEEF);
    CHECK(client.readMarkerByte(0) == 0); // not yet: no cycle boundary has passed
    server.synchronize();
    CHECK(client.readMarkerByte(0) == 0x5A);
    CHECK(client.readOutputByte(1) == 0x0F);
    CHECK(client.readDb(1, 2, 2) == std::vector<std::uint8_t>{0xBE, 0xEF});

    // client -> PLC: writes arrive at the next synchronize()
    client.writeMarkerByte(3, 0x42);
    client.writeInputByte(0, 0x01);
    CHECK(client.writeDb(1, 4, {0x12, 0x34}) == 0);
    CHECK(image.readByte(Area::Marker, 3) == 0); // the program doesn't see it mid-cycle
    server.synchronize();
    CHECK(image.readByte(Area::Marker, 3) == 0x42);
    CHECK(image.readBit(Area::Input, 0, 0));
    CHECK(image.read(parseAddress("%DB1.DBW4").value()) == 0x1234);

    CHECK(server.clientCount() == 1);
}

TEST_CASE("Clients can't read or write outside a data block, or a block that doesn't exist") {
    ProcessImage image(1, 1, 1);
    image.addDataBlock(1, 4);
    s7::S7Server server(image);
    const auto port = freePort();
    REQUIRE(server.start(port).has_value());
    Client client(port);
    CHECK(client.writeDb(1, 2, {1, 2, 3}) != 0); // bytes 2..4, but DB1 has 4 bytes
    CHECK(client.writeDb(9, 0, {1}) != 0);       // no DB9
}

TEST_CASE("Clients see the CPU as RUN or STOP") {
    ProcessImage image(1, 1, 1);
    s7::S7Server server(image);
    const auto port = freePort();
    REQUIRE(server.start(port).has_value());
    Client client(port);
    CHECK(client.plcStatus() == S7CpuStatusRun);
    server.setRunning(false);
    CHECK(client.plcStatus() == S7CpuStatusStop);
}

TEST_CASE("Starting on a busy port fails with a message") {
    ProcessImage image(1, 1, 1);
    s7::S7Server first(image);
    const auto port = freePort();
    REQUIRE(first.start(port).has_value());
    s7::S7Server second(image);
    const auto result = second.start(port);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("can't start the S7 server on 127.0.0.1:") == 0);
}

TEST_CASE("The S7 demo program, controlled by an S7 client like plc_gui") {
    auto unit = st::compileProgram(R"(
        PROGRAM Mixer
        VAR
            start    AT %DB1.DBX0.0 : BOOL;
            stop     AT %DB1.DBX0.1 : BOOL;
            setpoint AT %DB1.DBD2   : REAL;
            speed    AT %DB1.DBD6   : REAL;
            running  AT %DB1.DBX10.0 : BOOL;
            latch : RS;
        END_VAR
            latch(S := start, R1 := stop);
            running := latch.Q1;
            IF running AND speed < setpoint THEN speed := speed + 100.0; END_IF;
        END_PROGRAM)")
                    .value();
    ProcessImage image(1, 1, 1);
    for (const auto& [number, size] : st::dataBlocksUsedBy(unit)) {
        image.addDataBlock(number, size);
    }
    st::StProgram program(std::move(unit), {}, 1'000'000, testEngine());
    struct : IoDriver {
        void readInputs(ProcessImage&) override {}
        void writeOutputs(const ProcessImage&) override {}
    } io;
    ScanCycle cycle(image, io, program, 10ms);
    s7::S7Server server(image);
    const auto port = freePort();
    REQUIRE(server.start(port).has_value());
    cycle.setIdleTask([&] { server.synchronize(); });

    Client client(port);
    const auto setpoint = realBytes(500.0F);
    REQUIRE(client.writeDb(1, 2, {setpoint.begin(), setpoint.end()}) == 0);
    REQUIRE(client.writeDb(1, 0, {0b0000'0001}) == 0); // start

    std::atomic<bool> stop{false};
    std::thread scan([&] { cycle.run(stop); });
    std::this_thread::sleep_for(200ms); // ~20 cycles: enough to reach 500 rpm in steps of 100

    const auto db = client.readDb(1, 0, 11);
    CHECK((db[10] & 1) == 1); // running
    CHECK(realFrom(db, 6) == 500.0F);

    stop = true;
    scan.join();
}

TEST_CASE("A struct in a DB is where an S7 client expects it (examples/inspection.st)") {
    const auto source = readFile(std::string(SOFT_PLC_SOURCE_DIR) + "/examples/inspection.st");
    REQUIRE(source.has_value());
    auto unit = st::compileProgram(*source).value();
    ProcessImage image(1, 1, 1);
    for (const auto& [number, size] : st::dataBlocksUsedBy(unit)) {
        image.addDataBlock(number, size);
    }
    REQUIRE(image.dataBlock(10).size() == 72); // 4 results of 16 bytes, then 8 bytes of statistics

    st::StProgram program(std::move(unit), {}, 1'000'000, testEngine());
    struct : IoDriver {
        void readInputs(ProcessImage&) override {}
        void writeOutputs(const ProcessImage&) override {}
    } io;
    ScanCycle cycle(image, io, program, 10ms);
    s7::S7Server server(image);
    const auto port = freePort();
    REQUIRE(server.start(port).has_value());

    // 1.2 s of simulated time: two triggers, every 500 ms.
    auto now = TimePoint{} + 1h;
    for (int i = 0; i < 120; ++i) {
        cycle.runOnce(now);
        server.synchronize();
        now += 10ms;
    }

    Client client(port);
    const auto db = client.readDb(10, 0, 72);
    const auto& interpreter = program.interpreter();
    for (int cam = 1; cam <= 4; ++cam) {
        CAPTURE(cam);
        const auto base = static_cast<std::size_t>((cam - 1) * 16);
        const auto path = "cameras[" + std::to_string(cam) + "]";
        CHECK(((db[base] & 1) != 0) == std::get<bool>(*interpreter.valueOf(path + ".ok", image)));
        CHECK(((db[base] & 2) != 0) == std::get<bool>(*interpreter.valueOf(path + ".nok", image)));
        CHECK(db[base + 2] * 256 + db[base + 3] == 2); // sequence: two results so far
        CHECK(realFrom(db, base + 4) == static_cast<float>(std::get<double>(*interpreter.valueOf(path + ".score", image))));
        CHECK(realFrom(db, base + 8) == static_cast<float>(std::get<double>(*interpreter.valueOf(path + ".posX", image))));
    }
    CHECK(db[64] * 256 + db[65] == 2); // stats.inspected
    const auto rejected = std::get<std::int64_t>(*interpreter.valueOf("stats.rejected", image));
    CHECK(db[66] * 256 + db[67] == rejected);
    // stats.lastTen[0] and [1] are bits 0 and 1 of byte 68; one is set for each cycle with a reject.
    CHECK(((db[68] & 1) + ((db[68] >> 1) & 1)) == rejected);
    CHECK(db[70] * 256 + db[71] == 2); // stats.next
}
