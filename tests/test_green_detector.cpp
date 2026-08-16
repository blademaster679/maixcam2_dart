#include "dart/green_detector.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

dart::detail::CandidateObservation observation(
    float x,
    float y,
    float size,
    dart::detail::CandidateSource source = dart::detail::CandidateSource::Halo)
{
    dart::detail::CandidateObservation candidate;
    candidate.center_x = x;
    candidate.center_y = y;
    candidate.bbox_x = static_cast<int>(x - size * 0.5F);
    candidate.bbox_y = static_cast<int>(y - size * 0.5F);
    candidate.bbox_w = static_cast<int>(size);
    candidate.bbox_h = static_cast<int>(size);
    candidate.apparent_size = size;
    candidate.score = 0.9F;
    candidate.association_score = 0.9F;
    candidate.source = source;
    return candidate;
}

maix::image::Image green_blob_image(int image_width,
                                    int image_height,
                                    int x,
                                    int y,
                                    int width,
                                    int height,
                                    float center_x,
                                    float center_y)
{
    maix::image::Image image(image_width, image_height);
    image.fill_rect(x, y, width, height, 10, 250, 15);
    const maix::image::Blob halo(x, y, width, height, center_x, center_y,
                                 width * height, 0.9F);
    const int core_width = std::max(1, width / 2);
    const int core_height = std::max(1, height / 2);
    const int core_x = static_cast<int>(center_x - core_width * 0.5F);
    const int core_y = static_cast<int>(center_y - core_height * 0.5F);
    const maix::image::Blob core(core_x, core_y, core_width, core_height,
                                 center_x, center_y, core_width * core_height, 0.9F);
    image.set_blobs({halo}, {core});
    return image;
}

void test_confirmation_and_loss()
{
    dart::DetectorConfig config;
    dart::detail::TemporalTracker tracker(config);
    const auto candidate = observation(640.0F, 360.0F, 4.0F);

    auto result = tracker.update(&candidate, 1000000, config.camera_model);
    check(result.state == dart::TrackState::Candidate, "first hit is CANDIDATE");
    check(!result.valid, "unconfirmed first hit is not a control-valid result");
    result = tracker.update(&candidate, 1016667, config.camera_model);
    check(result.state == dart::TrackState::Candidate, "second hit is CANDIDATE");
    result = tracker.update(&candidate, 1033334, config.camera_model);
    check(result.state == dart::TrackState::Tracking, "third of five hits enters TRACKING");
    check(result.valid, "confirmed track is control-valid");

    for (int miss = 1; miss <= 4; ++miss) {
        result = tracker.update(nullptr,
                                1033334 + static_cast<uint64_t>(miss) * 16667,
                                config.camera_model);
        check(result.state == dart::TrackState::Tracking,
              "TRACKING is retained before the fifth consecutive miss");
    }
    result = tracker.update(nullptr, 1116669, config.camera_model);
    check(result.state == dart::TrackState::Lost, "fifth miss returns to LOST");
    check(!tracker.has_prediction(), "fifth miss clears the prediction");
}

void test_sparse_three_of_five_confirmation()
{
    dart::DetectorConfig config;
    dart::detail::TemporalTracker tracker(config);
    const auto candidate = observation(100.0F, 80.0F, 3.0F);
    tracker.update(&candidate, 1000000, config.camera_model);
    tracker.update(nullptr, 1010000, config.camera_model);
    tracker.update(&candidate, 1020000, config.camera_model);
    tracker.update(nullptr, 1030000, config.camera_model);
    const auto result = tracker.update(&candidate, 1040000, config.camera_model);
    check(result.state == dart::TrackState::Tracking,
          "three non-consecutive hits in five frames confirm tracking");
}

