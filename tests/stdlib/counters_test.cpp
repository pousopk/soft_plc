#include <doctest/doctest.h>
#include "stdlib/counters.hpp"

using namespace softplc;

TEST_CASE("CTU counts rising edges, not cycles") {
    Ctu counter;
    counter.update(true, false, 3);
    counter.update(true, false, 3); // still on: no new edge
    counter.update(true, false, 3);
    CHECK(counter.cv() == 1);

    counter.update(false, false, 3);
    counter.update(true, false, 3);
    CHECK(counter.cv() == 2);
    CHECK_FALSE(counter.q());

    counter.update(false, false, 3);
    counter.update(true, false, 3);
    CHECK(counter.cv() == 3);
    CHECK(counter.q()); // reached PV
}

TEST_CASE("CTU reset wins over counting") {
    Ctu counter;
    counter.update(true, false, 5);
    counter.update(false, false, 5);
    counter.update(true, true, 5); // edge and reset at the same time
    CHECK(counter.cv() == 0);
    CHECK_FALSE(counter.q());
}

TEST_CASE("CTU stops at the INT maximum instead of wrapping") {
    Ctu counter;
    for (int i = 0; i < 32'800; ++i) {
        counter.update(true, false, 10);
        counter.update(false, false, 10);
    }
    CHECK(counter.cv() == 32767);
}

TEST_CASE("CTD loads PV and counts down to zero") {
    Ctd counter;
    counter.update(false, true, 2); // load
    CHECK(counter.cv() == 2);
    CHECK_FALSE(counter.q());

    counter.update(true, false, 2);
    counter.update(false, false, 2);
    counter.update(true, false, 2);
    CHECK(counter.cv() == 0);
    CHECK(counter.q());
}
