#include "dart/green_detector.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace dart {
namespace {

struct Rect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

struct RgbStatistics {
    double brightness_sum = 0.0;
    double green_dominance_sum = 0.0;
    double positive_green_dominance_sum = 0.0;
    int samples = 0;
    int positive_green_samples = 0;

    void add(uint8_t red, uint8_t green, uint8_t blue)
    {
        brightness_sum += (0.299 * red + 0.587 * green + 0.114 * blue) / 255.0;
        const double green_dominance =
            (static_cast<int>(green) - std::max(static_cast<int>(red),
                                                static_cast<int>(blue))) /
            255.0;
        green_dominance_sum += green_dominance;
        if (green_dominance > 0.0) {
            positive_green_dominance_sum += green_dominance;
            ++positive_green_samples;
        }
        ++samples;
    }

    float brightness() const
    {
        return samples > 0 ? static_cast<float>(brightness_sum / samples) : 0.0F;
    }

    float green_dominance() const
    {
        if (samples <= 0) {
            return 0.0F;
        }

        const double full_box_mean = green_dominance_sum / samples;
        if (positive_green_samples <= 0) {
            return static_cast<float>(full_box_mean);
        }

        // A close lamp has a saturated white core surrounded by a green halo.
        // Averaging the full bounding box dilutes that halo until it fails the
        // color gate. Keep the full-box statistic for small/dim targets, but
        // also consider the mean positive-green response weighted by its
        // coverage so a few isolated green noise pixels cannot dominate.
        const double positive_mean =
            positive_green_dominance_sum / positive_green_samples;
        const double positive_fraction =
            static_cast<double>(positive_green_samples) / samples;
        const double coverage_weighted_positive =
            positive_mean * std::sqrt(positive_fraction);
        return static_cast<float>(
            std::max(full_box_mean, coverage_weighted_positive));
    }

    float positive_green_fraction() const
    {
        return samples > 0
                   ? static_cast<float>(positive_green_samples) / samples
                   : 0.0F;
    }
};

struct CoreBlob {
    float center_x = 0.0F;
    float center_y = 0.0F;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    float density = 0.0F;
    float roundness = 0.0F;
};

float clamp01(float value)
{
    return std::max(0.0F, std::min(1.0F, value));
}

Rect clip_rect(int x, int y, int width, int height, int image_width, int image_height)
{
    Rect result;
    result.x0 = std::max(0, std::min(image_width, x));
    result.y0 = std::max(0, std::min(image_height, y));
    result.x1 = std::max(result.x0, std::min(image_width, x + width));
    result.y1 = std::max(result.y0, std::min(image_height, y + height));
    return result;
}

RgbStatistics sample_rect(const uint8_t *rgb,
                          int image_width,
                          const Rect &rect,
                          int grid,
                          const Rect *excluded = nullptr)
{
    RgbStatistics statistics;
    const int width = rect.x1 - rect.x0;
    const int height = rect.y1 - rect.y0;
    if (rgb == nullptr || width <= 0 || height <= 0) {
        return statistics;
    }

    const int step_x = std::max(1, (width + grid - 1) / grid);
    const int step_y = std::max(1, (height + grid - 1) / grid);
    for (int y = rect.y0; y < rect.y1; y += step_y) {
        for (int x = rect.x0; x < rect.x1; x += step_x) {
            if (excluded != nullptr && x >= excluded->x0 && x < excluded->x1 &&
                y >= excluded->y0 && y < excluded->y1) {
                continue;
            }
            const std::size_t offset =
                (static_cast<std::size_t>(y) * image_width + x) * 3U;
            statistics.add(rgb[offset], rgb[offset + 1], rgb[offset + 2]);
        }
    }
    return statistics;
}

int intersection_area(int ax, int ay, int aw, int ah,
                      int bx, int by, int bw, int bh)
{
    const int left = std::max(ax, bx);
    const int top = std::max(ay, by);
    const int right = std::min(ax + aw, bx + bw);
    const int bottom = std::min(ay + ah, by + bh);
    return std::max(0, right - left) * std::max(0, bottom - top);
}

}  // namespace

