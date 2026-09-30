#pragma once

#if defined(ANDROID_XR)

#include "xmath.h"
#include <algorithm>
#include <cmath>

// Input is OpenXR eye space in meters (-Z forward, +Y up). Output is engine
// camera space (+Z forward, +Y up), in LockQuad vertex order.
inline bool xrBuildControllerLaserMesh(Vect3f start, Vect3f finish,
                                       float cameraNearMeters,
                                       Vect3f (&corners)[4]) {
    // Clip before projection, including when the controller is behind the eye.
    const float nearZ = -0.01f;
    if (start.z >= nearZ && finish.z >= nearZ) return false;
    if (start.z > nearZ)
        start += (finish - start) * ((nearZ - start.z) / (finish.z - start.z));
    else if (finish.z > nearZ)
        finish += (start - finish) * ((nearZ - finish.z) / (start.z - finish.z));

    // Keep a constant apparent width by expanding each end in proportion to
    // its depth. The reference is 3 mm at one meter from the eye.
    constexpr float widthAtReferenceMeters = 0.003f;
    constexpr float referenceDepthMeters = 1.0f;
    constexpr float halfWidthTangent = widthAtReferenceMeters / (2.0f * referenceDepthMeters);
    const Vect2f projectedStart(start.x / -start.z, start.y / -start.z);
    const Vect2f projectedFinish(finish.x / -finish.z, finish.y / -finish.z);
    const Vect2f projectedDirection = projectedFinish - projectedStart;
    const float projectedLength = std::hypot(projectedDirection.x, projectedDirection.y);
    // An end-on or zero-length beam has no projected area.
    if (projectedLength <= 1.0e-8f) return false;
    const Vect2f side(-projectedDirection.y * halfWidthTangent / projectedLength,
                      projectedDirection.x * halfWidthTangent / projectedLength);
    const Vect2f points[4] = {projectedStart + side, projectedStart - side,
                            projectedFinish + side, projectedFinish - side};
    for (unsigned i = 0; i < 4; ++i) {
        const float depth = i < 2 ? -start.z : -finish.z;
        // The world camera's near plane is in game units and moves in meters
        // as the tabletop scale changes. Keep the projected laser position,
        // but place any nearer portion just inside the visible depth range.
        const float renderDepth = std::max(depth, cameraNearMeters + 0.001f);
        corners[i] = Vect3f(points[i].x * renderDepth,
                            points[i].y * renderDepth, renderDepth);
    }
    return true;
}

#endif // ANDROID_XR
