#pragma once

#if defined(ANDROID_XR)

#include "AndroidXrBootstrap.h"
#include "xmath.h"
#include <cmath>

// Converts tracking-space poses into the engine's camera convention. The same
// instance must be used for eyes and controller rays during a frame.
class XrCameraRig {
public:
    void BeginFrame(const AndroidXrEyeView views[2]) {
        if (anchored_) return;
        for (int axis = 0; axis < 3; ++axis)
            anchor_[axis] = (views[0].position[axis] + views[1].position[axis]) * 0.5f;
        anchored_ = true;
    }

    MatXf Pose(const float position[3], const float orientation[4]) const {
        // Reflect OpenXR's -Z-forward frame into the engine's +Z-forward frame.
        const Mat3f trackingRotation(QuatF(orientation[3], orientation[0],
                                            orientation[1], orientation[2]));
        Mat3f engineRotation = trackingRotation;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                engineRotation[row][column] *=
                    (row == 2 ? -1.0f : 1.0f) * (column == 2 ? -1.0f : 1.0f);
        const Mat3f yaw(QuatF(std::cos(yaw_ * 0.5f), 0.0f,
                              std::sin(yaw_ * 0.5f), 0.0f));
        const Vect3f relative(position[0] - anchor_[0], position[1] - anchor_[1],
                              -(position[2] - anchor_[2]));
        return MatXf(yaw * engineRotation, offset_ + yaw * (relative * unitsPerMeter_));
    }

    // Keep the current head position fixed while rotating the tabletop.
    void SetYawAround(float radians, const float pivot[3]) {
        const Vect3f before = Pose(pivot, identity_).trans();
        yaw_ = radians;
        offset_ += before - Pose(pivot, identity_).trans();
    }

    void UpdateControls(const float leftStick[2], bool leftActive,
                        const float rightStick[2], bool rightActive,
                        float deltaSeconds, const float pivot[3]) {
        if (deltaSeconds <= 0.0f || deltaSeconds > 0.1f) return;
        constexpr float deadzone = 0.18f;
        const auto applyDeadzone = [](float value) {
            if (std::abs(value) < deadzone) return 0.0f;
            return std::copysign((std::abs(value) - deadzone) / (1.0f - deadzone), value);
        };
        if (leftActive) {
            const float x = applyDeadzone(leftStick[0]);
            const float y = applyDeadzone(leftStick[1]);
            if (x != 0.0f || y != 0.0f) {
                const Mat3f yaw(QuatF(std::cos(yaw_ * 0.5f), 0.0f,
                                      std::sin(yaw_ * 0.5f), 0.0f));
                offset_ += yaw * Vect3f(x, 0.0f, y) *
                    (unitsPerMeter_ * 1.8f * deltaSeconds);
            }
        }
        if (!rightActive) return;
        const float yawInput = applyDeadzone(rightStick[0]);
        if (yawInput != 0.0f)
            SetYawAround(yaw_ + yawInput * 1.2f * deltaSeconds, pivot);
    }

private:
    bool anchored_ = false;
    float anchor_[3]{};
    float unitsPerMeter_ = 100.0f;
    float yaw_ = 0.0f;
    Vect3f offset_ = Vect3f::ZERO;
    const float identity_[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

#endif
