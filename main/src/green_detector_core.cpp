#include "dart/green_detector.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dart {
namespace {

constexpr float kMinSize = 1.0F;
constexpr float kMinDt = 0.001F;
constexpr float kMaxDt = 0.100F;

float clamp01(float value)
{
    return std::max(0.0F, std::min(1.0F, value));
}

bool invert_3x3(const float input[3][3], float output[3][3])
{
    const float determinant =
        input[0][0] * (input[1][1] * input[2][2] - input[1][2] * input[2][1]) -
        input[0][1] * (input[1][0] * input[2][2] - input[1][2] * input[2][0]) +
        input[0][2] * (input[1][0] * input[2][1] - input[1][1] * input[2][0]);

    if (std::fabs(determinant) < 1.0e-9F) {
        return false;
    }

    const float inverse_determinant = 1.0F / determinant;
    output[0][0] = (input[1][1] * input[2][2] - input[1][2] * input[2][1]) * inverse_determinant;
    output[0][1] = (input[0][2] * input[2][1] - input[0][1] * input[2][2]) * inverse_determinant;
    output[0][2] = (input[0][1] * input[1][2] - input[0][2] * input[1][1]) * inverse_determinant;
    output[1][0] = (input[1][2] * input[2][0] - input[1][0] * input[2][2]) * inverse_determinant;
    output[1][1] = (input[0][0] * input[2][2] - input[0][2] * input[2][0]) * inverse_determinant;
    output[1][2] = (input[0][2] * input[1][0] - input[0][0] * input[1][2]) * inverse_determinant;
    output[2][0] = (input[1][0] * input[2][1] - input[1][1] * input[2][0]) * inverse_determinant;
    output[2][1] = (input[0][1] * input[2][0] - input[0][0] * input[2][1]) * inverse_determinant;
    output[2][2] = (input[0][0] * input[1][1] - input[0][1] * input[1][0]) * inverse_determinant;
    return true;
}

}  // namespace

const char *track_state_name(TrackState state)
{
    switch (state) {
    case TrackState::Lost:
        return "LOST";
    case TrackState::Candidate:
        return "CANDIDATE";
    case TrackState::Tracking:
        return "TRACKING";
    }
    return "UNKNOWN";
}

std::vector<int> LabThreshold::as_vector() const
{
    return {l_min, l_max, a_min, a_max, b_min, b_max};
}

