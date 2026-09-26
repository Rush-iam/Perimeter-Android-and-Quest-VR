#pragma once

#if defined(ANDROID_XR)

struct IDirect3DDevice9;
class cInterfaceRenderDevice;

struct AndroidXrEyeView {
    float position[3];
    float orientation[4];
    float fov[4]; // left, right, down, up, in radians
    unsigned width;
    unsigned height;
};

// Called after loading DXVK 2 and before Direct3DCreate9. The Android-only
// bootstrap mediates DXVK's Vulkan creation through the active OpenXR runtime.
bool androidXrBootstrapInitialize(void* dxvkHandle);

// Called after the renderer's initial D3D9 reset. A non-XR launch is a no-op.
bool androidXrBootstrapCreateSession(IDirect3DDevice9* device);
void androidXrBootstrapDestroySession();
// Returns true only when a paced frame has valid tracked views to render.
bool androidXrBeginFrame(AndroidXrEyeView views[2]);
bool androidXrBindEye(cInterfaceRenderDevice* renderer, unsigned eye);
void androidXrUnbindEye(cInterfaceRenderDevice* renderer);
void androidXrEndFrame(bool rendered);

// Called after the D3D9 device and interface have been released.
void androidXrBootstrapDestroy();

#endif // ANDROID_XR
