#pragma once

#include <cstdint>

namespace dart {

struct FrameContinuity {
    bool have_previous = false;
    std::uint64_t previous_sequence = 0;
    std::uint64_t previous_pts = 0;
    std::uint64_t sequence_gap_events = 0;
    std::uint64_t sequence_missing = 0;
    std::uint64_t sequence_duplicates = 0;
    std::uint64_t sequence_backwards = 0;
    std::uint64_t pts_nonincreasing = 0;

    void observe(std::uint64_t sequence, std::uint64_t pts) {
        if (have_previous) {
            if (sequence > previous_sequence) {
                const auto delta = sequence - previous_sequence;
                if (delta > 1) {
                    ++sequence_gap_events;
                    sequence_missing += delta - 1;
                }
            } else if (sequence == previous_sequence) {
                ++sequence_duplicates;
            } else if (sequence < previous_sequence) {
                ++sequence_backwards;
            }
            if (pts <= previous_pts) {
                ++pts_nonincreasing;
            }
        }
        have_previous = true;
        previous_sequence = sequence;
        previous_pts = pts;
    }

    bool fatal() const {
        return sequence_duplicates || sequence_backwards || pts_nonincreasing;
    }

    bool perfect() const {
        return !sequence_gap_events && !fatal();
    }
};

}  // namespace dart
