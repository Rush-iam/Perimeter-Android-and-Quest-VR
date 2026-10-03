#pragma once

#if defined(ANDROID_XR)
#include "xmath.h"

class cFont;
class cInterfaceRenderDevice;

void xrDrawHeightReadout(cInterfaceRenderDevice* renderer, cFont* font,
                         int value, const Vect3f& anchor,
                         const Vect3f& right, const Vect3f& up,
                         float height, float offset, bool rightSide);
#endif