namespace detail {

std::array<float, 2> pixel_to_angles(float pixel_x,
                                     float pixel_y,
                                     const CameraModel &model)
{
    if (model.fx <= 0.0F || model.fy <= 0.0F) {
        return {0.0F, 0.0F};
    }

    const float distorted_x = (pixel_x - model.principal_x) / model.fx;
    const float distorted_y = (pixel_y - model.principal_y) / model.fy;
    float undistorted_x = distorted_x;
    float undistorted_y = distorted_y;

    for (int iteration = 0; iteration < 6; ++iteration) {
        const float x2 = undistorted_x * undistorted_x;
        const float y2 = undistorted_y * undistorted_y;
        const float xy = undistorted_x * undistorted_y;
        const float r2 = x2 + y2;
        const float radial = 1.0F + model.k1 * r2 + model.k2 * r2 * r2 +
                             model.k3 * r2 * r2 * r2;
        if (std::fabs(radial) < 1.0e-6F) {
            break;
        }

        const float tangential_x = 2.0F * model.p1 * xy + model.p2 * (r2 + 2.0F * x2);
        const float tangential_y = model.p1 * (r2 + 2.0F * y2) + 2.0F * model.p2 * xy;
        undistorted_x = (distorted_x - tangential_x) / radial;
        undistorted_y = (distorted_y - tangential_y) / radial;
    }

    return {std::atan(undistorted_x), -std::atan(undistorted_y)};
}

TemporalTracker::TemporalTracker(const DetectorConfig &config)
    : config_(config)
{
    reset();
}

void TemporalTracker::reset()
{
    initialized_ = false;
    last_timestamp_us_ = 0;
    state_.fill(0.0F);
    for (auto &row : covariance_) {
        row.fill(0.0F);
    }
    hit_history_.clear();
    missed_frames_ = 0;
    track_state_ = TrackState::Lost;
    last_candidate_ = CandidateObservation{};
}

void TemporalTracker::initialize(const CandidateObservation &candidate,
                                 uint64_t timestamp_us)
{
    initialized_ = true;
    last_timestamp_us_ = timestamp_us;
    state_ = {candidate.center_x,
              candidate.center_y,
              0.0F,
              0.0F,
              std::log(std::max(candidate.apparent_size, kMinSize)),
              0.0F};

    for (auto &row : covariance_) {
        row.fill(0.0F);
    }
    covariance_[0][0] = 25.0F;
    covariance_[1][1] = 25.0F;
    covariance_[2][2] = 400.0F;
    covariance_[3][3] = 400.0F;
    covariance_[4][4] = 0.25F;
    covariance_[5][5] = 1.0F;
}

void TemporalTracker::predict(uint64_t timestamp_us)
{
    if (!initialized_) {
        return;
    }
    if (timestamp_us <= last_timestamp_us_) {
        return;
    }

    float dt = static_cast<float>(timestamp_us - last_timestamp_us_) * 1.0e-6F;
    dt = std::max(kMinDt, std::min(kMaxDt, dt));
    last_timestamp_us_ = timestamp_us;

    std::array<std::array<float, 6>, 6> transition{};
    for (int i = 0; i < 6; ++i) {
        transition[i][i] = 1.0F;
    }
    transition[0][2] = dt;
    transition[1][3] = dt;
    transition[4][5] = dt;

    state_[0] += state_[2] * dt;
    state_[1] += state_[3] * dt;
    state_[4] += state_[5] * dt;

    std::array<std::array<float, 6>, 6> intermediate{};
    std::array<std::array<float, 6>, 6> predicted_covariance{};
    for (int row = 0; row < 6; ++row) {
        for (int column = 0; column < 6; ++column) {
            for (int k = 0; k < 6; ++k) {
                intermediate[row][column] += transition[row][k] * covariance_[k][column];
            }
        }
    }
    for (int row = 0; row < 6; ++row) {
        for (int column = 0; column < 6; ++column) {
            for (int k = 0; k < 6; ++k) {
                predicted_covariance[row][column] += intermediate[row][k] * transition[column][k];
            }
        }
    }

    predicted_covariance[0][0] += config_.process_noise_position * dt;
    predicted_covariance[1][1] += config_.process_noise_position * dt;
    predicted_covariance[2][2] += config_.process_noise_velocity * dt;
    predicted_covariance[3][3] += config_.process_noise_velocity * dt;
    predicted_covariance[4][4] += config_.process_noise_log_size * dt;
    predicted_covariance[5][5] += config_.process_noise_size_rate * dt;
    covariance_ = predicted_covariance;
}

bool TemporalTracker::has_prediction() const
{
    return initialized_;
}

bool TemporalTracker::tracking_confirmed() const
{
    return initialized_ && track_state_ == TrackState::Tracking;
}

float TemporalTracker::predicted_x() const
{
    return state_[0];
}

float TemporalTracker::predicted_y() const
{
    return state_[1];
}

float TemporalTracker::predicted_size() const
{
    return initialized_ ? std::exp(state_[4]) : kMinSize;
}

float TemporalTracker::association_score(const CandidateObservation &candidate) const
{
    if (!initialized_) {
        return 0.5F;
    }

    const float dx = candidate.center_x - state_[0];
    const float dy = candidate.center_y - state_[1];
    const float distance = std::sqrt(dx * dx + dy * dy);
    const float predicted_apparent_size = std::max(predicted_size(), kMinSize);
    const float gate = std::min(
        config_.gate_max_px,
        std::max(config_.gate_min_px,
                 predicted_apparent_size * config_.gate_size_factor));
    const float spatial_score = std::exp(-0.5F * (distance / gate) * (distance / gate));

    const float log_size = std::log(std::max(candidate.apparent_size, kMinSize));
    const float log_difference = std::fabs(log_size - state_[4]);
    const float size_jump_limit =
        candidate.source == last_candidate_.source
            ? config_.max_log_size_jump
            : config_.max_cross_source_log_size_jump;
    const float size_scale = std::max(0.25F, size_jump_limit * 0.5F);
    const float size_score = std::exp(-0.5F * (log_difference / size_scale) *
                                     (log_difference / size_scale));
    return clamp01(spatial_score * size_score);
}

bool TemporalTracker::passes_association_gate(
    const CandidateObservation &candidate) const
{
    if (!initialized_) {
        return true;
    }

    const float dx = candidate.center_x - state_[0];
    const float dy = candidate.center_y - state_[1];
    const float distance = std::sqrt(dx * dx + dy * dy);
    const float gate = std::min(
        config_.gate_max_px,
        std::max(config_.gate_min_px,
                 std::max(predicted_size(), kMinSize) *
                     config_.gate_size_factor));
    if (distance > gate) {
        return false;
    }

    const float log_difference = std::fabs(
        std::log(std::max(candidate.apparent_size, kMinSize)) - state_[4]);
    const float size_jump_limit =
        candidate.source == last_candidate_.source
            ? config_.max_log_size_jump
            : config_.max_cross_source_log_size_jump;
    if (log_difference > size_jump_limit) {
        return false;
    }
    return association_score(candidate) >= config_.min_association_score;
}

bool TemporalTracker::matches_tentative_candidate(
    const CandidateObservation &candidate) const
{
    if (!initialized_) {
        return false;
    }
    const float dx = candidate.center_x - state_[0];
    const float dy = candidate.center_y - state_[1];
    if (std::sqrt(dx * dx + dy * dy) > config_.confirmation_gate_px) {
        return false;
    }

    const float log_difference = std::fabs(
        std::log(std::max(candidate.apparent_size, kMinSize)) - state_[4]);
    const float size_jump_limit =
        candidate.source == last_candidate_.source
            ? config_.max_log_size_jump
            : config_.max_cross_source_log_size_jump;
    return log_difference <= size_jump_limit;
}

void TemporalTracker::correct(const CandidateObservation &candidate)
{
    const float measurement[3] = {
        candidate.center_x,
        candidate.center_y,
        std::log(std::max(candidate.apparent_size, kMinSize)),
    };
    const int observed_state_index[3] = {0, 1, 4};

    float innovation[3]{};
    for (int i = 0; i < 3; ++i) {
        innovation[i] = measurement[i] - state_[observed_state_index[i]];
    }

    float innovation_covariance[3][3]{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            innovation_covariance[row][column] =
                covariance_[observed_state_index[row]][observed_state_index[column]];
        }
    }
    innovation_covariance[0][0] += config_.measurement_noise_position;
    innovation_covariance[1][1] += config_.measurement_noise_position;
    innovation_covariance[2][2] += config_.measurement_noise_log_size;

