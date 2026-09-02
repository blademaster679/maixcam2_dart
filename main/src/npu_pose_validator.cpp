#include "dart/green_detector.hpp"

#include "maix_basic.hpp"
#include "maix_nn_yolo11.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace dart {
namespace {

class YoloPoseValidator final : public TargetPoseValidator {
public:
    explicit YoloPoseValidator(const NpuConfig &config)
        : config_(config), detector_(config.model_path, false)
    {
        if (detector_.type() != "pose") {
            throw std::runtime_error("NPU model metadata type must be pose");
        }
        if (detector_.input_width() != config_.input_size ||
            detector_.input_height() != config_.input_size) {
            throw std::runtime_error(
                "NPU model input size does not match npu.input_size");
        }
        if (detector_.labels.size() != 1U ||
            detector_.labels.front() != "dart_target") {
            throw std::runtime_error(
                "NPU model must contain only the dart_target class");
        }
    }

    PoseValidation validate(maix::image::Image &frame,
                            const CandidateRoi &roi) override
    {
        PoseValidation result;
        if (roi.width <= 0 || roi.height <= 0) {
            return result;
        }
        std::unique_ptr<maix::image::Image> crop(
            frame.crop(roi.x, roi.y, roi.width, roi.height));
        if (!crop) {
            return result;
        }
        std::unique_ptr<maix::nn::Objects> objects(detector_.detect(
            *crop, config_.confidence_threshold, 0.45F,
            maix::image::FIT_CONTAIN, config_.keypoint_threshold, 0));
        if (!objects || objects->size() == 0U) {
            return result;
        }
        const maix::nn::Object *best = nullptr;
        for (const auto *object : *objects) {
            if (object != nullptr &&
                (best == nullptr || object->score > best->score)) {
                best = object;
            }
        }
        if (best == nullptr || best->points.size() != 10U) {
            return result;
        }
        result.confidence = best->score;
        for (std::size_t index = 0; index < result.keypoints.size(); ++index) {
            const int x = best->points[2 * index];
            const int y = best->points[2 * index + 1];
            result.keypoints[index].valid = x >= 0 && y >= 0;
            if (result.keypoints[index].valid) {
                result.keypoints[index].x = static_cast<float>(roi.x + x);
                result.keypoints[index].y = static_cast<float>(roi.y + y);
            }
        }
        result.valid = result.keypoints[0].valid &&
                       result.confidence >= config_.confidence_threshold;
        return result;
    }

private:
    NpuConfig config_;
    maix::nn::YOLO11 detector_;
};

}  // namespace

std::shared_ptr<TargetPoseValidator>
create_yolo_pose_validator(const NpuConfig &config)
{
    if (!config.enabled) {
        return nullptr;
    }
    try {
        return std::make_shared<YoloPoseValidator>(config);
    } catch (const std::exception &exception) {
        if (config.required) {
            throw;
        }
        maix::log::warn("NPU pose validator disabled: %s", exception.what());
        return nullptr;
    }
}

}  // namespace dart
