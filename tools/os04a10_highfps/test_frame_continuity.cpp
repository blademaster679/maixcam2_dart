#include <cassert>
#include <iostream>

#include "frame_continuity.hpp"

int main() {
    dart::FrameContinuity continuous;
    continuous.observe(10, 100);
    continuous.observe(11, 200);
    assert(continuous.perfect());
    assert(!continuous.fatal());

    dart::FrameContinuity missing;
    missing.observe(20, 100);
    missing.observe(23, 400);
    missing.observe(24, 500);
    assert(missing.sequence_gap_events == 1);
    assert(missing.sequence_missing == 2);
    assert(!missing.perfect());
    assert(!missing.fatal());

    dart::FrameContinuity invalid;
    invalid.observe(30, 100);
    invalid.observe(30, 100);
    invalid.observe(29, 90);
    assert(invalid.sequence_duplicates == 1);
    assert(invalid.sequence_backwards == 1);
    assert(invalid.pts_nonincreasing == 2);
    assert(invalid.fatal());

    std::cout << "PASS: missing frames are counted without hiding fatal ordering faults\n";
}
