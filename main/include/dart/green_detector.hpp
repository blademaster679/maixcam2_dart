#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "maix_image.hpp"

namespace dart {

enum class TrackState {
    Lost,
    Candidate,
    Tracking,
};

const char *track_state_name(TrackState state);

struct GreenLightDetection {
    bool valid = false;
    uint64_t timestamp_us = 0;
    TrackState state = TrackState::Lost;
    float center_x = 0.0F;
    float center_y = 0.0F;
    int bbox_x = 0;
    int bbox_y = 0;
    int bbox_w = 0;
    int bbox_h = 0;
    float apparent_size = 0.0F;
    float yaw_rad = 0.0F;
    float pitch_rad = 0.0F;
    float confidence = 0.0F;
};

struct GreenLightCandidateDebug {
    float center_x = 0.0F;
    float center_y = 0.0F;
    int bbox_x = 0;
    int bbox_y = 0;
    int bbox_w = 0;
    int bbox_h = 0;
    float apparent_size = 0.0F;
    float density = 0.0F;
    float green_dominance = 0.0F;
    float green_fraction = 0.0F;
    float local_contrast = 0.0F;
    float shape_score = 0.0F;
    float core_score = 0.0F;
    float temporal_score = 0.0F;
    float center_prior_score = 0.0F;
    float initial_size_score = 0.0F;
    float score = 0.0F;
    bool saturated_core = false;
    bool selected = false;
};

struct LabThreshold {
    int l_min = 0;
    int l_max = 100;
    int a_min = -128;
    int a_max = 127;
    int b_min = -128;
    int b_max = 127;

    std::vector<int> as_vector() const;
};

struct CameraModel {
    float fx = 4096.0F;
    float fy = 4096.0F;
    float principal_x = 640.0F;
    float principal_y = 360.0F;
    float k1 = 0.0F;
    float k2 = 0.0F;
    float p1 = 0.0F;
    float p2 = 0.0F;
    float k3 = 0.0F;
};

struct DetectorConfig {
    LabThreshold core_lab{70, 100, -90, -5, -40, 90};
    LabThreshold halo_lab{25, 100, -90, -3, -60, 100};

    float min_candidate_score = 0.42F;
    float min_tracking_score = 0.35F;
    float min_density = 0.03F;
    float min_green_dominance = 0.05F;
    float min_local_contrast = -0.03F;
    float min_halo_size_px = 1.0F;
    float core_center_max_fraction = 0.45F;
    float center_prior_radius_px = 160.0F;
    float initial_size_reference_px = 24.0F;
    bool enable_saturated_core_candidates = false;
    float min_core_brightness = 0.88F;
    float min_core_ring_green_dominance = 0.04F;
    float min_core_ring_green_fraction = 0.0F;
    float core_ring_scale = 0.35F;
    float min_core_size_px = 3.0F;

    float weight_color = 0.25F;
    float weight_contrast = 0.20F;
    float weight_density = 0.15F;
    float weight_shape = 0.10F;
    float weight_core = 0.10F;
    float weight_temporal = 0.20F;
    float weight_center_prior = 0.0F;
    float weight_initial_size = 0.0F;
    float weight_core_size = 0.0F;

    int confirm_window = 5;
    int confirm_hits = 3;
    int max_missed_frames = 5;
    float confirmation_gate_px = 48.0F;
    float gate_min_px = 32.0F;
    float gate_max_px = 96.0F;
    float gate_size_factor = 4.0F;
    float max_log_size_jump = 2.3F;
    float max_cross_source_log_size_jump = 2.3F;
    float min_association_score = 0.02F;

    float process_noise_position = 20.0F;
    float process_noise_velocity = 100.0F;
    float process_noise_log_size = 0.6F;
    float process_noise_size_rate = 2.0F;
    float measurement_noise_position = 4.0F;
    float measurement_noise_log_size = 0.08F;

    int sample_grid = 20;
    int ring_margin_px = 8;
    int merge_margin_px = 2;
    bool merge_blobs = true;

    CameraModel camera_model;
};

struct CameraSettings {
    int width = 1280;
    int height = 720;
    int fps = 60;
    int buffer_count = 3;
    int warmup_frames = 30;
    int exposure_us = 0;
    int gain = -1;
    bool manual_white_balance = false;
    std::array<float, 4> white_balance_gain{0.0682F, 0.0F, 0.0F, 0.04897F};
};

struct DebugSettings {
    bool enabled = false;
    std::string directory = "/tmp/dart_green_debug";
    int save_every_n_frames = 0;
    bool save_on_state_change = true;
    bool save_failed_frames = false;
    int max_saved_frames = 1000;
};

struct ApplicationConfig {
    DetectorConfig detector;
    CameraSettings camera;
    DebugSettings debug;
};

ApplicationConfig load_application_config(const std::string &path);

namespace detail {

enum class CandidateSource {
    Halo,
    SaturatedCore,
};

struct CandidateObservation {
    float center_x = 0.0F;
    float center_y = 0.0F;
    int bbox_x = 0;
    int bbox_y = 0;
    int bbox_w = 0;
    int bbox_h = 0;
    float apparent_size = 1.0F;
    float score = 0.0F;
    float association_score = 0.5F;
    CandidateSource source = CandidateSource::Halo;
};

class TemporalTracker {
public:
    explicit TemporalTracker(const DetectorConfig &config);

    void reset();
    void predict(uint64_t timestamp_us);
    bool has_prediction() const;
    bool tracking_confirmed() const;
    float predicted_x() const;
    float predicted_y() const;
    float predicted_size() const;
    float association_score(const CandidateObservation &candidate) const;
    bool passes_association_gate(const CandidateObservation &candidate) const;
    GreenLightDetection update(const CandidateObservation *candidate,
                               uint64_t timestamp_us,
                               const CameraModel &camera_model);

private:
    DetectorConfig config_;
    bool initialized_ = false;
    uint64_t last_timestamp_us_ = 0;
    std::array<float, 6> state_{};
    std::array<std::array<float, 6>, 6> covariance_{};
    std::deque<bool> hit_history_;
    int missed_frames_ = 0;
    TrackState track_state_ = TrackState::Lost;
    CandidateObservation last_candidate_{};

    void initialize(const CandidateObservation &candidate, uint64_t timestamp_us);
    void correct(const CandidateObservation &candidate);
    bool matches_tentative_candidate(const CandidateObservation &candidate) const;
    void push_hit(bool hit);
    int recent_hits() const;
};

std::array<float, 2> pixel_to_angles(float pixel_x,
                                     float pixel_y,
                                     const CameraModel &model);

}  // namespace detail

class GreenLightDetector {
public:
    explicit GreenLightDetector(const DetectorConfig &config = DetectorConfig());

    GreenLightDetection process(maix::image::Image &frame,
                                uint64_t timestamp_us);
    void reset();
    const std::vector<GreenLightCandidateDebug> &last_candidates() const;

private:
    struct Candidate;

    DetectorConfig config_;
    detail::TemporalTracker tracker_;
    std::vector<GreenLightCandidateDebug> last_candidates_;

    std::vector<Candidate> collect_candidates(maix::image::Image &frame);
};

}  // namespace dart
