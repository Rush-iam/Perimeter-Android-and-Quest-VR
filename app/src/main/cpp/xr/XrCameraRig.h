#pragma once

#if defined(ANDROID_XR)

#include "AndroidXrBootstrap.h"
#include "xmath.h"
#include <cmath>

// Converts tracking-space poses into the engine's camera convention. The same
// instance must be used for eyes and controller rays during a frame.
class XrCameraRig {
public:
    void BeginFrame(const AndroidXrEyeView views[2], bool recentered) {
        if (recentered) {
            anchored_ = false;
            ResetTableManipulation();
        }
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
        const Vect3f relative(position[0] - anchor_[0], position[1] - anchor_[1],
                              -(position[2] - anchor_[2]));
        return MatXf(engineRotation, offset_ + relative * unitsPerMeter_);
    }

    // A 2x tabletop enlargement moves the selected pivot 1.8x farther in XR,
    // while preserving its direction from the head.
    void SetScaleWithPivotDepth(float unitsPerMeter, const float headPosition[3],
                                const Vect3f& pivot) {
        if (unitsPerMeter == unitsPerMeter_) return;
        const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const Vect3f headBefore = Pose(headPosition, identity).trans();
        const float scaleRatio = unitsPerMeter_ / unitsPerMeter;
        constexpr float extraDepthExponent = -0.15200309f; // log2(1.8) - 1
        const Vect3f desiredHead = pivot +
            (headBefore - pivot) * std::pow(scaleRatio, extraDepthExponent);
        unitsPerMeter_ = unitsPerMeter;
        offset_ += desiredHead - Pose(headPosition, identity).trans();
    }

    bool NeedsTablePivot(float scaleStickY, bool rightActive) const {
        return rightActive && !tablePivotLatched_ &&
            std::abs(scaleStickY) > stickDeadzone_;
    }

    void ResetTableManipulation() {
        tablePivotLatched_ = false;
        tablePivotIdleSeconds_ = 0.0f;
    }

    void UpdateControls(const float leftStick[2], bool leftActive,
                        float scaleStickY, bool rightActive,
                        float deltaSeconds, const Vect3f& candidateWorldPivot,
                        const MatXf& centerWorld, const float headPosition[3]) {
        if (deltaSeconds <= 0.0f) return;
        const auto applyDeadzone = [](float value) {
            if (std::abs(value) <= stickDeadzone_) return 0.0f;
            return std::copysign((std::abs(value) - stickDeadzone_) /
                                 (1.0f - stickDeadzone_), value);
        };
        const float scaleInput = rightActive ? applyDeadzone(scaleStickY) : 0.0f;
        if (scaleInput != 0.0f) {
            if (!tablePivotLatched_) {
                tablePivotWorld_ = candidateWorldPivot;
                tablePivotLatched_ = true;
            }
            tablePivotIdleSeconds_ = 0.0f;
        } else if (tablePivotLatched_) {
            tablePivotIdleSeconds_ += deltaSeconds;
            if (tablePivotIdleSeconds_ >= 0.2f) ResetTableManipulation();
        }

        if (deltaSeconds > 0.1f) return;
        if (leftActive) {
            const float x = applyDeadzone(leftStick[0]);
            const float y = applyDeadzone(leftStick[1]);
            if (x != 0.0f || y != 0.0f) {
                // Pan linearly with enlargement, but ease zoomed-out panning
                // with a square-root response around the default scale.
                const float tableScale = defaultUnitsPerMeter_ / unitsPerMeter_;
                const float panScale = tableScale > 1.0f
                    ? tableScale : std::sqrt(tableScale);
                offset_ += Vect3f(x, 0.0f, y) *
                    (unitsPerMeter_ * panScale * 3.6f * deltaSeconds);
            }
        }
        if (scaleInput == 0.0f) return;
        MatXf worldToRig = centerWorld;
        worldToRig.invert();
        const Vect3f pivot = worldToRig * tablePivotWorld_;
        // Fewer engine units per tracking meter make the tabletop larger.
        float unitsPerMeter = unitsPerMeter_ *
            std::exp(-scaleInput * 2.1f * deltaSeconds);
        if (unitsPerMeter < 12.5f) unitsPerMeter = 12.5f;
        if (unitsPerMeter > 800.0f) unitsPerMeter = 800.0f;
        SetScaleWithPivotDepth(unitsPerMeter, headPosition, pivot);
    }

private:
    static constexpr float stickDeadzone_ = 0.18f;
    static constexpr float defaultUnitsPerMeter_ = 100.0f;
    bool anchored_ = false;
    float anchor_[3]{};
    float unitsPerMeter_ = defaultUnitsPerMeter_;
    Vect3f offset_ = Vect3f::ZERO;
    bool tablePivotLatched_ = false;
    float tablePivotIdleSeconds_ = 0.0f;
    Vect3f tablePivotWorld_ = Vect3f::ZERO;
};

#endif
