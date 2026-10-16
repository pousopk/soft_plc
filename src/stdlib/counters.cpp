#include "stdlib/counters.hpp"
#include <limits>

namespace softplc {

void Ctu::update(bool cu, bool reset, std::int16_t preset) {
    const bool risingEdge = cu && !previousCu_;
    previousCu_ = cu;

    if (reset) {
        cv_ = 0; // reset wins over counting
    } else if (risingEdge && cv_ < std::numeric_limits<std::int16_t>::max()) {
        ++cv_; // stops at the maximum instead of wrapping around
    }
    q_ = cv_ >= preset;
}

void Ctd::update(bool cd, bool load, std::int16_t preset) {
    const bool risingEdge = cd && !previousCd_;
    previousCd_ = cd;

    if (load) {
        cv_ = preset;
    } else if (risingEdge && cv_ > std::numeric_limits<std::int16_t>::min()) {
        --cv_;
    }
    q_ = cv_ <= 0;
}

} // namespace softplc
