#include "dart/green_detector.hpp"

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

void write_detection_json(std::ostream &output,
                          const dart::GreenLightDetection &detection)
{
    output << std::fixed << std::setprecision(6)
           << "{\"timestamp_us\":" << detection.timestamp_us
           << ",\"valid\":" << (detection.valid ? "true" : "false")
           << ",\"state\":\"" << dart::track_state_name(detection.state) << '"'
           << ",\"center_x\":" << detection.center_x
           << ",\"center_y\":" << detection.center_y
           << ",\"bbox_x\":" << detection.bbox_x
           << ",\"bbox_y\":" << detection.bbox_y
           << ",\"bbox_w\":" << detection.bbox_w
           << ",\"bbox_h\":" << detection.bbox_h
           << ",\"apparent_size\":" << detection.apparent_size
           << ",\"yaw_rad\":" << detection.yaw_rad
           << ",\"pitch_rad\":" << detection.pitch_rad
           << ",\"confidence\":" << detection.confidence
           << '}';
}

void write_candidates_json(
    std::ostream &output,
    const std::vector<dart::GreenLightCandidateDebug> &candidates)
{
    output << '[';
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (index > 0) {
            output << ',';
        }
        const auto &candidate = candidates[index];
        output << std::fixed << std::setprecision(6)
               << "{\"center_x\":" << candidate.center_x
               << ",\"center_y\":" << candidate.center_y
               << ",\"bbox_x\":" << candidate.bbox_x
               << ",\"bbox_y\":" << candidate.bbox_y
               << ",\"bbox_w\":" << candidate.bbox_w
               << ",\"bbox_h\":" << candidate.bbox_h
               << ",\"apparent_size\":" << candidate.apparent_size
               << ",\"density\":" << candidate.density
               << ",\"green_dominance\":" << candidate.green_dominance
               << ",\"local_contrast\":" << candidate.local_contrast
               << ",\"shape_score\":" << candidate.shape_score
               << ",\"core_score\":" << candidate.core_score
               << ",\"temporal_score\":" << candidate.temporal_score
               << ",\"center_prior_score\":" << candidate.center_prior_score
               << ",\"initial_size_score\":" << candidate.initial_size_score
               << ",\"score\":" << candidate.score
               << ",\"saturated_core\":"
               << (candidate.saturated_core ? "true" : "false")
               << ",\"selected\":" << (candidate.selected ? "true" : "false")
               << '}';
    }
    output << ']';
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
    write_detection_json(metadata, detection);
    metadata << ",\"candidates\":";
    write_candidates_json(metadata, candidates);
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

int run(int argc, char **argv)
{
    const CommandLine command_line = parse_command_line(argc, argv);
    const dart::ApplicationConfig config =
        dart::load_application_config(command_line.config_path);

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

    dart::GreenLightDetector detector(config.detector);
    dart::TrackState previous_state = dart::TrackState::Lost;
    uint64_t frame_index = 0;
    int saved_frames = 0;

    while (!maix::app::need_exit()) {
        std::unique_ptr<maix::image::Image> frame(camera.read(true, -1));
        if (!frame) {
            std::cerr << "warning: camera read failed\n";
            continue;
        }

        const uint64_t timestamp_us = maix::time::ticks_us();
        const dart::GreenLightDetection detection =
            detector.process(*frame, timestamp_us);
        write_detection_json(std::cout, detection);
        std::cout << '\n' << std::flush;

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
