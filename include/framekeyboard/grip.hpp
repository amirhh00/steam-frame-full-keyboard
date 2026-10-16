#pragma once
#include "framekeyboard/placement.hpp"
#include <optional>

namespace framekeyboard {
// Angular travel between a controller component's released and current poses.
// Translation is deliberately ignored: the render model defines the button pivot.
double component_rotation_degrees(const Transform& released, const Transform& current);

// Recover one signed stick axis from a render component's tilt. Calibration
// uses synthetic neutral/full-up poses, so model orientation and handedness do
// not need hardcoded Euler angles.
class ComponentAxis {
  public:
    bool calibrate(const Transform& neutral, const Transform& positive);
    double read(const Transform& current) const;

  private:
    Transform neutral_{};
    std::array<double, 3> axis_{};
    double full_angle_{};
};

struct GripTransition {
    bool held{}, pressed{};
};
class GripLatch {
  public:
    GripLatch() = default;
    GripLatch(double press_threshold, double release_threshold)
        : press_threshold_(press_threshold), release_threshold_(release_threshold) {}
    // Frame's grip component has 9.5 degrees of travel; bumper has 2.6 degrees.
    // A small dead zone and separate release threshold keep a resting finger from chattering capture.
    // Missing samples disarm it; startup/reconnect requires a release first.
    GripTransition update(std::optional<double> angle_degrees);
    void reset();

  private:
    double press_threshold_{1.0};
    double release_threshold_{0.5};
    bool armed_{}, held_{};
};
} // namespace framekeyboard
