#include "dart/green_detector.hpp"
#include "dart/target_json.hpp"
#include "dart/visual_motion.hpp"

#include "maix_image.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

namespace {

struct CommandLine {
    std::string input_path;
    std::string output_path;
    std::string jsonl_path;
    std::string config_path = "config/green_detector.conf";
    std::uint64_t max_frames = 0;
    bool native_resolution = false;
};

CommandLine parse_command_line(int argc, char **argv)
{
    CommandLine result;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto require_value = [&](const char *name) {
            if (++index >= argc) {
                throw std::runtime_error(std::string(name) + " requires a path");
            }
            return std::string(argv[index]);
        };

        if (argument == "--input") {
            result.input_path = require_value("--input");
        } else if (argument == "--output") {
            result.output_path = require_value("--output");
        } else if (argument == "--jsonl") {
            result.jsonl_path = require_value("--jsonl");
        } else if (argument == "--config") {
            result.config_path = require_value("--config");
        } else if (argument == "--max-frames") {
            const std::string value = require_value("--max-frames");
            std::size_t consumed = 0;
            result.max_frames = std::stoull(value, &consumed);
            if (consumed != value.size()) {
                throw std::runtime_error("--max-frames requires an integer");
            }
        } else if (argument == "--native-resolution") {
            result.native_resolution = true;
        } else if (argument == "--help" || argument == "-h") {
            std::cout
                << "Usage: dart_video_replay --input INPUT.mp4 --output OUTPUT.mp4 "
                   "[--jsonl OUTPUT.jsonl] [--config green_detector.conf] "
                   "[--max-frames N] [--native-resolution]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }
    if (result.input_path.empty() || result.output_path.empty()) {
        throw std::runtime_error("--input and --output are required");
    }
    if (result.jsonl_path.empty()) {
        result.jsonl_path = result.output_path + ".jsonl";
    }
    return result;
}

void create_parent_directory(const std::string &path)
{
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
}

cv::Scalar state_color(dart::TrackState state)
{
    switch (state) {
    case dart::TrackState::Tracking:
        return {0, 255, 0};
    case dart::TrackState::Candidate:
        return {0, 215, 255};
    case dart::TrackState::Lost:
        return {0, 0, 255};
    }
    return {255, 255, 255};
}

void draw_cross(cv::Mat &frame,
                const cv::Point &center,
                const cv::Scalar &color,
                int radius,
                int thickness)
{
    cv::line(frame,
             {center.x - radius, center.y},
             {center.x + radius, center.y},
             color,
             thickness,
             cv::LINE_AA);
    cv::line(frame,
             {center.x, center.y - radius},
             {center.x, center.y + radius},
             color,
             thickness,
             cv::LINE_AA);
}

void draw_overlay(cv::Mat &frame,
                  const dart::TargetEstimate &target,
                  const std::vector<dart::GreenLightCandidateDebug> &candidates,
                  std::uint64_t frame_index,
                  std::uint64_t total_frames,
                  double detector_fps,
                  double average_detector_fps,
                  double processing_ms)
{
    const auto &detection = target.green;
    const double size_scale = std::max(0.75, frame.rows / 720.0);
    const int line_thickness = std::max(1, static_cast<int>(std::lround(size_scale)));
    const int box_thickness = std::max(2, static_cast<int>(std::lround(2.0 * size_scale)));
    const double font_scale = 0.58 * size_scale;

    for (const auto &candidate : candidates) {
        const cv::Scalar color = candidate.selected
                                     ? cv::Scalar(255, 180, 0)
                                     : cv::Scalar(100, 100, 100);
        cv::rectangle(frame,
                      {candidate.bbox_x,
                       candidate.bbox_y,
                       candidate.bbox_w,
                       candidate.bbox_h},
                      color,
                      line_thickness,
                      cv::LINE_AA);
    }

    const cv::Scalar color = detection.predicted
                                 ? cv::Scalar(0, 165, 255)
                                 : state_color(detection.state);
    if (detection.valid) {
        cv::rectangle(frame,
                      {detection.bbox_x,
                       detection.bbox_y,
                       detection.bbox_w,
                       detection.bbox_h},
                      color,
                      box_thickness,
                      cv::LINE_AA);
        const cv::Point center(static_cast<int>(std::lround(detection.center_x)),
                               static_cast<int>(std::lround(detection.center_y)));
        draw_cross(frame,
                   center,
                   color,
                   std::max(6, static_cast<int>(std::lround(8.0 * size_scale))),
                   box_thickness);

        std::ostringstream target_label;
        target_label << (detection.predicted
                             ? "PREDICTED"
                             : dart::track_state_name(detection.state))
                     << " conf=" << std::fixed << std::setprecision(2)
                     << detection.confidence;
        const int label_y = std::max(24, detection.bbox_y - 8);
        cv::putText(frame,
                    target_label.str(),
                    {std::max(0, detection.bbox_x), label_y},
                    cv::FONT_HERSHEY_SIMPLEX,
                    font_scale,
                    color,
                    box_thickness,
                    cv::LINE_AA);
    }

    if (target.armor.valid) {
        const cv::Scalar armor_color = target.armor.color == dart::ArmorColor::Blue
                                           ? cv::Scalar(255, 120, 0)
                                           : cv::Scalar(0, 80, 255);
        const auto draw_bar = [&](const dart::LineSegment2f &bar) {
            cv::line(frame,
                     {static_cast<int>(std::lround(bar.top.x)),
                      static_cast<int>(std::lround(bar.top.y))},
                     {static_cast<int>(std::lround(bar.bottom.x)),
                      static_cast<int>(std::lround(bar.bottom.y))},
                     armor_color, box_thickness, cv::LINE_AA);
        };
        draw_bar(target.armor.left_bar);
        draw_bar(target.armor.right_bar);
        draw_cross(frame,
                   {static_cast<int>(std::lround(target.armor.center.x)),
                    static_cast<int>(std::lround(target.armor.center.y))},
                   armor_color, 7, box_thickness);
    }
    if (target.aim_point.valid) {
        draw_cross(frame,
                   {static_cast<int>(std::lround(target.aim_point.x)),
                    static_cast<int>(std::lround(target.aim_point.y))},
                   target.safe_for_control ? cv::Scalar(255, 255, 255)
                                           : cv::Scalar(80, 80, 255),
                   11, box_thickness);
    }

    std::vector<std::string> lines;
    {
        std::ostringstream stream;
        stream << "Frame " << frame_index + 1;
        if (total_frames > 0) {
            stream << '/' << total_frames;
        }
        stream << "  State: " << dart::track_state_name(detection.state)
               << (detection.predicted ? " (predicted)" : "")
               << "  Guidance: "
               << dart::guidance_track_state_name(target.state)
               << '/' << dart::guidance_mode_name(target.guidance_mode)
               << "  Safe: " << (target.safe_for_control ? "YES" : "NO")
               << "  Candidates: " << candidates.size();
        lines.push_back(stream.str());
    }
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(1)
               << "Detector FPS: " << detector_fps
               << "  Average: " << average_detector_fps
               << "  Time: " << processing_ms << " ms";
        lines.push_back(stream.str());
    }
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(1)
               << "Center: (" << detection.center_x << ", " << detection.center_y
               << ")  Size: " << detection.apparent_size;
        lines.push_back(stream.str());
    }

