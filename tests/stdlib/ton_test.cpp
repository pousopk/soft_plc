#include <doctest/doctest.h>
#include "stdlib/ton.hpp"

using namespace softplc;
using namespace std::chrono_literals;

// These tests describe how an IEC 61131-3 TON behaves. They drive the timer
// with explicit timestamps instead of the real clock, so they're instant and
// always give the same result.

namespace {
const TimePoint t0 = TimePoint{} + 10s; // an arbitrary starting moment
const Duration preset = 500ms;
} // namespace

TEST_CASE("TON is off with zero elapsed time before it's ever called") {
    Ton timer;
    CHECK_FALSE(timer.q());
    CHECK(timer.et() == 0ms);
}

TEST_CASE("TON stays off while IN is false") {
    Ton timer;
    timer.update(false, preset, t0);
    timer.update(false, preset, t0 + 1s);
    CHECK_FALSE(timer.q());
    CHECK(timer.et() == 0ms);
}

TEST_CASE("TON counts elapsed time while IN is true and switches on at the preset") {
    Ton timer;
    timer.update(true, preset, t0); // rising edge: timing starts here
    CHECK_FALSE(timer.q());
    CHECK(timer.et() == 0ms);

    timer.update(true, preset, t0 + 200ms);
    CHECK_FALSE(timer.q());
    CHECK(timer.et() == 200ms);

    timer.update(true, preset, t0 + 499ms);
    CHECK_FALSE(timer.q());

    timer.update(true, preset, t0 + 500ms);
    CHECK(timer.q());
    CHECK(timer.et() == 500ms);
}

TEST_CASE("TON elapsed time stops at the preset") {
    Ton timer;
    timer.update(true, preset, t0);
    timer.update(true, preset, t0 + 3s);
    CHECK(timer.q());
    CHECK(timer.et() == preset);
}

TEST_CASE("TON resets output and elapsed time as soon as IN goes false") {
    Ton timer;
    timer.update(true, preset, t0);
    timer.update(true, preset, t0 + 600ms);
    REQUIRE(timer.q());

    timer.update(false, preset, t0 + 700ms);
    CHECK_FALSE(timer.q());
    CHECK(timer.et() == 0ms);
}

TEST_CASE("TON restarts timing from zero on every new rising edge") {
    Ton timer;
    timer.update(true, preset, t0);
    timer.update(true, preset, t0 + 300ms);
    timer.update(false, preset, t0 + 400ms); // interrupted before the preset

    timer.update(true, preset, t0 + 1s); // new rising edge
    timer.update(true, preset, t0 + 1s + 300ms);
    CHECK_FALSE(timer.q()); // only 300 ms since the new edge, not 600 ms in total
    CHECK(timer.et() == 300ms);

    timer.update(true, preset, t0 + 1s + 500ms);
    CHECK(timer.q());
}

TEST_CASE("TON never switches on for a pulse shorter than the preset") {
    Ton timer;
    timer.update(true, preset, t0);
    timer.update(true, preset, t0 + 499ms);
    timer.update(false, preset, t0 + 520ms);
    CHECK_FALSE(timer.q());
    CHECK(timer.et() == 0ms);
}