void test_growing_size_tracker()
{
    dart::DetectorConfig config;
    dart::detail::TemporalTracker tracker(config);
    float previous_size = 0.0F;
    uint64_t timestamp = 1000000;
    int frame = 0;
    for (const float size : {2.0F, 4.0F, 8.0F, 16.0F, 32.0F, 64.0F}) {
        const auto candidate = observation(320.0F, 180.0F, size);
        const auto result = tracker.update(&candidate, timestamp, config.camera_model);
        check(result.valid == (frame >= 2),
              "growing target becomes valid only after confirmation");
        check(result.apparent_size > previous_size,
              "log-size state increases with an approaching target");
        previous_size = result.apparent_size;
        timestamp += 16667;
        ++frame;
    }
}

void test_confirmation_requires_same_candidate()
{
    dart::DetectorConfig config;
    config.confirmation_gate_px = 30.0F;
    dart::detail::TemporalTracker tracker(config);
    const auto first = observation(100.0F, 100.0F, 12.0F);
    const auto second = observation(220.0F, 100.0F, 12.0F);

    tracker.update(&first, 1000000, config.camera_model);
    tracker.update(&second, 1016667, config.camera_model);
    tracker.update(&first, 1033334, config.camera_model);
    const auto result = tracker.update(&second, 1050001, config.camera_model);
    check(result.state == dart::TrackState::Candidate && !result.valid,
          "different distractors cannot combine their hits to confirm a track");
}

void test_hard_association_gate()
{
    dart::DetectorConfig config;
    config.gate_min_px = 50.0F;
    config.gate_max_px = 80.0F;
    config.gate_size_factor = 2.0F;
    config.max_log_size_jump = 1.0F;
    config.max_cross_source_log_size_jump = 2.0F;
    config.min_association_score = 0.05F;
    dart::detail::TemporalTracker tracker(config);
    const auto target = observation(320.0F, 180.0F, 20.0F);
    tracker.update(&target, 1000000, config.camera_model);
    tracker.update(&target, 1016667, config.camera_model);
    tracker.update(&target, 1033334, config.camera_model);

    const auto far_distractor = observation(500.0F, 180.0F, 20.0F);
    check(!tracker.passes_association_gate(far_distractor),
          "candidate outside the hard position gate is rejected");
    const auto tiny_same_source = observation(322.0F, 181.0F, 3.0F);
    check(!tracker.passes_association_gate(tiny_same_source),
          "same-source size collapse is rejected");
    const auto compact_core = observation(
        322.0F, 181.0F, 5.0F,
        dart::detail::CandidateSource::SaturatedCore);
    check(tracker.passes_association_gate(compact_core),
          "nearby halo-to-core transition uses the cross-source size gate");

    const auto rejected = tracker.update(&far_distractor, 1050001,
                                         config.camera_model);
    check(!rejected.valid && rejected.state == dart::TrackState::Tracking,
          "rejected measurement produces an invalid prediction-only frame");
    const auto recovered = tracker.update(&target, 1066668, config.camera_model);
    check(recovered.valid && std::fabs(recovered.center_x - 320.0F) < 3.0F,
          "rejected distractor does not pull the Kalman state");
}

void test_angles_and_distortion()
{
    dart::CameraModel model;
    model.fx = 400.0F;
    model.fy = 400.0F;
    model.principal_x = 320.0F;
    model.principal_y = 240.0F;

    auto angles = dart::detail::pixel_to_angles(360.0F, 200.0F, model);
    check(angles[0] > 0.0F, "pixel right of principal point has positive yaw");
    check(angles[1] > 0.0F, "pixel above principal point has positive pitch");

    const float undistorted_yaw = angles[0];
    model.k1 = 0.4F;
    angles = dart::detail::pixel_to_angles(360.0F, 200.0F, model);
    check(std::fabs(angles[0]) < std::fabs(undistorted_yaw),
          "positive radial distortion is inverted for the output point");
}

