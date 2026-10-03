#include "AndroidXrBootstrap.h"

#if defined(ANDROID_XR)

#include "AndroidXrListenerPose.h"

#include "StdAfxRD.h"
#include "D3DRender.h"
#include <d3d9_interfaces.h>
#include <SDL.h>
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <vulkan/vulkan.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <dxvk_android_xr.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// Keep exported DXVK storage and OpenXR swapchains identical for direct copies.
constexpr VkFormat xrColorFormat = VK_FORMAT_R8G8B8A8_SRGB;
constexpr unsigned xrButtonActionCount = 5;

using SetHooksFn = bool (*)(const DxvkAndroidXrHooks*);
using ResetHooksFn = void (*)();

struct Bootstrap {
    struct EyeTarget {
        IDirect3DResource9* resource = nullptr;
        IDirect3DSurface9* surface = nullptr;
        IDirect3DSurface9* depth = nullptr;
        ID3D9VkInteropTexture* interopImage = nullptr;
        VkImage image = VK_NULL_HANDLE;
        VkImageLayout originalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    JavaVM* vm = nullptr;
    jobject activity = nullptr;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction aimAction = XR_NULL_HANDLE;
    XrAction gripAction = XR_NULL_HANDLE;
    XrAction thumbstickAction = XR_NULL_HANDLE;
    XrAction buttonActions[xrButtonActionCount]{};
    XrPath handPaths[2]{};
    XrSpace aimSpaces[2]{};
    XrSpace gripSpaces[2]{};
    unsigned previousHeld[2]{};
    bool previousActive[2]{};
    bool focused = false;
    XrSwapchain eyeSwapchains[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSwapchain panelSwapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkan2KHR> swapchainImages[2];
    std::vector<XrSwapchainImageVulkan2KHR> panelSwapchainImages;
    EyeTarget eyeTargets[2];
    EyeTarget panelTarget;
    IDirect3DDevice9* d3dDevice = nullptr;
    D3DFORMAT depthFormat = D3DFMT_UNKNOWN;
    XrPosef panelPose{{0, 0, 0, 1}, {0, 0, -1.4f}};
    float panelWidthMeters = 1.2f;
    float panelDistanceMeters = 1.4f;
    XrVector3f panelAnchor{};
    XrVector3f panelForward{0.0f, 0.0f, -1.0f};
    bool panelReady = false;
    bool panelPoseInitialized = false;
    bool panelHandAttached = false;
    bool panelVisible = true;
    bool panelUsesEyeImages = false;
    ID3D9VkInteropDevice* interop = nullptr;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    PFN_xrCreateVulkanInstanceKHR createInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR getGraphicsDevice = nullptr;
    PFN_xrCreateVulkanDeviceKHR createDevice = nullptr;
    ResetHooksFn resetHooks = nullptr;
    bool hooksInstalled = false;
    VkInstance vulkanInstance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice vulkanDevice = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    VkCommandPool copyPool = VK_NULL_HANDLE;
    VkCommandBuffer copyCommand = VK_NULL_HANDLE;
    VkFence copyFence = VK_NULL_HANDLE;
    bool running = false;
    bool frameBegun = false;
    bool viewsValid = false;
    XrTime recenterTime = 0;
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
};

Bootstrap bootstrap;
std::atomic<bool> standbyStop{false};
std::thread standbyThread;
std::atomic<bool> lifecyclePaused{false};
// Serialize the game thread's frame calls with the pause event pump.
std::mutex lifecycleMutex;
std::mutex pausedThreadMutex;
std::atomic<bool> gameThreadOwnsEvents{false};
std::thread pausedEventThread;
void logResult(const char* operation, XrResult result);

void recordReferenceSpaceChange(const XrEventDataBuffer& event) {
    const auto& change =
        *reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&event);
    if (change.session == bootstrap.session &&
        change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
        bootstrap.recenterTime = change.changeTime;
        __android_log_write(ANDROID_LOG_INFO, "PerimeterXr",
                            "LOCAL reference space recenter pending");
    }
}

XrVector3f rotateByQuaternion(const XrQuaternionf& q, XrVector3f v) {
    const XrVector3f u{q.x, q.y, q.z};
    const float dotUV = u.x * v.x + u.y * v.y + u.z * v.z;
    const float dotUU = u.x * u.x + u.y * u.y + u.z * u.z;
    const XrVector3f cross{u.y * v.z - u.z * v.y,
                           u.z * v.x - u.x * v.z,
                           u.x * v.y - u.y * v.x};
    return {2.0f * dotUV * u.x + (q.w * q.w - dotUU) * v.x + 2.0f * q.w * cross.x,
            2.0f * dotUV * u.y + (q.w * q.w - dotUU) * v.y + 2.0f * q.w * cross.y,
            2.0f * dotUV * u.z + (q.w * q.w - dotUU) * v.z + 2.0f * q.w * cross.z};
}

bool createInputActions() {
    auto& state = bootstrap;
    const char* handNames[2] = {"/user/hand/left", "/user/hand/right"};
    for (unsigned hand = 0; hand < 2; ++hand)
        if (xrStringToPath(state.instance, handNames[hand], &state.handPaths[hand]) != XR_SUCCESS)
            return false;
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(setInfo.actionSetName, sizeof(setInfo.actionSetName), "gameplay");
    std::snprintf(setInfo.localizedActionSetName, sizeof(setInfo.localizedActionSetName), "Gameplay");
    setInfo.priority = 0;
    if (xrCreateActionSet(state.instance, &setInfo, &state.actionSet) != XR_SUCCESS) return false;
    const auto makeAction = [&](XrAction& action, XrActionType type,
                                const char* name, const char* label) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        info.actionType = type;
        std::snprintf(info.actionName, sizeof(info.actionName), "%s", name);
        std::snprintf(info.localizedActionName, sizeof(info.localizedActionName), "%s", label);
        info.countSubactionPaths = 2;
        info.subactionPaths = state.handPaths;
        return xrCreateAction(state.actionSet, &info, &action) == XR_SUCCESS;
    };
    if (!makeAction(state.aimAction, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim pose") ||
        !makeAction(state.gripAction, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Grip pose") ||
        !makeAction(state.thumbstickAction, XR_ACTION_TYPE_VECTOR2F_INPUT,
                    "table_control", "Table control") ||
        !makeAction(state.buttonActions[0], XR_ACTION_TYPE_BOOLEAN_INPUT, "select", "Select") ||
        !makeAction(state.buttonActions[1], XR_ACTION_TYPE_BOOLEAN_INPUT,
                    "panel_toggle", "Panel toggle") ||
        !makeAction(state.buttonActions[2], XR_ACTION_TYPE_BOOLEAN_INPUT, "cancel", "Cancel") ||
        !makeAction(state.buttonActions[3], XR_ACTION_TYPE_BOOLEAN_INPUT, "rotate", "Rotate") ||
        !makeAction(state.buttonActions[4], XR_ACTION_TYPE_BOOLEAN_INPUT, "drag", "Drag table"))
        return false;

    XrPath profile = XR_NULL_PATH;
    if (xrStringToPath(state.instance, "/interaction_profiles/oculus/touch_controller", &profile)
        != XR_SUCCESS) return false;
    struct Binding { XrAction action; const char* path; };
    const Binding paths[] = {
        {state.aimAction, "/user/hand/left/input/aim/pose"},
        {state.aimAction, "/user/hand/right/input/aim/pose"},
        {state.gripAction, "/user/hand/left/input/grip/pose"},
        {state.gripAction, "/user/hand/right/input/grip/pose"},
        {state.thumbstickAction, "/user/hand/left/input/thumbstick"},
        {state.thumbstickAction, "/user/hand/right/input/thumbstick"},
        {state.buttonActions[0], "/user/hand/left/input/trigger/value"},
        {state.buttonActions[0], "/user/hand/right/input/trigger/value"},
        {state.buttonActions[1], "/user/hand/left/input/x/click"},
        {state.buttonActions[1], "/user/hand/right/input/a/click"},
        {state.buttonActions[2], "/user/hand/left/input/y/click"},
        {state.buttonActions[2], "/user/hand/right/input/b/click"},
        {state.buttonActions[3], "/user/hand/left/input/thumbstick/click"},
        {state.buttonActions[3], "/user/hand/right/input/thumbstick/click"},
        {state.buttonActions[4], "/user/hand/left/input/squeeze/value"},
        {state.buttonActions[4], "/user/hand/right/input/squeeze/value"},
    };
    std::vector<XrActionSuggestedBinding> bindings;
    bindings.reserve(sizeof(paths) / sizeof(paths[0]));
    for (const auto& binding : paths) {
        XrPath path = XR_NULL_PATH;
        if (xrStringToPath(state.instance, binding.path, &path) != XR_SUCCESS) return false;
        bindings.push_back({binding.action, path});
    }
    XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestion.interactionProfile = profile;
    suggestion.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    suggestion.suggestedBindings = bindings.data();
    const XrResult result = xrSuggestInteractionProfileBindings(state.instance, &suggestion);
    logResult("xrSuggestInteractionProfileBindings(Touch)", result);
    return result == XR_SUCCESS;
}

bool attachInputActions() {
    auto& state = bootstrap;
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &state.actionSet;
    if (xrAttachSessionActionSets(state.session, &attach) != XR_SUCCESS) return false;
    for (unsigned hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        info.subactionPath = state.handPaths[hand];
        info.poseInActionSpace.orientation.w = 1.0f;
        info.action = state.aimAction;
        if (xrCreateActionSpace(state.session, &info, &state.aimSpaces[hand]) != XR_SUCCESS)
            return false;
        info.action = state.gripAction;
        if (xrCreateActionSpace(state.session, &info, &state.gripSpaces[hand]) != XR_SUCCESS)
            return false;
    }
    return true;
}

void resetInputEdges() {
    auto& state = bootstrap;
    for (unsigned hand = 0; hand < 2; ++hand) {
        state.previousHeld[hand] = 0;
        state.previousActive[hand] = false;
    }
}

void snapshotInput(AndroidXrInputFrame* output) {
    auto& state = bootstrap;
    *output = {};
    output->focused = state.focused;
    if (!state.focused) { resetInputEdges(); return; }
    XrActiveActionSet active{state.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (xrSyncActions(state.session, &sync) != XR_SUCCESS) {
        resetInputEdges();
        return;
    }
    for (unsigned hand = 0; hand < 2; ++hand) {
        auto& value = output->hands[hand];
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.subactionPath = state.handPaths[hand];
        XrActionStatePose aim{XR_TYPE_ACTION_STATE_POSE};
        get.action = state.aimAction;
        if (xrGetActionStatePose(state.session, &get, &aim) != XR_SUCCESS || !aim.isActive) {
            state.previousActive[hand] = false;
            state.previousHeld[hand] = 0;
            continue;
        }
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (xrLocateSpace(state.aimSpaces[hand], state.localSpace,
                          state.frameState.predictedDisplayTime, &location) != XR_SUCCESS) {
            state.previousActive[hand] = false;
            state.previousHeld[hand] = 0;
            continue;
        }
        const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                           XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        value.aimValid = (location.locationFlags & valid) == valid;
        if (!value.aimValid) {
            state.previousActive[hand] = false;
            state.previousHeld[hand] = 0;
            continue;
        }
        const auto& p = location.pose;
        value.aimPosition[0] = p.position.x; value.aimPosition[1] = p.position.y;
        value.aimPosition[2] = p.position.z;
        value.aimOrientation[0] = p.orientation.x; value.aimOrientation[1] = p.orientation.y;
        value.aimOrientation[2] = p.orientation.z; value.aimOrientation[3] = p.orientation.w;
        get.action = state.gripAction;
        XrActionStatePose grip{XR_TYPE_ACTION_STATE_POSE};
        XrSpaceLocation gripLocation{XR_TYPE_SPACE_LOCATION};
        if (xrGetActionStatePose(state.session, &get, &grip) == XR_SUCCESS && grip.isActive &&
            xrLocateSpace(state.gripSpaces[hand], state.localSpace,
                          state.frameState.predictedDisplayTime, &gripLocation) == XR_SUCCESS &&
            (gripLocation.locationFlags & valid) == valid) {
            const auto& pose = gripLocation.pose;
            value.gripPosition[0] = pose.position.x;
            value.gripPosition[1] = pose.position.y;
            value.gripPosition[2] = pose.position.z;
            value.gripOrientation[0] = pose.orientation.x;
            value.gripOrientation[1] = pose.orientation.y;
            value.gripOrientation[2] = pose.orientation.z;
            value.gripOrientation[3] = pose.orientation.w;
            value.gripValid = true;
        }
        get.action = state.thumbstickAction;
        XrActionStateVector2f thumbstick{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (xrGetActionStateVector2f(state.session, &get, &thumbstick) == XR_SUCCESS &&
            thumbstick.isActive) {
            value.thumbstick[0] = thumbstick.currentState.x;
            value.thumbstick[1] = thumbstick.currentState.y;
            value.thumbstickActive = true;
        }
        unsigned held = 0;
        for (unsigned button = 0; button < xrButtonActionCount; ++button) {
            get.action = state.buttonActions[button];
            XrActionStateBoolean buttonState{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (xrGetActionStateBoolean(state.session, &get, &buttonState) == XR_SUCCESS &&
                buttonState.isActive && buttonState.currentState)
                held |= 1u << button;
        }
        if (state.previousActive[hand]) {
            value.pressed = held & ~state.previousHeld[hand];
            value.released = state.previousHeld[hand] & ~held;
        }
        value.held = held;
        state.previousHeld[hand] = held;
        state.previousActive[hand] = true;
    }
}

struct QueueLock {
    explicit QueueLock(ID3D9VkInteropDevice* device) : interop(device) {
        interop->LockSubmissionQueue();
    }
    ~QueueLock() { interop->ReleaseSubmissionQueue(); }
    ID3D9VkInteropDevice* interop;
};

void runStandbyFrames() {
    auto& state = bootstrap;
    while (!standbyStop.load()) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result;
        while ((result = xrPollEvent(state.instance, &event)) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto& change = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                if (change.session == state.session) {
                    state.focused = change.state == XR_SESSION_STATE_FOCUSED;
                    if (!state.focused) resetInputEdges();
                    if (change.state == XR_SESSION_STATE_READY && !state.running) {
                        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        result = xrBeginSession(state.session, &begin);
                        logResult("xrBeginSession(standby)", result);
                        state.running = result == XR_SUCCESS;
                    } else if (change.state == XR_SESSION_STATE_STOPPING && state.running) {
                        xrEndSession(state.session);
                        state.running = false;
                    }
                }
            }
            if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
                recordReferenceSpaceChange(event);
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!state.running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frame{XR_TYPE_FRAME_STATE};
        if (xrWaitFrame(state.session, &wait, &frame) != XR_SUCCESS) break;
        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
        { QueueLock lock(state.interop); result = xrBeginFrame(state.session, &begin); }
        if (result != XR_SUCCESS) break;
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = frame.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        { QueueLock lock(state.interop); result = xrEndFrame(state.session, &end); }
        if (result != XR_SUCCESS) break;
    }
}

// SDL suspends its game thread on Activity pause. Keep processing the runtime's
// STOPPING event so tracking is released instead of leaving a running session
// behind while Android has withdrawn focus and tracker access.
void runPausedEvents() {
    while (lifecyclePaused.load()) {
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex);
            if (!bootstrap.session || !bootstrap.running) break;
            if (!bootstrap.frameBegun) {
                XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
                XrResult pollResult;
                while ((pollResult = xrPollEvent(bootstrap.instance, &event)) == XR_SUCCESS) {
                    if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                        const auto& change = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                        if (change.session == bootstrap.session) {
                            bootstrap.focused = change.state == XR_SESSION_STATE_FOCUSED;
                            if (!bootstrap.focused) resetInputEdges();
                            if (change.state == XR_SESSION_STATE_STOPPING) {
                                const XrResult endResult = xrEndSession(bootstrap.session);
                                logResult("xrEndSession(paused)", endResult);
                                bootstrap.running = false;
                                break;
                            }
                        }
                    } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
                        recordReferenceSpaceChange(event);
                    } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                        bootstrap.running = false;
                        bootstrap.focused = false;
                        resetInputEdges();
                        break;
                    }
                    event = {XR_TYPE_EVENT_DATA_BUFFER};
                }
                if (pollResult != XR_EVENT_UNAVAILABLE && pollResult != XR_SUCCESS) {
                    logResult("xrPollEvent(paused)", pollResult);
                    break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void logResult(const char* operation, XrResult result) {
    if (result != XR_SUCCESS)
        __android_log_print(ANDROID_LOG_ERROR, "PerimeterXr", "%s: XrResult=%d",
                            operation, static_cast<int>(result));
}

bool loadProc(const char* name, PFN_xrVoidFunction* output) {
    const XrResult result = xrGetInstanceProcAddr(bootstrap.instance, name, output);
    if (result != XR_SUCCESS || !*output) {
        logResult(name, result);
        return false;
    }
    return true;
}

VkResult createVkInstance(void* user, const VkInstanceCreateInfo* info,
                          const VkAllocationCallbacks* allocator, VkInstance* output) {
    auto& state = *static_cast<Bootstrap*>(user);
    XrVulkanInstanceCreateInfoKHR xrInfo{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xrInfo.systemId = state.system;
    xrInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xrInfo.vulkanCreateInfo = info;
    xrInfo.vulkanAllocator = allocator;
    VkResult vkResult = VK_ERROR_INITIALIZATION_FAILED;
    const XrResult result = state.createInstance(state.instance, &xrInfo, output, &vkResult);
    logResult("xrCreateVulkanInstanceKHR", result);
    if (result == XR_SUCCESS && vkResult == VK_SUCCESS) state.vulkanInstance = *output;
    return result == XR_SUCCESS ? vkResult : VK_ERROR_INITIALIZATION_FAILED;
}

VkResult selectVkDevice(void* user, VkInstance instance, VkPhysicalDevice* output) {
    auto& state = *static_cast<Bootstrap*>(user);
    XrVulkanGraphicsDeviceGetInfoKHR info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    info.systemId = state.system;
    info.vulkanInstance = instance;
    const XrResult result = state.getGraphicsDevice(state.instance, &info, output);
    logResult("xrGetVulkanGraphicsDevice2KHR", result);
    if (result == XR_SUCCESS) state.physicalDevice = *output;
    return result == XR_SUCCESS ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

VkResult createVkDevice(void* user, VkPhysicalDevice physical,
                        const VkDeviceCreateInfo* info,
                        const VkAllocationCallbacks* allocator, VkDevice* output) {
    auto& state = *static_cast<Bootstrap*>(user);
    XrVulkanDeviceCreateInfoKHR xrInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xrInfo.systemId = state.system;
    xrInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xrInfo.vulkanPhysicalDevice = physical;
    xrInfo.vulkanCreateInfo = info;
    xrInfo.vulkanAllocator = allocator;
    VkResult vkResult = VK_ERROR_INITIALIZATION_FAILED;
    const XrResult result = state.createDevice(state.instance, &xrInfo, output, &vkResult);
    logResult("xrCreateVulkanDeviceKHR", result);
    if (result == XR_SUCCESS && vkResult == VK_SUCCESS) {
        state.vulkanDevice = *output;
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i) {
            const uint32_t family = info->pQueueCreateInfos[i].queueFamilyIndex;
            if (family < count && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                state.graphicsQueueFamily = family;
                break;
            }
        }
    }
    return result == XR_SUCCESS ? vkResult : VK_ERROR_INITIALIZATION_FAILED;
}

bool createEyes(IDirect3DDevice9* device) {
    auto& state = bootstrap;
    uint32_t formatCount = 0;
    if (xrEnumerateSwapchainFormats(state.session, 0, &formatCount, nullptr) != XR_SUCCESS ||
        !formatCount) return false;
    std::vector<int64_t> formats(formatCount);
    if (xrEnumerateSwapchainFormats(state.session, formatCount, &formatCount,
                                    formats.data()) != XR_SUCCESS) return false;
    // Quest DXVK interop targets use sRGB storage with UNORM rendering views.
    // Copy display-encoded bytes unchanged between matching sRGB images so
    // OpenXR decodes them before composition, without cross-format transfers.
    if (std::find(formats.begin(), formats.end(),
                  static_cast<int64_t>(xrColorFormat)) == formats.end()) {
        __android_log_write(ANDROID_LOG_ERROR, "PerimeterXr",
                            "OpenXR runtime does not support R8G8B8A8_SRGB swapchains");
        return false;
    }
    uint32_t viewCount = 0;
    if (xrEnumerateViewConfigurationViews(state.instance, state.system,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr) != XR_SUCCESS ||
        viewCount != 2) return false;
    XrViewConfigurationView views[2] = {
        {XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (xrEnumerateViewConfigurationViews(state.instance, state.system,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, views) != XR_SUCCESS) return false;

    IDirect3DSurface9* previousColor = nullptr;
    IDirect3DSurface9* previousDepth = nullptr;
    if (FAILED(device->GetRenderTarget(0, &previousColor)) ||
        FAILED(device->GetDepthStencilSurface(&previousDepth))) {
        if (previousColor) previousColor->Release();
        if (previousDepth) previousDepth->Release();
        return false;
    }
    D3DSURFACE_DESC depthDesc{};
    const bool depthInfoValid = SUCCEEDED(previousDepth->GetDesc(&depthDesc));
    bool success = depthInfoValid;
    for (uint32_t eye = 0; eye < 2 && success; ++eye) {
        auto& target = state.eyeTargets[eye];
        target.width = views[eye].recommendedImageRectWidth;
        target.height = views[eye].recommendedImageRectHeight;
        success = target.width > 0 && target.height > 0;
        if (!success) break;
        D3D9VkExtImageDesc desc{};
        desc.Type = D3DRTYPE_TEXTURE;
        desc.Width = target.width;
        desc.Height = target.height;
        desc.Depth = 1;
        desc.MipLevels = 1;
        desc.Usage = D3DUSAGE_RENDERTARGET;
        desc.Format = D3DFMT_A8B8G8R8;
        desc.Pool = D3DPOOL_DEFAULT;
        desc.MultiSample = D3DMULTISAMPLE_NONE;
        desc.ImageUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        success = SUCCEEDED(state.interop->CreateImage(&desc, &target.resource));
        if (!success) break;
        auto* texture = static_cast<IDirect3DTexture9*>(target.resource);
        success = SUCCEEDED(texture->GetSurfaceLevel(0, &target.surface)) &&
            SUCCEEDED(device->CreateDepthStencilSurface(target.width, target.height,
                depthDesc.Format, D3DMULTISAMPLE_NONE, 0, TRUE, &target.depth, nullptr)) &&
            SUCCEEDED(device->SetRenderTarget(0, target.surface)) &&
            SUCCEEDED(device->SetDepthStencilSurface(target.depth)) &&
            SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0)) &&
            SUCCEEDED(target.resource->QueryInterface(__uuidof(ID3D9VkInteropTexture),
                reinterpret_cast<void**>(&target.interopImage)));
    }
    device->SetRenderTarget(0, previousColor);
    device->SetDepthStencilSurface(previousDepth);
    previousColor->Release();
    previousDepth->Release();
    if (!success) return false;
    state.interop->FlushRenderingCommands();
    for (auto& target : state.eyeTargets) {
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        success = state.interop->WaitForResource(target.resource, 0) &&
            SUCCEEDED(target.interopImage->GetVulkanImageInfo(&target.image,
                &target.originalLayout, &imageInfo)) && target.image != VK_NULL_HANDLE &&
            target.originalLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
            imageInfo.extent.width == target.width && imageInfo.extent.height == target.height &&
            (imageInfo.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
            imageInfo.format == xrColorFormat;
        if (!success) break;
    }
    if (!success) return false;
    state.depthFormat = depthDesc.Format;

    XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                               XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.format = static_cast<int64_t>(xrColorFormat);
    swapchainInfo.sampleCount = 1;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        swapchainInfo.width = state.eyeTargets[eye].width;
        swapchainInfo.height = state.eyeTargets[eye].height;
        XrResult result = xrCreateSwapchain(state.session, &swapchainInfo,
                                             &state.eyeSwapchains[eye]);
        logResult("xrCreateSwapchain(eye)", result);
        if (result != XR_SUCCESS) return false;
        uint32_t imageCount = 0;
        if (xrEnumerateSwapchainImages(state.eyeSwapchains[eye], 0,
                                       &imageCount, nullptr) != XR_SUCCESS || !imageCount) return false;
        auto& images = state.swapchainImages[eye];
        images.resize(imageCount);
        for (auto& image : images) image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
        if (xrEnumerateSwapchainImages(state.eyeSwapchains[eye], imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())) != XR_SUCCESS) return false;
    }
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = state.graphicsQueueFamily;
    if (vkCreateCommandPool(state.vulkanDevice, &poolInfo, nullptr, &state.copyPool) != VK_SUCCESS)
        return false;
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = state.copyPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(state.vulkanDevice, &allocate, &state.copyCommand) != VK_SUCCESS)
        return false;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(state.vulkanDevice, &fenceInfo, nullptr, &state.copyFence) != VK_SUCCESS)
        return false;
    return true;
}

void destroyPanelTarget() {
    auto& state = bootstrap;
    state.panelReady = false;
    state.panelSwapchainImages.clear();
    if (state.panelSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(state.panelSwapchain);
        state.panelSwapchain = XR_NULL_HANDLE;
    }
    auto& target = state.panelTarget;
    if (target.interopImage) target.interopImage->Release();
    if (target.depth) target.depth->Release();
    if (target.surface) target.surface->Release();
    if (target.resource) target.resource->Release();
    target = Bootstrap::EyeTarget{};
}

void initializePanelPose() {
    auto& state = bootstrap;
    if (state.panelHandAttached || state.panelPoseInitialized || !state.viewsValid) return;
    const auto& left = state.views[0].pose;
    const auto& right = state.views[1].pose;
    XrVector3f center{(left.position.x + right.position.x) * 0.5f,
                      (left.position.y + right.position.y) * 0.5f,
                      (left.position.z + right.position.z) * 0.5f};
    XrVector3f forward = rotateByQuaternion(left.orientation, {0, 0, -1});
    const float horizontalLength = std::sqrt(forward.x * forward.x +
                                             forward.z * forward.z);
    if (horizontalLength > 1.0e-5f) {
        forward.x /= horizontalLength;
        forward.z /= horizontalLength;
    } else {
        forward.x = 0.0f;
        forward.z = -1.0f;
    }
    const float halfYaw = 0.5f * std::atan2(-forward.x, -forward.z);
    state.panelPose.orientation = {0.0f, std::sin(halfYaw), 0.0f,
                                   std::cos(halfYaw)};
    state.panelAnchor = center;
    state.panelForward = {forward.x, 0.0f, forward.z};
    state.panelPose.position = {center.x + state.panelForward.x * state.panelDistanceMeters,
                                center.y,
                                center.z + state.panelForward.z * state.panelDistanceMeters};
    state.panelPoseInitialized = true;
}

XrQuaternionf multiplyQuaternions(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

bool ensurePanelTarget(uint32_t width, uint32_t height) {
    auto& state = bootstrap;
    if (state.panelReady) {
        if (state.panelTarget.width == width && state.panelTarget.height == height) {
            initializePanelPose();
            return true;
        }
        state.interop->FlushRenderingCommands();
        if (!state.interop->WaitForResource(state.panelTarget.resource, 0)) return false;
        destroyPanelTarget();
    }
    if (!state.d3dDevice || !state.interop || !width || !height ||
        state.depthFormat == D3DFMT_UNKNOWN)
        return false;

    auto& target = state.panelTarget;
    target.width = width;
    target.height = height;
    const auto cleanupPanelTarget = [&]() {
        destroyPanelTarget();
        return false;
    };
    D3D9VkExtImageDesc desc{};
    desc.Type = D3DRTYPE_TEXTURE;
    desc.Width = width;
    desc.Height = height;
    desc.Depth = 1;
    desc.MipLevels = 1;
    desc.Usage = D3DUSAGE_RENDERTARGET;
    desc.Format = D3DFMT_A8B8G8R8;
    desc.Pool = D3DPOOL_DEFAULT;
    desc.MultiSample = D3DMULTISAMPLE_NONE;
    desc.ImageUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (FAILED(state.interop->CreateImage(&desc, &target.resource))) return cleanupPanelTarget();
    auto* texture = static_cast<IDirect3DTexture9*>(target.resource);
    if (FAILED(texture->GetSurfaceLevel(0, &target.surface)) ||
        FAILED(state.d3dDevice->CreateDepthStencilSurface(width, height,
            state.depthFormat, D3DMULTISAMPLE_NONE, 0, TRUE, &target.depth, nullptr)) ||
        FAILED(target.resource->QueryInterface(__uuidof(ID3D9VkInteropTexture),
            reinterpret_cast<void**>(&target.interopImage)))) return cleanupPanelTarget();

    state.interop->FlushRenderingCommands();
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    if (!state.interop->WaitForResource(target.resource, 0) ||
        FAILED(target.interopImage->GetVulkanImageInfo(&target.image,
            &target.originalLayout, &imageInfo)) || target.image == VK_NULL_HANDLE ||
        target.originalLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
        imageInfo.extent.width != width || imageInfo.extent.height != height ||
        imageInfo.format != xrColorFormat ||
        !(imageInfo.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return cleanupPanelTarget();

    XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                               XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.format = static_cast<int64_t>(xrColorFormat);
    swapchainInfo.sampleCount = 1;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;
    swapchainInfo.width = width;
    swapchainInfo.height = height;
    XrResult result = xrCreateSwapchain(state.session, &swapchainInfo,
                                        &state.panelSwapchain);
    logResult("xrCreateSwapchain(panel)", result);
    if (result != XR_SUCCESS) return cleanupPanelTarget();
    uint32_t imageCount = 0;
    if (xrEnumerateSwapchainImages(state.panelSwapchain, 0, &imageCount, nullptr) != XR_SUCCESS ||
        !imageCount) return cleanupPanelTarget();
    state.panelSwapchainImages.resize(imageCount);
    for (auto& image : state.panelSwapchainImages)
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    if (xrEnumerateSwapchainImages(state.panelSwapchain, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(state.panelSwapchainImages.data())) != XR_SUCCESS)
        return cleanupPanelTarget();

    initializePanelPose();
    state.panelReady = true;
    return true;
}

} // namespace

static void endFrameLocked(bool rendered);

bool androidXrBootstrapInitialize(void* dxvkHandle) {
    if (bootstrap.hooksInstalled) return true;
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject localActivity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !localActivity || env->GetJavaVM(&bootstrap.vm) != JNI_OK) {
        __android_log_write(ANDROID_LOG_ERROR, "PerimeterXr", "Android XR JNI context unavailable");
        return false;
    }
    bootstrap.activity = env->NewGlobalRef(localActivity);
    env->DeleteLocalRef(localActivity);
    if (!bootstrap.activity) return false;

    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    if (xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader)) != XR_SUCCESS ||
        !initializeLoader) return false;
    XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInfo.applicationVM = bootstrap.vm;
    loaderInfo.applicationContext = bootstrap.activity;
    XrResult result = initializeLoader(
        reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo));
    logResult("xrInitializeLoaderKHR", result);
    if (result != XR_SUCCESS) return false;

