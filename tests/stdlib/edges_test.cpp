#include <doctest/doctest.h>
#include "stdlib/edges.hpp"
#include <iterator>

using namespace softplc;

TEST_CASE("R_TRIG is on for one call at each rising edge") {
    RTrig trigger;
    const bool clk[] = {false, true, true, true, false, true};
    const bool expected[] = {false, true, false, false, false, true};
    for (std::size_t i = 0; i < std::size(clk); ++i) {
        CAPTURE(i);
        trigger.update(clk[i]);
        CHECK(trigger.q() == expected[i]);
    }
}

TEST_CASE("F_TRIG is on for one call at each falling edge, and not at start-up") {
    FTrig trigger;
    const bool clk[] = {false, true, true, false, false, true, false};
    const bool expected[] = {false, false, false, true, false, false, true};
    for (std::size_t i = 0; i < std::size(clk); ++i) {
        CAPTURE(i);
        trigger.update(clk[i]);
        CHECK(trigger.q() == expected[i]);
    }
}

TEST_CASE("SR and RS remember their state and differ only when S and R are both on") {
    Sr setDominant;
    Rs resetDominant;

    setDominant.update(true, false);
    resetDominant.update(true, false);
    CHECK(setDominant.q1());
    CHECK(resetDominant.q1());

    setDominant.update(false, false); // both off: remember
    resetDominant.update(false, false);
    CHECK(setDominant.q1());
    CHECK(resetDominant.q1());

    setDominant.update(true, true); // both on: here they differ
    resetDominant.update(true, true);
    CHECK(setDominant.q1());
    CHECK_FALSE(resetDominant.q1());

    setDominant.update(false, true);
    CHECK_FALSE(setDominant.q1());
}