void test_single_pixel_and_reflection()
{
    dart::DetectorConfig config;
    dart::GreenLightDetector detector(config);

    auto one_pixel = green_blob_image(32, 24, 10, 8, 1, 1, 10.0F, 8.0F);
    auto result = detector.process(one_pixel, 1000000);
    result = detector.process(one_pixel, 1016667);
    result = detector.process(one_pixel, 1033334);
    check(result.valid, "single-pixel green target is accepted after confirmation");
    check(result.bbox_w == 1 && result.bbox_h == 1,
          "single-pixel bounding box is preserved");

    detector.reset();
    maix::image::Image reflection(32, 24);
    reflection.set_pixel(10, 8, 180, 190, 180);
    reflection.set_blobs({maix::image::Blob(10, 8, 1, 1, 10.0F, 8.0F)}, {});
    result = detector.process(reflection, 1016667);
    check(!result.valid, "weak green reflection fails RGB dominance threshold");
}

void test_large_growth_and_partial_frame()
{
    dart::DetectorConfig config;
    dart::GreenLightDetector detector(config);
    uint64_t timestamp = 1000000;
    float previous_size = 0.0F;
    struct Size { int width; int height; };
    const std::vector<Size> sizes{{2, 2}, {4, 3}, {8, 5}, {16, 10},
                                  {32, 20}, {50, 40}, {80, 50}};
    int frame = 0;
    for (const auto size : sizes) {
        const int x = 50 - size.width / 2;
        const int y = 50 - size.height / 2;
        auto image = green_blob_image(100, 100, x, y, size.width, size.height,
                                      50.0F, 50.0F);
        const auto result = detector.process(image, timestamp);
        check(result.valid == (frame >= 2),
              "growing image target becomes valid after confirmation");
        check(result.apparent_size >= previous_size,
              "filtered apparent size does not reverse during monotonic growth");
        check(std::fabs(result.center_x - 50.0F) < 1.0F &&
                  std::fabs(result.center_y - 50.0F) < 1.0F,
              "large-blob center remains stable");
        previous_size = result.apparent_size;
        timestamp += 16667;
        ++frame;
    }

    detector.reset();
    auto partial = green_blob_image(40, 30, 0, 8, 8, 8, 2.0F, 12.0F);
    detector.process(partial, timestamp);
    detector.process(partial, timestamp + 16667);
    const auto result = detector.process(partial, timestamp + 33334);
    check(result.valid, "partially clipped target at image edge is accepted");
}

void test_short_occlusion_and_multiple_candidates()
{
    dart::DetectorConfig config;
    dart::GreenLightDetector detector(config);
    uint64_t timestamp = 1000000;
    for (int frame = 0; frame < 3; ++frame) {
        auto image = green_blob_image(120, 90, 46, 36, 8, 8, 50.0F, 40.0F);
        detector.process(image, timestamp);
        timestamp += 16667;
    }

    for (int miss = 0; miss < 2; ++miss) {
        maix::image::Image blank(120, 90);
        blank.set_blobs({}, {});
        const auto result = detector.process(blank, timestamp);
        check(result.state == dart::TrackState::Tracking,
              "short occlusion does not drop TRACKING state");
        timestamp += 16667;
    }

    maix::image::Image multiple(120, 90);
    multiple.fill_rect(46, 36, 8, 8, 10, 250, 15);
    multiple.fill_rect(88, 68, 12, 12, 0, 255, 0);
    const maix::image::Blob target(46, 36, 8, 8, 50.0F, 40.0F, 64, 0.9F);
    const maix::image::Blob distractor(88, 68, 12, 12, 94.0F, 74.0F, 144, 1.0F);
    const maix::image::Blob target_core(48, 38, 4, 4, 50.0F, 40.0F, 16, 0.9F);
    const maix::image::Blob distractor_core(91, 71, 6, 6, 94.0F, 74.0F, 36, 1.0F);
    multiple.set_blobs({distractor, target}, {distractor_core, target_core});
    const auto result = detector.process(multiple, timestamp);
    check(result.valid && std::fabs(result.center_x - 50.0F) < 3.0F,
          "temporal association selects the prior target among green objects");
    check(result.state == dart::TrackState::Tracking,
          "target is reacquired after a short occlusion");
}

