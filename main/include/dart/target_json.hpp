#pragma once

#include "dart/green_detector.hpp"

#include <ostream>
#include <string>
#include <vector>

namespace dart {

void write_green_detection_json(std::ostream &output,
                                const GreenLightDetection &detection);

void write_candidate_list_json(
    std::ostream &output,
    const std::vector<GreenLightCandidateDebug> &candidates);

// extra_fields is intended for replay-only metrics and must either be empty or
// begin with a comma. Keeping the core serializer shared guarantees that the
// device and replay tools emit the same schema_version=2 TargetEstimate while
// retaining all v0.1 flat green-light fields.
void write_target_estimate_json(
    std::ostream &output,
    const TargetEstimate &target,
    const std::vector<GreenLightCandidateDebug> *candidates = nullptr,
    const std::string &extra_fields = {});

std::string target_estimate_json(
    const TargetEstimate &target,
    const std::vector<GreenLightCandidateDebug> *candidates = nullptr,
    const std::string &extra_fields = {});

}  // namespace dart
