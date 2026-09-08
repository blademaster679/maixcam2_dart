#include "dart/green_detector.hpp"
#include "dart/target_json.hpp"
#include "dart/visual_motion.hpp"

#include "maix_basic.hpp"
#include "maix_camera.hpp"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct CommandLine {
    std::string config_path = "green_detector.conf";
};

CommandLine parse_command_line(int argc, char **argv)
{
    CommandLine result;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--config") {
            if (++index >= argc) {
                throw std::runtime_error("--config requires a path");
            }
            result.config_path = argv[index];
        } else if (argument == "--help" || argument == "-h") {
            std::cerr << "Usage: dart_green_detect [--config PATH]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }
    return result;
}

void save_debug_frame(maix::image::Image &frame,
                      const dart::GreenLightDetection &detection,
                      const std::vector<dart::GreenLightCandidateDebug> &candidates,
                      const std::string &directory,
                      uint64_t frame_index)
{
    std::ostringstream base_name;
    base_name << directory << "/frame_" << std::setw(10) << std::setfill('0')
              << frame_index << '_' << detection.timestamp_us;

    const std::string image_path = base_name.str() + ".jpg";
    if (frame.save(image_path.c_str(), 90) != maix::err::ERR_NONE) {
        std::cerr << "failed to save debug image: " << image_path << '\n';
    }

    const std::string metadata_path = base_name.str() + ".json";
    std::ofstream metadata(metadata_path);
    if (!metadata) {
        std::cerr << "failed to save debug metadata: " << metadata_path << '\n';
        return;
    }
    metadata << "{\"detection\":";
    dart::write_green_detection_json(metadata, detection);
    metadata << ",\"candidates\":";
    dart::write_candidate_list_json(metadata, candidates);
    metadata << "}\n";
}

void apply_camera_settings(maix::camera::Camera &camera,
                           const dart::CameraSettings &settings)
{
    camera.skip_frames(settings.warmup_frames);

    if (settings.exposure_us > 0) {
        camera.exp_mode(maix::camera::AeMode::Manual);
        camera.exposure(settings.exposure_us);
    } else {
        std::cerr << "warning: camera.exposure_us is not calibrated; auto exposure remains enabled\n";
    }
    if (settings.gain >= 0) {
        camera.gain(settings.gain);
    } else {
        std::cerr << "warning: camera.gain is not calibrated\n";
    }
    if (settings.manual_white_balance) {
        camera.awb_mode(maix::camera::AwbMode::Manual);
        camera.set_wb_gain(std::vector<float>(settings.white_balance_gain.begin(),
                                              settings.white_balance_gain.end()));
    } else {
        std::cerr << "warning: manual white balance is disabled\n";
    }

    camera.skip_frames(3);
}

long read_resident_memory_kb()
{
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") {
            long value = 0;
            status >> value;
            return value;
        }
        std::string remainder;
        std::getline(status, remainder);
    }
    return -1;
}

