#pragma once
#include "core/plc_time.hpp"
#include "st/ast.hpp"
#include <memory>
#include <string_view>
#include <vector>

namespace softplc::st {

// The behaviour of a standard function block, implemented in C++.
class NativeBlock {
public:
    virtual ~NativeBlock() = default;

    // One call of the block. `variables` holds the instance's inputs and
    // outputs in declaration order: read the inputs, write the outputs.
    virtual void execute(std::vector<Value>& variables, TimePoint now) = 0;
};

// The standard function blocks: TON, TOF, TP, CTU, CTD, R_TRIG, F_TRIG, SR, RS.
// Their interfaces (inputs and outputs) are ordinary POUs with `builtin` set
// and an empty body, so the analyzer checks calls to them like any other block.
const std::vector<Pou>& builtinFunctionBlocks();

// Creates the C++ behaviour for one instance of a standard block.
std::unique_ptr<NativeBlock> createBuiltin(std::string_view name);

} // namespace softplc::st
