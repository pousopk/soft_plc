#include <doctest/doctest.h>
#include "core/scan_cycle.hpp"
#include "monitor/monitor_client.hpp"
#include "monitor/monitor_server.hpp"
#include "monitor/monitor_service.hpp"
#include "st/analyzer.hpp"
#include "st/st_program.hpp"
#include "st/value_text.hpp"
#include "support/engine.hpp"
#include "util/text.hpp"
#include <chrono>
#include <future>
#include <limits>
#include <thread>

using namespace softplc;
using namespace softplc::monitor;
using namespace std::chrono_literals;

namespace {

class NullIo : public IoDriver {
public:
    void readInputs(ProcessImage&) override {}
    void writeOutputs(const ProcessImage&) override {}
};

const char* kProgram = "PROGRAM Main\n"
                       "VAR\n"
                       "    count : INT;\n"
                       "    lamp AT %QX0.0 : BOOL;\n"
                       "    t : TON;\n"
                       "    divisor : INT := 1;\n"
                       "    result : INT;\n"
                       "END_VAR\n"
                       "    count := count + 1;\n"
                       "    lamp := count > 100;\n"
                       "    t(IN := TRUE, PT := T#1s);\n"
                       "    result := 10 / divisor;\n"
                       "END_PROGRAM\n";

// A PLC with the test program, driven cycle by cycle.
struct Plc {
    Plc() : image(1, 1, 1), program(compile(), {}, 1'000'000, testEngine()), cycle(image, io, program, 10ms),
          service(program, image, cycle, kProgram) {}

    static st::CompilationUnit compile() {
        auto unit = st::compileProgram(kProgram);
        REQUIRE(unit.has_value());
        return std::move(*unit);
    }

    void cycles(int count) {
        for (int i = 0; i < count; ++i) {
            cycle.runOnce(now);
            now += 10ms;
        }
    }

