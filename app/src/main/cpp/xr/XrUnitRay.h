#pragma once

#if defined(ANDROID_XR)

#include "XrControllerRay.h"

class cObjectNodeRoot;

// Trace both controllers against a visible unit model. Reject rays outside
// the model bounds before updating its animation and testing its meshes.
void xrIntersectUnitRays(cObjectNodeRoot& model, XrWorldRay (&rays)[2]);

#endif // ANDROID_XR
