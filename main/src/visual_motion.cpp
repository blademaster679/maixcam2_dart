#include "dart/visual_motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace dart {
namespace {

constexpr float kPi = 3.14159265358979323846F;

std::vector<uint8_t> downsample_gray(maix::image::Image &frame,
                                     int output_width,
                                     int output_height)
{
    if (frame.format() != maix::image::Format::FMT_RGB888) {
        throw std::invalid_argument("VisualMotionEstimator requires RGB888");
    }
    const auto *rgb = static_cast<const uint8_t *>(frame.data());
    if (rgb == nullptr || output_width <= 0 || output_height <= 0) {
        return {};
    }
    std::vector<uint8_t> gray(
        static_cast<std::size_t>(output_width) * output_height);
    for (int y = 0; y < output_height; ++y) {
        const int source_y = std::min(
            frame.height() - 1,
            static_cast<int>((static_cast<int64_t>(2 * y + 1) *
                              frame.height()) /
                             (2 * output_height)));
        for (int x = 0; x < output_width; ++x) {
            const int source_x = std::min(
                frame.width() - 1,
                static_cast<int>((static_cast<int64_t>(2 * x + 1) *
                                  frame.width()) /
                                 (2 * output_width)));
            const std::size_t offset =
                (static_cast<std::size_t>(source_y) * frame.width() +
                 source_x) * 3U;
            gray[static_cast<std::size_t>(y) * output_width + x] =
                static_cast<uint8_t>((77U * rgb[offset] +
                                      150U * rgb[offset + 1] +
                                      29U * rgb[offset + 2]) >> 8U);
        }
    }
    return gray;
}

float image_variance(const std::vector<uint8_t> &image)
{
    if (image.empty()) {
        return 0.0F;
    }
    double sum = 0.0;
    double squared_sum = 0.0;
    for (const uint8_t value : image) {
        sum += value;
        squared_sum += static_cast<double>(value) * value;
    }
    const double mean = sum / image.size();
    return static_cast<float>(std::max(
        0.0, squared_sum / image.size() - mean * mean));
}

struct Feature {
    float x = 0.0F;
    float y = 0.0F;
    float score = 0.0F;
};

struct FlowMatch {
    Feature previous;
    Feature current;
};

float sample_bilinear(const std::vector<uint8_t> &image,
                      int width,
                      int height,
                      float x,
                      float y)
{
    if (x < 0.0F || y < 0.0F || x >= width - 1.0F || y >= height - 1.0F) {
        return 0.0F;
    }
    const int x0 = static_cast<int>(x);
    const int y0 = static_cast<int>(y);
    const float fx = x - x0;
    const float fy = y - y0;
    const auto at = [&](int px, int py) {
        return static_cast<float>(
            image[static_cast<std::size_t>(py) * width + px]);
    };
    return (1.0F - fy) * ((1.0F - fx) * at(x0, y0) +
                          fx * at(x0 + 1, y0)) +
           fy * ((1.0F - fx) * at(x0, y0 + 1) +
                 fx * at(x0 + 1, y0 + 1));
}

std::vector<Feature> select_features(const std::vector<uint8_t> &image,
                                     int width,
                                     int height)
{
    std::vector<Feature> candidates;
    for (int y = 3; y < height - 3; y += 2) {
        for (int x = 3; x < width - 3; x += 2) {
            float xx = 0.0F;
            float xy = 0.0F;
            float yy = 0.0F;
            for (int py = -1; py <= 1; ++py) {
                for (int px = -1; px <= 1; ++px) {
                    const std::size_t index =
                        static_cast<std::size_t>(y + py) * width + x + px;
                    const float gx = image[index + 1] - image[index - 1];
                    const float gy = image[index + width] -
                                     image[index - width];
                    xx += gx * gx;
                    xy += gx * gy;
                    yy += gy * gy;
                }
            }
            const float trace = xx + yy;
            const float discriminant = std::sqrt(
                std::max(0.0F, (xx - yy) * (xx - yy) + 4.0F * xy * xy));
            const float minimum_eigenvalue = 0.5F * (trace - discriminant);
            if (minimum_eigenvalue > 600.0F) {
                candidates.push_back(
                    {static_cast<float>(x), static_cast<float>(y),
                     minimum_eigenvalue});
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Feature &left, const Feature &right) {
                  return left.score > right.score;
              });
    std::vector<Feature> selected;
    selected.reserve(80);
    for (const auto &candidate : candidates) {
        bool close = false;
        for (const auto &feature : selected) {
            if (std::hypot(candidate.x - feature.x,
                           candidate.y - feature.y) < 4.0F) {
                close = true;
                break;
            }
        }
        if (!close) {
            selected.push_back(candidate);
            if (selected.size() >= 80U) {
                break;
            }
        }
    }
    return selected;
}

float patch_error(const std::vector<uint8_t> &previous,
                  const std::vector<uint8_t> &current,
                  int width,
                  int height,
                  const Feature &feature,
                  float dx,
                  float dy)
{
    float error = 0.0F;
    int samples = 0;
    for (int py = -2; py <= 2; ++py) {
        for (int px = -2; px <= 2; ++px) {
            const float previous_value = sample_bilinear(
                previous, width, height, feature.x + px, feature.y + py);
            const float current_value = sample_bilinear(
                current, width, height, feature.x + dx + px,
                feature.y + dy + py);
            error += std::fabs(current_value - previous_value);
            ++samples;
        }
    }
    return samples > 0 ? error / samples : 255.0F;
}

std::vector<FlowMatch> track_features(
    const std::vector<uint8_t> &previous,
    const std::vector<uint8_t> &current,
    int width,
    int height,
    int max_shift)
{
    std::vector<FlowMatch> matches;
    for (const auto &feature : select_features(previous, width, height)) {
        if (feature.x < max_shift + 4 || feature.y < max_shift + 4 ||
            feature.x >= width - max_shift - 4 ||
            feature.y >= height - max_shift - 4) {
            continue;
        }
        float best_error = std::numeric_limits<float>::max();
        float second_error = std::numeric_limits<float>::max();
        float dx = 0.0F;
        float dy = 0.0F;
        for (int candidate_y = -max_shift; candidate_y <= max_shift;
             ++candidate_y) {
            for (int candidate_x = -max_shift; candidate_x <= max_shift;
                 ++candidate_x) {
                const float error = patch_error(
                    previous, current, width, height, feature,
                    static_cast<float>(candidate_x),
                    static_cast<float>(candidate_y));
                if (error < best_error) {
                    second_error = best_error;
                    best_error = error;
                    dx = static_cast<float>(candidate_x);
                    dy = static_cast<float>(candidate_y);
                } else if (error < second_error) {
                    second_error = error;
                }
            }
        }
        if (best_error > 42.0F ||
            second_error - best_error < std::max(0.8F, 0.04F * best_error)) {
            continue;
        }

        // Sub-pixel Lucas-Kanade refinement around the best integer flow.
        for (int iteration = 0; iteration < 4; ++iteration) {
            float hxx = 0.0F;
            float hxy = 0.0F;
            float hyy = 0.0F;
            float bx = 0.0F;
            float by = 0.0F;
            for (int py = -2; py <= 2; ++py) {
                for (int px = -2; px <= 2; ++px) {
                    const float x = feature.x + dx + px;
                    const float y = feature.y + dy + py;
                    const float current_value = sample_bilinear(
                        current, width, height, x, y);
                    const float previous_value = sample_bilinear(
                        previous, width, height, feature.x + px,
                        feature.y + py);
                    const float gx = 0.5F * (sample_bilinear(
                        current, width, height, x + 1.0F, y) -
                        sample_bilinear(current, width, height, x - 1.0F, y));
                    const float gy = 0.5F * (sample_bilinear(
                        current, width, height, x, y + 1.0F) -
                        sample_bilinear(current, width, height, x, y - 1.0F));
                    const float residual = current_value - previous_value;
                    hxx += gx * gx;
                    hxy += gx * gy;
                    hyy += gy * gy;
                    bx += gx * residual;
                    by += gy * residual;
                }
            }
            const float determinant = hxx * hyy - hxy * hxy;
            if (determinant < 1.0e-3F) {
                break;
            }
            const float step_x = (-hyy * bx + hxy * by) / determinant;
            const float step_y = (hxy * bx - hxx * by) / determinant;
            dx += std::max(-0.75F, std::min(0.75F, step_x));
            dy += std::max(-0.75F, std::min(0.75F, step_y));
            if (std::hypot(step_x, step_y) < 0.02F) {
                break;
            }
        }
        if (std::fabs(dx) <= max_shift + 1.0F &&
            std::fabs(dy) <= max_shift + 1.0F) {
            matches.push_back(
                {feature, {feature.x + dx, feature.y + dy, feature.score}});
        }
    }
    return matches;
}

struct SimilarityTransform {
    float a = 1.0F;
    float b = 0.0F;
    float tx = 0.0F;
    float ty = 0.0F;
};

float transform_error(const SimilarityTransform &model,
                      const FlowMatch &match)
{
    const float x = model.a * match.previous.x -
                    model.b * match.previous.y + model.tx;
    const float y = model.b * match.previous.x +
                    model.a * match.previous.y + model.ty;
    return std::hypot(x - match.current.x, y - match.current.y);
}

bool model_from_pair(const FlowMatch &first,
                     const FlowMatch &second,
                     SimilarityTransform &model)
{
    const float px = second.previous.x - first.previous.x;
    const float py = second.previous.y - first.previous.y;
    const float qx = second.current.x - first.current.x;
    const float qy = second.current.y - first.current.y;
    const float denominator = px * px + py * py;
    if (denominator < 25.0F) {
        return false;
    }
    model.a = (px * qx + py * qy) / denominator;
    model.b = (px * qy - py * qx) / denominator;
    const float scale = std::hypot(model.a, model.b);
    if (scale < 0.85F || scale > 1.15F) {
        return false;
    }
    model.tx = first.current.x - model.a * first.previous.x +
               model.b * first.previous.y;
    model.ty = first.current.y - model.b * first.previous.x -
               model.a * first.previous.y;
    return true;
}

bool estimate_similarity_ransac(const std::vector<FlowMatch> &matches,
                                SimilarityTransform &model,
                                float &mean_error,
                                float &inlier_ratio)
{
    if (matches.size() < 4U) {
        return false;
    }
    std::vector<std::size_t> best_inliers;
    float best_error = std::numeric_limits<float>::max();
    for (std::size_t first = 0; first < matches.size(); ++first) {
        for (std::size_t second = first + 1; second < matches.size(); ++second) {
            SimilarityTransform candidate;
            if (!model_from_pair(matches[first], matches[second], candidate)) {
                continue;
            }
            std::vector<std::size_t> inliers;
            float error_sum = 0.0F;
            for (std::size_t index = 0; index < matches.size(); ++index) {
                const float error = transform_error(candidate, matches[index]);
                if (error <= 1.5F) {
                    inliers.push_back(index);
                    error_sum += error;
                }
            }
            if (inliers.size() > best_inliers.size() ||
                (inliers.size() == best_inliers.size() &&
                 error_sum < best_error)) {
                best_inliers = std::move(inliers);
                best_error = error_sum;
            }
        }
    }
    if (best_inliers.size() < 4U) {
        return false;
    }

    float previous_x_mean = 0.0F;
    float previous_y_mean = 0.0F;
    float current_x_mean = 0.0F;
    float current_y_mean = 0.0F;
    for (const auto index : best_inliers) {
        previous_x_mean += matches[index].previous.x;
        previous_y_mean += matches[index].previous.y;
        current_x_mean += matches[index].current.x;
        current_y_mean += matches[index].current.y;
    }
    const float inverse_count = 1.0F / best_inliers.size();
    previous_x_mean *= inverse_count;
    previous_y_mean *= inverse_count;
    current_x_mean *= inverse_count;
    current_y_mean *= inverse_count;
    float denominator = 0.0F;
    float a_numerator = 0.0F;
    float b_numerator = 0.0F;
    for (const auto index : best_inliers) {
        const float px = matches[index].previous.x - previous_x_mean;
        const float py = matches[index].previous.y - previous_y_mean;
        const float qx = matches[index].current.x - current_x_mean;
        const float qy = matches[index].current.y - current_y_mean;
        denominator += px * px + py * py;
        a_numerator += px * qx + py * qy;
        b_numerator += px * qy - py * qx;
    }
    if (denominator < 1.0e-4F) {
        return false;
    }
    model.a = a_numerator / denominator;
    model.b = b_numerator / denominator;
    model.tx = current_x_mean - model.a * previous_x_mean +
               model.b * previous_y_mean;
    model.ty = current_y_mean - model.b * previous_x_mean -
               model.a * previous_y_mean;
    mean_error = 0.0F;
    for (const auto index : best_inliers) {
        mean_error += transform_error(model, matches[index]);
    }
    mean_error *= inverse_count;
    inlier_ratio = static_cast<float>(best_inliers.size()) / matches.size();
    return true;
}

}  // namespace

VisualMotionEstimator::VisualMotionEstimator(const VisualMotionConfig &config)
    : config_(config)
{
}

void VisualMotionEstimator::reset()
{
    previous_gray_.clear();
    previous_frame_width_ = 0;
    previous_frame_height_ = 0;
}

MotionPrior VisualMotionEstimator::update(maix::image::Image &frame,
                                          uint64_t timestamp_us)
{
    MotionPrior result;
    result.timestamp_us = timestamp_us;
    if (!config_.enabled) {
        return result;
    }
    const auto current = downsample_gray(frame, config_.grid_width,
                                         config_.grid_height);
    if (current.empty()) {
        return result;
    }
    if (previous_gray_.size() != current.size() ||
        previous_frame_width_ != frame.width() ||
        previous_frame_height_ != frame.height()) {
        previous_gray_ = current;
        previous_frame_width_ = frame.width();
        previous_frame_height_ = frame.height();
        return result;
    }

    const float variance = image_variance(current);
    if (variance < 16.0F) {
        previous_gray_ = current;
        return result;
    }

    const auto matches = track_features(previous_gray_, current,
                                        config_.grid_width,
                                        config_.grid_height,
                                        config_.max_shift_px);
    SimilarityTransform transform;
    float mean_error = 0.0F;
    float inlier_ratio = 0.0F;
    previous_gray_ = current;
    if (!estimate_similarity_ransac(matches, transform, mean_error,
                                    inlier_ratio)) {
        return result;
    }
    const float rotation = std::atan2(transform.b, transform.a);
    const float scale = std::hypot(transform.a, transform.b);
    if (std::fabs(rotation) >
            (config_.max_rotation_deg + config_.rotation_step_deg) *
                kPi / 180.0F ||
        scale < 0.90F || scale > 1.10F) {
        return result;
    }
    const float center_x = 0.5F * (config_.grid_width - 1);
    const float center_y = 0.5F * (config_.grid_height - 1);
    const float center_dx = transform.tx + transform.a * center_x -
                            transform.b * center_y - center_x;
    const float center_dy = transform.ty + transform.b * center_x +
                            transform.a * center_y - center_y;
    const float texture_score = std::min(1.0F, variance / 1024.0F);
    const float residual_score = std::max(0.0F, 1.0F - mean_error / 2.0F);
    const float feature_score = std::min(1.0F, matches.size() / 30.0F);
    result.confidence = std::min(
        1.0F, 0.20F * texture_score + 0.30F * residual_score +
                  0.30F * inlier_ratio + 0.20F * feature_score);
    if (result.confidence < config_.min_response) {
        return result;
    }
    result.valid = true;
    result.image_transform_valid = true;
    result.image_dx_px = center_dx * frame.width() /
                         config_.grid_width;
    result.image_dy_px = center_dy * frame.height() /
                         config_.grid_height;
    result.image_rotation_rad = rotation;
    result.image_scale = scale;
    return result;
}

}  // namespace dart
