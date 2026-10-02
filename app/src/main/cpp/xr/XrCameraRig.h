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
    void ResetNavigation() {
        anchored_ = false;
        menuHeadingAligned_ = false;
        menuHeadingAnchored_ = false;
        menuHeadingRotation_ = Mat3f::ID;
        panHeadingValid_ = false;
        navigationRotation_ = Mat3f::ID;
        snapReady_ = false;
        tiltReady_ = false;
        tableForwardValid_ = false;
        terrainLiftActive_ = false;
        scaleInThisFrame_ = false;
        ResetGripDrag();
        unitsPerMeter_ = defaultUnitsPerMeter_;
        offset_ = Vect3f::ZERO;
        scriptedAlignmentActive_ = false;
    }

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
        return MatXf(navigationRotation_ * heading * engineRotation,
                      offset_ + navigationRotation_ * heading * relative * unitsPerMeter_);
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

    void AlignOffsetToScriptedCamera(float durationSeconds) {
        terrainLiftActive_ = false;
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

    Vect3f HeadWorldPosition(const MatXf& centerWorld,
                             const float headPosition[3]) const {
        const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        return centerWorld * Pose(headPosition, identity).trans();
    }

    void UpdateTerrainClearance(float terrainHeight, const MatXf& centerWorld,
                                const float headPosition[3], float deltaSeconds,
                                float maxHeight, bool moved) {
        if (deltaSeconds <= 0.0f || deltaSeconds > 0.1f) return;
        const float headHeight = HeadWorldPosition(centerWorld, headPosition).z;
        float adjustedHeight = headHeight;
        const float targetHeight = std::min(
            terrainHeight + minTerrainClearanceGameUnits_, maxHeight);
        if (scaleInThisFrame_ && adjustedHeight < targetHeight) {
            // Preserve any clearance that existed before scaling. If already
            // too low, stop further descent without snapping upward.
            const float safeHeight = std::min(scaleHeadBeforeHeight_, targetHeight);
            adjustedHeight = std::max(adjustedHeight, safeHeight);
        }
        if (scaleInThisFrame_ && adjustedHeight < terrainHeight) {
            // A new cliff can be higher than the old position. Exit the mesh
            // immediately, then ease the remaining distance to full clearance.
            adjustedHeight = terrainHeight + 1.0f;
        }
        // Scaling in also advances the head over terrain; a higher slope can
        // require an upward recovery even when scale descent was prevented.
        if ((moved || scaleInThisFrame_) && adjustedHeight < targetHeight)
            terrainLiftActive_ = true;
        if (terrainLiftActive_) {
            const float gap = targetHeight - adjustedHeight;
            if (gap <= 0.1f) {
                terrainLiftActive_ = false;
            } else {
                // Recover faster after crossing the terrain, but ease both paths.
                const float response = adjustedHeight < terrainHeight ? 24.0f : 12.0f;
                adjustedHeight += gap * (1.0f - std::exp(-response * deltaSeconds));
            }
        }
        if (adjustedHeight <= headHeight) return;
        MatXf worldToRig = centerWorld;
        worldToRig.invert();
        offset_ += worldToRig.rot() *
            Vect3f(0.0f, 0.0f, adjustedHeight - headHeight);
    }

    void ResetTilt(const MatXf& centerWorld, const float headPosition[3]) {
        MatXf worldToRig = centerWorld;
        worldToRig.invert();
        const float pitch = TablePitch(centerWorld);
        if (std::abs(pitch) > 0.0001f) {
            const Vect3f terrainForward = TerrainForward(centerWorld);
            const Vect3f right(terrainForward.y, -terrainForward.x, 0.0f);
            RotateAroundHead(Mat3f(worldToRig.rot() * right, pitch),
                             headPosition);
        }
        tiltReady_ = false;
    }

    void RebaseCameraPan(const Vect3f& appliedLocalPan) {
        // The game camera now carries this translation, so remove the same
        // component from the XR offset without changing the rendered view.
        offset_ -= appliedLocalPan;
    }

    void ResetGripDrag() {
        gripDragHand_ = -1;
        twoGripActive_ = false;
    }

    static bool BothGripsHeld(const AndroidXrHandState hands[2], bool enabled) {
        return enabled && hands[0].gripValid && hands[1].gripValid &&
            (hands[0].held & ANDROID_XR_GRIP) &&
            (hands[1].held & ANDROID_XR_GRIP);
    }

    // Drag with either one grip or the midpoint of both grips. A two-grip
    // distance change also scales the table. Gameplay rebases the drag's
    // terrain-plane part into the game camera.
    Vect3f UpdateGripDrag(const AndroidXrHandState hands[2], bool enabled,
                          const Vect3f& worldPivot, const MatXf& centerWorld,
                          const float headPosition[3]) {
        const bool bothHeld = BothGripsHeld(hands, enabled);
        float position[3]{};
        if (bothHeld) {
            const bool startingTwoGrip = !twoGripActive_;
            const Vect3f separation(
                hands[1].gripPosition[0] - hands[0].gripPosition[0],
                hands[1].gripPosition[1] - hands[0].gripPosition[1],
                -(hands[1].gripPosition[2] - hands[0].gripPosition[2]));
            const float distance = separation.norm();
            for (int axis = 0; axis < 3; ++axis)
                position[axis] = (hands[0].gripPosition[axis] +
                                  hands[1].gripPosition[axis]) * 0.5f;
            if (!twoGripActive_) {
                twoGripActive_ = true;
                gripDragHand_ = -1;
            } else {
                const Vect3f leftDelta(
                    hands[0].gripPosition[0] - gripPositions_[0][0],
                    hands[0].gripPosition[1] - gripPositions_[0][1],
                    hands[0].gripPosition[2] - gripPositions_[0][2]);
                const Vect3f rightDelta(
                    hands[1].gripPosition[0] - gripPositions_[1][0],
                    hands[1].gripPosition[1] - gripPositions_[1][1],
                    hands[1].gripPosition[2] - gripPositions_[1][2]);
                if (leftDelta.norm2() > 0.25f * 0.25f ||
                    rightDelta.norm2() > 0.25f * 0.25f) {
                    for (int axis = 0; axis < 3; ++axis)
                        gripDragPosition_[axis] = position[axis];
                    // Rebase after a tracking jump so the next valid frame
                    // does not turn the discontinuity into a scale change.
                    RememberTwoGrips(hands, distance);
                    return Vect3f::ZERO;
                }
                // Use this frame's distance change so reversing at a scale
                // limit immediately scales back in the opposite direction.
                const float distanceRatio =
                    std::max(gripDistance_, 0.001f) /
                    std::max(distance, 0.001f);
                // Slow grip zoom by the square root when zoomed out, and
                // increase it by the fourth root when zoomed in.
                const float scaleSpeed = std::pow(
                    defaultUnitsPerMeter_ / unitsPerMeter_,
                    unitsPerMeter_ >= defaultUnitsPerMeter_ ? 0.5f : 0.25f);
                const float unitsPerMeter = ClampUnitsPerMeter(
                    unitsPerMeter_ * std::pow(distanceRatio,
                        gripScaleBaseExponent_ * scaleSpeed));
                if (unitsPerMeter != unitsPerMeter_)
                    ApplyScale(unitsPerMeter, true, worldPivot, centerWorld,
                               headPosition);
            }
            RememberTwoGrips(hands, distance);
            if (startingTwoGrip) {
                for (int axis = 0; axis < 3; ++axis)
                    gripDragPosition_[axis] = position[axis];
                return Vect3f::ZERO;
            }
        } else {
            if (twoGripActive_) ResetGripDrag();
            int hand = -1;
            if (enabled) {
                if (gripDragHand_ >= 0 && hands[gripDragHand_].gripValid &&
                    (hands[gripDragHand_].held & ANDROID_XR_GRIP))
                    hand = gripDragHand_;
                else
                    for (int candidate = 0; candidate < 2; ++candidate)
                        if (hands[candidate].gripValid &&
                            (hands[candidate].held & ANDROID_XR_GRIP)) {
                            hand = candidate;
                            break;
                        }
            }
            if (hand < 0) {
                ResetGripDrag();
                return Vect3f::ZERO;
            }
            for (int axis = 0; axis < 3; ++axis)
                position[axis] = hands[hand].gripPosition[axis];
            if (hand != gripDragHand_) {
                gripDragHand_ = hand;
                for (int axis = 0; axis < 3; ++axis)
                    gripDragPosition_[axis] = position[axis];
                return Vect3f::ZERO;
            }
        }
        const Vect3f trackingDelta(position[0] - gripDragPosition_[0],
                                   position[1] - gripDragPosition_[1],
                                   -(position[2] - gripDragPosition_[2]));
        for (int axis = 0; axis < 3; ++axis)
            gripDragPosition_[axis] = position[axis];
        // Ignore a tracking jump instead of moving the camera across the map.
        if (trackingDelta.norm2() > 0.25f * 0.25f)
            return Vect3f::ZERO;
        const Mat3f heading = menuHeadingAligned_ ?
            menuHeadingRotation_ : Mat3f::ID;
        // Keep drag speed independent of the table's scale limits. Two grips
        // move the table at half the single-grip speed.
        const float dragUnitsPerMeter = gripDragUnitsPerMeter_ *
            (bothHeld ? 0.5f : 1.0f);
        const Vect3f localDrag = navigationRotation_ * heading * trackingDelta *
                                 -dragUnitsPerMeter;
        offset_ += localDrag;
        return localDrag;
    }

    // Return only this frame's bounded stick pan. Gameplay rebases it into
    // the game camera; the menu can keep it entirely in the rig.
    Vect3f UpdateControls(const float leftStick[2], bool leftActive,
                        const float rightStick[2], bool rightActive,
                        bool missionControls, float deltaSeconds,
                        const MatXf& centerWorld, const float headPosition[3],
                        const Vect3f& skyCenter, float skyRadius) {
        scaleInThisFrame_ = false;
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
        if (deltaSeconds <= 0.0f || deltaSeconds > 0.1f) return Vect3f::ZERO;
        if (missionControls && rightActive) {
            MatXf worldToRig = centerWorld;
            worldToRig.invert();
            const Vect3f up = worldToRig.rot() * Vect3f(0.0f, 0.0f, 1.0f);
            Vect3f forward = TerrainForward(centerWorld);
            const float x = rightStick[0];
            const float y = rightStick[1];
            if (std::abs(x) < 0.35f) snapReady_ = true;
            if (snapReady_ && std::abs(x) > 0.7f) {
                const float snapAngle = x > 0.0f ? XM_PI / 6.0f : -XM_PI / 6.0f;
                RotateAroundHead(Mat3f(up, snapAngle), headPosition);
                forward = Mat3f(snapAngle, Z_AXIS) * forward;
                tableForward_ = forward;
                snapReady_ = false;
            }
            if (std::abs(y) < 0.35f) tiltReady_ = true;
            if (tiltReady_ && std::abs(y) > 0.7f) {
                const Vect3f right(forward.y, -forward.x, 0.0f);
                const float pitch = TablePitch(centerWorld);
                const float target = std::clamp(
                    pitch - std::copysign(tiltStepRadians_, y),
                    0.0f, maxTiltRadians_);
                const float tiltDelta = pitch - target;
                if (std::abs(tiltDelta) > 0.0001f) {
                    RotateAroundHead(Mat3f(worldToRig.rot() * right, tiltDelta),
                                     headPosition);
                }
                tiltReady_ = false;
            }
        } else {
            snapReady_ = false;
            tiltReady_ = false;
        }
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
        // Scripted positions are authored independently of stick bounds.
        if (skyRadius > 0.0f && cameraPan.norm2() > 0.0f) {
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
    static float ClampUnitsPerMeter(float unitsPerMeter) {
        return std::clamp(unitsPerMeter,
            defaultUnitsPerMeter_ / maxZoomInFactor_,
            defaultUnitsPerMeter_ * maxZoomOutFactor_);
    }

    void RememberTwoGrips(const AndroidXrHandState hands[2], float distance) {
        gripDistance_ = distance;
        for (int hand = 0; hand < 2; ++hand)
            for (int axis = 0; axis < 3; ++axis)
                gripPositions_[hand][axis] = hands[hand].gripPosition[axis];
    }

    void ApplyScale(float unitsPerMeter, bool missionControls,
                    const Vect3f& worldPivot, const MatXf& centerWorld,
                    const float headPosition[3]) {
        if (unitsPerMeter == unitsPerMeter_) return;
        MatXf worldToRig = centerWorld;
        worldToRig.invert();
        if (missionControls && unitsPerMeter < unitsPerMeter_) {
            scaleInThisFrame_ = true;
            scaleHeadBeforeHeight_ =
                HeadWorldPosition(centerWorld, headPosition).z;
        }
        // Zoom out moves backward and up; zoom in moves forward and down.
        // World Z stays perpendicular to terrain, including when tilted.
        const Vect3f scaleForward = missionControls ?
            TerrainForward(centerWorld) : panForward_;
        const Vect3f scaleDirection = scaleForward - Vect3f(0.0f, 0.0f, 1.0f);
        SetScaleWithPivotDepth(unitsPerMeter, headPosition,
            worldToRig * worldPivot, worldToRig.rot() * scaleDirection);
    }

    float TablePitch(const MatXf& centerWorld) const {
        const Vect3f forward = centerWorld.rot() * navigationRotation_ *
            Vect3f(0.0f, 0.0f, 1.0f);
        return std::atan2(-forward.z,
            std::sqrt(forward.x * forward.x + forward.y * forward.y));
    }

    Vect3f TerrainForward(const MatXf& centerWorld) {
        Vect3f forward = centerWorld.rot() * navigationRotation_ *
            Vect3f(0.0f, 0.0f, 1.0f);
        forward.z = 0.0f;
        if (forward.norm2() > 0.0001f) {
            forward.normalize();
            tableForward_ = forward;
            tableForwardValid_ = true;
        } else if (!tableForwardValid_) {
            tableForward_ = Vect3f(0.0f, 1.0f, 0.0f);
            tableForwardValid_ = true;
        }
        return tableForward_;
    }

    void RotateAroundHead(const Mat3f& rotation, const float headPosition[3]) {
        const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const Vect3f before = Pose(headPosition, identity).trans();
        navigationRotation_ = rotation * navigationRotation_;
        offset_ += before - Pose(headPosition, identity).trans();
    }

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
    static constexpr float maxZoomOutFactor_ = 20.0f;
    static constexpr float maxZoomInFactor_ = 20.0f;
    static constexpr float gripDragUnitsPerMeter_ = 1600.0f;
    static constexpr float gripScaleBaseExponent_ = 3.0f;
    static constexpr float panSpeedGameUnitsPerSecond_ = 540.0f;
    static constexpr float scaleMovementMultiplier_ = 0.5f;
    static constexpr float minTerrainClearanceGameUnits_ = 100.0f;
    static constexpr float tiltStepRadians_ = 0.17453292f;
    static constexpr float maxTiltRadians_ = 1.57079633f;
    bool anchored_ = false;
    bool menuHeadingAligned_ = false;
    bool menuHeadingAnchored_ = false;
    Mat3f menuHeadingRotation_ = Mat3f::ID;
    Mat3f navigationRotation_ = Mat3f::ID;
    bool snapReady_ = false;
    bool tiltReady_ = false;
    Vect3f tableForward_ = Vect3f::ZERO;
    bool tableForwardValid_ = false;
    bool terrainLiftActive_ = false;
    bool scaleInThisFrame_ = false;
    float scaleHeadBeforeHeight_ = 0.0f;
    float anchor_[3]{};
    float headOrientation_[4]{0.0f, 0.0f, 0.0f, 1.0f};
    Vect3f panForward_ = Vect3f::ZERO;
    bool panHeadingValid_ = false;
    float unitsPerMeter_ = defaultUnitsPerMeter_;
    int gripDragHand_ = -1;
    float gripDragPosition_[3]{};
    bool twoGripActive_ = false;
    float gripPositions_[2][3]{};
    float gripDistance_ = 0.0f;
    Vect3f offset_ = Vect3f::ZERO;
    Vect3f scriptedAlignmentStart_ = Vect3f::ZERO;
    float scriptedAlignmentElapsed_ = 0.0f;
    float scriptedAlignmentDuration_ = 0.0f;
    bool scriptedAlignmentActive_ = false;
};

#endif
