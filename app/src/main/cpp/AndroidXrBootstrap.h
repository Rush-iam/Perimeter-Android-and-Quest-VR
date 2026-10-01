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

enum AndroidXrButton : unsigned {
    ANDROID_XR_SELECT = 1u << 0,
    ANDROID_XR_COMMAND = 1u << 1,
    ANDROID_XR_PANEL = 1u << 2,
    ANDROID_XR_CANCEL = 1u << 3,
    ANDROID_XR_ROTATE = 1u << 4,
};

struct AndroidXrHandState {
    float aimPosition[3]{};
    float aimOrientation[4]{};
    float gripPosition[3]{};
    float gripOrientation[4]{};
    float thumbstick[2]{};
    unsigned pressed = 0;
    unsigned released = 0;
    bool aimValid = false;
    bool gripValid = false;
    bool thumbstickActive = false;
};

struct AndroidXrInputFrame {
    AndroidXrHandState hands[2]; // left, right
    bool focused = false;
    bool recentered = false;
};

// Called after loading DXVK 2 and before Direct3DCreate9. The Android-only
// bootstrap mediates DXVK's Vulkan creation through the active OpenXR runtime.
bool androidXrBootstrapInitialize(void* dxvkHandle);

// Called after the renderer's initial D3D9 reset.
bool androidXrBootstrapCreateSession(IDirect3DDevice9* device);
void androidXrBootstrapDestroySession();
// Returns true only when a paced frame has valid tracked views to render.
bool androidXrBeginFrame(AndroidXrEyeView views[2], AndroidXrInputFrame* input);
bool androidXrIsFocused();
bool androidXrSessionActive();
bool androidXrBindEye(cInterfaceRenderDevice* renderer, unsigned eye);
void androidXrUnbindEye(cInterfaceRenderDevice* renderer);
bool androidXrPrepareUiPanel(unsigned width, unsigned height);
bool androidXrBeginUiPanel(cInterfaceRenderDevice* renderer, unsigned width, unsigned height);
void androidXrRestoreUiPanelViewport(cInterfaceRenderDevice* renderer,
                                     unsigned width, unsigned height);
void androidXrEndUiPanel(cInterfaceRenderDevice* renderer);
bool androidXrDrawUiPanelInEye(cInterfaceRenderDevice* renderer, unsigned eye);
bool androidXrHitUiPanel(const AndroidXrHandState& hand, unsigned width, unsigned height,
                         float* pixelX, float* pixelY, float* distanceMeters = nullptr);
void androidXrSetUiPanelFixed();
void androidXrSetUiPanelFixed(float distanceMeters, float widthMeters);
void androidXrResetUiPanelPose();
bool androidXrAttachUiPanelToHand(const AndroidXrHandState& hand, unsigned handIndex);
void androidXrSetUiPanelVisible(bool visible);
void androidXrEndFrame(bool rendered);

// Called after the D3D9 device and interface have been released.
void androidXrBootstrapDestroy();

#endif // ANDROID_XR
