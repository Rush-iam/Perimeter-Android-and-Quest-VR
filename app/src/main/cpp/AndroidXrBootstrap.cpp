#include "AndroidXrBootstrap.h"

#if defined(ANDROID_XR)

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
#include <cstdio>
#include <thread>
#include <vector>

namespace {

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
    XrSwapchain eyeSwapchains[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::vector<XrSwapchainImageVulkan2KHR> swapchainImages[2];
    EyeTarget eyeTargets[2];
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
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t frameCount = 0;
};

Bootstrap bootstrap;
std::atomic<bool> standbyStop{false};
std::thread standbyThread;
void logResult(const char* operation, XrResult result);

struct QueueLock {
    explicit QueueLock(ID3D9VkInteropDevice* device) : interop(device) {
        interop->LockSubmissionQueue();
    }
    ~QueueLock() { interop->ReleaseSubmissionQueue(); }
    ID3D9VkInteropDevice* interop;
};

void runStandbyFrames() {
    auto& state = bootstrap;
    uint32_t frames = 0;
    while (!standbyStop.load()) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result;
        while ((result = xrPollEvent(state.instance, &event)) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto& change = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                if (change.session == state.session) {
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
        if (++frames % 120 == 0)
            __android_log_print(ANDROID_LOG_INFO, "PerimeterXr", "standby frames=%u", frames);
    }
    __android_log_print(ANDROID_LOG_INFO, "PerimeterXr", "standby stopped after %u frames", frames);
}

void logResult(const char* operation, XrResult result) {
    __android_log_print(result == XR_SUCCESS ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
        "PerimeterXr", "%s: XrResult=%d", operation, static_cast<int>(result));
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
    VkFormat eyeFormat = VK_FORMAT_UNDEFINED;
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
            std::find(formats.begin(), formats.end(), static_cast<int64_t>(imageInfo.format)) != formats.end();
        if (!success) break;
        if (eyeFormat == VK_FORMAT_UNDEFINED) eyeFormat = imageInfo.format;
        if (imageInfo.format != eyeFormat) return false;
    }
    if (!success) return false;

    XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                               XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.format = static_cast<int64_t>(eyeFormat);
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
        __android_log_print(ANDROID_LOG_INFO, "PerimeterXr",
            "eye %u: %ux%u format=%d swapchainImages=%u", eye,
            state.eyeTargets[eye].width, state.eyeTargets[eye].height,
            static_cast<int>(eyeFormat), imageCount);
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

} // namespace

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
    if (result != XR_SUCCESS ||
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
    __android_log_print(bootstrap.hooksInstalled ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
        "PerimeterXr", "DXVK/OpenXR creation hooks installed=%d", bootstrap.hooksInstalled);
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
    if (!createEyes(device)) {
        __android_log_write(ANDROID_LOG_ERROR, "PerimeterXr", "XR eye allocation failed");
        androidXrBootstrapDestroySession();
        return false;
    }
    standbyStop.store(false);
    standbyThread = std::thread(runStandbyFrames);
    return true;
}

bool androidXrBeginFrame(AndroidXrEyeView output[2]) {
    auto& state = bootstrap;
    if (!state.session || !output) return false;
    standbyStop.store(true);
    if (standbyThread.joinable()) standbyThread.join();
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    XrResult result;
    while ((result = xrPollEvent(state.instance, &event)) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& change = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (change.session == state.session) {
                __android_log_print(ANDROID_LOG_INFO, "PerimeterXr", "session state=%d", change.state);
                if (change.state == XR_SESSION_STATE_READY && !state.running) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    state.running = xrBeginSession(state.session, &begin) == XR_SUCCESS;
                } else if (change.state == XR_SESSION_STATE_STOPPING && state.running) {
                    xrEndSession(state.session);
                    state.running = false;
                }
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            state.running = false;
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
        androidXrEndFrame(false);
        return false;
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

bool androidXrBindEye(cInterfaceRenderDevice* renderer, unsigned eye) {
    if (!bootstrap.frameBegun || eye >= 2 || !renderer) return false;
    auto* d3d = dynamic_cast<cD3DRender*>(renderer);
    if (!d3d) return false;
    d3d->SetFrameRenderTarget(bootstrap.eyeTargets[eye].surface,
                              bootstrap.eyeTargets[eye].depth);
    return true;
}

void androidXrUnbindEye(cInterfaceRenderDevice* renderer) {
    if (auto* d3d = dynamic_cast<cD3DRender*>(renderer)) d3d->SetFrameRenderTarget(nullptr, nullptr);
}

void androidXrEndFrame(bool rendered) {
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
    const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = state.frameState.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = copied ? 1 : 0;
    end.layers = copied ? &layer : nullptr;
    XrResult result;
    { QueueLock lock(state.interop); result = xrEndFrame(state.session, &end); }
    if (result != XR_SUCCESS) logResult("xrEndFrame", result);
    state.frameBegun = false;
    if (++state.frameCount % 120 == 0)
        __android_log_print(ANDROID_LOG_INFO, "PerimeterXr", "frames=%u copied=%d",
            state.frameCount, copied);
}

void androidXrBootstrapDestroySession() {
    standbyStop.store(true);
    if (standbyThread.joinable()) standbyThread.join();
    if (bootstrap.frameBegun) androidXrEndFrame(false);
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
    bootstrap.frameCount = 0;
}

void androidXrBootstrapDestroy() {
    androidXrBootstrapDestroySession();
    if (bootstrap.hooksInstalled && bootstrap.resetHooks) bootstrap.resetHooks();
    if (bootstrap.instance != XR_NULL_HANDLE) xrDestroyInstance(bootstrap.instance);
    if (bootstrap.activity) {
        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (env) env->DeleteGlobalRef(bootstrap.activity);
    }
    bootstrap = Bootstrap{};
}

#endif
