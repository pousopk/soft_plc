#include "st/builtins.hpp"
#include "st/parser.hpp"
#include "stdlib/counters.hpp"
#include "stdlib/edges.hpp"
#include "stdlib/timers.hpp"
#include "util/text.hpp"
#include <chrono>
#include <stdexcept>

namespace softplc::st {

namespace {

// The interfaces, written in Structured Text. The order of the variables is
// the order the C++ classes below find them in.
constexpr const char* kInterfaces = R"(
FUNCTION_BLOCK TON
VAR_INPUT IN : BOOL; PT : TIME; END_VAR
VAR_OUTPUT Q : BOOL; ET : TIME; END_VAR
END_FUNCTION_BLOCK

FUNCTION_BLOCK TOF
VAR_INPUT IN : BOOL; PT : TIME; END_VAR
VAR_OUTPUT Q : BOOL; ET : TIME; END_VAR
END_FUNCTION_BLOCK

FUNCTION_BLOCK TP
VAR_INPUT IN : BOOL; PT : TIME; END_VAR
VAR_OUTPUT Q : BOOL; ET : TIME; END_VAR
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTU
VAR_INPUT CU : BOOL; R : BOOL; PV : INT; END_VAR
VAR_OUTPUT Q : BOOL; CV : INT; END_VAR
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTD
VAR_INPUT CD : BOOL; LD : BOOL; PV : INT; END_VAR
VAR_OUTPUT Q : BOOL; CV : INT; END_VAR
END_FUNCTION_BLOCK

FUNCTION_BLOCK R_TRIG
VAR_INPUT CLK : BOOL; END_VAR
VAR_OUTPUT Q : BOOL; END_VAR
END_FUNCTION_BLOCK

FUNCTION_BLOCK F_TRIG
VAR_INPUT CLK : BOOL; END_VAR
VAR_OUTPUT Q : BOOL; END_VAR
END_FUNCTION_BLOCK

FUNCTION_BLOCK SR
VAR_INPUT S1 : BOOL; R : BOOL; END_VAR
VAR_OUTPUT Q1 : BOOL; END_VAR
END_FUNCTION_BLOCK

FUNCTION_BLOCK RS
VAR_INPUT S : BOOL; R1 : BOOL; END_VAR
VAR_OUTPUT Q1 : BOOL; END_VAR
END_FUNCTION_BLOCK
)";

// --- Helpers to read inputs and write outputs ---

bool boolAt(const std::vector<Value>& v, std::size_t i) { return std::get<bool>(v[i]); }
std::int64_t intAt(const std::vector<Value>& v, std::size_t i) { return std::get<std::int64_t>(v[i]); }

Duration toDuration(std::int64_t milliseconds) { return std::chrono::milliseconds(milliseconds); }
std::int64_t toMilliseconds(Duration d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); }

// TON, TOF and TP share one interface (IN, PT -> Q, ET), so one template
// adapts all three. `Timer` is softplc::Ton, Tof or Tp.
template <class Timer> class TimerBlock : public NativeBlock {
public:
    void execute(std::vector<Value>& v, TimePoint now) override {
        timer_.update(boolAt(v, 0), toDuration(intAt(v, 1)), now);
        v[2] = timer_.q();
        v[3] = toMilliseconds(timer_.et());
    }

private:
    Timer timer_;
};

// CTU and CTD: (count, reset/load, PV) -> Q, CV.
template <class Counter> class CounterBlock : public NativeBlock {
public:
    void execute(std::vector<Value>& v, TimePoint) override {
        counter_.update(boolAt(v, 0), boolAt(v, 1), static_cast<std::int16_t>(intAt(v, 2)));
        v[3] = counter_.q();
        v[4] = std::int64_t{counter_.cv()};
    }

private:
    Counter counter_;
};

// R_TRIG and F_TRIG: CLK -> Q.
template <class Edge> class EdgeBlock : public NativeBlock {
public:
    void execute(std::vector<Value>& v, TimePoint) override {
        edge_.update(boolAt(v, 0));
        v[1] = edge_.q();
    }

private:
    Edge edge_;
};

// SR and RS: (set, reset) -> Q1.
template <class FlipFlop> class FlipFlopBlock : public NativeBlock {
public:
    void execute(std::vector<Value>& v, TimePoint) override {
        flipFlop_.update(boolAt(v, 0), boolAt(v, 1));
        v[2] = flipFlop_.q1();
    }

private:
    FlipFlop flipFlop_;
};

} // namespace

const std::vector<Pou>& builtinFunctionBlocks() {
    // Parsed once, the first time anyone asks (a "function-local static").
    // C++ guarantees this initialisation runs only once, even with threads.
    static const std::vector<Pou> blocks = [] {
        auto parsed = parseFunctionBlocks(kInterfaces);
        if (!parsed) {
            throw std::logic_error("built-in function block interfaces don't parse: " + parsed.error().message);
        }
        for (auto& block : *parsed) {
            block.builtin = true;
        }
        return std::move(*parsed);
    }();
    return blocks;
}

std::unique_ptr<NativeBlock> createBuiltin(std::string_view name) {
    const auto upper = toUpper(name);
    if (upper == "TON") return std::make_unique<TimerBlock<Ton>>();
    if (upper == "TOF") return std::make_unique<TimerBlock<Tof>>();
    if (upper == "TP") return std::make_unique<TimerBlock<Tp>>();
    if (upper == "CTU") return std::make_unique<CounterBlock<Ctu>>();
    if (upper == "CTD") return std::make_unique<CounterBlock<Ctd>>();
    if (upper == "R_TRIG") return std::make_unique<EdgeBlock<RTrig>>();
    if (upper == "F_TRIG") return std::make_unique<EdgeBlock<FTrig>>();
    if (upper == "SR") return std::make_unique<FlipFlopBlock<Sr>>();
    if (upper == "RS") return std::make_unique<FlipFlopBlock<Rs>>();
    return nullptr;
}

} // namespace softplc::st
