#pragma once

#if defined(ANDROID_XR)

#include "XrControllerRay.h"

class cObjectNodeRoot;

// Trace both controllers with shared mesh transforms and bounds checks.
void xrIntersectBuildingRays(cObjectNodeRoot& model, XrWorldRay (&rays)[2]);

#endif // ANDROID_XR