void test_saturated_core_with_green_ring()
{
    dart::DetectorConfig config;
    config.enable_saturated_core_candidates = true;
    config.min_core_size_px = 3.0F;
    config.min_core_brightness = 0.88F;
    config.min_core_ring_green_dominance = 0.04F;
    config.center_prior_radius_px = 30.0F;
    config.initial_size_reference_px = 12.0F;
    config.weight_center_prior = 1.0F;
    config.weight_initial_size = 0.3F;
    config.camera_model.principal_x = 50.0F;
    config.camera_model.principal_y = 40.0F;

    maix::image::Image image(100, 80);
    image.fill_rect(38, 28, 24, 24, 0, 255, 0);
    image.fill_rect(45, 35, 10, 10, 255, 255, 255);
    const maix::image::Blob lamp_core(45, 35, 10, 10,
                                      49.5F, 39.5F, 100, 0.95F);
    const maix::image::Blob isolated_noise(51, 66, 1, 1,
                                           51.0F, 66.0F, 1, 1.0F);
    image.set_blobs({}, {isolated_noise, lamp_core});

    dart::GreenLightDetector detector(config);
    detector.process(image, 1000000);
    detector.process(image, 1016667);
    const auto result = detector.process(image, 1033334);
    check(result.valid, "saturated core surrounded by green is accepted");
    check(std::fabs(result.center_x - 49.5F) < 1.0F &&
              std::fabs(result.center_y - 39.5F) < 1.0F,
          "saturated core supplies the lamp center");
    check(result.bbox_w == 10 && result.bbox_h == 10,
          "saturated core supplies the apparent lamp bounds");
    check(detector.last_candidates().size() == 1U &&
              detector.last_candidates().front().saturated_core,
          "sub-minimum isolated highlights are rejected");
}

void test_configuration()
{
    const auto project_config =
        std::filesystem::path(TEST_PROJECT_ROOT) / "config" / "green_detector.conf";
    const auto config = dart::load_application_config(project_config.string());
    check(config.camera.width == 1280 && config.camera.height == 720,
          "sample configuration parses");
    check(std::fabs(config.detector.min_association_score - 0.05F) < 1.0e-6F,
          "association setting parses");
    check(config.detector.enable_saturated_core_candidates &&
              !config.detector.merge_blobs,
          "saturated core and non-merging settings parse");
    check(std::fabs(config.detector.min_core_size_px - 3.0F) < 1.0e-6F &&
              std::fabs(config.detector.weight_center_prior - 0.25F) < 1.0e-6F &&
              std::fabs(config.detector.gate_max_px - 100.0F) < 1.0e-6F,
          "core size and center-prior settings parse");

    const auto invalid_path =
        std::filesystem::temp_directory_path() / "dart_green_detector_invalid.conf";
    {
        std::ofstream invalid(invalid_path);
        invalid << "unknown.key=1\n";
    }
    bool threw = false;
    try {
        (void)dart::load_application_config(invalid_path.string());
    } catch (const std::runtime_error &) {
        threw = true;
    }
    std::filesystem::remove(invalid_path);
    check(threw, "unknown configuration keys are rejected");
}

}  // namespace

int main()
{
    test_confirmation_and_loss();
    test_sparse_three_of_five_confirmation();
    test_growing_size_tracker();
    test_confirmation_requires_same_candidate();
    test_hard_association_gate();
    test_angles_and_distortion();
    test_single_pixel_and_reflection();
    test_large_growth_and_partial_frame();
    test_short_occlusion_and_multiple_candidates();
    test_saturated_core_with_green_ring();
    test_configuration();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "all green detector tests passed\n";
    return 0;
}