int run(int argc, char **argv)
{
    const CommandLine command_line = parse_command_line(argc, argv);
    const dart::ApplicationConfig config =
        dart::load_application_config(command_line.config_path);

    if (config.camera.fps > 60) {
        throw std::runtime_error("High-fps profiles require the isolated business_capture VIN entry point");
    }

    maix::camera::Camera camera(config.camera.width,
                                config.camera.height,
                                maix::image::Format::FMT_RGB888,
                                nullptr,
                                config.camera.fps,
                                config.camera.buffer_count);
    apply_camera_settings(camera, config.camera);

    if (camera.width() != config.camera.width ||
        camera.height() != config.camera.height) {
        throw std::runtime_error("camera did not accept the configured resolution");
    }

    if (config.debug.enabled &&
        maix::fs::mkdir(config.debug.directory, true, true) != maix::err::ERR_NONE) {
        throw std::runtime_error("cannot create debug directory: " +
                                 config.debug.directory);
    }

    const auto pose_validator = dart::create_yolo_pose_validator(config.npu);
    dart::GreenLightDetector detector(config.detector,
                                      config.armor,
                                      config.target_geometry,
                                      config.npu,
                                      pose_validator);
    dart::VisualMotionEstimator visual_motion(config.visual_motion);
    dart::TrackState previous_state = dart::TrackState::Lost;
    uint64_t frame_index = 0;
    uint64_t previous_capture_timestamp_us = 0;
    int saved_frames = 0;
    long resident_memory_kb = read_resident_memory_kb();

    while (!maix::app::need_exit()) {
        std::unique_ptr<maix::image::Image> frame(camera.read(true, -1));
        if (!frame) {
            std::cerr << "warning: camera read failed\n";
            continue;
        }

        const uint64_t timestamp_us = maix::time::ticks_us();
        const uint64_t capture_interval_us = previous_capture_timestamp_us == 0
                                                 ? 0
                                                 : timestamp_us -
                                                       previous_capture_timestamp_us;
        previous_capture_timestamp_us = timestamp_us;
        const uint64_t processing_started_us = maix::time::ticks_us();
        const bool visual_motion_allowed =
            !config.visual_motion.tracking_only ||
            previous_state == dart::TrackState::Tracking;
        if (config.visual_motion.tracking_only && !visual_motion_allowed) {
            visual_motion.reset();
        }
        const bool visual_motion_ran = config.visual_motion.enabled &&
            visual_motion_allowed &&
            frame_index % static_cast<uint64_t>(
                config.visual_motion.interval_frames) == 0;
        const uint64_t visual_motion_started_us = maix::time::ticks_us();
        dart::MotionPrior motion_prior;
        if (visual_motion_ran) {
            motion_prior = visual_motion.update(*frame, timestamp_us);
        }
        const uint64_t visual_motion_finished_us = maix::time::ticks_us();
        const dart::TargetEstimate target = detector.process(
            *frame, timestamp_us, motion_prior.valid ? &motion_prior : nullptr);
        const uint64_t processing_finished_us = maix::time::ticks_us();
        const auto &detection = target.green;
        if (frame_index % 60U == 0U) {
            resident_memory_kb = read_resident_memory_kb();
        }
        const bool write_json_log =
            config.debug.json_log_every_n_frames > 0 &&
            frame_index % static_cast<uint64_t>(
                config.debug.json_log_every_n_frames) == 0;
        if (write_json_log) {
            std::ostringstream runtime_fields;
            runtime_fields << std::fixed << std::setprecision(6)
                           << ",\"frame\":" << frame_index
                           << ",\"processing_ms\":"
                           << (processing_finished_us - processing_started_us) /
                                  1000.0
                           << ",\"visual_motion_ran\":"
                           << (visual_motion_ran ? "true" : "false")
                           << ",\"visual_motion_ms\":"
                           << (visual_motion_finished_us -
                               visual_motion_started_us) / 1000.0
                           << ",\"capture_interval_us\":"
                           << capture_interval_us
                           << ",\"rss_kb\":" << resident_memory_kb;
            const auto *logged_candidates = config.debug.log_candidates
                ? &detector.last_candidates()
                : nullptr;
            dart::write_target_estimate_json(std::cout, target,
                                             logged_candidates,
                                             runtime_fields.str());
            std::cout << '\n' << std::flush;
        }

        const bool periodic_save = config.debug.save_every_n_frames > 0 &&
                                   frame_index % config.debug.save_every_n_frames == 0;
        const bool state_change = config.debug.save_on_state_change &&
                                  detection.state != previous_state;
        const bool failed_frame = config.debug.save_failed_frames &&
                                  !detection.valid;
        const bool below_save_limit = config.debug.max_saved_frames == 0 ||
                                      saved_frames < config.debug.max_saved_frames;
        if (config.debug.enabled && below_save_limit &&
            (periodic_save || state_change || failed_frame)) {
            save_debug_frame(*frame,
                             detection,
                             detector.last_candidates(),
                             config.debug.directory,
                             frame_index);
            ++saved_frames;
        }

        previous_state = detection.state;
        ++frame_index;
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    maix::sys::register_default_signal_handle();
    try {
        return run(argc, argv);
    } catch (const std::exception &exception) {
        std::cerr << "fatal: " << exception.what() << '\n';
        return 1;
    }
}
