#include "AndroidDxvkLoader.h"
#if defined(ANDROID_XR)
#include "AndroidXrBootstrap.h"
#endif

#if defined(__ANDROID__)

#include <android/log.h>
#include <cstdarg>
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>
#include <string>

extern const char* check_command_line(const char* switch_str);

namespace {

using Direct3DCreate9Fn = IDirect3D9* (*)(UINT);
using DxvkSetSdl2WindowFn = void (*)(SDL_Window*);

std::once_flag loadOnce;
void* dxvkHandle = nullptr;
Direct3DCreate9Fn direct3DCreate9 = nullptr;
DxvkSetSdl2WindowFn dxvkSetSdl2Window = nullptr;

void logError(const char* format, ...) {
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_ERROR, "PerimeterDxvkLoader", format, args);
    va_end(args);
}

void configureAndroidMemoryLogging() {
    const char* requested = check_command_line("android_memory_monitor");
    const bool enabled = requested && requested[0] == '1' && requested[1] == '\0';
    setenv("PERIMETER_ANDROID_MEMORY_LOG", enabled ? "1" : "0", 1);
}

void loadSelectedDxvk() {
    configureAndroidMemoryLogging();

    const char* requestedVersion = check_command_line("android_dxvk_version");
    const bool validVersion = requestedVersion && requestedVersion[1] == '\0' &&
            (requestedVersion[0] == '1' || requestedVersion[0] == '2');
    if (!validVersion) {
        logError("No valid android_dxvk_version was selected");
        return;
    }
    const char* version = requestedVersion;
    const std::string libraryName = "libdxvk_d3d9_v" + std::string(version) + ".so";

    dxvkHandle = dlopen(libraryName.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!dxvkHandle) {
        const char* loadError = dlerror();
        logError("Unable to load selected DXVK %s: %s", version,
            loadError ? loadError : "unknown error");
        return;
    }

    dlerror();
    direct3DCreate9 = reinterpret_cast<Direct3DCreate9Fn>(
        dlsym(dxvkHandle, "Direct3DCreate9"));
    const char* symbolError = dlerror();
    if (symbolError || !direct3DCreate9) {
        logError("Selected DXVK %s does not export Direct3DCreate9: %s",
            version, symbolError ? symbolError : "unknown error");
        return;
    }

    // This hook is optional for compatibility with builds that use another
    // Android WSI setup. The selected DSO remains pinned for the process.
    dlerror();
    dxvkSetSdl2Window = reinterpret_cast<DxvkSetSdl2WindowFn>(
        dlsym(dxvkHandle, "dxvkSetSdl2Window"));
    if (dlerror() != nullptr)
        dxvkSetSdl2Window = nullptr;

    __android_log_print(ANDROID_LOG_INFO, "PerimeterDxvkLoader",
        "Loaded DXVK %s from %s", version, libraryName.c_str());
}

void ensureLoaded() {
    std::call_once(loadOnce, loadSelectedDxvk);
}

}

void androidDxvkSetSdl2Window(SDL_Window* window) {
    ensureLoaded();
    if (dxvkSetSdl2Window)
        dxvkSetSdl2Window(window);
}

IDirect3D9* androidDxvkCreateD3D9(UINT sdkVersion) {
    ensureLoaded();
#if defined(ANDROID_XR)
    const char* version = check_command_line("android_dxvk_version");
    if (!dxvkHandle || !version || version[0] != '2' || version[1] != '\0' ||
        !androidXrBootstrapInitialize(dxvkHandle)) {
        logError("Quest VR requires DXVK 2 and a working OpenXR bootstrap");
        androidXrBootstrapDestroy();
        return nullptr;
    }
#endif
    return direct3DCreate9 ? direct3DCreate9(sdkVersion) : nullptr;
}

#endif