    float inverse_innovation_covariance[3][3]{};
    if (!invert_3x3(innovation_covariance, inverse_innovation_covariance)) {
        return;
    }

    float kalman_gain[6][3]{};
    for (int row = 0; row < 6; ++row) {
        for (int column = 0; column < 3; ++column) {
            for (int k = 0; k < 3; ++k) {
                kalman_gain[row][column] +=
                    covariance_[row][observed_state_index[k]] *
                    inverse_innovation_covariance[k][column];
            }
        }
    }

    for (int row = 0; row < 6; ++row) {
        for (int i = 0; i < 3; ++i) {
            state_[row] += kalman_gain[row][i] * innovation[i];
        }
    }

    const auto previous_covariance = covariance_;
    for (int row = 0; row < 6; ++row) {
        for (int column = 0; column < 6; ++column) {
            float correction = 0.0F;
            for (int k = 0; k < 3; ++k) {
                correction += kalman_gain[row][k] *
                              previous_covariance[observed_state_index[k]][column];
            }
            covariance_[row][column] = previous_covariance[row][column] - correction;
        }
    }
}

void TemporalTracker::push_hit(bool hit)
{
    hit_history_.push_back(hit);
    while (static_cast<int>(hit_history_.size()) > config_.confirm_window) {
        hit_history_.pop_front();
    }
}

int TemporalTracker::recent_hits() const
{
    return static_cast<int>(std::count(hit_history_.begin(), hit_history_.end(), true));
}

GreenLightDetection TemporalTracker::update(const CandidateObservation *candidate,
                                            uint64_t timestamp_us,
                                            const CameraModel &camera_model)
{
    predict(timestamp_us);

    GreenLightDetection result;
    result.timestamp_us = timestamp_us;

    if (candidate != nullptr && tracking_confirmed() &&
        !passes_association_gate(*candidate)) {
        candidate = nullptr;
    }

    if (candidate == nullptr) {
        push_hit(false);
        ++missed_frames_;
        if (missed_frames_ >= config_.max_missed_frames) {
            reset();
        } else if (initialized_ && track_state_ != TrackState::Tracking) {
            track_state_ = TrackState::Candidate;
        }
        result.state = track_state_;
        return result;
    }

    if (!initialized_) {
        initialize(*candidate, timestamp_us);
        hit_history_.clear();
    } else if (track_state_ != TrackState::Tracking &&
               !matches_tentative_candidate(*candidate)) {
        initialize(*candidate, timestamp_us);
        hit_history_.clear();
    } else {
        correct(*candidate);
    }

    last_candidate_ = *candidate;
    missed_frames_ = 0;
    push_hit(true);
    if (track_state_ != TrackState::Tracking) {
        track_state_ = recent_hits() >= config_.confirm_hits
                           ? TrackState::Tracking
                           : TrackState::Candidate;
    }

    const auto angles = pixel_to_angles(state_[0], state_[1], camera_model);
    const float hit_ratio = config_.confirm_window > 0
                                ? static_cast<float>(recent_hits()) /
                                      static_cast<float>(config_.confirm_window)
                                : 1.0F;

    // A tentative observation is useful for visualization, but must not be
    // consumed by aiming/control until the same physical candidate has been
    // confirmed across multiple frames.
    result.valid = track_state_ == TrackState::Tracking;
    result.state = track_state_;
    result.center_x = state_[0];
    result.center_y = state_[1];
    result.bbox_x = candidate->bbox_x;
    result.bbox_y = candidate->bbox_y;
    result.bbox_w = candidate->bbox_w;
    result.bbox_h = candidate->bbox_h;
    result.apparent_size = std::exp(state_[4]);
    result.yaw_rad = angles[0];
    result.pitch_rad = angles[1];
    result.confidence = result.valid
                            ? clamp01(0.60F * candidate->score +
                                      0.25F * candidate->association_score +
                                      0.15F * hit_ratio)
                            : 0.0F;
    return result;
}

}  // namespace detail
}  // namespace dart
