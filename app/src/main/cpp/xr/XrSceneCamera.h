#pragma once

#if defined(ANDROID_XR)

#include "xr/XrCameraRig.h"
#include "Scene.h"
#include "cCamera.h"

// Keep scene-owned eye cameras in sync with the authored camera while applying
// the headset pose and OpenXR's asymmetric per-eye projection.
inline void xrPrepareSceneCameras(cScene* scene, cCamera* center,
                                  cCamera* eyes[2], const XrCameraRig& rig,
                                  const AndroidXrEyeView views[2])
{
    for (unsigned eye = 0; eye < 2; ++eye) {
        if (!eyes[eye]) eyes[eye] = scene->CreateCamera();
        cCamera* camera = eyes[eye];
        center->SetCopy(camera);
        const auto& view = views[eye];
        MatXf eyePose = rig.Pose(view.position, view.orientation);
        eyePose.invert();
        camera->SetPosition(eyePose * center->GetMatrix());
        camera->SetViewSizeOverride(static_cast<float>(view.width),
                                    static_cast<float>(view.height));
        camera->SetClip(sRectangle4f(-0.5f, -0.5f, 0.5f, 0.5f));
        const Vect2f zPlane = camera->GetZPlane();
        camera->SetZPlaneTemp(Vect2f(std::min(zPlane.x, rig.UnitsPerMeter() * 0.05f),
                                     zPlane.y));
        camera->SetAsymmetricPerspective(std::tan(view.fov[0]),
            std::tan(view.fov[1]), std::tan(view.fov[2]), std::tan(view.fov[3]));
    }
    // A model near the LOD boundary must use the same mesh in both eyes.
    const Vect3f lodPosition = (eyes[0]->GetPos() + eyes[1]->GetPos()) * 0.5f;
    const float lodFocus = (eyes[0]->GetFocusViewPort().x +
                            eyes[1]->GetFocusViewPort().x) * 0.5f;
    for (cCamera* eye : {eyes[0], eyes[1]})
        eye->SetStereoLodReference(lodPosition, lodFocus);
}

#endif
