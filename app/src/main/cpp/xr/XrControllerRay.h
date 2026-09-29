#pragma once

#if defined(ANDROID_XR)

#include "xmath.h"
#include <algorithm>
#include <cmath>

struct XrWorldRay {
    Vect3f origin = Vect3f::ZERO;
    Vect3f direction = Vect3f::ZERO;
    float distance = 0.0f;
};

inline bool xrClipRayAxis(float origin, float direction, float minimum, float maximum,
                          float& entry, float& exit) {
    if (direction == 0.0f) return origin >= minimum && origin <= maximum;
    float first = (minimum - origin) / direction;
    float last = (maximum - origin) / direction;
    if (first > last) std::swap(first, last);
    entry = std::max(entry, first);
    exit = std::min(exit, last);
    return entry <= exit;
}

// Local directions keep their scale, so the ray parameter stays in world units.
inline bool xrRayIntersectsBox(const Vect3f& origin, const Vect3f& direction,
                               const Vect3f& minimum, const Vect3f& maximum, float limit) {
    float entry = 0.0f, exit = limit;
    for (int axis = 0; axis < 3; ++axis)
        if (!xrClipRayAxis(origin[axis], direction[axis], minimum[axis],
                          maximum[axis], entry, exit)) return false;
    return true;
}

inline float xrRayTriangleDistance(const Vect3f& origin, const Vect3f& direction,
                                    const Vect3f& a, const Vect3f& b, const Vect3f& c) {
    const Vect3f edge1 = b - a, edge2 = c - a;
    const Vect3f cross = direction % edge2;
    const float determinant = edge1.dot(cross);
    if (std::abs(determinant) < 1.0e-8f) return -1.0f;
    const float inverse = 1.0f / determinant;
    const Vect3f offset = origin - a;
    const float u = offset.dot(cross) * inverse;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    const Vect3f q = offset % edge1;
    const float v = direction.dot(q) * inverse;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    return edge2.dot(q) * inverse;
}

// All distances here are in game units and directions must be normalized.
inline float xrRaySphereDistance(const Vect3f& origin, const Vect3f& direction,
                                  const Vect3f& center, float radius) {
    const Vect3f offset = origin - center;
    const float along = offset.dot(direction);
    const float discriminant = along * along - offset.norm2() + radius * radius;
    if (discriminant < 0.0f) return -1.0f;
    const float root = std::sqrt(discriminant);
    const float entry = -along - root;
    return entry > 0.0f ? entry : -along + root;
}

// Clip to the terrain rectangle, then march at most half a heightmap cell
// per step. Refine the first crossing so the endpoint stays on the aim ray.
template<class HeightAt>
float xrRayTerrainDistance(const Vect3f& origin, const Vect3f& direction,
                            int width, int height, float limit, HeightAt heightAt) {
    float entry = 0.0f;
    float exit = limit;
    if (width < 2 || height < 2 || limit <= 0.0f ||
        !xrClipRayAxis(origin.x, direction.x, 0.0f, static_cast<float>(width - 1), entry, exit) ||
        !xrClipRayAxis(origin.y, direction.y, 0.0f, static_cast<float>(height - 1), entry, exit))
        return -1.0f;

    const auto clearance = [&](float distance) {
        const Vect3f point = origin + direction * distance;
        const float x = std::clamp(point.x, 0.0f, static_cast<float>(width - 1));
        const float y = std::clamp(point.y, 0.0f, static_cast<float>(height - 1));
        const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
        const int x1 = std::min(x0 + 1, width - 1);
        const int y1 = std::min(y0 + 1, height - 1);
        const float fx = x - x0, fy = y - y0;
        const float lower = heightAt(x0, y0) * (1.0f - fx) + heightAt(x1, y0) * fx;
        const float upper = heightAt(x0, y1) * (1.0f - fx) + heightAt(x1, y1) * fx;
        return point.z - (lower * (1.0f - fy) + upper * fy);
    };
    float previous = entry;
    float previousClearance = clearance(entry);
    if (std::abs(previousClearance) < 1.0e-4f) return entry;
    const float horizontal = std::max(std::abs(direction.x), std::abs(direction.y));
    // Vertical rays need only one step: the height at their XY is constant.
    const float step = horizontal > 1.0e-6f ? 0.5f / horizontal : exit - entry;
    while (previous < exit) {
        const float current = std::min(previous + step, exit);
        if (current <= previous) break;
        const float currentClearance = clearance(current);
        if (std::abs(currentClearance) < 1.0e-4f) return current;
        if ((previousClearance > 0.0f) != (currentClearance > 0.0f)) {
            float low = previous, high = current;
            for (int iteration = 0; iteration < 14; ++iteration) {
                const float middle = (low + high) * 0.5f;
                if ((clearance(middle) > 0.0f) == (previousClearance > 0.0f))
                    low = middle;
                else
                    high = middle;
            }
            return (low + high) * 0.5f;
        }
        previous = current;
        previousClearance = currentClearance;
    }
    return -1.0f;
}

#endif // ANDROID_XR
