#include "dart/target_json.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace dart {
namespace {

void write_point(std::ostream &output, const Point2f &point)
{
    output << "{\"valid\":" << (point.valid ? "true" : "false")
           << ",\"x\":" << point.x << ",\"y\":" << point.y << '}';
}

void write_bar(std::ostream &output, const LineSegment2f &bar)
{
    output << "{\"top\":";
    write_point(output, bar.top);
    output << ",\"bottom\":";
    write_point(output, bar.bottom);
    output << ",\"center\":";
    write_point(output, bar.center);
    output << ",\"length\":" << bar.length
           << ",\"width\":" << bar.width
           << ",\"angle_rad\":" << bar.angle_rad
           << ",\"confidence\":" << bar.confidence << '}';
}

}  // namespace

void write_green_detection_json(std::ostream &output,
                                const GreenLightDetection &detection)
{
    output << std::fixed << std::setprecision(6)
           << "{\"timestamp_us\":" << detection.timestamp_us
           << ",\"measurement_age_us\":" << detection.measurement_age_us
           << ",\"missed_frames\":" << detection.missed_frames
           << ",\"valid\":" << (detection.valid ? "true" : "false")
           << ",\"predicted\":" << (detection.predicted ? "true" : "false")
           << ",\"state\":\"" << track_state_name(detection.state) << '"'
           << ",\"center_x\":" << detection.center_x
           << ",\"center_y\":" << detection.center_y
           << ",\"bbox_x\":" << detection.bbox_x
           << ",\"bbox_y\":" << detection.bbox_y
           << ",\"bbox_w\":" << detection.bbox_w
           << ",\"bbox_h\":" << detection.bbox_h
           << ",\"apparent_size\":" << detection.apparent_size
           << ",\"yaw_rad\":" << detection.yaw_rad
           << ",\"pitch_rad\":" << detection.pitch_rad
           << ",\"confidence\":" << detection.confidence << '}';
}

void write_candidate_list_json(
    std::ostream &output,
    const std::vector<GreenLightCandidateDebug> &candidates)
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
               << ",\"green_fraction\":" << candidate.green_fraction
               << ",\"local_contrast\":" << candidate.local_contrast
               << ",\"shape_score\":" << candidate.shape_score
               << ",\"core_score\":" << candidate.core_score
               << ",\"temporal_score\":" << candidate.temporal_score
               << ",\"center_prior_score\":" << candidate.center_prior_score
               << ",\"initial_size_score\":" << candidate.initial_size_score
               << ",\"score\":" << candidate.score
               << ",\"normalized_response\":"
               << candidate.normalized_response
               << ",\"model_score\":" << candidate.model_score
               << ",\"inside_capture_cone\":"
               << (candidate.inside_capture_cone ? "true" : "false")
               << ",\"model_validated\":"
               << (candidate.model_validated ? "true" : "false")
               << ",\"saturated_core\":"
               << (candidate.saturated_core ? "true" : "false")
               << ",\"selected\":"
               << (candidate.selected ? "true" : "false") << '}';
    }
    output << ']';
}

