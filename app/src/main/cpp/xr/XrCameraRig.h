#pragma once

#if defined(ANDROID_XR)

#include "AndroidXrBootstrap.h"
#include "xmath.h"
#include <algorithm>
#include <cmath>

// Converts tracking-space poses into the engine's camera convention. The same
// instance must be used for eyes and controller rays during a frame.
class XrCameraRig {
public:
    void BeginFrame(const AndroidXrEyeView views[2], bool recentered) {
        if (recentered) anchored_ = false;
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

    // A 2x tabletop enlargement moves the current pivot 1.8x farther in XR,
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
        return rightActive && std::abs(scaleStickY) > stickDeadzone_;
    }

    void AlignOffsetToScriptedCamera(float durationSeconds) {
        if (durationSeconds <= 0.0f) {
            offset_ = Vect3f::ZERO;
            scriptedAlignmentActive_ = false;
            return;
        }
        scriptedAlignmentStart_ = offset_;
        scriptedAlignmentElapsed_ = 0.0f;
        scriptedAlignmentDuration_ = durationSeconds;
        scriptedAlignmentActive_ = true;
    }

    bool IsAligningToScriptedCamera() const { return scriptedAlignmentActive_; }

    void RebaseCameraPan(const Vect3f& appliedLocalPan) {
        // The game camera now carries this translation, so remove the same
        // component from the XR offset without changing the rendered view.
        offset_ -= appliedLocalPan;
    }

    // Return only this frame's bounded stick pan. Gameplay rebases it into
    // the game camera; the menu can keep it entirely in the rig.
    Vect3f UpdateControls(const float leftStick[2], bool leftActive,
                        float scaleStickY, bool rightActive,
                        float deltaSeconds, const Vect3f& currentWorldPivot,
                        const MatXf& centerWorld, const float headPosition[3],
                        const Vect3f& skyCenter, float skyRadius) {
        const auto applyDeadzone = [](float value) {
            if (std::abs(value) <= stickDeadzone_) return 0.0f;
            return std::copysign((std::abs(value) - stickDeadzone_) /
                                 (1.0f - stickDeadzone_), value);
        };
        if (scriptedAlignmentActive_) {
            // Input must not overwrite the offset while it is aligning.
            leftActive = rightActive = false;
            scriptedAlignmentElapsed_ += std::max(deltaSeconds, 0.0f);
            const float t = std::min(scriptedAlignmentElapsed_ /
                                     scriptedAlignmentDuration_, 1.0f);
            const float eased = t * t * (3.0f - 2.0f * t);
            offset_ = scriptedAlignmentStart_ * (1.0f - eased);
            if (t >= 1.0f)
                scriptedAlignmentActive_ = false;
        }
        const float scaleInput = rightActive ? applyDeadzone(scaleStickY) : 0.0f;
        if (deltaSeconds <= 0.0f || deltaSeconds > 0.1f) return Vect3f::ZERO;
        Vect3f cameraPan = Vect3f::ZERO;
        if (leftActive) {
            const float x = applyDeadzone(leftStick[0]);
            const float y = applyDeadzone(leftStick[1]);
            if (x != 0.0f || y != 0.0f) {
                // Pan linearly with enlargement, but ease zoomed-out panning
                // with a cube-root response around the default scale.
                const float tableScale = defaultUnitsPerMeter_ / unitsPerMeter_;
                const float panScale = tableScale > 1.0f
                    ? tableScale : std::cbrt(tableScale);
                cameraPan = Vect3f(x, 0.0f, y) *
                    (unitsPerMeter_ * panScale * 5.4f * deltaSeconds);
                offset_ += cameraPan;
            }
        }
        if (scaleInput != 0.0f) {
            MatXf worldToRig = centerWorld;
            worldToRig.invert();
            const Vect3f pivot = worldToRig * currentWorldPivot;
            // Fewer engine units per tracking meter make the tabletop larger.
            float unitsPerMeter = unitsPerMeter_ *
                std::exp(-scaleInput * 2.1f * deltaSeconds);
            if (unitsPerMeter < 12.5f) unitsPerMeter = 12.5f;
            if (unitsPerMeter > 800.0f) unitsPerMeter = 800.0f;
            SetScaleWithPivotDepth(unitsPerMeter, headPosition, pivot);
        }
        // Scripted positions are authored independently of stick bounds.
        if (skyRadius > 0.0f && (cameraPan.norm2() > 0.0f || scaleInput != 0.0f)) {
            // Bound stick movement of the table anchor, leaving natural head
            // motion untouched. Reserve room for a physical lean at any scale.
            const float margin = std::min(skyRadius * 0.25f,
                                          unitsPerMeter_ * 1.5f);
            const float limit = skyRadius - margin;
            const Vect3f fromCenter = centerWorld * offset_ - skyCenter;
            // Clamp to the horizontal cross-section at the current height.
            // A 3D radial clamp pulls the camera downward at the upper edge.
            const float verticalDistanceSquared = fromCenter.z * fromCenter.z;
            // With no horizontal cross-section, reject stick pan while
            // retaining the current height and scale placement.
            if (verticalDistanceSquared >= limit * limit) {
                offset_ -= cameraPan;
                return Vect3f::ZERO;
            }
            const float horizontalLimitSquared =
                limit * limit - verticalDistanceSquared;
            const float horizontalDistanceSquared =
                fromCenter.x * fromCenter.x + fromCenter.y * fromCenter.y;
            if (horizontalDistanceSquared > horizontalLimitSquared) {
                const float factor = std::sqrt(horizontalLimitSquared /
                                               horizontalDistanceSquared);
                const Vect3f boundedWorld(skyCenter.x + fromCenter.x * factor,
                                          skyCenter.y + fromCenter.y * factor,
                                          skyCenter.z + fromCenter.z);
                MatXf worldToRig = centerWorld;
                worldToRig.invert();
                const Vect3f boundedOffset = worldToRig * boundedWorld;
                if (cameraPan.norm2() > 0.0f)
                    cameraPan += boundedOffset - offset_;
                offset_ = boundedOffset;
            }
        }
        return cameraPan;
    }

private:
    static constexpr float stickDeadzone_ = 0.18f;
    static constexpr float defaultUnitsPerMeter_ = 100.0f;
    bool anchored_ = false;
    float anchor_[3]{};
    float unitsPerMeter_ = defaultUnitsPerMeter_;
    Vect3f offset_ = Vect3f::ZERO;
    Vect3f scriptedAlignmentStart_ = Vect3f::ZERO;
    float scriptedAlignmentElapsed_ = 0.0f;
    float scriptedAlignmentDuration_ = 0.0f;
    bool scriptedAlignmentActive_ = false;
};

#endif