    int baseline = 0;
    int maximum_width = 0;
    const int line_height = cv::getTextSize(
                                "Ag", cv::FONT_HERSHEY_SIMPLEX, font_scale,
                                line_thickness, &baseline)
                                .height +
                            12;
    for (const auto &line : lines) {
        maximum_width = std::max(
            maximum_width,
            cv::getTextSize(line,
                            cv::FONT_HERSHEY_SIMPLEX,
                            font_scale,
                            line_thickness,
                            &baseline)
                .width);
    }
    const int panel_width = std::min(frame.cols, maximum_width + 24);
    const int panel_height = static_cast<int>(lines.size()) * line_height + 12;
    cv::Mat panel = frame(cv::Rect(0, 0, panel_width,
                                   std::min(frame.rows, panel_height)));
    cv::Mat dark = cv::Mat::zeros(panel.size(), panel.type());
    cv::addWeighted(panel, 0.35, dark, 0.65, 0.0, panel);

    for (std::size_t index = 0; index < lines.size(); ++index) {
        cv::putText(frame,
                    lines[index],
                    {12, 8 + static_cast<int>(index + 1) * line_height - 5},
                    cv::FONT_HERSHEY_SIMPLEX,
                    font_scale,
                    index == 0 ? color : cv::Scalar(255, 255, 255),
                    line_thickness,
                    cv::LINE_AA);
    }
}

void write_json_line(std::ostream &output,
                     std::uint64_t frame_index,
                     const dart::TargetEstimate &target,
                     const std::vector<dart::GreenLightCandidateDebug> &candidates,
                     double processing_ms,
                     double detector_fps,
                     double average_detector_fps)
{
    std::ostringstream extra;
    extra << std::fixed << std::setprecision(6)
          << ",\"frame\":" << frame_index
          << ",\"processing_ms\":" << processing_ms
          << ",\"detector_fps\":" << detector_fps
          << ",\"average_detector_fps\":" << average_detector_fps;
    dart::write_target_estimate_json(output, target, &candidates,
                                     extra.str());
    output << '\n';
}

