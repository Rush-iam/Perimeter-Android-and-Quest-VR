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
    void SetMenuHeadingAligned(bool enabled) {
        if (menuHeadingAligned_ == enabled) return;
        menuHeadingAligned_ = enabled;
        anchored_ = false;
        menuHeadingAnchored_ = false;
        if (enabled) menuHeadingRotation_ = Mat3f::ID;
    }

    void BeginFrame(const AndroidXrEyeView views[2], bool recentered) {
        const auto& left = views[0].orientation;
        const auto& right = views[1].orientation;
        const QuatF leftQuat(left[3], left[0], left[1], left[2]);
        QuatF rightQuat(right[3], right[0], right[1], right[2]);
        if (leftQuat.dot(rightQuat) < 0.0f) rightQuat.negate();
        QuatF headQuat = leftQuat + rightQuat;
        headQuat.normalize();
        headOrientation_[0] = headQuat.x();
        headOrientation_[1] = headQuat.y();
        headOrientation_[2] = headQuat.z();
        headOrientation_[3] = headQuat.s();
        if (recentered) {
            anchored_ = false;
            menuHeadingAnchored_ = false;
        }
        if (menuHeadingAligned_ && !menuHeadingAnchored_) {
            // Match the authored menu camera to the headset's current heading.
            // Tracking-space yaw can differ after a Quest recenter or relaunch.
            const Vect3f forward = ToEngineRotation(headOrientation_) *
                Vect3f(0.0f, 0.0f, 1.0f);
            if (forward.x * forward.x + forward.z * forward.z > 0.01f) {
                const float yaw = std::atan2(forward.x, forward.z);
                menuHeadingRotation_ = Mat3f(-yaw, Y_AXIS);
                menuHeadingAnchored_ = true;
            }
        }
        if (anchored_) return;
        for (int axis = 0; axis < 3; ++axis)
            anchor_[axis] = (views[0].position[axis] + views[1].position[axis]) * 0.5f;
        anchored_ = true;
    }

    MatXf Pose(const float position[3], const float orientation[4]) const {
        // Reflect OpenXR's -Z-forward frame into the engine's +Z-forward frame.
        const Mat3f engineRotation = ToEngineRotation(orientation);
        const Vect3f relative(position[0] - anchor_[0], position[1] - anchor_[1],
                              -(position[2] - anchor_[2]));
        const Mat3f heading = menuHeadingAligned_
            ? menuHeadingRotation_ : Mat3f::ID;
        return MatXf(heading * engineRotation,
                      offset_ + heading * relative * unitsPerMeter_);
    }

    // Apply independently adjustable scale-depth compensation in the supplied
    // direction, with equal terrain-plane and vertical movement components.
    void SetScaleWithPivotDepth(float unitsPerMeter, const float headPosition[3],
                                const Vect3f& pivot, const Vect3f& scaleDirection) {
        if (unitsPerMeter == unitsPerMeter_) return;
        const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const Vect3f headBefore = Pose(headPosition, identity).trans();
        const float scaleRatio = unitsPerMeter_ / unitsPerMeter;
        constexpr float extraDepthExponent = -0.15200309f; // log2(1.8) - 1
        const float depthDelta = (headBefore - pivot).norm() *
            (1.0f - std::pow(scaleRatio, extraDepthExponent)) *
            scaleMovementMultiplier_;
        const Vect3f desiredHead = headBefore + scaleDirection * depthDelta;
        unitsPerMeter_ = unitsPerMeter;
        // Compensate the changed scale of physical head displacement so only
        // the intentional movement changes the head's world position.
        offset_ += desiredHead - Pose(headPosition, identity).trans();
    }

    bool NeedsTablePivot(float scaleStickY, bool rightActive) const {
        return rightActive && scaleStickY != 0.0f;
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

    float UnitsPerMeter() const { return unitsPerMeter_; }

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
        const float scaleInput = rightActive ? scaleStickY : 0.0f;
        if (deltaSeconds <= 0.0f || deltaSeconds > 0.1f) return Vect3f::ZERO;
        Vect3f cameraPan = Vect3f::ZERO;
        // Project the tracked head's forward axis onto the game Z-up plane,
        // after composing with the game camera. Tracking X/Z is not that plane
        // when the tabletop camera is tilted.
        const Mat3f headWorldRotation = centerWorld.rot() *
            Pose(headPosition, headOrientation_).rot();
        Vect3f forward = headWorldRotation * Vect3f(0.0f, 0.0f, 1.0f);
        forward.z = 0.0f;
        if (forward.norm2() > 0.0001f) {
            forward.normalize();
            panForward_ = forward;
            panHeadingValid_ = true;
        } else if (!panHeadingValid_) {
            // If the first view is vertical, infer heading from head-right.
            const Vect3f right = headWorldRotation * Vect3f(1.0f, 0.0f, 0.0f);
            panForward_ = Vect3f(right.y, -right.x, 0.0f);
            panForward_.normalize();
            panHeadingValid_ = true;
        }
        if (leftActive) {
            const float x = leftStick[0];
            const float y = leftStick[1];
            if (x != 0.0f || y != 0.0f) {
                // Increase game-world speed gently when zoomed out; retain
                // the default speed when the tabletop is enlarged.
                const float zoomOutFactor = std::max(
                    1.0f, unitsPerMeter_ / defaultUnitsPerMeter_);
                const Vect3f right(-panForward_.y, panForward_.x, 0.0f);
                const Vect3f worldPan = (right * x + panForward_ * y) *
                    (panSpeedGameUnitsPerSecond_ * std::pow(zoomOutFactor, 0.25f) *
                     deltaSeconds);
                MatXf worldToRig = centerWorld;
                worldToRig.invert();
                cameraPan = worldToRig.rot() * worldPan;
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
            // Zoom out moves backward and up; zoom in moves forward and down.
            // World Z is perpendicular to terrain, regardless of camera tilt.
            // Keep both components at the same speed rather than normalizing
            // their combined direction.
            const Vect3f scaleDirection = panForward_ - Vect3f(0.0f, 0.0f, 1.0f);
            SetScaleWithPivotDepth(unitsPerMeter, headPosition, pivot,
                                   worldToRig.rot() * scaleDirection);
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
    static Mat3f ToEngineRotation(const float orientation[4]) {
        const Mat3f trackingRotation(QuatF(orientation[3], orientation[0],
                                            orientation[1], orientation[2]));
        Mat3f engineRotation = trackingRotation;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                engineRotation[row][column] *=
                    (row == 2 ? -1.0f : 1.0f) * (column == 2 ? -1.0f : 1.0f);
        return engineRotation;
    }

    static constexpr float defaultUnitsPerMeter_ = 100.0f;
    static constexpr float panSpeedGameUnitsPerSecond_ = 540.0f;
    static constexpr float scaleMovementMultiplier_ = 0.5f;
    bool anchored_ = false;
    bool menuHeadingAligned_ = false;
    bool menuHeadingAnchored_ = false;
    Mat3f menuHeadingRotation_ = Mat3f::ID;
    float anchor_[3]{};
    float headOrientation_[4]{0.0f, 0.0f, 0.0f, 1.0f};
    Vect3f panForward_ = Vect3f::ZERO;
    bool panHeadingValid_ = false;
    float unitsPerMeter_ = defaultUnitsPerMeter_;
    Vect3f offset_ = Vect3f::ZERO;
    Vect3f scriptedAlignmentStart_ = Vect3f::ZERO;
    float scriptedAlignmentElapsed_ = 0.0f;
    float scriptedAlignmentDuration_ = 0.0f;
    bool scriptedAlignmentActive_ = false;
};

#endif