void write_target_estimate_json(
    std::ostream &output,
    const TargetEstimate &target,
    const std::vector<GreenLightCandidateDebug> *candidates,
    const std::string &extra_fields)
{
    if (!extra_fields.empty() && extra_fields.front() != ',') {
        throw std::invalid_argument("TargetEstimate JSON extra fields must begin with ','");
    }
    const auto &green = target.green;
    output << std::fixed << std::setprecision(6)
           << "{\"schema_version\":" << target.schema_version
           << ",\"timestamp_us\":" << target.timestamp_us
           << ",\"measurement_age_us\":" << target.measurement_age_us
           << ",\"angles_valid\":" << (target.angles_valid ? "true" : "false")
           << ",\"source_metadata_valid\":" << (target.source_metadata_valid ? "true" : "false")
           << ",\"source_sequence\":" << target.source_sequence
           << ",\"source_pts_raw\":" << target.source_pts_raw
           << ",\"source_received_us\":" << target.source_received_us
           << ",\"armor_source_received_us\":" << target.armor_source_received_us
           << ",\"application_dropped\":" << target.application_dropped
           << ",\"upstream_missing\":" << target.upstream_missing
           << ",\"upstream_duplicate\":" << target.upstream_duplicate
           << ",\"upstream_reversed\":" << target.upstream_reversed
           << ",\"valid\":" << (target.valid ? "true" : "false")
           << ",\"safe_for_control\":"
           << (target.safe_for_control ? "true" : "false")
           << ",\"predicted\":" << (target.predicted ? "true" : "false")
           << ",\"armor_detection_ran\":" << (target.armor_detection_ran ? "true" : "false")
           << ",\"classical_detection_ran\":"
           << (target.classical_detection_ran ? "true" : "false")
           << ",\"classical_detection_ms\":"
           << target.classical_detection_ms
           << ",\"model_ran\":" << (target.model_ran ? "true" : "false")
           << ",\"model_inference_ms\":" << target.model_inference_ms
           << ",\"guidance_state\":\""
           << guidance_track_state_name(target.state) << '"'
           << ",\"guidance_mode\":\""
           << guidance_mode_name(target.guidance_mode) << '"'
           // v0.1 compatibility fields.
           << ",\"state\":\"" << track_state_name(green.state) << '"'
           << ",\"center_x\":" << green.center_x
           << ",\"center_y\":" << green.center_y
           << ",\"bbox_x\":" << green.bbox_x
           << ",\"bbox_y\":" << green.bbox_y
           << ",\"bbox_w\":" << green.bbox_w
           << ",\"bbox_h\":" << green.bbox_h
           << ",\"apparent_size\":" << green.apparent_size
           << ",\"yaw_rad\":" << target.yaw_rad
           << ",\"pitch_rad\":" << target.pitch_rad
           << ",\"confidence\":" << target.confidence
           << ",\"green\":";
    write_green_detection_json(output, green);
    output << ",\"armor\":{\"valid\":"
           << (target.armor.valid ? "true" : "false")
           << ",\"color\":\"" << armor_color_name(target.armor.color)
           << "\",\"center\":";
    write_point(output, target.armor.center);
    output << ",\"left_bar\":";
    write_bar(output, target.armor.left_bar);
    output << ",\"right_bar\":";
    write_bar(output, target.armor.right_bar);
    output << ",\"separation_px\":" << target.armor.separation_px
           << ",\"geometry_confidence\":"
           << target.armor.geometry_confidence
           << ",\"model_validated\":"
           << (target.armor.model_validated ? "true" : "false")
           << ",\"model_confidence\":" << target.armor.model_confidence
           << "},\"pose\":{\"valid\":"
           << (target.pose.valid ? "true" : "false")
           << ",\"translation_m\":[" << target.pose.translation_m[0] << ','
           << target.pose.translation_m[1] << ',' << target.pose.translation_m[2]
           << "],\"orientation_wxyz\":[" << target.pose.orientation_wxyz[0]
           << ',' << target.pose.orientation_wxyz[1] << ','
           << target.pose.orientation_wxyz[2] << ','
           << target.pose.orientation_wxyz[3]
           << "],\"distance_m\":" << target.pose.distance_m
           << ",\"reprojection_error_px\":"
           << target.pose.reprojection_error_px << "},\"aim_point\":";
    write_point(output, target.aim_point);
    output << ",\"line_of_sight_camera\":["
           << target.line_of_sight_camera[0] << ','
           << target.line_of_sight_camera[1] << ','
           << target.line_of_sight_camera[2]
           << "],\"line_of_sight_rate_rad_s\":["
           << target.line_of_sight_rate_rad_s[0] << ','
           << target.line_of_sight_rate_rad_s[1]
           << "],\"angular_covariance\":["
           << target.angular_covariance[0] << ','
           << target.angular_covariance[1] << ']';
    if (candidates != nullptr) {
        output << ",\"candidate_count\":" << candidates->size()
               << ",\"candidates\":";
        write_candidate_list_json(output, *candidates);
    }
    output << extra_fields << '}';
}

std::string target_estimate_json(
    const TargetEstimate &target,
    const std::vector<GreenLightCandidateDebug> *candidates,
    const std::string &extra_fields)
{
    std::ostringstream output;
    write_target_estimate_json(output, target, candidates, extra_fields);
    return output.str();
}

}  // namespace dart
