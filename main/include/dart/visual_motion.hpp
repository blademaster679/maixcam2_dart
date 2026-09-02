#pragma once

#include <cstdint>
#include <vector>

#include "dart/green_detector.hpp"

namespace dart {

class VisualMotionEstimator {
public:
    explicit VisualMotionEstimator(
        const VisualMotionConfig &config = VisualMotionConfig());

    MotionPrior update(maix::image::Image &frame, uint64_t timestamp_us);
    void reset();

private:
    VisualMotionConfig config_;
    std::vector<uint8_t> previous_gray_;
    int previous_frame_width_ = 0;
    int previous_frame_height_ = 0;
};

}  // namespace dart