int run(int argc, char **argv)
{
    const CommandLine command_line = parse_command_line(argc, argv);
    dart::ApplicationConfig config =
        dart::load_application_config(command_line.config_path);

    cv::VideoCapture capture(command_line.input_path);
    if (!capture.isOpened()) {
        throw std::runtime_error("cannot open input video: " +
                                 command_line.input_path);
    }
    const int source_width = static_cast<int>(
        std::lround(capture.get(cv::CAP_PROP_FRAME_WIDTH)));
    const int source_height = static_cast<int>(
        std::lround(capture.get(cv::CAP_PROP_FRAME_HEIGHT)));
    double source_fps = capture.get(cv::CAP_PROP_FPS);
    if (!std::isfinite(source_fps) || source_fps <= 0.0) {
        source_fps = 30.0;
    }
    const std::uint64_t total_frames = static_cast<std::uint64_t>(
        std::max(0.0, capture.get(cv::CAP_PROP_FRAME_COUNT)));

    const int width = command_line.native_resolution
                          ? source_width
                          : config.camera.width;
    const int height = command_line.native_resolution
                           ? source_height
                           : config.camera.height;
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("configured processing resolution is invalid");
    }

    if (command_line.native_resolution && config.camera.width > 0 &&
        config.camera.height > 0) {
        const float scale_x = static_cast<float>(source_width) /
                              config.camera.width;
        const float scale_y = static_cast<float>(source_height) /
                              config.camera.height;
        const float spatial_scale = std::sqrt(scale_x * scale_y);
        const float variance_scale = spatial_scale * spatial_scale;
        config.detector.camera_model.fx *= scale_x;
        config.detector.camera_model.fy *= scale_y;
        config.detector.camera_model.principal_x *= scale_x;
        config.detector.camera_model.principal_y *= scale_y;
        config.detector.center_prior_radius_px *= spatial_scale;
        config.detector.initial_size_reference_px *= spatial_scale;
        config.detector.min_core_size_px *= spatial_scale;
        config.detector.min_halo_size_px *= spatial_scale;
        config.detector.confirmation_gate_px *= spatial_scale;
        config.detector.gate_min_px *= spatial_scale;
        config.detector.gate_max_px *= spatial_scale;
        config.detector.ring_margin_px = static_cast<int>(std::lround(
            config.detector.ring_margin_px * spatial_scale));
        config.detector.merge_margin_px = static_cast<int>(std::lround(
            config.detector.merge_margin_px * spatial_scale));
        config.detector.process_noise_position *= variance_scale;
        config.detector.process_noise_velocity *= variance_scale;
        config.detector.measurement_noise_position *= variance_scale;
    }

    create_parent_directory(command_line.output_path);
    create_parent_directory(command_line.jsonl_path);
    cv::VideoWriter writer(command_line.output_path,
                           cv::VideoWriter::fourcc('m', 'p', '4', 'v'),
                           source_fps,
                           {width, height});
    if (!writer.isOpened()) {
        throw std::runtime_error("cannot create output video: " +
                                 command_line.output_path);
    }
    std::ofstream jsonl(command_line.jsonl_path);
    if (!jsonl) {
        throw std::runtime_error("cannot create JSONL output: " +
                                 command_line.jsonl_path);
    }

    dart::GreenLightDetector detector(config.detector,
                                      config.armor,
                                      config.target_geometry,
                                      config.npu,
                                      nullptr);
    dart::VisualMotionEstimator visual_motion(config.visual_motion);
    std::uint64_t frame_index = 0;
    std::uint64_t valid_frames = 0;
    std::uint64_t observed_frames = 0;
    std::uint64_t predicted_frames = 0;
    std::uint64_t safe_frames = 0;
    std::uint64_t armor_frames = 0;
    std::uint64_t candidate_frames = 0;
    std::uint64_t tracking_frames = 0;
    std::uint64_t lost_frames = 0;
    double total_processing_seconds = 0.0;
    double detector_fps_ema = 0.0;
    dart::TrackState previous_state = dart::TrackState::Lost;

    cv::Mat source_bgr;
    while (capture.read(source_bgr)) {
        if (command_line.max_frames > 0 &&
            frame_index >= command_line.max_frames) {
            break;
        }
        cv::Mat bgr;
        if (source_bgr.cols == width && source_bgr.rows == height) {
            bgr = source_bgr.clone();
        } else {
            cv::resize(source_bgr, bgr, {width, height}, 0.0, 0.0,
                       cv::INTER_AREA);
        }
        cv::Mat rgb;
        cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
        maix::image::Image frame(rgb);
        const std::uint64_t timestamp_us = static_cast<std::uint64_t>(
            std::llround(frame_index * 1000000.0 / source_fps));

        const auto started = std::chrono::steady_clock::now();
        dart::MotionPrior motion_prior;
        const bool visual_motion_allowed =
            !config.visual_motion.tracking_only ||
            previous_state == dart::TrackState::Tracking;
        if (config.visual_motion.tracking_only && !visual_motion_allowed) {
            visual_motion.reset();
        }
        if (config.visual_motion.enabled &&
            visual_motion_allowed &&
            frame_index % static_cast<std::uint64_t>(
                config.visual_motion.interval_frames) == 0) {
            motion_prior = visual_motion.update(frame, timestamp_us);
        }
        const dart::TargetEstimate target = detector.process(
            frame, timestamp_us, motion_prior.valid ? &motion_prior : nullptr);
        const auto &detection = target.green;
        const auto finished = std::chrono::steady_clock::now();
        const double processing_seconds =
            std::chrono::duration<double>(finished - started).count();
        const double processing_ms = processing_seconds * 1000.0;
        const double detector_fps = processing_seconds > 0.0
                                        ? 1.0 / processing_seconds
                                        : 0.0;
        detector_fps_ema = frame_index == 0
                               ? detector_fps
                               : 0.90 * detector_fps_ema + 0.10 * detector_fps;
        total_processing_seconds += processing_seconds;
        const double average_detector_fps = total_processing_seconds > 0.0
                                                ? (frame_index + 1) /
                                                      total_processing_seconds
                                                : 0.0;

        if (detection.valid) {
            ++valid_frames;
            if (detection.predicted) {
                ++predicted_frames;
            } else {
                ++observed_frames;
            }
        }
        if (target.safe_for_control) {
            ++safe_frames;
        }
        if (target.armor.valid) {
            ++armor_frames;
        }
        switch (detection.state) {
        case dart::TrackState::Candidate:
            ++candidate_frames;
            break;
        case dart::TrackState::Tracking:
            ++tracking_frames;
            break;
        case dart::TrackState::Lost:
            ++lost_frames;
            break;
        }
        previous_state = detection.state;

        const auto &candidates = detector.last_candidates();
        draw_overlay(bgr,
                     target,
                     candidates,
                     frame_index,
                     total_frames,
                     detector_fps_ema,
                     average_detector_fps,
                     processing_ms);
        writer.write(bgr);
        write_json_line(jsonl,
                        frame_index,
                        target,
                        candidates,
                        processing_ms,
                        detector_fps_ema,
                        average_detector_fps);
        ++frame_index;
    }

    if (frame_index == 0) {
        throw std::runtime_error("input video contains no decodable frames");
    }
    const double average_detector_fps = total_processing_seconds > 0.0
                                            ? frame_index / total_processing_seconds
                                            : 0.0;
    std::cout << std::fixed << std::setprecision(3)
              << "{\"input\":\"" << command_line.input_path
              << "\",\"output\":\"" << command_line.output_path
              << "\",\"jsonl\":\"" << command_line.jsonl_path
              << "\",\"source_width\":" << source_width
              << ",\"source_height\":" << source_height
              << ",\"processing_width\":" << width
              << ",\"processing_height\":" << height
              << ",\"native_resolution\":"
              << (command_line.native_resolution ? "true" : "false")
              << ",\"source_fps\":" << source_fps
              << ",\"frames\":" << frame_index
              << ",\"valid_frames\":" << valid_frames
              << ",\"observed_frames\":" << observed_frames
              << ",\"predicted_frames\":" << predicted_frames
              << ",\"safe_frames\":" << safe_frames
              << ",\"armor_frames\":" << armor_frames
              << ",\"candidate_frames\":" << candidate_frames
              << ",\"tracking_frames\":" << tracking_frames
              << ",\"lost_frames\":" << lost_frames
              << ",\"valid_rate\":"
              << static_cast<double>(valid_frames) / frame_index
              << ",\"tracking_rate\":"
              << static_cast<double>(tracking_frames) / frame_index
              << ",\"safe_rate\":"
              << static_cast<double>(safe_frames) / frame_index
              << ",\"armor_rate\":"
              << static_cast<double>(armor_frames) / frame_index
              << ",\"average_detector_fps\":" << average_detector_fps
              << "}\n";
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    try {
        return run(argc, argv);
    } catch (const std::exception &exception) {
        std::cerr << "fatal: " << exception.what() << '\n';
        return 1;
    }
}