    const char* extensions[] = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                                XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = bootstrap.vm;
    androidInfo.applicationActivity = bootstrap.activity;
    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.next = &androidInfo;
    std::snprintf(createInfo.applicationInfo.applicationName,
                  sizeof(createInfo.applicationInfo.applicationName), "Perimeter VR");
    std::snprintf(createInfo.applicationInfo.engineName,
                  sizeof(createInfo.applicationInfo.engineName), "Perimeter");
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = 2;
    createInfo.enabledExtensionNames = extensions;
    result = xrCreateInstance(&createInfo, &bootstrap.instance);
    logResult("xrCreateInstance", result);
    if (result != XR_SUCCESS) return false;
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = xrGetSystem(bootstrap.instance, &systemInfo, &bootstrap.system);
    logResult("xrGetSystem", result);
    if (result != XR_SUCCESS || !createInputActions() ||
        !loadProc("xrCreateVulkanInstanceKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&bootstrap.createInstance)) ||
        !loadProc("xrGetVulkanGraphicsDevice2KHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&bootstrap.getGraphicsDevice)) ||
        !loadProc("xrCreateVulkanDeviceKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&bootstrap.createDevice))) return false;

    PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
    if (!loadProc("xrGetVulkanGraphicsRequirements2KHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements))) return false;
    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    result = getRequirements(bootstrap.instance, bootstrap.system, &requirements);
    logResult("xrGetVulkanGraphicsRequirements2KHR", result);
    if (result != XR_SUCCESS) return false;

    auto setHooks = reinterpret_cast<SetHooksFn>(dlsym(dxvkHandle, "dxvkSetAndroidXrHooks"));
    bootstrap.resetHooks = reinterpret_cast<ResetHooksFn>(
        dlsym(dxvkHandle, "dxvkResetAndroidXrHooks"));
    if (!setHooks || !bootstrap.resetHooks) {
        __android_log_write(ANDROID_LOG_ERROR, "PerimeterXr", "DXVK 2 XR hooks unavailable");
        return false;
    }
    DxvkAndroidXrHooks hooks{};
    hooks.version = DXVK_ANDROID_XR_HOOKS_VERSION;
    hooks.size = sizeof(hooks);
    hooks.userData = &bootstrap;
    hooks.createInstance = &createVkInstance;
    hooks.selectPhysicalDevice = &selectVkDevice;
    hooks.createDevice = &createVkDevice;
    bootstrap.hooksInstalled = setHooks(&hooks);
    if (!bootstrap.hooksInstalled)
        __android_log_write(ANDROID_LOG_ERROR, "PerimeterXr",
                            "DXVK/OpenXR creation hooks could not be installed");
    return bootstrap.hooksInstalled;
}

bool androidXrBootstrapCreateSession(IDirect3DDevice9* device) {
    if (!bootstrap.hooksInstalled) return true;
    if (bootstrap.session != XR_NULL_HANDLE) return true;
    if (!device || !bootstrap.vulkanInstance || !bootstrap.physicalDevice ||
        !bootstrap.vulkanDevice || bootstrap.graphicsQueueFamily == VK_QUEUE_FAMILY_IGNORED) {
        __android_log_write(ANDROID_LOG_ERROR, "PerimeterXr", "Shared DXVK Vulkan device unavailable");
        return false;
    }
    const HRESULT queryResult = device->QueryInterface(__uuidof(ID3D9VkInteropDevice),
        reinterpret_cast<void**>(&bootstrap.interop));
    if (FAILED(queryResult) || !bootstrap.interop) {
        __android_log_print(ANDROID_LOG_ERROR, "PerimeterXr",
            "D3D9 Vulkan interop unavailable: HRESULT=0x%08x", static_cast<unsigned>(queryResult));
        return false;
    }
    bootstrap.d3dDevice = device;
    uint32_t queueIndex = UINT32_MAX;
    uint32_t queueFamily = UINT32_MAX;
    bootstrap.interop->GetSubmissionQueue(&bootstrap.graphicsQueue, &queueIndex, &queueFamily);
    if (!bootstrap.graphicsQueue || queueIndex != 0 ||
        queueFamily != bootstrap.graphicsQueueFamily) {
        __android_log_write(ANDROID_LOG_ERROR, "PerimeterXr", "DXVK/OpenXR queue mismatch");
        androidXrBootstrapDestroySession();
        return false;
    }
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = bootstrap.vulkanInstance;
    binding.physicalDevice = bootstrap.physicalDevice;
    binding.device = bootstrap.vulkanDevice;
    binding.queueFamilyIndex = queueFamily;
    binding.queueIndex = queueIndex;
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = bootstrap.system;
    XrResult result = xrCreateSession(bootstrap.instance, &info, &bootstrap.session);
    logResult("xrCreateSession(shared DXVK device)", result);
    if (result != XR_SUCCESS) {
        androidXrBootstrapDestroySession();
        return false;
    }
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    result = xrCreateReferenceSpace(bootstrap.session, &spaceInfo, &bootstrap.localSpace);
    logResult("xrCreateReferenceSpace(LOCAL)", result);
    if (result != XR_SUCCESS) {
        androidXrBootstrapDestroySession();
        return false;
    }
    if (!attachInputActions()) {
        __android_log_write(ANDROID_LOG_ERROR, "PerimeterXr", "XR input actions unavailable");
        androidXrBootstrapDestroySession();
        return false;
    }
    if (!createEyes(device)) {
        __android_log_write(ANDROID_LOG_ERROR, "PerimeterXr", "XR eye allocation failed");
        androidXrBootstrapDestroySession();
        return false;
    }
    gameThreadOwnsEvents.store(false);
    standbyStop.store(false);
    standbyThread = std::thread(runStandbyFrames);
    return true;
}

bool androidXrBeginFrame(AndroidXrEyeView output[2], AndroidXrInputFrame* input) {
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    auto& state = bootstrap;
    if (input) *input = {};
    state.viewsValid = false;
    if (lifecyclePaused.load() || !state.session || !output) return false;
    standbyStop.store(true);
    if (standbyThread.joinable()) standbyThread.join();
    gameThreadOwnsEvents.store(true);
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    XrResult result;
    while ((result = xrPollEvent(state.instance, &event)) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& change = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (change.session == state.session) {
                if (change.state == XR_SESSION_STATE_READY && !state.running) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    state.running = xrBeginSession(state.session, &begin) == XR_SUCCESS;
                } else if (change.state == XR_SESSION_STATE_STOPPING && state.running) {
                    xrEndSession(state.session);
                    state.running = false;
                }
                state.focused = change.state == XR_SESSION_STATE_FOCUSED;
                if (!state.focused) resetInputEdges();
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            recordReferenceSpaceChange(event);
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            state.running = false;
            state.focused = false;
            resetInputEdges();
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (result != XR_EVENT_UNAVAILABLE || !state.running || state.frameBegun) return false;
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    state.frameState = {XR_TYPE_FRAME_STATE};
    result = xrWaitFrame(state.session, &wait, &state.frameState);
    if (result != XR_SUCCESS) { logResult("xrWaitFrame", result); return false; }
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    {
        QueueLock lock(state.interop);
        result = xrBeginFrame(state.session, &begin);
    }
    if (result != XR_SUCCESS) { logResult("xrBeginFrame", result); return false; }
    state.frameBegun = true;
    state.panelUsesEyeImages = false;
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = state.frameState.predictedDisplayTime;
    locate.space = state.localSpace;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    state.views[0] = {XR_TYPE_VIEW}; state.views[1] = {XR_TYPE_VIEW};
    uint32_t viewCount = 0;
    result = xrLocateViews(state.session, &locate, &viewState, 2, &viewCount, state.views);
    state.viewsValid = result == XR_SUCCESS && viewCount == 2 && state.frameState.shouldRender &&
        (viewState.viewStateFlags & (XR_VIEW_STATE_ORIENTATION_VALID_BIT |
            XR_VIEW_STATE_POSITION_VALID_BIT)) ==
        (XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT);
    if (!state.viewsValid) {
        resetInputEdges();
        endFrameLocked(false);
        return false;
    }
    if (input) snapshotInput(input);
    if (state.recenterTime &&
        state.frameState.predictedDisplayTime >= state.recenterTime) {
        if (input) input->recentered = true;
        state.panelPoseInitialized = false;
        state.recenterTime = 0;
    }
    for (unsigned eye = 0; eye < 2; ++eye) {
        const auto& view = state.views[eye];
        output[eye] = {{view.pose.position.x, view.pose.position.y, view.pose.position.z},
            {view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z,
             view.pose.orientation.w},
            {view.fov.angleLeft, view.fov.angleRight, view.fov.angleDown, view.fov.angleUp},
            state.eyeTargets[eye].width, state.eyeTargets[eye].height};
    }
    return true;
}

bool androidXrIsFocused() {
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    return bootstrap.focused && bootstrap.viewsValid;
}

bool androidXrSessionActive() {
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    return bootstrap.session != XR_NULL_HANDLE;
}

bool androidXrBindEye(cInterfaceRenderDevice* renderer, unsigned eye) {
    if (!bootstrap.frameBegun || eye >= 2 || !renderer) return false;
    auto* d3d = dynamic_cast<cD3DRender*>(renderer);
    if (!d3d) return false;
    d3d->SetFrameRenderTarget(bootstrap.eyeTargets[eye].surface,
                              bootstrap.eyeTargets[eye].depth);
    // The previous panel/frame leaves a UI-sized scissor rectangle behind.
    renderer->SetClipRect(0, 0, static_cast<int>(bootstrap.eyeTargets[eye].width),
                          static_cast<int>(bootstrap.eyeTargets[eye].height));
    return true;
}

void androidXrUnbindEye(cInterfaceRenderDevice* renderer) {
    if (auto* d3d = dynamic_cast<cD3DRender*>(renderer)) d3d->SetFrameRenderTarget(nullptr, nullptr);
}

bool androidXrPrepareUiPanel(unsigned width, unsigned height) {
    return bootstrap.frameBegun && bootstrap.viewsValid &&
           ensurePanelTarget(width, height);
}

bool androidXrBeginUiPanel(cInterfaceRenderDevice* renderer, unsigned width, unsigned height) {
    auto* d3d = dynamic_cast<cD3DRender*>(renderer);
    if (!d3d || !androidXrPrepareUiPanel(width, height)) return false;
    d3d->SetFrameRenderTarget(bootstrap.panelTarget.surface, bootstrap.panelTarget.depth, true);
    D3DVIEWPORT9 viewport{0, 0, width, height, 0.0f, 1.0f};
    if (FAILED(d3d->lpD3DDevice->SetViewport(&viewport))) {
        d3d->SetFrameRenderTarget(nullptr, nullptr);
        return false;
    }
    return true;
}

void androidXrRestoreUiPanelViewport(cInterfaceRenderDevice* renderer,
                                     unsigned width, unsigned height) {
    auto* d3d = dynamic_cast<cD3DRender*>(renderer);
    if (!d3d || !d3d->frameColorTarget || !width || !height) return;
    D3DVIEWPORT9 viewport{0, 0, width, height, 0.0f, 1.0f};
    d3d->lpD3DDevice->SetViewport(&viewport);
    d3d->SetClipRect(0, 0, static_cast<int>(width), static_cast<int>(height));
}

void androidXrEndUiPanel(cInterfaceRenderDevice* renderer) {
    if (auto* d3d = dynamic_cast<cD3DRender*>(renderer)) {
        d3d->SetFrameRenderTarget(nullptr, nullptr);
        D3DVIEWPORT9 viewport{0, 0,
            static_cast<DWORD>(d3d->GetSizeX()), static_cast<DWORD>(d3d->GetSizeY()),
            0.0f, 1.0f};
        d3d->lpD3DDevice->SetViewport(&viewport);
    }
}

bool androidXrDrawUiPanelInEye(cInterfaceRenderDevice* renderer, unsigned eye) {
    auto& state = bootstrap;
    auto* d3d = dynamic_cast<cD3DRender*>(renderer);
    if (!d3d || !state.frameBegun || !state.panelReady || !state.panelVisible ||
        eye >= 2 || d3d->frameColorTarget != state.eyeTargets[eye].surface)
        return false;

    const float halfWidth = state.panelWidthMeters * 0.5f;
    const float halfHeight = halfWidth * static_cast<float>(state.panelTarget.height) /
                             static_cast<float>(state.panelTarget.width);
    const auto& view = state.views[eye];
    const XrQuaternionf inverseView{-view.pose.orientation.x, -view.pose.orientation.y,
                                     -view.pose.orientation.z, view.pose.orientation.w};
    const float left = std::tan(view.fov.angleLeft);
    const float right = std::tan(view.fov.angleRight);
    const float down = std::tan(view.fov.angleDown);
    const float up = std::tan(view.fov.angleUp);
    if (right <= left || up <= down) return false;

    struct Vertex { float x, y, z, rhw, u, v; };
    Vertex vertices[4]{};
    const float localX[4] = {-halfWidth, halfWidth, -halfWidth, halfWidth};
    const float localY[4] = {halfHeight, halfHeight, -halfHeight, -halfHeight};
    for (unsigned corner = 0; corner < 4; ++corner) {
        const XrVector3f rotated = rotateByQuaternion(state.panelPose.orientation,
                                                       {localX[corner], localY[corner], 0});
        const XrVector3f relative{
            state.panelPose.position.x + rotated.x - view.pose.position.x,
            state.panelPose.position.y + rotated.y - view.pose.position.y,
            state.panelPose.position.z + rotated.z - view.pose.position.z};
        const XrVector3f inEye = rotateByQuaternion(inverseView, relative);
        if (inEye.z >= -0.05f) {
            // The fixed panel can be behind the head. Keep the scene frame and
            // omit this eye's panel instead of treating that pose as a failure.
            state.panelUsesEyeImages = true;
            return true;
        }
        const float reciprocalDepth = -1.0f / inEye.z;
        vertices[corner] = {
            (inEye.x * reciprocalDepth - left) / (right - left) * state.eyeTargets[eye].width,
            (up - inEye.y * reciprocalDepth) / (up - down) * state.eyeTargets[eye].height,
            0.0f, reciprocalDepth,
            corner & 1 ? 1.0f : 0.0f, corner & 2 ? 1.0f : 0.0f};
    }

    d3d->FlushActiveDrawBuffer();
    auto* device = d3d->lpD3DDevice;
    struct RenderState { D3DRENDERSTATETYPE type; DWORD value, previous; };
    // The panel texture already contains source-over UI and additive text.
    // ONE / INVSRCALPHA preserves both when drawn into the eye image.
    RenderState renderStates[] = {
        {D3DRS_ZENABLE, D3DZB_FALSE}, {D3DRS_ZWRITEENABLE, FALSE},
        {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_ALPHATESTENABLE, FALSE},
        {D3DRS_ALPHABLENDENABLE, TRUE}, {D3DRS_SRCBLEND, D3DBLEND_ONE},
        {D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA}, {D3DRS_BLENDOP, D3DBLENDOP_ADD},
        {D3DRS_SEPARATEALPHABLENDENABLE, FALSE}, {D3DRS_LIGHTING, FALSE},
        {D3DRS_FOGENABLE, FALSE}, {D3DRS_STENCILENABLE, FALSE},
        {D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                  D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA},
        // DXVK renders through a UNORM view of the sRGB storage image. Blend
        // the menu in display-encoded values before the eye image is copied.
        {D3DRS_SRGBWRITEENABLE, FALSE}};
    for (auto& setting : renderStates)
        if (FAILED(device->GetRenderState(setting.type, &setting.previous))) return false;

    struct StageState { unsigned stage; D3DTEXTURESTAGESTATETYPE type; DWORD value, previous; };
    StageState stageStates[] = {
        {0, D3DTSS_COLOROP, D3DTOP_SELECTARG1},
        {0, D3DTSS_COLORARG1, D3DTA_TEXTURE},
        {0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1},
        {0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE},
        {0, D3DTSS_TEXCOORDINDEX, 0},
        {0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE},
        {1, D3DTSS_COLOROP, D3DTOP_DISABLE}};
    for (auto& setting : stageStates)
        if (FAILED(device->GetTextureStageState(setting.stage, setting.type,
                                                 &setting.previous))) return false;

    struct SamplerState { D3DSAMPLERSTATETYPE type; DWORD value, previous; };
    SamplerState samplerStates[] = {
        {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
        {D3DSAMP_MAGFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MINFILTER, D3DTEXF_LINEAR},
        {D3DSAMP_MIPFILTER, D3DTEXF_NONE}, {D3DSAMP_SRGBTEXTURE, FALSE}};
    for (auto& setting : samplerStates)
        if (FAILED(device->GetSamplerState(0, setting.type, &setting.previous))) return false;
    IDirect3DVertexShader9* vertexShader = nullptr;
    IDirect3DPixelShader9* pixelShader = nullptr;
    if (FAILED(device->GetVertexShader(&vertexShader))) return false;
    if (FAILED(device->GetPixelShader(&pixelShader))) {
        if (vertexShader) vertexShader->Release();
        return false;
    }
    D3DVIEWPORT9 previousViewport{};
    if (FAILED(device->GetViewport(&previousViewport))) {
        if (vertexShader) vertexShader->Release();
        if (pixelShader) pixelShader->Release();
        return false;
    }
    IDirect3DBaseTexture9* previousTexture = d3d->GetTextureD3D(0);

    for (const auto& setting : renderStates)
        d3d->SetRenderState(setting.type, setting.value);
    for (const auto& setting : stageStates)
        d3d->SetTextureStageState(setting.stage, setting.type, setting.value);
    for (const auto& setting : samplerStates)
        d3d->SetSamplerState(0, setting.type, setting.value);

    HRESULT result = device->SetVertexShader(nullptr);
    if (SUCCEEDED(result)) result = device->SetPixelShader(nullptr);
    D3DVIEWPORT9 eyeViewport{0, 0, state.eyeTargets[eye].width,
                             state.eyeTargets[eye].height, 0.0f, 1.0f};
    if (SUCCEEDED(result)) result = device->SetViewport(&eyeViewport);
    if (SUCCEEDED(result)) result = device->SetTexture(0,
        static_cast<IDirect3DTexture9*>(state.panelTarget.resource));
    if (SUCCEEDED(result)) result = device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    if (SUCCEEDED(result))
        result = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));

    device->SetTexture(0, previousTexture);
    device->SetVertexShader(vertexShader);
    device->SetPixelShader(pixelShader);
    device->SetViewport(&previousViewport);
    if (vertexShader) vertexShader->Release();
    if (pixelShader) pixelShader->Release();
    for (const auto& setting : samplerStates)
        d3d->SetSamplerState(0, setting.type, setting.previous);
    for (const auto& setting : stageStates)
        d3d->SetTextureStageState(setting.stage, setting.type, setting.previous);
    for (const auto& setting : renderStates)
        d3d->SetRenderState(setting.type, setting.previous);

    if (FAILED(result)) {
        __android_log_print(ANDROID_LOG_ERROR, "PerimeterXr",
                            "Draw menu panel in eye %u failed: 0x%08x", eye,
                            static_cast<unsigned>(result));
        return false;
    }
    state.panelUsesEyeImages = true;
    return true;
}

bool androidXrHitUiPanel(const AndroidXrHandState& hand, unsigned width, unsigned height,
                         float* pixelX, float* pixelY, float* distanceMeters) {
    const auto& state = bootstrap;
    if (!state.panelReady || !state.panelVisible || !hand.aimValid ||
        !width || !height || !pixelX || !pixelY) return false;
    const XrVector3f origin{hand.aimPosition[0], hand.aimPosition[1], hand.aimPosition[2]};
    const XrQuaternionf aimOrientation{hand.aimOrientation[0], hand.aimOrientation[1],
                                       hand.aimOrientation[2], hand.aimOrientation[3]};
    const XrVector3f direction = rotateByQuaternion(aimOrientation, {0, 0, -1});
    const XrVector3f normal = rotateByQuaternion(state.panelPose.orientation, {0, 0, 1});
    const XrVector3f offset{state.panelPose.position.x - origin.x,
                            state.panelPose.position.y - origin.y,
                            state.panelPose.position.z - origin.z};
    const float denominator = direction.x * normal.x + direction.y * normal.y +
                              direction.z * normal.z;
    if (denominator >= -1.0e-5f) return false;
    const float distance = (offset.x * normal.x + offset.y * normal.y +
                            offset.z * normal.z) / denominator;
    if (distance <= 0.0f) return false;
    const XrVector3f relative{origin.x + direction.x * distance - state.panelPose.position.x,
                              origin.y + direction.y * distance - state.panelPose.position.y,
                              origin.z + direction.z * distance - state.panelPose.position.z};
    const XrQuaternionf inverse{-state.panelPose.orientation.x, -state.panelPose.orientation.y,
                                -state.panelPose.orientation.z, state.panelPose.orientation.w};
    const XrVector3f local = rotateByQuaternion(inverse, relative);
    const float panelHeight = state.panelWidthMeters *
        static_cast<float>(state.panelTarget.height) / state.panelTarget.width;
    if (std::abs(local.x) > state.panelWidthMeters * 0.5f ||
        std::abs(local.y) > panelHeight * 0.5f) return false;
    *pixelX = (local.x / state.panelWidthMeters + 0.5f) * width;
    *pixelY = (0.5f - local.y / panelHeight) * height;
    if (distanceMeters) *distanceMeters = distance;
    return true;
}

void androidXrSetUiPanelFixed() {
    androidXrSetUiPanelFixed(1.4f, 1.2f);
}

void androidXrSetUiPanelFixed(float distanceMeters, float widthMeters) {
    auto& state = bootstrap;
    if (state.panelHandAttached) {
        state.panelHandAttached = false;
        state.panelPoseInitialized = false;
    }
    state.panelDistanceMeters = std::max(distanceMeters, 0.05f);
    state.panelWidthMeters = std::max(widthMeters, 0.05f);
    initializePanelPose();
    if (state.panelPoseInitialized) {
        state.panelPose.position = {
            state.panelAnchor.x + state.panelForward.x * state.panelDistanceMeters,
            state.panelAnchor.y,
            state.panelAnchor.z + state.panelForward.z * state.panelDistanceMeters};
    }
}

void androidXrResetUiPanelPose() {
    if (!bootstrap.panelHandAttached)
        bootstrap.panelPoseInitialized = false;
}

bool androidXrAttachUiPanelToHand(const AndroidXrHandState& hand, unsigned handIndex) {
    auto& state = bootstrap;
    state.panelHandAttached = true;
    state.panelWidthMeters = 0.34f;
    if (handIndex > 1 || !hand.gripValid || !hand.aimValid) return false;
    const XrQuaternionf orientation{hand.gripOrientation[0], hand.gripOrientation[1],
                                    hand.gripOrientation[2], hand.gripOrientation[3]};
    // Keep the tuned offset in grip space so it pivots around the hand. Grip -Y
    // produced the desired front-back movement in the headset, but the OpenXR
    // grip pose does not define that axis as controller-forward.
    const float side = handIndex == 0 ? 1.0f : -1.0f;
    const XrVector3f offset = rotateByQuaternion(orientation,
        {side * 0.09f, -0.11f, -0.08f});
    const XrVector3f basePosition{hand.gripPosition[0] + offset.x,
                                  hand.gripPosition[1] + offset.y,
                                  hand.gripPosition[2] + offset.z};
    const XrQuaternionf aimOrientation{hand.aimOrientation[0], hand.aimOrientation[1],
                                       hand.aimOrientation[2], hand.aimOrientation[3]};
    // The pointer aims along aim -Z; aim +Y is its actual up direction.
    const XrVector3f aimUp = rotateByQuaternion(aimOrientation, {0.0f, 1.0f, 0.0f});
    const XrVector3f fromPointer{basePosition.x - hand.aimPosition[0],
                                 basePosition.y - hand.aimPosition[1],
                                 basePosition.z - hand.aimPosition[2]};
    const float aimHeight = fromPointer.x * aimUp.x + fromPointer.y * aimUp.y +
                            fromPointer.z * aimUp.z;
    const float heightCorrection = 0.10f - aimHeight;
    // Grip +Z points down with a naturally held Touch controller. Rotate the
    // tablet around the grip's X axis so its front faces the player.
    constexpr float halfTiltRadians = -55.0f * (3.14159265f / 180.0f);
    state.panelPose.orientation = multiplyQuaternions(orientation,
        {std::sin(halfTiltRadians), 0.0f, 0.0f,
         std::cos(halfTiltRadians)});
    state.panelPose.position = {basePosition.x + heightCorrection * aimUp.x,
                                basePosition.y + heightCorrection * aimUp.y,
                                basePosition.z + heightCorrection * aimUp.z};
    return true;
}

void androidXrSetUiPanelVisible(bool visible) {
    bootstrap.panelVisible = visible;
}

static void endFrameLocked(bool rendered) {
    auto& state = bootstrap;
    if (!state.frameBegun) return;
    bool copied = false;
    uint32_t indexes[2]{};
    bool acquired[2]{};
    bool waited[2]{};
    if (rendered && state.viewsValid) {
        // Complete D3D9 drawing before accessing its VkImages outside DXVK.
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (auto& target : state.eyeTargets)
            state.interop->TransitionTextureLayout(target.interopImage, &range,
                target.originalLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        state.interop->FlushRenderingCommands();
        copied = true;
        for (auto& target : state.eyeTargets)
            copied &= state.interop->WaitForResource(target.resource, 0);
        for (unsigned eye = 0; eye < 2 && copied; ++eye) {
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrResult result;
            { QueueLock lock(state.interop);
              result = xrAcquireSwapchainImage(state.eyeSwapchains[eye], &acquire, &indexes[eye]); }
            acquired[eye] = XR_SUCCEEDED(result);
            copied = result == XR_SUCCESS && indexes[eye] < state.swapchainImages[eye].size();
            if (!copied) { logResult("xrAcquireSwapchainImage", result); break; }
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            result = xrWaitSwapchainImage(state.eyeSwapchains[eye], &wait);
            waited[eye] = result == XR_SUCCESS;
            copied = result == XR_SUCCESS;
            if (!copied) logResult("xrWaitSwapchainImage", result);
        }
        if (copied) {
            copied = vkResetCommandBuffer(state.copyCommand, 0) == VK_SUCCESS;
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            copied &= vkBeginCommandBuffer(state.copyCommand, &begin) == VK_SUCCESS;
            if (copied) for (unsigned eye = 0; eye < 2; ++eye) {
                VkImageMemoryBarrier barriers[2]{};
                for (auto& barrier : barriers) {
                    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.subresourceRange = range;
                }
                barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barriers[0].image = state.eyeTargets[eye].image;
                barriers[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barriers[1].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barriers[1].image = state.swapchainImages[eye][indexes[eye]].image;
                vkCmdPipelineBarrier(state.copyCommand, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
                VkImageCopy copy{};
                copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.extent = {state.eyeTargets[eye].width, state.eyeTargets[eye].height, 1};
                vkCmdCopyImage(state.copyCommand, barriers[0].image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, barriers[1].image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barriers[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
                barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barriers[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                vkCmdPipelineBarrier(state.copyCommand, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                    1, &barriers[1]);
            }
            copied &= vkEndCommandBuffer(state.copyCommand) == VK_SUCCESS;
            if (copied) {
                copied = vkResetFences(state.vulkanDevice, 1, &state.copyFence) == VK_SUCCESS;
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.commandBufferCount = 1;
                submit.pCommandBuffers = &state.copyCommand;
                if (copied) { QueueLock lock(state.interop);
                    copied = vkQueueSubmit(state.graphicsQueue, 1, &submit, state.copyFence) == VK_SUCCESS; }
                // Do not restore or release either image while this copy is in flight.
                if (copied) copied = vkWaitForFences(state.vulkanDevice, 1,
                    &state.copyFence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
            }
        }
        // Return the D3D9 targets to the layout DXVK expects on the next pass.
        for (auto& target : state.eyeTargets)
            state.interop->TransitionTextureLayout(target.interopImage, &range,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.originalLayout);
        state.interop->FlushRenderingCommands();
        for (auto& target : state.eyeTargets)
            copied &= state.interop->WaitForResource(target.resource, 0);
    }
    for (unsigned eye = 0; eye < 2; ++eye) if (acquired[eye]) {
        if (!waited[eye]) {
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = 1000000000;
            waited[eye] = xrWaitSwapchainImage(state.eyeSwapchains[eye], &wait) == XR_SUCCESS;
        }
        if (waited[eye]) {
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            QueueLock lock(state.interop);
            if (xrReleaseSwapchainImage(state.eyeSwapchains[eye], &release) != XR_SUCCESS)
                copied = false;
        }
    }
    bool panelCopied = false;
    if (copied && rendered && state.viewsValid && state.panelReady && state.panelVisible &&
        !state.panelUsesEyeImages) {
        auto& target = state.panelTarget;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        state.interop->TransitionTextureLayout(target.interopImage, &range,
            target.originalLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        state.interop->FlushRenderingCommands();
        panelCopied = state.interop->WaitForResource(target.resource, 0);
        uint32_t index = 0;
        bool acquiredPanel = false;
        bool waitedPanel = false;
        if (panelCopied) {
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrResult acquireResult;
            { QueueLock lock(state.interop);
              acquireResult = xrAcquireSwapchainImage(state.panelSwapchain, &acquire, &index); }
            acquiredPanel = XR_SUCCEEDED(acquireResult);
            panelCopied = acquireResult == XR_SUCCESS && index < state.panelSwapchainImages.size();
            if (!panelCopied) logResult("xrAcquireSwapchainImage(panel)", acquireResult);
        }
        if (panelCopied) {
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            const XrResult waitResult = xrWaitSwapchainImage(state.panelSwapchain, &wait);
            waitedPanel = waitResult == XR_SUCCESS;
            panelCopied = waitedPanel;
            if (!panelCopied) logResult("xrWaitSwapchainImage(panel)", waitResult);
        }
        if (panelCopied) {
            panelCopied = vkResetCommandBuffer(state.copyCommand, 0) == VK_SUCCESS;
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            panelCopied &= vkBeginCommandBuffer(state.copyCommand, &begin) == VK_SUCCESS;
            if (panelCopied) {
                VkImageMemoryBarrier barriers[2]{};
                for (auto& barrier : barriers) {
                    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.subresourceRange = range;
                }
                barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barriers[0].image = target.image;
                barriers[1].srcAccessMask = 0;
                barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barriers[1].image = state.panelSwapchainImages[index].image;
                vkCmdPipelineBarrier(state.copyCommand, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
                VkImageCopy copy{};
                copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.extent = {target.width, target.height, 1};
                vkCmdCopyImage(state.copyCommand, target.image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, barriers[1].image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                vkCmdPipelineBarrier(state.copyCommand, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                    1, &barriers[1]);
            }
            panelCopied &= vkEndCommandBuffer(state.copyCommand) == VK_SUCCESS;
            if (panelCopied) panelCopied = vkResetFences(state.vulkanDevice, 1,
                                                          &state.copyFence) == VK_SUCCESS;
            if (panelCopied) {
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.commandBufferCount = 1;
                submit.pCommandBuffers = &state.copyCommand;
                { QueueLock lock(state.interop);
                  panelCopied = vkQueueSubmit(state.graphicsQueue, 1, &submit,
                                               state.copyFence) == VK_SUCCESS; }
                if (panelCopied) panelCopied = vkWaitForFences(state.vulkanDevice, 1,
                    &state.copyFence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
            }
        }
        state.interop->TransitionTextureLayout(target.interopImage, &range,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.originalLayout);
        state.interop->FlushRenderingCommands();
        panelCopied &= state.interop->WaitForResource(target.resource, 0);
        if (acquiredPanel) {
            if (!waitedPanel) {
                XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wait.timeout = 1000000000;
                waitedPanel = xrWaitSwapchainImage(state.panelSwapchain, &wait) == XR_SUCCESS;
            }
            if (waitedPanel) {
                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                QueueLock lock(state.interop);
                if (xrReleaseSwapchainImage(state.panelSwapchain, &release) != XR_SUCCESS)
                    panelCopied = false;
            } else {
                panelCopied = false;
            }
        }
    }
    XrCompositionLayerProjectionView projectionViews[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for (unsigned eye = 0; eye < 2; ++eye) {
        projectionViews[eye].pose = state.views[eye].pose;
        projectionViews[eye].fov = state.views[eye].fov;
        projectionViews[eye].subImage.swapchain = state.eyeSwapchains[eye];
        projectionViews[eye].subImage.imageRect.extent = {
            static_cast<int32_t>(state.eyeTargets[eye].width),
            static_cast<int32_t>(state.eyeTargets[eye].height)};
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space = state.localSpace;
    projection.viewCount = 2;
    projection.views = projectionViews;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = state.localSpace;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.subImage.swapchain = state.panelSwapchain;
    quad.subImage.imageRect.extent = {static_cast<int32_t>(state.panelTarget.width),
                                      static_cast<int32_t>(state.panelTarget.height)};
    quad.pose = state.panelPose;
    quad.size = {state.panelWidthMeters,
        state.panelWidthMeters * static_cast<float>(state.panelTarget.height) /
            static_cast<float>(state.panelTarget.width)};
    const XrCompositionLayerBaseHeader* layers[2] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = state.frameState.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = copied ? (panelCopied ? 2 : 1) : 0;
    end.layers = copied ? layers : nullptr;
    XrResult result;
    { QueueLock lock(state.interop); result = xrEndFrame(state.session, &end); }
    if (result != XR_SUCCESS) logResult("xrEndFrame", result);
    state.frameBegun = false;
}

void androidXrEndFrame(bool rendered) {
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    endFrameLocked(rendered);
}

void androidXrBootstrapDestroySession() {
    std::lock_guard<std::mutex> threadLock(pausedThreadMutex);
    lifecyclePaused.store(false);
    if (pausedEventThread.joinable()) pausedEventThread.join();
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    gameThreadOwnsEvents.store(false);
    androidXrClearListenerView();
    standbyStop.store(true);
    if (standbyThread.joinable()) standbyThread.join();
    if (bootstrap.frameBegun) endFrameLocked(false);
    bootstrap.focused = false;
    resetInputEdges();
    if (bootstrap.graphicsQueue && bootstrap.interop) {
        QueueLock lock(bootstrap.interop);
        vkQueueWaitIdle(bootstrap.graphicsQueue);
    }
    if (bootstrap.copyFence) vkDestroyFence(bootstrap.vulkanDevice, bootstrap.copyFence, nullptr);
    bootstrap.copyFence = VK_NULL_HANDLE;
    if (bootstrap.copyPool) vkDestroyCommandPool(bootstrap.vulkanDevice, bootstrap.copyPool, nullptr);
    bootstrap.copyPool = VK_NULL_HANDLE;
    bootstrap.copyCommand = VK_NULL_HANDLE;
    for (auto& target : bootstrap.eyeTargets) {
        if (target.interopImage) target.interopImage->Release();
        if (target.depth) target.depth->Release();
        if (target.surface) target.surface->Release();
        if (target.resource) target.resource->Release();
        target = Bootstrap::EyeTarget{};
    }
    destroyPanelTarget();
    bootstrap.panelPoseInitialized = false;
    bootstrap.panelHandAttached = false;
    bootstrap.panelWidthMeters = 1.2f;
    bootstrap.panelDistanceMeters = 1.4f;
    bootstrap.recenterTime = 0;
    bootstrap.d3dDevice = nullptr;
    bootstrap.depthFormat = D3DFMT_UNKNOWN;
    for (unsigned hand = 0; hand < 2; ++hand) {
        if (bootstrap.aimSpaces[hand]) xrDestroySpace(bootstrap.aimSpaces[hand]);
        bootstrap.aimSpaces[hand] = XR_NULL_HANDLE;
        if (bootstrap.gripSpaces[hand]) xrDestroySpace(bootstrap.gripSpaces[hand]);
        bootstrap.gripSpaces[hand] = XR_NULL_HANDLE;
    }
    for (uint32_t eye = 0; eye < 2; ++eye) {
        bootstrap.swapchainImages[eye].clear();
        if (bootstrap.eyeSwapchains[eye] != XR_NULL_HANDLE)
            xrDestroySwapchain(bootstrap.eyeSwapchains[eye]);
        bootstrap.eyeSwapchains[eye] = XR_NULL_HANDLE;
    }
    if (bootstrap.localSpace != XR_NULL_HANDLE) xrDestroySpace(bootstrap.localSpace);
    bootstrap.localSpace = XR_NULL_HANDLE;
    if (bootstrap.session != XR_NULL_HANDLE) xrDestroySession(bootstrap.session);
    bootstrap.session = XR_NULL_HANDLE;
    if (bootstrap.interop) bootstrap.interop->Release();
    bootstrap.interop = nullptr;
    bootstrap.graphicsQueue = VK_NULL_HANDLE;
    bootstrap.running = false;
}

extern "C" JNIEXPORT void JNICALL
Java_com_queststoredb_perimeter_QuestGameActivity_nativeSetXrPaused(
    JNIEnv*, jobject, jboolean paused) {
    std::lock_guard<std::mutex> threadLock(pausedThreadMutex);
    if (paused) {
        lifecyclePaused.store(true);
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        // The startup standby thread already owns event polling until the
        // first rendered frame takes ownership on the game thread.
        if (gameThreadOwnsEvents.load() && !pausedEventThread.joinable())
            pausedEventThread = std::thread(runPausedEvents);
    } else {
        lifecyclePaused.store(false);
        if (pausedEventThread.joinable()) pausedEventThread.join();
    }
}

void androidXrBootstrapDestroy() {
    androidXrBootstrapDestroySession();
    if (bootstrap.actionSet) xrDestroyActionSet(bootstrap.actionSet);
    if (bootstrap.hooksInstalled && bootstrap.resetHooks) bootstrap.resetHooks();
    if (bootstrap.instance != XR_NULL_HANDLE) xrDestroyInstance(bootstrap.instance);
    if (bootstrap.activity) {
        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (env) env->DeleteGlobalRef(bootstrap.activity);
    }
    bootstrap = Bootstrap{};
}

#endif
