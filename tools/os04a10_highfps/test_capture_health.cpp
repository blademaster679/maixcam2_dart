#include "capture_health.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    dart::CaptureHealthPolicy strict;
    strict.observe(45.0, 0);
    assert(!strict.fatal());
    strict.observe(46.0, 2);
    assert(strict.fatal());
    assert(std::strcmp(strict.failure_reason(), "mipi_error_limit") == 0);

    dart::CaptureHealthPolicy bounded(2);
    bounded.observe(45.0, 0);
    bounded.observe(46.0, 2);
    assert(!bounded.fatal());
    assert(bounded.recovered_mipi_error());
    assert(bounded.mipi_errors_max == 2);
    bounded.observe(47.0, 3);
    assert(bounded.fatal());
    assert(std::strcmp(bounded.failure_reason(), "mipi_error_limit") == 0);

    dart::CaptureHealthPolicy hot(2);
    hot.observe(80.0, 0);
    assert(hot.fatal());
    assert(std::strcmp(hot.failure_reason(), "temperature_limit") == 0);

    std::cout << "PASS: bounded recovered MIPI errors remain distinguishable from fatal health faults\n";
    return 0;
}
