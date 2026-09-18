#pragma once

namespace dart {

// A single receiver recovery currently increments ErrorCount by two on the
// MaixCAM2.  Offline recording may retain that bounded event as explicitly
// degraded data, while sustained link errors and over-temperature remain
// fatal.  The default limit is strict so callers must opt in deliberately.
struct CaptureHealthPolicy {
    explicit CaptureHealthPolicy(long allowed_mipi_errors = 0,
                                 double allowed_temperature_c = 80.0)
        : mipi_error_limit(allowed_mipi_errors),
          temperature_limit_c(allowed_temperature_c) {}

    void observe(double temperature_c, long mipi_errors) {
        if (temperature_c > temperature_max_c) {
            temperature_max_c = temperature_c;
        }
        if (mipi_errors > mipi_errors_max) {
            mipi_errors_max = mipi_errors;
        }
        if (temperature_c >= temperature_limit_c) {
            temperature_limit_exceeded = true;
        }
        if (mipi_errors >= 0 && mipi_errors > mipi_error_limit) {
            mipi_error_limit_exceeded = true;
        }
    }

    bool fatal() const {
        return temperature_limit_exceeded || mipi_error_limit_exceeded;
    }

    bool recovered_mipi_error() const {
        return mipi_errors_max > 0 && !mipi_error_limit_exceeded;
    }

    const char *failure_reason() const {
        if (temperature_limit_exceeded) return "temperature_limit";
        if (mipi_error_limit_exceeded) return "mipi_error_limit";
        return "";
    }

    long mipi_error_limit = 0;
    double temperature_limit_c = 80.0;
    double temperature_max_c = -999.0;
    long mipi_errors_max = -1;
    bool temperature_limit_exceeded = false;
    bool mipi_error_limit_exceeded = false;
};

}  // namespace dart
