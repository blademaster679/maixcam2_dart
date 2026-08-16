#include "dart/green_detector.hpp"

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
        } else if (argument == "--help" || argument == "-h") {
            std::cout
                << "Usage: dart_video_replay --input INPUT.mp4 --output OUTPUT.mp4 "
                   "[--jsonl OUTPUT.jsonl] [--config green_detector.conf] "
                   "[--max-frames N]\n";
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
                  const dart::GreenLightDetection &detection,
                  const std::vector<dart::GreenLightCandidateDebug> &candidates,
                  std::uint64_t frame_index,
                  std::uint64_t total_frames,
                  double detector_fps,
                  double average_detector_fps,
                  double processing_ms)
{
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

    const cv::Scalar color = state_color(detection.state);
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
        target_label << dart::track_state_name(detection.state)
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

    std::vector<std::string> lines;
    {
        std::ostringstream stream;
        stream << "Frame " << frame_index + 1;
        if (total_frames > 0) {
            stream << '/' << total_frames;
        }
        stream << "  State: " << dart::track_state_name(detection.state)
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
                     const dart::GreenLightDetection &detection,
                     const std::vector<dart::GreenLightCandidateDebug> &candidates,
                     double processing_ms,
                     double detector_fps,
                     double average_detector_fps)
{
    output << std::fixed << std::setprecision(6)
           << "{\"frame\":" << frame_index
           << ",\"timestamp_us\":" << detection.timestamp_us
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
           << ",\"candidate_count\":" << candidates.size()
           << ",\"processing_ms\":" << processing_ms
           << ",\"detector_fps\":" << detector_fps
           << ",\"average_detector_fps\":" << average_detector_fps
           << ",\"candidates\":[";
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (index > 0) {
            output << ',';
        }
        const auto &candidate = candidates[index];
        output << "{\"center_x\":" << candidate.center_x
               << ",\"center_y\":" << candidate.center_y
               << ",\"bbox_x\":" << candidate.bbox_x
               << ",\"bbox_y\":" << candidate.bbox_y
               << ",\"bbox_w\":" << candidate.bbox_w
               << ",\"bbox_h\":" << candidate.bbox_h
               << ",\"apparent_size\":" << candidate.apparent_size
               << ",\"density\":" << candidate.density
               << ",\"green_dominance\":" << candidate.green_dominance
               << ",\"green_fraction\":" << candidate.green_fraction
               << ",\"local_contrast\":" << candidate.local_contrast
               << ",\"shape_score\":" << candidate.shape_score
               << ",\"core_score\":" << candidate.core_score
               << ",\"temporal_score\":" << candidate.temporal_score
               << ",\"center_prior_score\":"
               << candidate.center_prior_score
               << ",\"initial_size_score\":" << candidate.initial_size_score
               << ",\"score\":" << candidate.score
               << ",\"saturated_core\":"
               << (candidate.saturated_core ? "true" : "false")
               << ",\"selected\":"
               << (candidate.selected ? "true" : "false") << '}';
    }
    output << "]}\n";
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
    const int width = static_cast<int>(
        std::lround(capture.get(cv::CAP_PROP_FRAME_WIDTH)));
    const int height = static_cast<int>(
        std::lround(capture.get(cv::CAP_PROP_FRAME_HEIGHT)));
    double source_fps = capture.get(cv::CAP_PROP_FPS);
    if (!std::isfinite(source_fps) || source_fps <= 0.0) {
        source_fps = 30.0;
    }
    const std::uint64_t total_frames = static_cast<std::uint64_t>(
        std::max(0.0, capture.get(cv::CAP_PROP_FRAME_COUNT)));

    if (config.camera.width > 0 && config.camera.height > 0) {
        const float scale_x = static_cast<float>(width) / config.camera.width;
        const float scale_y = static_cast<float>(height) / config.camera.height;
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

    dart::GreenLightDetector detector(config.detector);
    std::uint64_t frame_index = 0;
    std::uint64_t valid_frames = 0;
    std::uint64_t candidate_frames = 0;
    std::uint64_t tracking_frames = 0;
    std::uint64_t lost_frames = 0;
    double total_processing_seconds = 0.0;
    double detector_fps_ema = 0.0;

    cv::Mat bgr;
    while (capture.read(bgr)) {
        if (command_line.max_frames > 0 &&
            frame_index >= command_line.max_frames) {
            break;
        }
        cv::Mat rgb;
        cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
        maix::image::Image frame(rgb);
        const std::uint64_t timestamp_us = static_cast<std::uint64_t>(
            std::llround(frame_index * 1000000.0 / source_fps));

        const auto started = std::chrono::steady_clock::now();
        const dart::GreenLightDetection detection =
            detector.process(frame, timestamp_us);
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

        const auto &candidates = detector.last_candidates();
        draw_overlay(bgr,
                     detection,
                     candidates,
                     frame_index,
                     total_frames,
                     detector_fps_ema,
                     average_detector_fps,
                     processing_ms);
        writer.write(bgr);
        write_json_line(jsonl,
                        frame_index,
                        detection,
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
              << "\",\"width\":" << width
              << ",\"height\":" << height
              << ",\"source_fps\":" << source_fps
              << ",\"frames\":" << frame_index
              << ",\"valid_frames\":" << valid_frames
              << ",\"candidate_frames\":" << candidate_frames
              << ",\"tracking_frames\":" << tracking_frames
              << ",\"lost_frames\":" << lost_frames
              << ",\"valid_rate\":"
              << static_cast<double>(valid_frames) / frame_index
              << ",\"tracking_rate\":"
              << static_cast<double>(tracking_frames) / frame_index
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