    ProcessImage image;
    NullIo io;
    st::StProgram program;
    ScanCycle cycle;
    MonitorService service;
    TimePoint now = TimePoint{} + 1h;
};

// As a client sends a program in LOAD: one line, \n for line ends, \\ for backslashes.
std::string escaped(std::string_view source) {
    std::string result;
    for (const char c : source) {
        if (c == '\n') {
            result += "\\n";
        } else if (c == '\\') {
            result += "\\\\";
        } else {
            result += c;
        }
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------- Values as text

TEST_CASE("parseValue reads values for each type") {
    using st::DataType;
    CHECK(std::get<bool>(st::parseValue("true", DataType::Bool).value()));
    CHECK_FALSE(std::get<bool>(st::parseValue("0", DataType::Bool).value()));
    CHECK(std::get<std::int64_t>(st::parseValue("-5", DataType::Int).value()) == -5);
    CHECK(std::get<std::int64_t>(st::parseValue("16#FF", DataType::DInt).value()) == 255);
    CHECK(std::get<double>(st::parseValue("2.5", DataType::Real).value()) == 2.5);
    CHECK(std::get<double>(st::parseValue("-7", DataType::Real).value()) == -7.0);
    CHECK(std::get<std::int64_t>(st::parseValue("T#1m30s", DataType::Time).value()) == 90'000);
}

TEST_CASE("parseValue rejects values that don't match the type") {
    using st::DataType;
    CHECK(st::parseValue("maybe", DataType::Bool).error() == "'maybe' is not a BOOL (use TRUE or FALSE)");
    CHECK(st::parseValue("40000", DataType::Int).error() == "40000 doesn't fit in INT (-32768 to 32767)");
    CHECK(st::parseValue("2.5", DataType::Int).error() == "'2.5' is not an integer");
    CHECK(st::parseValue("500", DataType::Time).error() == "'500' is not a TIME (write it like T#2s or T#500ms)");
    CHECK(st::parseValue("1 2", DataType::Int).error() == "'1 2' is not a single value");
    CHECK_FALSE(st::parseValue("$", DataType::Int).has_value());
}

TEST_CASE("parseValue reads the new integer types and bit strings, in any base") {
    using st::DataType;
    CHECK(std::get<std::int64_t>(st::parseValue("16#FFFF", DataType::Word).value()) == 65535);
    CHECK(std::get<std::int64_t>(st::parseValue("16#FFFF", DataType::UInt).value()) == 65535);
    CHECK(std::get<std::int64_t>(st::parseValue("WORD#16#00FF", DataType::Word).value()) == 255);
    CHECK(std::get<std::int64_t>(st::parseValue("2#1010", DataType::Byte).value()) == 10);
    CHECK(std::get<std::int64_t>(st::parseValue("SINT#-5", DataType::SInt).value()) == -5);
    CHECK(std::get<std::int64_t>(st::parseValue("9223372036854775807", DataType::LInt).value()) ==
          std::numeric_limits<std::int64_t>::max());
    CHECK(std::get<std::int64_t>(st::parseValue("-9223372036854775808", DataType::LInt).value()) ==
          std::numeric_limits<std::int64_t>::min());
    CHECK(st::parseValue("16#FFFF", DataType::Int).error() == "65535 doesn't fit in INT (-32768 to 32767)");
    CHECK(st::parseValue("-1", DataType::UDInt).error() == "-1 doesn't fit in UDINT (0 to 4294967295)");
    CHECK(st::parseValue("INT#5", DataType::Word).error() == "'INT#5' is an INT, not a WORD");
}

TEST_CASE("Bit strings are shown in hexadecimal, padded to their width") {
    using st::DataType;
    CHECK(st::formatValue(st::Value{std::int64_t{255}}, DataType::Word) == "16#00FF");
    CHECK(st::formatValue(st::Value{std::int64_t{15}}, DataType::Byte) == "16#0F");
    CHECK(st::formatValue(st::Value{std::int64_t{0xBEEF}}, DataType::DWord) == "16#0000BEEF");
    CHECK(st::formatValue(st::Value{std::int64_t{65535}}, DataType::UInt) == "65535");
}

TEST_CASE("The monitor lists and writes the new types") {
    Plc plc;
    REQUIRE(plc.service.handle("LOAD " + escaped("PROGRAM Main VAR w : WORD := 16#00FF; l : LINT; END_VAR END_PROGRAM"))
                .rfind("OK", 0) == 0);
    plc.cycles(1);
    CHECK(plc.service.handle("LIST").find(" w:WORD") != std::string::npos);
    CHECK(plc.service.handle("READ w") == "OK 16#00FF");
    CHECK(plc.service.handle("WRITE w 16#1234") == "OK");
    CHECK(plc.service.handle("READ w") == "OK 16#1234");
    CHECK(plc.service.handle("WRITE l 9223372036854775807") == "OK");
    CHECK(plc.service.handle("READ l") == "OK 9223372036854775807");
}

TEST_CASE("The monitor reads and writes strings with spaces and quotes") {
    Plc plc;
    REQUIRE(plc.service.handle("LOAD " + escaped("PROGRAM Main VAR msg : STRING; n : INT; c : CHAR; END_VAR END_PROGRAM"))
                .rfind("OK", 0) == 0);
    plc.cycles(1);
    CHECK(plc.service.handle("LIST").find(" msg:STRING") != std::string::npos);
    CHECK(plc.service.handle("WRITE msg 'Hello World'") == "OK");
    CHECK(plc.service.handle("READ msg n") == "OK 'Hello World' 0");
    CHECK(plc.service.handle("WRITE msg 'it$'s'") == "OK");
    CHECK(plc.service.handle("READ msg") == "OK 'it$'s'");
    CHECK(plc.service.handle("WRITE msg '\xC3\xA9'") == "OK");
    CHECK(plc.service.handle("READ msg") == "OK '$E9'");
    CHECK(plc.service.handle("WRITE c 'z'") == "OK");
    CHECK(plc.service.handle("READ c") == "OK 'z'");
    CHECK(plc.service.handle("WRITE msg 5") == "ERR '5' is not a STRING: write it like 'text'");
    CHECK(plc.service.handle("WRITE c 'zz'") == "ERR a CHAR is one character, not 2");
    CHECK(plc.service.handle("FORCE msg 'x y'") == "OK");
    CHECK(plc.service.handle("FORCES").find(" msg='x y'") != std::string::npos);
    REQUIRE(plc.service.handle("LOAD " + escaped("PROGRAM Main VAR msg : STRING; n : INT; c : CHAR; END_VAR END_PROGRAM"))
                .rfind("OK", 0) == 0);
    plc.cycles(1);
    CHECK(plc.service.handle("READ msg") == "OK 'x y'");
}

TEST_CASE("Requests and replies are split into words, keeping quoted texts whole") {
    CHECK(splitQuotedWords("READ a 'b c'  \"d$\"e f\" g") ==
          std::vector<std::string>{"READ", "a", "'b c'", "\"d$\"e f\"", "g"});
    CHECK(splitQuotedWords(" x='it$'s here' y=2 ") == std::vector<std::string>{"x='it$'s here'", "y=2"});
    CHECK(splitQuotedWords("").empty());
}

// ---------------------------------------------------------------- Variable paths

TEST_CASE("The interpreter lists every variable, including inside instances") {
    Plc plc;
    const auto variables = plc.program.interpreter().variables();
    std::vector<std::string> paths;
    for (const auto& variable : variables) {
        paths.push_back(variable.path);
    }
    CHECK(paths == std::vector<std::string>{"count", "lamp", "t.IN", "t.PT", "t.Q", "t.ET", "divisor", "result"});
}

TEST_CASE("describe() finds variables by path, case-insensitively, and spells them as declared") {
    Plc plc;
    const auto& interpreter = plc.program.interpreter();
    CHECK(interpreter.describe("T.et")->path == "t.ET");
    CHECK(interpreter.describe("t.ET")->type == st::DataType::Time);
    CHECK_FALSE(interpreter.describe("t").has_value());       // an instance, not a value
    CHECK_FALSE(interpreter.describe("count.x").has_value()); // count has no members
    CHECK_FALSE(interpreter.describe("t.Missing").has_value());
}

TEST_CASE("setValue writes variables, including AT variables and instance members") {
    Plc plc;
    auto& interpreter = plc.program.interpreter();
    CHECK(interpreter.setValue("count", std::int64_t{40}, plc.image));
    CHECK(std::get<std::int64_t>(*interpreter.valueOf("count", plc.image)) == 40);

    CHECK(interpreter.setValue("lamp", true, plc.image));
    CHECK(plc.image.readBit(Area::Output, 0, 0)); // straight into the process image

    CHECK(interpreter.setValue("t.PT", std::int64_t{250}, plc.image));
    CHECK(std::get<std::int64_t>(*interpreter.valueOf("t.PT", plc.image)) == 250);

    CHECK_FALSE(interpreter.setValue("missing", true, plc.image));
}

// ---------------------------------------------------------------- Forcing

TEST_CASE("A forced variable holds its value whatever the program does") {
    Plc plc;
    REQUIRE(plc.program.force("LAMP", true)); // case-insensitive
    CHECK(plc.program.forces().count("lamp") == 1);

    plc.cycles(3); // the program writes lamp := count > 100, which is FALSE
    CHECK(plc.image.readBit(Area::Output, 0, 0));

    REQUIRE(plc.program.unforce("lamp"));
    plc.cycles(1);
    CHECK_FALSE(plc.image.readBit(Area::Output, 0, 0));
    CHECK_FALSE(plc.program.unforce("lamp")); // not forced any more
}

TEST_CASE("A forced value is what the program reads") {
    Plc plc;
    plc.program.force("count", std::int64_t{1000});
    plc.cycles(1);
    // The program read 1000, added 1 and wrote 1001; then the force put 1000 back.
    CHECK(std::get<std::int64_t>(*plc.program.interpreter().valueOf("count", plc.image)) == 1000);
    CHECK(plc.image.readBit(Area::Output, 0, 0)); // lamp := 1000 > 100
}

// ---------------------------------------------------------------- The service

TEST_CASE("LIST and READ") {
    Plc plc;
    plc.cycles(5);
    CHECK(plc.service.handle("LIST") ==
          "OK count:INT lamp:BOOL t.IN:BOOL t.PT:TIME t.Q:BOOL t.ET:TIME divisor:INT result:INT");
    CHECK(plc.service.handle("READ count lamp t.ET nope") == "OK 5 FALSE T#40ms ?");
    CHECK(plc.service.handle("read count") == "OK 5"); // commands are case-insensitive
    CHECK(plc.service.handle("READ") == "ERR READ needs at least one variable");
}

TEST_CASE("WRITE changes a value once; the program may overwrite it") {
    Plc plc;
    CHECK(plc.service.handle("WRITE count 500") == "OK");
    plc.cycles(1);
    CHECK(plc.service.handle("READ count lamp") == "OK 501 TRUE");

    CHECK(plc.service.handle("WRITE count abc") == "ERR 'abc' is not an integer");
    CHECK(plc.service.handle("WRITE nope 1") == "ERR unknown variable 'nope'");
    CHECK(plc.service.handle("WRITE count") == "ERR usage: WRITE <variable> <value>");
}

TEST_CASE("FORCE, FORCES and UNFORCE") {
    Plc plc;
    CHECK(plc.service.handle("FORCE t.PT T#5s") == "OK");
    CHECK(plc.service.handle("READ t.PT") == "OK T#5s"); // visible straight away
    plc.cycles(2); // the program's call sets PT := T#1s, but the force wins
    CHECK(plc.service.handle("READ t.PT") == "OK T#5s");
    CHECK(plc.service.handle("FORCES") == "OK t.PT=T#5s");

    CHECK(plc.service.handle("UNFORCE t.pt") == "OK");
    CHECK(plc.service.handle("FORCES") == "OK");
    CHECK(plc.service.handle("UNFORCE t.PT") == "ERR 't.PT' is not forced");
    CHECK(plc.service.handle("FORCE lamp maybe") == "ERR 'maybe' is not a BOOL (use TRUE or FALSE)");
}

TEST_CASE("STATUS shows RUN, or STOP with the error") {
    Plc plc;
    plc.cycles(3);
    CHECK(plc.service.handle("STATUS").rfind("OK RUN cycles=3 cycle_ms=10 ", 0) == 0);

    plc.service.handle("WRITE divisor 0");
    plc.cycles(1);
    const auto status = plc.service.handle("STATUS");
    CHECK(status.rfind("OK STOP cycles=4 ", 0) == 0);
    CHECK(status.find("error=line 12, column 18: division by zero") != std::string::npos);
}

TEST_CASE("Unknown and empty requests") {
    Plc plc;
    CHECK(plc.service.handle("BOGUS") ==
          "ERR unknown command 'BOGUS' (try LIST, READ, WRITE, FORCE, UNFORCE, FORCES, STATUS, LOAD, SOURCE)");
    CHECK(plc.service.handle("   ") == "ERR empty request");
}

// ---------------------------------------------------------------- Server and client

namespace {

// Calls processPending() until `future` is ready, like the scan cycle's idle task would.
template <class T> T serveUntilReady(MonitorServer& server, std::future<T>& future, const MonitorServer::Handler& handler) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (future.wait_for(1ms) != std::future_status::ready) {
        server.processPending(handler);
        REQUIRE(std::chrono::steady_clock::now() < deadline);
    }
    return future.get();
}

} // namespace

TEST_CASE("A client's requests are answered on the thread that calls processPending") {
    MonitorServer server(0); // any free port
    REQUIRE(server.port() != 0);

    const auto handlerThread = std::this_thread::get_id();
    bool answeredOnHandlerThread = false;
    const MonitorServer::Handler handler = [&](std::string_view request) {
        answeredOnHandlerThread = std::this_thread::get_id() == handlerThread;
        return "OK you said " + std::string(request);
    };

    auto client = MonitorClient::connect("127.0.0.1", server.port());
    REQUIRE(client.has_value());

    auto reply = std::async(std::launch::async, [&] { return client->request("hello"); });
    const auto result = serveUntilReady(server, reply, handler);
    REQUIRE(result.has_value());
    CHECK(*result == "OK you said hello");
    CHECK(answeredOnHandlerThread);

    // A second request on the same connection
    auto again = std::async(std::launch::async, [&] { return client->request("READ x"); });
    CHECK(serveUntilReady(server, again, handler).value() == "OK you said READ x");
}

TEST_CASE("Several clients can be connected at once") {
    MonitorServer server(0);
    const MonitorServer::Handler handler = [](std::string_view request) { return std::string(request); };

    auto first = MonitorClient::connect("localhost", server.port());
    auto second = MonitorClient::connect("localhost", server.port());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    auto a = std::async(std::launch::async, [&] { return first->request("one"); });
    auto b = std::async(std::launch::async, [&] { return second->request("two"); });
    CHECK(serveUntilReady(server, a, handler).value() == "one");
    CHECK(serveUntilReady(server, b, handler).value() == "two");
}

TEST_CASE("The client reports a timeout when nobody answers") {
    MonitorServer server(0);
    auto client = MonitorClient::connect("127.0.0.1", server.port(), 200ms);
    REQUIRE(client.has_value());
    const auto reply = client->request("STATUS"); // processPending() is never called
    REQUIRE_FALSE(reply.has_value());
    CHECK(reply.error() == "no reply from the PLC (timeout)");
}

TEST_CASE("Connecting to a closed port fails with a message") {
    std::uint16_t port = 0;
    {
        MonitorServer server(0);
        port = server.port();
    } // closed again
    const auto client = MonitorClient::connect("127.0.0.1", port);
    REQUIRE_FALSE(client.has_value());
    CHECK(client.error().find("can't connect to 127.0.0.1:") == 0);
}

TEST_CASE("Destroying the server with a client still waiting doesn't hang") {
    auto server = std::make_unique<MonitorServer>(0);
    auto client = MonitorClient::connect("127.0.0.1", server->port());
    REQUIRE(client.has_value());
    auto reply = std::async(std::launch::async, [&] { return client->request("STATUS"); });
    std::this_thread::sleep_for(50ms); // let the request reach the queue
    server.reset();                    // answers "ERR server stopping" and joins every thread
    const auto result = reply.get();
    if (result) {
        CHECK(*result == "ERR server stopping");
    } // (or the connection closed first: also fine, as long as nothing hangs)
}

TEST_CASE("The whole path: client -> server -> service -> PLC") {
    Plc plc;
    MonitorServer server(0);
    plc.cycle.setIdleTask([&] { server.processPending([&](std::string_view r) { return plc.service.handle(r); }); });

    std::atomic<bool> stop{false};
    std::thread scan([&] { plc.cycle.run(stop); });

    auto client = MonitorClient::connect("127.0.0.1", server.port());
    REQUIRE(client.has_value());
    CHECK(client->request("FORCE lamp TRUE").value() == "OK");
    std::this_thread::sleep_for(50ms);
    CHECK(client->request("READ lamp").value() == "OK TRUE");
    CHECK(client->request("STATUS").value().rfind("OK RUN", 0) == 0);

    stop = true;
    scan.join();
}

// ---------------------------------------------------------------- Online change


TEST_CASE("LOAD changes the running program and keeps its values") {
    Plc plc;
    plc.cycles(5);
    CHECK(plc.service.handle("READ count") == "OK 5");
    std::string changed = kProgram;
    changed.replace(changed.find("count := count + 1;"), 19, "count := count + 100;");
    changed.replace(changed.find("    result : INT;\n"), 18, "    result : INT;\n    extra : BOOL := TRUE;\n");
    // Kept: count, lamp, divisor, result, and the TON's IN, PT, Q and ET.
    CHECK(plc.service.handle("LOAD " + escaped(changed)) == "OK kept=8 initialized=1 extra");
    plc.cycles(1);
    CHECK(plc.service.handle("READ count extra") == "OK 105 TRUE");
}

TEST_CASE("LOAD refuses a program that doesn't compile, and the old one goes on") {
    Plc plc;
    const auto reply = plc.service.handle("LOAD PROGRAM Main\\n    x := 1;\\nEND_PROGRAM");
    CHECK(reply.rfind("ERR line 2, column 5: ", 0) == 0);
    plc.cycles(1);
    CHECK(plc.service.handle("READ count") == "OK 1");
}

TEST_CASE("soft_plc's empty program runs, and a download replaces it") {
    auto empty = st::compileProgram(st::kEmptyProgram);
    REQUIRE(empty.has_value());
    ProcessImage image(1, 1, 1);
    NullIo io;
    st::StProgram program(std::move(*empty), {}, 1'000'000, testEngine());
    ScanCycle cycle(image, io, program, 10ms);
    MonitorService service(program, image, cycle, std::string(st::kEmptyProgram));
    TimePoint now = TimePoint{} + 1h;
    cycle.runOnce(now);
    CHECK(service.handle("LIST") == "OK");

    const char* downloaded = "PROGRAM Main\\nVAR\\n    count AT %DB3.DBW0 : INT;\\nEND_VAR\\n"
                             "    count := count + 1;\\nEND_PROGRAM";
    CHECK(service.handle(std::string("LOAD ") + downloaded) == "OK kept=1 initialized=0"); // AT: its value lives in DB3
    CHECK(image.hasDataBlock(3));
    cycle.runOnce(now + 10ms);
    cycle.runOnce(now + 20ms);
    CHECK(service.handle("READ count") == "OK 2");
}

TEST_CASE("SOURCE gives the running program, escaped like LOAD") {
    Plc plc;
    CHECK(plc.service.handle("SOURCE") == "OK " + escaped(kProgram));
    CHECK(plc.service.handle("source") == "OK " + escaped(kProgram)); // any case, like the others
}

TEST_CASE("SOURCE follows a successful LOAD, not a refused one") {
    Plc plc;
    std::string changed = kProgram;
    changed.replace(changed.find("count + 1"), 9, "count + 2");
    changed.insert(0, "(* a \\ backslash *)\n");
    REQUIRE(plc.service.handle("LOAD " + escaped(changed)).rfind("OK", 0) == 0);
    CHECK(plc.service.handle("SOURCE") == "OK " + escaped(changed));
    REQUIRE(plc.service.handle("LOAD PROGRAM Main\\n    x := 1;\\nEND_PROGRAM").rfind("ERR", 0) == 0);
    CHECK(plc.service.handle("SOURCE") == "OK " + escaped(changed));
}
