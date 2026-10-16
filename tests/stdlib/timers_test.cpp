#include <doctest/doctest.h>
#include "stdlib/timers.hpp"

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

// ---------------------------------------------------------------- TOF

TEST_CASE("TOF switches on with IN and stays on for PT after IN goes off") {
    Tof timer;
    timer.update(false, preset, t0);
    CHECK_FALSE(timer.q()); // never been on: nothing to delay

    timer.update(true, preset, t0 + 100ms);
    CHECK(timer.q()); // on at once
    CHECK(timer.et() == 0ms);

    timer.update(false, preset, t0 + 1s); // falling edge: the delay starts
    CHECK(timer.q());
    timer.update(false, preset, t0 + 1s + 499ms);
    CHECK(timer.q());
    CHECK(timer.et() == 499ms);

    timer.update(false, preset, t0 + 1s + 500ms);
    CHECK_FALSE(timer.q());
    CHECK(timer.et() == preset); // holds at PT
}

TEST_CASE("TOF restarts the delay if IN comes back on in time") {
    Tof timer;
    timer.update(true, preset, t0);
    timer.update(false, preset, t0 + 100ms);
    timer.update(true, preset, t0 + 400ms); // back on before PT ran out
    CHECK(timer.q());
    CHECK(timer.et() == 0ms);
    timer.update(false, preset, t0 + 2s);
    timer.update(false, preset, t0 + 2s + 300ms);
    CHECK(timer.q()); // a fresh 500 ms delay, only 300 ms in
}

// ---------------------------------------------------------------- TP

TEST_CASE("TP gives a pulse of exactly PT, however long IN stays on") {
    Tp timer;
    timer.update(true, preset, t0);
    CHECK(timer.q());
    timer.update(true, preset, t0 + 499ms);
    CHECK(timer.q());
    timer.update(true, preset, t0 + 500ms);
    CHECK_FALSE(timer.q());
    CHECK(timer.et() == preset); // holds while IN is still on

    timer.update(false, preset, t0 + 2s);
    CHECK(timer.et() == 0ms); // reset once IN goes off
}

TEST_CASE("TP finishes its pulse even if IN goes off early") {
    Tp timer;
    timer.update(true, preset, t0);
    timer.update(false, preset, t0 + 50ms);
    CHECK(timer.q());
    timer.update(false, preset, t0 + 500ms);
    CHECK_FALSE(timer.q());
}

TEST_CASE("TP ignores new edges during a pulse, but not right after it") {
    Tp timer;
    timer.update(true, preset, t0);
    timer.update(false, preset, t0 + 100ms);
    timer.update(true, preset, t0 + 200ms); // a new edge during the pulse: ignored
    timer.update(false, preset, t0 + 500ms); // pulse ends at 500 ms
    CHECK_FALSE(timer.q());

    timer.update(true, preset, t0 + 510ms); // a new edge right after: a new pulse
    CHECK(timer.q());
    timer.update(true, preset, t0 + 1009ms);
    CHECK(timer.q());
    timer.update(true, preset, t0 + 1010ms);
    CHECK_FALSE(timer.q());
}