struct GreenLightDetector::Candidate {
    detail::CandidateObservation observation;
    GreenLightCandidateDebug debug;
};

GreenLightDetector::GreenLightDetector(const DetectorConfig &config)
    : config_(config), tracker_(config)
{
}

void GreenLightDetector::reset()
{
    tracker_.reset();
    last_candidates_.clear();
}

const std::vector<GreenLightCandidateDebug> &GreenLightDetector::last_candidates() const
{
    return last_candidates_;
}

std::vector<GreenLightDetector::Candidate>
GreenLightDetector::collect_candidates(maix::image::Image &frame)
{
    const int image_width = frame.width();
    const int image_height = frame.height();
    const auto *rgb = static_cast<const uint8_t *>(frame.data());
    if (rgb == nullptr || frame.data_size() < image_width * image_height * 3) {
        throw std::runtime_error("RGB888 frame has an invalid data buffer");
    }

    const std::vector<std::vector<int>> halo_thresholds{
        config_.halo_lab.as_vector(),
    };
    const std::vector<std::vector<int>> core_thresholds{
        config_.core_lab.as_vector(),
    };
    auto halo_blobs = frame.find_blobs(halo_thresholds, false, {}, 1, 1, 1, 1,
                                       config_.merge_blobs,
                                       config_.merge_margin_px);
    auto core_blobs = frame.find_blobs(core_thresholds, false, {}, 1, 1, 1, 1,
                                       config_.merge_blobs,
                                       config_.merge_margin_px);

    std::vector<CoreBlob> cores;
    cores.reserve(core_blobs.size());
    for (auto &blob : core_blobs) {
        if (blob.w() > 0 && blob.h() > 0) {
            cores.push_back({blob.cxf(), blob.cyf(), blob.x(), blob.y(),
                             blob.w(), blob.h(), blob.density(),
                             blob.roundness()});
        }
    }

    std::vector<Candidate> candidates;
    candidates.reserve(halo_blobs.size() + cores.size());
    const float frame_area = static_cast<float>(image_width) * image_height;
    const bool acquiring = !tracker_.tracking_confirmed();
    const float initial_size_weight =
        acquiring ? config_.weight_initial_size : 0.0F;
    const float center_prior_weight =
        acquiring ? config_.weight_center_prior : 0.0F;
    const float temporal_weight =
        tracker_.has_prediction() ? config_.weight_temporal : 0.0F;
    const float total_weight = config_.weight_color + config_.weight_contrast +
                               config_.weight_density + config_.weight_shape +
                               config_.weight_core + temporal_weight +
                               center_prior_weight + initial_size_weight;

    for (auto &halo : halo_blobs) {
        const int x = halo.x();
        const int y = halo.y();
        const int width = halo.w();
        const int height = halo.h();
        if (width <= 0 || height <= 0) {
            continue;
        }

        const float density = clamp01(halo.density());
        if (density < config_.min_density) {
            continue;
        }

        const Rect inner = clip_rect(x, y, width, height, image_width, image_height);
        const Rect outer = clip_rect(x - config_.ring_margin_px,
                                     y - config_.ring_margin_px,
                                     width + 2 * config_.ring_margin_px,
                                     height + 2 * config_.ring_margin_px,
                                     image_width,
                                     image_height);
        const RgbStatistics inside =
            sample_rect(rgb, image_width, inner, config_.sample_grid);
        const RgbStatistics ring =
            sample_rect(rgb, image_width, outer, config_.sample_grid, &inner);
        const float green_dominance = inside.green_dominance();
        const float local_contrast = ring.samples > 0
                                         ? inside.brightness() - ring.brightness()
                                         : 0.0F;
        if (green_dominance < config_.min_green_dominance ||
            local_contrast < config_.min_local_contrast) {
            continue;
        }

        const float halo_center_x = halo.cxf();
        const float halo_center_y = halo.cyf();
        const float geometric_center_x = x + 0.5F * width;
        const float geometric_center_y = y + 0.5F * height;
        const float apparent_size =
            std::sqrt(std::max(1.0F, static_cast<float>(width) * height));
        if (apparent_size < config_.min_halo_size_px) {
            continue;
        }

        const CoreBlob *matched_core = nullptr;
        float matched_core_score = 0.0F;
        for (const auto &core : cores) {
            const int overlap = intersection_area(x, y, width, height,
                                                  core.x, core.y, core.w, core.h);
            if (overlap <= 0) {
                continue;
            }
            const float dx = core.center_x - halo_center_x;
            const float dy = core.center_y - halo_center_y;
            const float center_offset = std::sqrt(dx * dx + dy * dy) /
                                        std::max(1.0F, apparent_size);
            const float center_score = clamp01(
                1.0F - center_offset /
                           std::max(0.01F, config_.core_center_max_fraction));
            const float overlap_score = clamp01(
                static_cast<float>(overlap) /
                std::max(1.0F, static_cast<float>(core.w) * core.h));
            const float core_score = 0.75F * center_score + 0.25F * overlap_score;
            if (core_score > matched_core_score) {
                matched_core_score = core_score;
                matched_core = &core;
            }
        }

        const float area_fraction =
            static_cast<float>(width) * height / std::max(1.0F, frame_area);
        const float large_blob_factor =
            clamp01((std::sqrt(area_fraction) - 0.03F) / 0.32F);
        float center_x = halo_center_x;
        float center_y = halo_center_y;
        if (matched_core != nullptr) {
            const float core_weight = 0.65F * large_blob_factor;
            const float geometry_weight = 0.20F * large_blob_factor;
            const float halo_weight = 1.0F - core_weight - geometry_weight;
            center_x = halo_weight * halo_center_x +
                       core_weight * matched_core->center_x +
                       geometry_weight * geometric_center_x;
            center_y = halo_weight * halo_center_y +
                       core_weight * matched_core->center_y +
                       geometry_weight * geometric_center_y;
        } else {
            const float geometry_weight = 0.40F * large_blob_factor;
            center_x = (1.0F - geometry_weight) * halo_center_x +
                       geometry_weight * geometric_center_x;
            center_y = (1.0F - geometry_weight) * halo_center_y +
                       geometry_weight * geometric_center_y;
        }

        const float aspect_score = static_cast<float>(std::min(width, height)) /
                                   std::max(width, height);
        float roundness = halo.roundness();
        if (!std::isfinite(roundness)) {
            roundness = 0.0F;
        }
        const float shape_score = width * height <= 4
                                      ? 0.8F
                                      : clamp01(0.5F * aspect_score +
                                                0.5F * clamp01(roundness));
        const float color_score = clamp01((green_dominance + 0.02F) / 0.48F);
        const float contrast_score = clamp01((local_contrast + 0.05F) / 0.40F);
        const float density_score = clamp01(
            (density - config_.min_density) /
            std::max(0.01F, 1.0F - config_.min_density));
        const float center_prior_dx =
            center_x - config_.camera_model.principal_x;
        const float center_prior_dy =
            center_y - config_.camera_model.principal_y;
        const float center_prior_distance_squared =
            center_prior_dx * center_prior_dx + center_prior_dy * center_prior_dy;
        const float center_prior_radius =
            std::max(1.0F, config_.center_prior_radius_px);
        const float center_prior_score = std::exp(
            -0.5F * center_prior_distance_squared /
            (center_prior_radius * center_prior_radius));
        const float initial_size_score = clamp01(
            std::log1p(apparent_size) /
            std::log1p(std::max(1.0F, config_.initial_size_reference_px)));

        detail::CandidateObservation observation;
        observation.center_x = center_x;
        observation.center_y = center_y;
        observation.bbox_x = x;
        observation.bbox_y = y;
        observation.bbox_w = width;
        observation.bbox_h = height;
        observation.apparent_size = apparent_size;
        observation.source = detail::CandidateSource::Halo;

        const float temporal_score = tracker_.association_score(observation);
        observation.association_score = temporal_score;
        if (tracker_.tracking_confirmed() &&
            !tracker_.passes_association_gate(observation)) {
            continue;
        }

        observation.score = clamp01(
            (config_.weight_color * color_score +
             config_.weight_contrast * contrast_score +
             config_.weight_density * density_score +
             config_.weight_shape * shape_score +
             config_.weight_core * matched_core_score +
             temporal_weight * temporal_score +
             center_prior_weight * center_prior_score +
             initial_size_weight * initial_size_score) /
            std::max(1.0e-6F, total_weight));

        Candidate candidate;
        candidate.observation = observation;
        candidate.debug.center_x = center_x;
        candidate.debug.center_y = center_y;
        candidate.debug.bbox_x = x;
        candidate.debug.bbox_y = y;
        candidate.debug.bbox_w = width;
        candidate.debug.bbox_h = height;
        candidate.debug.apparent_size = apparent_size;
        candidate.debug.density = density;
        candidate.debug.green_dominance = green_dominance;
        candidate.debug.green_fraction = inside.positive_green_fraction();
        candidate.debug.local_contrast = local_contrast;
        candidate.debug.shape_score = shape_score;
        candidate.debug.core_score = matched_core_score;
        candidate.debug.temporal_score = temporal_score;
        candidate.debug.center_prior_score = center_prior_score;
        candidate.debug.initial_size_score = initial_size_score;
        candidate.debug.score = observation.score;
        candidates.push_back(candidate);
    }

    if (!config_.enable_saturated_core_candidates) {
        return candidates;
    }
    const float core_size_weight = config_.weight_core_size;
    const float core_total_weight =
        total_weight - initial_size_weight + core_size_weight;

    // When the lamp approaches the camera, its center clips to white and the
    // surrounding green halo can become connected to the green structure
    // behind it. A halo-only detector then follows tiny edge fragments. Treat
    // compact saturated cores as a second candidate source, but only when the
    // immediately surrounding ring is green. This rejects ordinary white
    // lights while preserving an accurate lamp center and apparent size.
    for (const auto &core : cores) {
        const int width = core.w;
        const int height = core.h;
        if (width <= 0 || height <= 0) {
            continue;
        }

        const float density = clamp01(core.density);
        if (density < config_.min_density) {
            continue;
        }

        const int adaptive_margin = std::max(
            config_.ring_margin_px,
            static_cast<int>(std::lround(
                config_.core_ring_scale * std::max(width, height))));
        const Rect inner = clip_rect(core.x, core.y, width, height,
                                     image_width, image_height);
        const Rect outer = clip_rect(core.x - adaptive_margin,
                                     core.y - adaptive_margin,
                                     width + 2 * adaptive_margin,
                                     height + 2 * adaptive_margin,
                                     image_width,
                                     image_height);
        const RgbStatistics inside =
            sample_rect(rgb, image_width, inner, config_.sample_grid);
        const RgbStatistics ring =
            sample_rect(rgb, image_width, outer, config_.sample_grid, &inner);
        const float core_brightness = inside.brightness();
        const float ring_green_dominance = ring.green_dominance();
        const float ring_green_fraction = ring.positive_green_fraction();
        const float local_contrast = ring.samples > 0
                                         ? core_brightness - ring.brightness()
                                         : 0.0F;
        if (core_brightness < config_.min_core_brightness ||
            ring_green_dominance <
                config_.min_core_ring_green_dominance ||
            ring_green_fraction < config_.min_core_ring_green_fraction) {
            continue;
        }

        const float geometric_center_x = core.x + 0.5F * width;
        const float geometric_center_y = core.y + 0.5F * height;
        const float center_x = 0.75F * core.center_x +
                               0.25F * geometric_center_x;
        const float center_y = 0.75F * core.center_y +
                               0.25F * geometric_center_y;
        const float apparent_size =
            std::sqrt(std::max(1.0F, static_cast<float>(width) * height));
        if (apparent_size < config_.min_core_size_px) {
            continue;
        }
        const float aspect_score = static_cast<float>(std::min(width, height)) /
                                   std::max(width, height);
        const float roundness = std::isfinite(core.roundness)
                                    ? clamp01(core.roundness)
                                    : 0.0F;
        const float shape_score = width * height <= 4
                                      ? 0.8F
                                      : clamp01(0.5F * aspect_score +
                                                0.5F * roundness);
        const float color_score = clamp01(
            (ring_green_dominance + 0.02F) / 0.48F);
        const float contrast_score = clamp01(
            (local_contrast + 0.05F) / 0.40F);
        const float density_score = clamp01(
            (density - config_.min_density) /
            std::max(0.01F, 1.0F - config_.min_density));
        const float core_score = clamp01(
            (core_brightness - config_.min_core_brightness) /
            std::max(0.01F, 1.0F - config_.min_core_brightness));
        const float center_prior_dx =
            center_x - config_.camera_model.principal_x;
        const float center_prior_dy =
            center_y - config_.camera_model.principal_y;
        const float center_prior_distance_squared =
            center_prior_dx * center_prior_dx + center_prior_dy * center_prior_dy;
        const float center_prior_radius =
            std::max(1.0F, config_.center_prior_radius_px);
        const float center_prior_score = std::exp(
            -0.5F * center_prior_distance_squared /
            (center_prior_radius * center_prior_radius));
        const float initial_size_score = clamp01(
            std::log1p(apparent_size) /
            std::log1p(std::max(1.0F, config_.initial_size_reference_px)));

        detail::CandidateObservation observation;
        observation.center_x = center_x;
        observation.center_y = center_y;
        observation.bbox_x = core.x;
        observation.bbox_y = core.y;
        observation.bbox_w = width;
        observation.bbox_h = height;
        observation.apparent_size = apparent_size;
        observation.source = detail::CandidateSource::SaturatedCore;

        const float temporal_score = tracker_.association_score(observation);
        observation.association_score = temporal_score;
        if (tracker_.tracking_confirmed() &&
            !tracker_.passes_association_gate(observation)) {
            continue;
        }

        observation.score = clamp01(
            (config_.weight_color * color_score +
             config_.weight_contrast * contrast_score +
             config_.weight_density * density_score +
             config_.weight_shape * shape_score +
             config_.weight_core * core_score +
             temporal_weight * temporal_score +
             center_prior_weight * center_prior_score +
             core_size_weight * initial_size_score) /
            std::max(1.0e-6F, core_total_weight));

        Candidate candidate;
        candidate.observation = observation;
        candidate.debug.center_x = center_x;
        candidate.debug.center_y = center_y;
        candidate.debug.bbox_x = core.x;
        candidate.debug.bbox_y = core.y;
        candidate.debug.bbox_w = width;
        candidate.debug.bbox_h = height;
        candidate.debug.apparent_size = apparent_size;
        candidate.debug.density = density;
        candidate.debug.green_dominance = ring_green_dominance;
        candidate.debug.green_fraction = ring_green_fraction;
        candidate.debug.local_contrast = local_contrast;
        candidate.debug.shape_score = shape_score;
        candidate.debug.core_score = core_score;
        candidate.debug.temporal_score = temporal_score;
        candidate.debug.center_prior_score = center_prior_score;
        candidate.debug.initial_size_score = initial_size_score;
        candidate.debug.score = observation.score;
        candidate.debug.saturated_core = true;
        candidates.push_back(candidate);
    }

    return candidates;
}

GreenLightDetection GreenLightDetector::process(maix::image::Image &frame,
                                                 uint64_t timestamp_us)
{
    if (frame.format() != maix::image::Format::FMT_RGB888) {
        throw std::invalid_argument("GreenLightDetector requires an RGB888 frame");
    }

    tracker_.predict(timestamp_us);
    auto candidates = collect_candidates(frame);
    last_candidates_.clear();
    last_candidates_.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        last_candidates_.push_back(candidate.debug);
    }

    const float minimum_score = tracker_.tracking_confirmed()
                                    ? config_.min_tracking_score
                                    : config_.min_candidate_score;
    int selected_index = -1;
    float best_score = minimum_score;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (candidates[index].observation.score >= best_score) {
            best_score = candidates[index].observation.score;
            selected_index = static_cast<int>(index);
        }
    }

    if (selected_index < 0) {
        return tracker_.update(nullptr, timestamp_us, config_.camera_model);
    }

    last_candidates_[selected_index].selected = true;
    return tracker_.update(&candidates[selected_index].observation,
                           timestamp_us,
                           config_.camera_model);
}

}  // namespace dart
