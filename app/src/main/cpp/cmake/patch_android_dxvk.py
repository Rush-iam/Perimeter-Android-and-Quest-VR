"""Apply Android adaptations to a disposable copy of pinned DXVK source.

The caller supplies a clean staging tree. Fail on source drift rather than
silently applying a partial patch. The Vulkan loader already supports
libvulkan.so; SDL still owns Android surface creation.
"""
import pathlib
import sys

if len(sys.argv) != 3:
    raise SystemExit("Usage: patch_android_dxvk.py <source-dir> <native|v2>")

root = pathlib.Path(sys.argv[1])
variant = sys.argv[2]
if not root.is_dir():
    raise SystemExit(f"DXVK source directory does not exist: {root}")
if variant not in ("native", "v2", "v2-xr"):
    raise SystemExit(f"Unknown DXVK variant: {variant!r}")


def replace(relative_path, old, new):
    """Apply one exact patch, failing on missing or ambiguous anchors."""
    path = root / relative_path
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(
            f"Expected one patch anchor in {relative_path}, found {count}: {old!r}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")


def replace_all(relative_path, old, new, expected_count):
    """Replace an exact number of repeated anchors in pinned source."""
    path = root / relative_path
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected_count:
        raise RuntimeError(
            f"Expected {expected_count} pristine patch anchors in {relative_path}, "
            f"found {count}: {old!r}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")


def write_new(relative_path, content):
    """Create an Android-only source file, requiring a clean source tree."""
    path = root / relative_path
    if path.exists():
        raise RuntimeError(f"Android-generated file already exists: {relative_path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def rename_d3d9_library(generation):
    """Give the Android D3D9 DSO a generation-specific filename and SONAME."""
    if generation == 1:
        old = "shared_library(so_prefix+'d3d9'+dll_ext,"
        new = "shared_library(so_prefix+'d3d9_v1'+dll_ext,"
    elif generation == 2:
        old = "shared_library(dxvk_name_prefix+'d3d9',"
        new = "shared_library(dxvk_name_prefix+'d3d9_v2',"
    else:
        raise ValueError(f"Unsupported DXVK generation: {generation}")

    replace("src/d3d9/meson.build", old, new)


def patch_v2():
    """Patch Android SDL loading, build integration, and D3D9 interval support."""
    rename_d3d9_library(2)
    # Android packages SDL2 as libSDL2.so rather than a desktop SONAME.
    replace("src/wsi/sdl2/wsi_platform_sdl2.cpp",
            '#elif defined(__APPLE__)',
            '#elif defined(__ANDROID__)\n        "libSDL2.so"\n#elif defined(__APPLE__)')
    # DXVK's SDL2 WSI dynamically resolves symbols, so only the matching app
    # headers are needed at compile time.
    replace("meson_options.txt", "option('enable_dxgi',",
            "option('android_sdl2_include', type: 'string', value: '', description: 'Android SDL2 headers')\noption('enable_dxgi',")
    replace("meson.build",
            "  lib_sdl2 = dependency('sdl2', required: get_option('native_sdl2'))",
            """  if platform == 'android'
    assert(get_option('android_sdl2_include') != '', 'Android SDL2 headers required')
    lib_sdl2 = declare_dependency(compile_args: ['-I' + get_option('android_sdl2_include')])
  else
    lib_sdl2 = dependency('sdl2', required: get_option('native_sdl2'))
  endif""")
    # The resource and shader generators execute on the host during a cross build.
    replace("meson.build", "find_program('touch')",
            "find_program('touch', native: true)")
    replace("meson.build", "find_program('glslang', 'glslangValidator')",
            "find_program('glslang', 'glslangValidator', native: true)")
    # Android uses the app's shared libc++ and unversioned ELF SONAMEs.
    replace("meson.build", """  link_args += [
    '-static-libgcc',
    '-static-libstdc++',
  ]""", """  if platform == 'android'
    dxvk_so_version = {}
  else
    link_args += ['-static-libgcc', '-static-libstdc++']
  endif""")
    # DXVK's monolithic graphics-pipeline fallback does not use this extension.
    # It remains required on desktop, while Android accepts adapters without it.
    replace("src/dxvk/dxvk_device_info.cpp",
            """      /* Dependency for graphics pipeline library */
      ENABLE_EXT(khrPipelineLibrary, true),""",
            """      /* Required only when the optional graphics-pipeline-library path is used. */
#if defined(__ANDROID__)
      ENABLE_EXT(khrPipelineLibrary, false),
#else
      ENABLE_EXT(khrPipelineLibrary, true),
#endif""")

    # Perimeter requests D3DPRESENT_INTERVAL_TWO in its Android window. DXVK
    # normally rejects interval 2 for windowed swap chains before the presenter
    # can apply its frame-rate limiter; Android needs that validation exception.
    replace("src/d3d9/d3d9_interface.cpp",
            """    // In windowed mode, only a subset of the presentation interval flags can be used.
    if (unlikely(pPresentationParameters->Windowed
            && !(pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_DEFAULT
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_ONE
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE)))
      return D3DERR_INVALIDCALL;""",
            """    // In windowed mode, only a subset of the presentation interval flags can be used.
    if (unlikely(pPresentationParameters->Windowed
            && !(pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_DEFAULT
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_ONE
#if defined(__ANDROID__)
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_TWO
#endif
              || pPresentationParameters->PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE)))
      return D3DERR_INVALIDCALL;""")


def patch_v2_xr():
    """Expose the Vulkan creation calls needed by OpenXR in the Quest probe."""
    replace("src/d3d9/d3d9.sym",
            "    Direct3DCreate9;",
            "    Direct3DCreate9;\n    dxvkSetAndroidXrHooks;\n    dxvkResetAndroidXrHooks;")
    write_new("include/native/directx/dxvk_android_xr.h", r'''#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DXVK_ANDROID_XR_HOOKS_VERSION 1

typedef struct DxvkAndroidXrHooks {
  uint32_t version;
  uint32_t size;
  void* userData;
  VkResult (*createInstance)(void*, const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
  VkResult (*selectPhysicalDevice)(void*, VkInstance, VkPhysicalDevice*);
  VkResult (*createDevice)(void*, VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
} DxvkAndroidXrHooks;

// Call once before Direct3DCreate9. Keep userData alive until DXVK is destroyed.
bool dxvkSetAndroidXrHooks(const DxvkAndroidXrHooks* hooks);
// Call only after all D3D9 objects and in-flight Vulkan work have been released.
void dxvkResetAndroidXrHooks(void);

#ifdef __cplusplus
}
#endif
''')
    write_new("src/dxvk/dxvk_android_xr.h", r'''#pragma once

#include "../../include/native/directx/dxvk_android_xr.h"

namespace dxvk {
  extern DxvkAndroidXrHooks g_androidXrHooks;
  extern bool g_androidXrStarted;
}
''')
    replace("src/d3d9/d3d9_main.cpp",
            '#include "../dxvk/dxvk_instance.h"',
            '#include "../dxvk/dxvk_instance.h"\n#include "../dxvk/dxvk_android_xr.h"')
    replace("src/d3d9/d3d9_main.cpp",
            'namespace dxvk {\n  Logger Logger::s_instance("d3d9.log");',
            '''namespace dxvk {
  DxvkAndroidXrHooks g_androidXrHooks = { };
  bool g_androidXrStarted = false;
  Logger Logger::s_instance("d3d9.log");''')
    replace("src/d3d9/d3d9_main.cpp",
            'extern "C" {\n\n  DLLEXPORT IDirect3D9* __stdcall Direct3DCreate9',
            '''extern "C" {

  DLLEXPORT bool dxvkSetAndroidXrHooks(const DxvkAndroidXrHooks* hooks) {
    if (dxvk::g_androidXrStarted || !hooks
     || hooks->version != DXVK_ANDROID_XR_HOOKS_VERSION
     || hooks->size != sizeof(DxvkAndroidXrHooks)
     || !hooks->createInstance || !hooks->selectPhysicalDevice || !hooks->createDevice)
      return false;
    dxvk::g_androidXrHooks = *hooks;
    return true;
  }

  DLLEXPORT void dxvkResetAndroidXrHooks(void) {
    dxvk::g_androidXrHooks = { };
    dxvk::g_androidXrStarted = false;
  }

  DLLEXPORT IDirect3D9* __stdcall Direct3DCreate9''')
    replace("src/d3d9/d3d9_main.cpp",
            '''  DLLEXPORT IDirect3D9* __stdcall Direct3DCreate9(UINT nSDKVersion) {
    IDirect3D9Ex* pDirect3D = nullptr;''',
            '''  DLLEXPORT IDirect3D9* __stdcall Direct3DCreate9(UINT nSDKVersion) {
    dxvk::g_androidXrStarted = true;
    IDirect3D9Ex* pDirect3D = nullptr;''')
    replace("src/dxvk/dxvk_instance.cpp",
            '#include "dxvk_instance.h"',
            '#include "dxvk_instance.h"\n#include "dxvk_android_xr.h"')
    replace("src/dxvk/dxvk_instance.cpp",
            'VkResult status = m_vkl->vkCreateInstance(&info, nullptr, &instance);',
            '''VkResult status = g_androidXrHooks.createInstance
        ? g_androidXrHooks.createInstance(g_androidXrHooks.userData, &info, nullptr, &instance)
        : m_vkl->vkCreateInstance(&info, nullptr, &instance);''')
    replace("src/dxvk/dxvk_instance.cpp",
            '''  bool DxvkInstance::initAdapters() {
    uint32_t numAdapters = 0;''',
            '''  bool DxvkInstance::initAdapters() {
    VkPhysicalDevice xrDevice = VK_NULL_HANDLE;
    if (g_androidXrHooks.selectPhysicalDevice &&
        (g_androidXrHooks.selectPhysicalDevice(g_androidXrHooks.userData,
          m_vki->instance(), &xrDevice) != VK_SUCCESS || !xrDevice))
      throw DxvkError("OpenXR did not select a Vulkan physical device");

    uint32_t numAdapters = 0;''')
    replace("src/dxvk/dxvk_instance.cpp",
            '''    for (uint32_t i = 0; i < numAdapters; i++) {
      Rc<DxvkAdapter> adapter = new DxvkAdapter(*this, adapters[i]);''',
            '''    for (uint32_t i = 0; i < numAdapters; i++) {
      if (xrDevice && adapters[i] != xrDevice)
        continue;
      Rc<DxvkAdapter> adapter = new DxvkAdapter(*this, adapters[i]);''')
    replace("src/dxvk/dxvk_adapter.cpp",
            '#include "dxvk_adapter.h"',
            '#include "dxvk_adapter.h"\n#include "dxvk_android_xr.h"')
    replace("src/dxvk/dxvk_adapter.cpp",
            'VkResult vr = vk->vkCreateDevice(m_handle, &deviceInfo, nullptr, &device);',
            '''VkResult vr = g_androidXrHooks.createDevice
      ? g_androidXrHooks.createDevice(g_androidXrHooks.userData,
          m_handle, &deviceInfo, nullptr, &device)
      : vk->vkCreateDevice(m_handle, &deviceInfo, nullptr, &device);''')


def patch_v1():
    """Patch the legacy DXVK 1.x fork used by the native compatibility path."""
    rename_d3d9_library(1)
    patch_v1_presenter_and_pacing()
    patch_v1_allocator_diagnostics()
    patch_v1_allocator_policy()
    patch_v1_timing_instrumentation()
    patch_v1_mobile_compatibility()
    patch_v1_android_build_integration()


def patch_v1_allocator_diagnostics():
    """Log Android DXVK 1 heap, memory-type, and empty-chunk usage."""
    replace("src/dxvk/dxvk_memory.h", "#pragma once\n\n#include \"dxvk_adapter.h\"",
            "#pragma once\n\n#include <chrono>\n\n#include \"dxvk_adapter.h\"")
    replace("src/dxvk/dxvk_memory.h",
            "    VkDeviceSize      chunkSize;\n\n    std::vector<Rc<DxvkMemoryChunk>> chunks;",
            """    VkDeviceSize      chunkSize;

#if defined(__ANDROID__)
    VkDeviceSize      memoryAllocated = 0;
    VkDeviceSize      memoryUsed      = 0;
#endif

    std::vector<Rc<DxvkMemoryChunk>> chunks;""")
    replace("src/dxvk/dxvk_memory.h",
            "  private:\n    \n    struct FreeSlice {",
            "  private:\n    friend class DxvkMemoryAllocator;\n\n    struct FreeSlice {")
    replace("src/dxvk/dxvk_memory.h",
            "    DxvkMemory alloc(\n      const VkMemoryRequirements*             req,\n      const VkMemoryDedicatedRequirements&    dedAllocReq,\n      const VkMemoryDedicatedAllocateInfo&    dedAllocInfo,\n            VkMemoryPropertyFlags             flags,\n            float                             priority);",
            """    DxvkMemory alloc(
      const VkMemoryRequirements*             req,
      const VkMemoryDedicatedRequirements&    dedAllocReq,
      const VkMemoryDedicatedAllocateInfo&    dedAllocInfo,
            VkMemoryPropertyFlags             flags,
            float                             priority);

#if defined(__ANDROID__)
    void logAndroidMemoryStats();
#endif""")
    replace("src/dxvk/dxvk_memory.h",
            "    dxvk::mutex                                     m_mutex;\n    std::array<DxvkMemoryHeap, VK_MAX_MEMORY_HEAPS> m_memHeaps;",
            """    dxvk::mutex                                     m_mutex;
#if defined(__ANDROID__)
    std::chrono::steady_clock::time_point           m_nextAndroidMemoryLog = { };
#endif
    std::array<DxvkMemoryHeap, VK_MAX_MEMORY_HEAPS> m_memHeaps;""")

    replace("src/dxvk/dxvk_memory.cpp",
            '#include "dxvk_device.h"\n#include "dxvk_memory.h"',
            '#include <cstdlib>\n#include <string>\n\n#include "dxvk_device.h"\n#include "dxvk_memory.h"')
    replace("src/dxvk/dxvk_memory.cpp",
            "      m_memTypes[i].chunkSize  = pickChunkSize(i);",
            """      m_memTypes[i].chunkSize  = pickChunkSize(i);
#if defined(__ANDROID__)
      m_memTypes[i].memoryAllocated = 0;
      m_memTypes[i].memoryUsed      = 0;
#endif""")
    replace("src/dxvk/dxvk_memory.cpp",
            "    if (memory)\n      type->heap->stats.memoryUsed += memory.m_length;",
            """    if (memory) {
      type->heap->stats.memoryUsed += memory.m_length;
#if defined(__ANDROID__)
      type->memoryUsed += memory.m_length;
#endif
    }""")
    replace("src/dxvk/dxvk_memory.cpp",
            "    type->heap->stats.memoryAllocated += size;\n    m_device->adapter()->notifyHeapMemoryAlloc(type->heapId, size);",
            """    type->heap->stats.memoryAllocated += size;
#if defined(__ANDROID__)
    type->memoryAllocated += size;
#endif
    m_device->adapter()->notifyHeapMemoryAlloc(type->heapId, size);""")
    replace("src/dxvk/dxvk_memory.cpp",
            "    memory.m_type->heap->stats.memoryUsed -= memory.m_length;",
            """    memory.m_type->heap->stats.memoryUsed -= memory.m_length;
#if defined(__ANDROID__)
    memory.m_type->memoryUsed -= memory.m_length;
#endif""")
    replace("src/dxvk/dxvk_memory.cpp",
            "    type->heap->stats.memoryAllocated -= memory.memSize;\n    m_device->adapter()->notifyHeapMemoryFree(type->heapId, memory.memSize);",
            """    type->heap->stats.memoryAllocated -= memory.memSize;
#if defined(__ANDROID__)
    type->memoryAllocated -= memory.memSize;
#endif
    m_device->adapter()->notifyHeapMemoryFree(type->heapId, memory.memSize);""")
    replace("src/dxvk/dxvk_memory.cpp",
            "  VkDeviceSize DxvkMemoryAllocator::pickChunkSize(uint32_t memTypeId) const {",
            r'''#if defined(__ANDROID__)
  void DxvkMemoryAllocator::logAndroidMemoryStats() {
    const char* memoryLogging = std::getenv("PERIMETER_ANDROID_MEMORY_LOG");
    if (!memoryLogging || memoryLogging[0] != '1' || memoryLogging[1] != '\0')
      return;

    const auto now = std::chrono::steady_clock::now();
    if (m_nextAndroidMemoryLog != std::chrono::steady_clock::time_point()
     && now < m_nextAndroidMemoryLog)
      return;

    m_nextAndroidMemoryLog = now + std::chrono::seconds(10);
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    const bool hasDriverBudget = m_device->extensions().extMemoryBudget;
    DxvkAdapterMemoryInfo driverInfo = { };
    if (hasDriverBudget)
      driverInfo = m_device->adapter()->getMemoryHeapInfo();

    for (uint32_t heapId = 0; heapId < m_memProps.memoryHeapCount; heapId++) {
      const DxvkMemoryHeap& heap = m_memHeaps[heapId];
      std::string driverAllocated = "unavailable";
      std::string driverBudget = "unavailable";
      if (hasDriverBudget && heapId < driverInfo.heapCount) {
        driverAllocated = str::format(driverInfo.heaps[heapId].memoryAllocated);
        driverBudget = str::format(driverInfo.heaps[heapId].memoryBudget);
      }

      Logger::info(str::format(
        "Android DXVK memory heap=", heapId,
        " allocatedBytes=", heap.stats.memoryAllocated,
        " usedBytes=", heap.stats.memoryUsed,
        " allocatorBudgetBytes=", heap.budget,
        " driverAllocatedBytes=", driverAllocated,
        " driverBudgetBytes=", driverBudget,
        " heapSizeBytes=", heap.properties.size));
    }

    for (uint32_t typeId = 0; typeId < m_memProps.memoryTypeCount; typeId++) {
      const DxvkMemoryType& type = m_memTypes[typeId];
      if (!type.memoryAllocated && !type.memoryUsed && type.chunks.empty())
        continue;

      uint32_t emptyChunkCount = 0;
      VkDeviceSize emptyChunkBytes = 0;
      uint32_t chunkId = 0;
      for (const auto& chunk : type.chunks) {
        VkDeviceSize freeBytes = 0;
        for (const auto& slice : chunk->m_freeList)
          freeBytes += slice.length;

        if (freeBytes == chunk->m_memory.memSize) {
          emptyChunkCount += 1;
          emptyChunkBytes += chunk->m_memory.memSize;
        }

        Logger::info(str::format(
          "Android DXVK memory heap=", type.heapId,
          " type=", typeId,
          " chunk=", chunkId++,
          " sizeBytes=", chunk->m_memory.memSize,
          " usedBytes=", chunk->m_memory.memSize - freeBytes,
          " freeBytes=", freeBytes,
          " freeSlices=", chunk->m_freeList.size(),
          " priority=", chunk->m_memory.priority));
      }

      const VkDeviceSize cachedFreeBytes = type.memoryAllocated > type.memoryUsed
        ? type.memoryAllocated - type.memoryUsed : 0;
      const std::string memoryFlags = str::format("0x", std::hex,
        type.memType.propertyFlags);

      Logger::info(str::format(
        "Android DXVK memory heap=", type.heapId,
        " type=", typeId,
        " flags=", memoryFlags,
        " allocatedBytes=", type.memoryAllocated,
        " usedBytes=", type.memoryUsed,
        " cachedFreeBytes=", cachedFreeBytes,
        " chunks=", type.chunks.size(),
        " emptyChunks=", emptyChunkCount,
        " emptyChunkBytes=", emptyChunkBytes));
    }
  }
#endif


  VkDeviceSize DxvkMemoryAllocator::pickChunkSize(uint32_t memTypeId) const {''')

    replace("src/dxvk/dxvk_queue.cpp",
            "      if (entry.status)\n        entry.status->result = status;",
            """#if defined(__ANDROID__)
      m_device->m_objects.memoryManager().logAndroidMemoryStats();
#endif

      if (entry.status)
        entry.status->result = status;""")


def patch_v1_allocator_policy():
    """Use 64 MiB backing chunks for Android DXVK 1 allocations."""
    replace("src/dxvk/dxvk_memory.cpp",
            "    return chunkSize;",
            """#if defined(__ANDROID__)
    // Keep Android DXVK 1 backing chunks at 64 MiB. This reduces retained
    // graphics memory and internal fragmentation compared with the upstream
    // 128 MiB default while avoiding the excessive chunk count seen at 32 MiB.
    constexpr VkDeviceSize androidMaxChunkSize = 64ull << 20;
    if (chunkSize > androidMaxChunkSize)
      chunkSize = androidMaxChunkSize;
#endif
    return chunkSize;""")


def patch_v1_presenter_and_pacing():
    """Patch Android surface recovery, Swappy, and presenter timing."""
    replace("src/vulkan/vulkan_presenter.h",
            "namespace dxvk::vk {\n",
            """namespace dxvk {
  class DxvkDevice;
}

namespace dxvk::vk {
""")
    replace("src/vulkan/vulkan_presenter.h",
            "    PresenterFeatures   features    = { };",
            """    PresenterFeatures   features    = { };

#if defined(__ANDROID__)
    dxvk::DxvkDevice* dxvkDevice = nullptr;
#endif""")
    # Android replaces the SurfaceView's ANativeWindow when an activity is
    # restored. The native DXVK 1.x presenter otherwise keeps the Vulkan
    # surface created for the old window and can block in vkAcquireNextImageKHR
    # until the abandoned BufferQueue times out. Track the SDL window handle
    # and rebuild the Vulkan surface before acquiring the first post-resume
    # image.
    replace("src/vulkan/vulkan_presenter.h",
            """    VkResult recreateSwapChain(
      const PresenterDesc&  desc);""",
            """    VkResult recreateSwapChain(
      const PresenterDesc&  desc);

#if defined(__ANDROID__)
    VkResult refreshAndroidSurface();
    void invalidateAndroidSurface();
    void resumeAndroidSurface();
    bool androidSurfacePaused() const { return m_androidSurfacePaused; }
    bool androidSurfaceNeedsRefresh() const { return m_androidNativeWindow == nullptr; }
#endif""")
    replace("src/vulkan/vulkan_presenter.h",
            "    HWND              m_window      = nullptr;",
            """    HWND              m_window      = nullptr;

#if defined(__ANDROID__)
    void*               m_androidNativeWindow = nullptr;
    PresenterDesc       m_androidSwapChainDesc = { };
    bool                m_androidSurfacePaused = false;
#endif""")
    replace("src/vulkan/vulkan_presenter.cpp",
            """  VkResult Presenter::acquireNextImage(PresenterSync& sync, uint32_t& index) {
    sync = m_semaphores.at(m_frameIndex);""",
            """#if defined(__ANDROID__)
  VkResult Presenter::refreshAndroidSurface() {
    if (m_androidSurfacePaused)
      return VK_NOT_READY;

    SDL_SysWMinfo wmInfo { };
    SDL_VERSION(&wmInfo.version);

    void* nativeWindow = nullptr;
    if (SDL_GetWindowWMInfo(reinterpret_cast<SDL_Window*>(m_window), &wmInfo))
      nativeWindow = wmInfo.info.android.window;

    if (nativeWindow == m_androidNativeWindow)
      return nativeWindow ? VK_SUCCESS : VK_NOT_READY;

    // The present status only tells us that the present operation was queued.
    // Command lists submitted for the old swapchain may still be waiting on
    // their fences, so the old swapchain, semaphores, and surface cannot be
    // destroyed until the device is idle. Destroying them first can make the
    // Android driver report VK_ERROR_DEVICE_LOST on the finish thread.
    if (m_swapchain || m_surface) {
      if (m_device.dxvkDevice) {
        m_device.dxvkDevice->waitForIdle();
      } else {
        VkResult idleStatus = m_vkd->vkDeviceWaitIdle(m_vkd->device());
        if (idleStatus != VK_SUCCESS)
          return idleStatus;
      }
    }

    if (m_swapchain)
      destroySwapchain();
    if (m_surface)
      destroySurface();

    m_androidNativeWindow = nullptr;
    if (!nativeWindow)
      return VK_NOT_READY;

    VkResult status = createSurface();
    if (status != VK_SUCCESS)
      return status;

    m_androidNativeWindow = nativeWindow;
    return recreateSwapChain(m_androidSwapChainDesc);
  }
#endif


  VkResult Presenter::acquireNextImage(PresenterSync& sync, uint32_t& index) {
#if defined(__ANDROID__)
    VkResult surfaceStatus = refreshAndroidSurface();
    if (surfaceStatus != VK_SUCCESS)
      return surfaceStatus;
#endif
    sync = m_semaphores.at(m_frameIndex);""")
    # The native 1.x fork links SDL2 directly and has no dynamic WSI loader.
    # Android surfaces must use a composite-alpha mode reported by the driver.
    replace("src/vulkan/vulkan_presenter.cpp",
            "    swapInfo.compositeAlpha         = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;",
            """    swapInfo.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
      caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1u));""")
    replace("src/vulkan/vulkan_presenter.cpp",
            "  VkResult Presenter::recreateSwapChain(const PresenterDesc& desc) {\n    if (m_swapchain)",
            "  VkResult Presenter::recreateSwapChain(const PresenterDesc& desc) {\n#if defined(__ANDROID__)\n    m_androidSwapChainDesc = desc;\n#endif\n    if (m_swapchain)")
    replace("src/vulkan/vulkan_presenter.cpp",
            "  void Presenter::destroySurface() {\n    m_vki->vkDestroySurfaceKHR(m_vki->instance(), m_surface, nullptr);\n  }",
            "  void Presenter::destroySurface() {\n    m_vki->vkDestroySurfaceKHR(m_vki->instance(), m_surface, nullptr);\n    m_surface = VK_NULL_HANDLE;\n  }")
    replace("src/vulkan/vulkan_presenter.cpp",
            "    if (!supportStatus) {\n      m_vki->vkDestroySurfaceKHR(m_vki->instance(), m_surface, nullptr);\n      return VK_ERROR_OUT_OF_HOST_MEMORY; // just abuse this\n    }\n\n    return VK_SUCCESS;\n  }\n\n\n  void Presenter::destroySwapchain()",
            "    if (!supportStatus) {\n      m_vki->vkDestroySurfaceKHR(m_vki->instance(), m_surface, nullptr);\n      return VK_ERROR_OUT_OF_HOST_MEMORY; // just abuse this\n    }\n\n#if defined(__ANDROID__)\n    SDL_SysWMinfo wmInfo { };\n    SDL_VERSION(&wmInfo.version);\n    if (SDL_GetWindowWMInfo(reinterpret_cast<SDL_Window*>(m_window), &wmInfo))\n      m_androidNativeWindow = wmInfo.info.android.window;\n#endif\n\n    return VK_SUCCESS;\n  }\n\n\n  void Presenter::destroySwapchain()")
    replace("src/dxvk/dxvk_adapter.cpp",
            "#include <cstring>\n#include <unordered_set>",
            """#include <cstring>
#include <unordered_set>

#if defined(__ANDROID__)
#include <cstdlib>
#endif""")
    # Swappy is runtime-gated so the same binary retains the ordinary Vulkan
    # path for controls and for devices without VK_GOOGLE_display_timing.
    replace("src/dxvk/dxvk_adapter.cpp",
            """    // Enable additional extensions if necessary
    extensionsEnabled.merge(m_extraExtensions);""",
            """    // Enable additional extensions if necessary
    extensionsEnabled.merge(m_extraExtensions);
#if defined(__ANDROID__)
    const char* swappy = std::getenv("DXVK_ANDROID_SWAPPY");
    if (swappy && *swappy == '1') {
      if (m_deviceExtensions.supports(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME)) {
        extensionsEnabled.add(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
        Logger::info("Android SwappyVk: enabling VK_GOOGLE_display_timing");
      } else {
        Logger::warn("Android SwappyVk: VK_GOOGLE_display_timing unavailable; using fallback pacing");
      }
    }
#endif""")
    # Diagnostic instrumentation is independent of the pacing policy. Record
    # individual WSI stages using CLOCK_MONOTONIC so they can be correlated
    # with the renderer-independent application trace.
    write_new("src/vulkan/vulkan_android_timing.h", r'''#pragma once

#if defined(__ANDROID__)
#include <cstdint>

namespace dxvk::vk::android_timing {
uint64_t nowNs();
void record(const char* event, uint64_t startNs, uint64_t endNs, int32_t status);
}
#endif
''')
    write_new("src/vulkan/vulkan_android_timing.cpp", r'''#include "vulkan_android_timing.h"

#if defined(__ANDROID__)
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

namespace dxvk::vk::android_timing {
namespace {
std::mutex outputMutex;
std::atomic<uint64_t> sequence { 0 };
FILE* output = nullptr;
unsigned pendingRecords = 0;
bool initialized = false;

void closeOutput() {
  std::lock_guard<std::mutex> lock(outputMutex);
  if (!output) return;
  std::fflush(output);
  std::fclose(output);
  output = nullptr;
}

void initializeLocked() {
  if (initialized) return;
  initialized = true;
  const char* path = std::getenv("DXVK_FRAME_TIMING_PATH");
  if (!path || !*path) return;
  output = std::fopen(path, "w");
  if (!output) return;
  std::setvbuf(output, nullptr, _IOFBF, 64 * 1024);
  std::fputs("sequence,event,start_ns,end_ns,duration_ns,status\n", output);
  std::atexit(closeOutput);
}
}

uint64_t nowNs() {
  timespec value { };
  clock_gettime(CLOCK_MONOTONIC, &value);
  return uint64_t(value.tv_sec) * 1000000000ull + uint64_t(value.tv_nsec);
}

void record(const char* event, uint64_t startNs, uint64_t endNs, int32_t status) {
  std::lock_guard<std::mutex> lock(outputMutex);
  initializeLocked();
  if (!output) return;
  std::fprintf(output, "%" PRIu64 ",%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRId32 "\n",
    ++sequence, event, startNs, endNs, endNs - startNs, status);
  if (++pendingRecords >= 30) {
    std::fflush(output);
    pendingRecords = 0;
  }
}
}
#endif
''')
    replace("src/vulkan/meson.build",
            "  'vulkan_presenter.cpp',",
            "  'vulkan_presenter.cpp',\n  'vulkan_android_timing.cpp',")
    replace("src/vulkan/vulkan_presenter.cpp",
            '#include "vulkan_presenter.h"',
            '''#include "vulkan_presenter.h"

#include "vulkan_android_timing.h"

#if defined(__ANDROID__)
#include <cstdlib>
#include <SDL_system.h>
#include <SDL_syswm.h>
#include <swappy/swappyVk.h>
#include "../dxvk/dxvk_device.h"
#endif''')
    replace("src/vulkan/vulkan_presenter.cpp",
            """#endif


  VkResult Presenter::acquireNextImage(PresenterSync& sync, uint32_t& index) {""",
            """#endif

#if defined(__ANDROID__)
  void Presenter::invalidateAndroidSurface() {
    if (m_device.dxvkDevice)
      m_device.dxvkDevice->waitForIdle();
    m_androidNativeWindow = nullptr;
    m_androidSurfacePaused = true;
  }

  void Presenter::resumeAndroidSurface() {
    m_androidSurfacePaused = false;
  }
#endif


  VkResult Presenter::acquireNextImage(PresenterSync& sync, uint32_t& index) {""")
    replace("src/vulkan/vulkan_presenter.cpp",
            "    VkResult status = m_vkd->vkQueuePresentKHR(m_device.queue, &info);",
            """#if defined(__ANDROID__)
    const uint64_t queuePresentStartNs = android_timing::nowNs();
#endif
    VkResult status = m_swappyEnabled
      ? SwappyVk_queuePresent(m_device.queue, &info)
      : m_vkd->vkQueuePresentKHR(m_device.queue, &info);
#if defined(__ANDROID__)
    android_timing::record("queue_present", queuePresentStartNs,
      android_timing::nowNs(), static_cast<int32_t>(status));
#endif""")
    replace("src/vulkan/vulkan_presenter.h",
            "    VkResult m_acquireStatus = VK_NOT_READY;",
            """    VkResult m_acquireStatus = VK_NOT_READY;

#if defined(__ANDROID__)
    bool m_swappyEnabled = false;
#endif""")
    replace("src/vulkan/vulkan_presenter.cpp",
            "    // Acquire images and create views",
            """#if defined(__ANDROID__)
    m_swappyEnabled = false;
    const char* swappy = std::getenv("DXVK_ANDROID_SWAPPY");
    if (swappy && *swappy == '1') {
      uint64_t refreshDuration = 0;
      SwappyVk_setQueueFamilyIndex(m_vkd->device(), m_device.queue, m_device.queueFamily);
      m_swappyEnabled = SwappyVk_initAndGetRefreshCycleDuration(
        static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv()),
        static_cast<jobject>(SDL_AndroidGetActivity()),
        m_device.adapter, m_vkd->device(), m_swapchain, &refreshDuration);
      if (m_swappyEnabled) {
        SDL_SysWMinfo wmInfo { };
        SDL_VERSION(&wmInfo.version);
        if (SDL_GetWindowWMInfo(reinterpret_cast<SDL_Window*>(m_window), &wmInfo))
          SwappyVk_setWindow(m_vkd->device(), m_swapchain, wmInfo.info.android.window);
        else
          Logger::warn(str::format("Android SwappyVk: SDL native window unavailable: ", SDL_GetError()));
        SwappyVk_setAutoSwapInterval(false);
        SwappyVk_setAutoPipelineMode(false);
        SwappyVk_setSwapIntervalNS(m_vkd->device(), m_swapchain, refreshDuration * 2u);
        Logger::info(str::format("Android SwappyVk enabled: refresh interval ", refreshDuration, " ns"));
      } else {
        Logger::err("Android SwappyVk initialization failed; using vkQueuePresentKHR");
      }
    }
#endif

    // Acquire images and create views""")
    replace("src/vulkan/vulkan_presenter.cpp",
            """  void Presenter::destroySwapchain() {
    for (const auto& img : m_images)""",
            """  void Presenter::destroySwapchain() {
#if defined(__ANDROID__)
    if (m_swappyEnabled && m_swapchain) {
      SwappyVk_destroySwapchain(m_vkd->device(), m_swapchain);
      m_swappyEnabled = false;
    }
#endif
    for (const auto& img : m_images)""")
    replace("src/vulkan/vulkan_presenter.cpp",
            """    m_acquireStatus = m_vkd->vkAcquireNextImageKHR(m_vkd->device(),
      m_swapchain, std::numeric_limits<uint64_t>::max(),
      sync.acquire, VK_NULL_HANDLE, &m_imageIndex);

    bool vsync =""",
            """#if defined(__ANDROID__)
    const uint64_t acquireStartNs = android_timing::nowNs();
#endif
    m_acquireStatus = m_vkd->vkAcquireNextImageKHR(m_vkd->device(),
      m_swapchain, std::numeric_limits<uint64_t>::max(),
      sync.acquire, VK_NULL_HANDLE, &m_imageIndex);
#if defined(__ANDROID__)
    android_timing::record("acquire_next_image", acquireStartNs,
      android_timing::nowNs(), static_cast<int32_t>(m_acquireStatus));
#endif

    bool vsync =""")
    replace("src/vulkan/vulkan_presenter.cpp",
            "    m_fpsLimiter.delay(vsync);\n    return status;",
            """#if defined(__ANDROID__)
    const uint64_t limiterStartNs = android_timing::nowNs();
#endif
    m_fpsLimiter.delay(vsync);
#if defined(__ANDROID__)
    android_timing::record("fps_limiter", limiterStartNs,
      android_timing::nowNs(), 0);
#endif
    return status;""")


def patch_v1_timing_instrumentation():
    """Add queue and D3D9 presentation timing and lifecycle handling."""
    # Add timing and Android logging dependencies beside the queue implementation.
    replace("src/dxvk/dxvk_queue.cpp",
            '#include "dxvk_queue.h"\n',
            '''#include "dxvk_queue.h"

#include "../vulkan/vulkan_android_timing.h"

#if defined(__ANDROID__)
#include <cstdlib>
#include <SDL_log.h>
#endif
''')
    replace("src/dxvk/dxvk_queue.cpp",
            """        if (entry.submit.cmdList != nullptr) {
          status = entry.submit.cmdList->submit(
            entry.submit.waitSync,
            entry.submit.wakeSync);
        } else if (entry.present.presenter != nullptr) {""",
            """        if (entry.submit.cmdList != nullptr) {
#if defined(__ANDROID__)
          const uint64_t queueSubmitStartNs = vk::android_timing::nowNs();
#endif
          status = entry.submit.cmdList->submit(
            entry.submit.waitSync,
            entry.submit.wakeSync);
#if defined(__ANDROID__)
          vk::android_timing::record("queue_submit", queueSubmitStartNs,
            vk::android_timing::nowNs(), static_cast<int32_t>(status));
#endif
        } else if (entry.present.presenter != nullptr) {""")
    replace("src/d3d9/d3d9_swapchain.cpp",
            '#include "d3d9_hud.h"',
            '''#include "d3d9_hud.h"

#include "../vulkan/vulkan_android_timing.h"

#if defined(__ANDROID__)
#include <cstdlib>
#include <SDL_events.h>
#endif''')
    replace("src/d3d9/d3d9_swapchain.cpp",
            "namespace dxvk {\n",
            """namespace dxvk {

#if defined(__ANDROID__)
  static int DxvkAndroidPauseEventWatch(void* userdata, SDL_Event* event) {
    if (event->type == SDL_APP_WILLENTERBACKGROUND)
      static_cast<D3D9SwapChainEx*>(userdata)->OnAndroidPause();
    else if (event->type == SDL_APP_WILLENTERFOREGROUND)
      static_cast<D3D9SwapChainEx*>(userdata)->OnAndroidResume();
    return 0;
  }
#endif
""")
    replace("src/d3d9/d3d9_swapchain.h",
            "    ~D3D9SwapChainEx();",
            """    ~D3D9SwapChainEx();

#if defined(__ANDROID__)
    void OnAndroidPause();
    void OnAndroidResume();
#endif""")
    replace("src/d3d9/d3d9_swapchain.cpp",
            """    if (!m_presentParams.Windowed && FAILED(EnterFullscreenMode(pPresentParams, pFullscreenDisplayMode)))
      throw DxvkError("D3D9: Failed to set initial fullscreen state");
  }


  D3D9SwapChainEx::~D3D9SwapChainEx() {""",
            """    if (!m_presentParams.Windowed && FAILED(EnterFullscreenMode(pPresentParams, pFullscreenDisplayMode)))
      throw DxvkError("D3D9: Failed to set initial fullscreen state");
#if defined(__ANDROID__)
    SDL_AddEventWatch(DxvkAndroidPauseEventWatch, this);
#endif
  }


#if defined(__ANDROID__)
  void D3D9SwapChainEx::OnAndroidPause() {
    if (m_presenter != nullptr)
      m_presenter->invalidateAndroidSurface();
    else
      m_device->waitForIdle();
  }

  void D3D9SwapChainEx::OnAndroidResume() {
    if (m_presenter != nullptr)
      m_presenter->resumeAndroidSurface();
  }
#endif


  D3D9SwapChainEx::~D3D9SwapChainEx() {""")
    replace("src/d3d9/d3d9_swapchain.cpp",
            """  D3D9SwapChainEx::~D3D9SwapChainEx() {
    DestroyBackBuffers();""",
            """  D3D9SwapChainEx::~D3D9SwapChainEx() {
#if defined(__ANDROID__)
    SDL_DelEventWatch(DxvkAndroidPauseEventWatch, this);
#endif
    DestroyBackBuffers();""")
    replace("src/d3d9/d3d9_swapchain.cpp",
            "    presenterDevice.adapter       = m_device->adapter()->handle();",
            """    presenterDevice.adapter       = m_device->adapter()->handle();
#if defined(__ANDROID__)
    presenterDevice.dxvkDevice = m_device.ptr();
#endif""")
    replace("src/d3d9/d3d9_swapchain.cpp",
            """  void D3D9SwapChainEx::PresentImage(UINT SyncInterval) {
    m_parent->Flush();

    // Retrieve the image and image view to present""",
            """  void D3D9SwapChainEx::PresentImage(UINT SyncInterval) {
#if defined(__ANDROID__)
    if (m_presenter->androidSurfacePaused())
      return;
    if (m_presenter->androidSurfaceNeedsRefresh()) {
      if (m_presenter->refreshAndroidSurface() != VK_SUCCESS)
        return;
      CreateRenderTargetViews();
    }
#endif

    m_parent->Flush();

    // Retrieve the image and image view to present""")
    replace("src/d3d9/d3d9_swapchain.cpp",
            """  void D3D9SwapChainEx::SynchronizePresent() {
    // Recreate swap chain if the previous present call failed
    VkResult status = m_device->waitForSubmission(&m_presentStatus);""",
            """  void D3D9SwapChainEx::SynchronizePresent() {
    // Recreate swap chain if the previous present call failed
#if defined(__ANDROID__)
    const uint64_t waitStartNs = vk::android_timing::nowNs();
#endif
    VkResult status = m_device->waitForSubmission(&m_presentStatus);
#if defined(__ANDROID__)
    vk::android_timing::record("wait_for_present_submission", waitStartNs,
      vk::android_timing::nowNs(), static_cast<int32_t>(status));
#endif""")
    replace("src/d3d9/d3d9_swapchain.cpp",
            """  void D3D9SwapChainEx::SyncFrameLatency() {
    // Wait for the sync event so that we respect the maximum frame latency
    m_frameLatencySignal->wait(m_frameId - GetActualFrameLatency());
  }""",
            """  void D3D9SwapChainEx::SyncFrameLatency() {
    // Wait for the sync event so that we respect the maximum frame latency
#if defined(__ANDROID__)
    const uint64_t waitStartNs = vk::android_timing::nowNs();
#endif
    m_frameLatencySignal->wait(m_frameId - GetActualFrameLatency());
#if defined(__ANDROID__)
    vk::android_timing::record("frame_latency_wait", waitStartNs,
      vk::android_timing::nowNs(), 0);
#endif
  }""")
    replace("src/dxvk/dxvk_graphics.cpp",
            '#include "dxvk_graphics.h"',
            '#include "dxvk_graphics.h"\n\n#include "../vulkan/vulkan_android_timing.h"')
    replace("src/dxvk/dxvk_graphics.cpp",
            """    VkPipeline pipeline = VK_NULL_HANDLE;
    if (m_vkd->vkCreateGraphicsPipelines(m_vkd->device(),
          m_pipeMgr->m_cache->handle(), 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
      Logger::err("DxvkGraphicsPipeline: Failed to compile pipeline");""",
            """    VkPipeline pipeline = VK_NULL_HANDLE;
#if defined(__ANDROID__)
    const uint64_t compileStartNs = vk::android_timing::nowNs();
#endif
    const VkResult compileStatus = m_vkd->vkCreateGraphicsPipelines(m_vkd->device(),
      m_pipeMgr->m_cache->handle(), 1, &info, nullptr, &pipeline);
#if defined(__ANDROID__)
    vk::android_timing::record("graphics_pipeline_compile", compileStartNs,
      vk::android_timing::nowNs(), static_cast<int32_t>(compileStatus));
#endif
    if (compileStatus != VK_SUCCESS) {
      Logger::err("DxvkGraphicsPipeline: Failed to compile pipeline");""")
    replace("src/dxvk/dxvk_compute.cpp",
            '#include "dxvk_compute.h"',
            '#include "dxvk_compute.h"\n\n#include "../vulkan/vulkan_android_timing.h"')
    replace("src/dxvk/dxvk_compute.cpp",
            """    VkPipeline pipeline = VK_NULL_HANDLE;
    if (m_vkd->vkCreateComputePipelines(m_vkd->device(),
          m_pipeMgr->m_cache->handle(), 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
      Logger::err("DxvkComputePipeline: Failed to compile pipeline");""",
            """    VkPipeline pipeline = VK_NULL_HANDLE;
#if defined(__ANDROID__)
    const uint64_t compileStartNs = vk::android_timing::nowNs();
#endif
    const VkResult compileStatus = m_vkd->vkCreateComputePipelines(m_vkd->device(),
      m_pipeMgr->m_cache->handle(), 1, &info, nullptr, &pipeline);
#if defined(__ANDROID__)
    vk::android_timing::record("compute_pipeline_compile", compileStartNs,
      vk::android_timing::nowNs(), static_cast<int32_t>(compileStatus));
#endif
    if (compileStatus != VK_SUCCESS) {
      Logger::err("DxvkComputePipeline: Failed to compile pipeline");""")
    # VK_SUBOPTIMAL_KHR is persistent on some Android WSI implementations. Do
    # not recreate the swap chain every frame while presentation is valid.
    replace("src/d3d9/d3d9_swapchain.cpp",
            "    if (status != VK_SUCCESS)\n      RecreateSwapChain(m_vsync);",
            """    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)
      RecreateSwapChain(m_vsync);""")


def patch_v1_mobile_compatibility():
    """Adapt graphics features and synchronization to mobile Vulkan devices."""
    # Mali exposes 16 vertex inputs; omit unused fixed-function blend inputs.
    replace("src/dxvk/dxvk_graphics.cpp",
            "    for (uint32_t i = 0; i < state.il.attributeCount(); i++) {\n      viAttribs[i] = state.ilAttributes[i].description();\n      viAttribs[i].binding = viBindingMap[state.ilAttributes[i].binding()];\n    }",
            """    uint32_t viAttributeCount = 0;
    for (uint32_t i = 0; i < state.il.attributeCount(); i++) {
      if (!(m_shaders.vs->interfaceSlots().inputSlots & (1u << state.ilAttributes[i].location())))
        continue;
      viAttribs[viAttributeCount] = state.ilAttributes[i].description();
      viAttribs[viAttributeCount++].binding = viBindingMap[state.ilAttributes[i].binding()];
    }""")
    replace("src/dxvk/dxvk_graphics.cpp",
            "viInfo.vertexAttributeDescriptionCount  = state.il.attributeCount();",
            "viInfo.vertexAttributeDescriptionCount  = viAttributeCount;")
    replace("src/dxvk/dxvk_graphics.cpp",
            "    uint32_t sampleMask = state.ms.sampleMask();",
            """    rsInfo.depthClampEnable &= m_pipeMgr->m_device->features().core.features.depthClamp;

    uint32_t sampleMask = state.ms.sampleMask();""")
    # Compatible render passes need matching dependencies. Use conservative
    # Android barriers and retain the attachment-sampling self dependency.
    replace("src/dxvk/dxvk_renderpass.cpp",
            "    VkRenderPassCreateInfo info;",
            """#if defined(__ANDROID__)
    subpassDepCount = 3;
    subpassDeps[0] = { VK_SUBPASS_EXTERNAL, 0,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
      VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 };
    subpassDeps[1] = { 0, 0,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
      VK_DEPENDENCY_BY_REGION_BIT };
    subpassDeps[2] = { 0, VK_SUBPASS_EXTERNAL,
      VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 };
#endif

    VkRenderPassCreateInfo info;""")
    # ClipDistance is optional on mobile. Do not emit its SPIR-V capability
    # (even for disabled planes) when the Vulkan device cannot support it.
    replace("src/dxso/dxso_options.h", "    bool useDemoteToHelperInvocation = false;",
            "    bool useDemoteToHelperInvocation = false;\n    bool useClipDistance = true;")
    replace("src/dxso/dxso_options.cpp",
            "    const DxvkDeviceFeatures& devFeatures = device->features();",
            "    const DxvkDeviceFeatures& devFeatures = device->features();\n    useClipDistance = devFeatures.core.features.shaderClipDistance;")
    replace("src/dxso/dxso_compiler.cpp",
            "    m_module.enableCapability(spv::CapabilityClipDistance);",
            "    if (m_moduleInfo.options.useClipDistance)\n      m_module.enableCapability(spv::CapabilityClipDistance);")
    replace("src/dxso/dxso_compiler.cpp", "  void DxsoCompiler::emitVsClipping() {",
            "  void DxsoCompiler::emitVsClipping() {\n    if (!m_moduleInfo.options.useClipDistance) return;")
    replace("src/d3d9/d3d9_fixed_function.h",
            "D3D9FixedFunctionOptions(const D3D9Options* options);",
            "D3D9FixedFunctionOptions(D3D9DeviceEx* device);\n    bool useClipDistance;")
    replace("src/d3d9/d3d9_fixed_function.cpp",
            "D3D9FixedFunctionOptions::D3D9FixedFunctionOptions(const D3D9Options* options) {",
            "D3D9FixedFunctionOptions::D3D9FixedFunctionOptions(D3D9DeviceEx* device) {\n    const auto* options = device->GetOptions();\n    useClipDistance = device->GetDXVKDevice()->features().core.features.shaderClipDistance;")
    replace_all("src/d3d9/d3d9_fixed_function.cpp",
                "      pDevice->GetOptions());",
                "      D3D9FixedFunctionOptions(pDevice));", expected_count=2)
    replace("src/d3d9/d3d9_fixed_function.cpp",
            "    m_module.enableCapability(spv::CapabilityClipDistance);",
            "    if (m_options.useClipDistance)\n      m_module.enableCapability(spv::CapabilityClipDistance);")
    replace("src/d3d9/d3d9_fixed_function.cpp",
            "  void D3D9FFShaderCompiler::emitVsClipping(uint32_t vtx) {",
            "  void D3D9FFShaderCompiler::emitVsClipping(uint32_t vtx) {\n    if (!m_options.useClipDistance) return;")
    replace("src/d3d9/d3d9_adapter.cpp",
            "pCaps->MaxUserClipPlanes         = MaxClipPlanes;",
            "pCaps->MaxUserClipPlanes         = m_adapter->features().core.features.shaderClipDistance ? MaxClipPlanes : 0;")
    replace("src/d3d9/d3d9_device.cpp",
            "  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {",
            """  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
    if (State == D3DRS_CLIPPLANEENABLE && Value
     && !m_dxvkDevice->features().core.features.shaderClipDistance) {
      Logger::err("D3D9: User clip planes are unsupported by this Vulkan device");
      return D3DERR_INVALIDCALL;
    }""")
    replace_all("src/dxvk/dxvk_queue.cpp",
                       "        m_device->waitForIdle();",
                       '''#if defined(__ANDROID__)
        if (status == VK_ERROR_DEVICE_LOST) {
          SDL_LogError(SDL_LOG_CATEGORY_RENDER, "DXVK v1: Vulkan device lost in %s", __func__);
          std::_Exit(2);
        }
#endif
        m_device->waitForIdle();''', expected_count=2)


def patch_v1_android_build_integration():
    """Patch Meson and platform sources for the Android build."""
    replace("meson.build", "lib_sdl2    = dependency('SDL2')",
                       "lib_sdl2    = declare_dependency(include_directories: include_directories(get_option('android_sdl2_include')), link_args: ['-L' + get_option('android_sdl2_lib'), '-lSDL2'])" )
    replace("meson.build", "lib_vulkan  = dependency('vulkan')",
            "lib_vulkan  = dxvk_compiler.find_library('vulkan')")
    replace("meson.build", "dxvk_extradep = [ ]",
            """dxvk_extradep = [ ]
lib_swappy = declare_dependency(
  compile_args: ['-I' + get_option('android_swappy_include')],
  link_args: [get_option('android_swappy_lib'), '-landroid', '-llog'])""")
    replace_all("meson.build",
                "lib_vulkan  = dxvk_compiler.find_library('vulkan-1', dirs : dxvk_library_path)",
                "lib_vulkan  = dxvk_compiler.find_library('vulkan')", expected_count=2)
    replace("meson.build", "wrc = find_program('echo')",
                       "wrc = find_program('cmake', native: true)")
    replace("meson.build", "arguments : [ 'Ignoring: ', '@INPUT@' ]",
                       "arguments : [ '-E', 'touch', '@OUTPUT@' ]")
    replace("meson.build", "glsl_compiler = find_program('glslangValidator')",
                       "glsl_compiler = find_program('glslang', native: true)")
    replace("src/d3d9/meson.build", "dependencies        : [ dxso_dep, dxvk_dep, wsi_dep ],",
                       "dependencies        : [ dxso_dep, dxvk_dep, wsi_dep, lib_sdl2 ],")
    replace("src/d3d9/meson.build", "vs_module_defs      : 'd3d9'+def_spec_ext,",
                       "vs_module_defs      : 'd3d9'+def_spec_ext,\n  link_args           : ['-L' + get_option('android_sdl2_lib'), '-lSDL2'],")
    replace("src/dxvk/meson.build", "dependencies        : [ thread_dep, vkcommon_dep ] + dxvk_extradep,",
                       "dependencies        : [ thread_dep, vkcommon_dep, lib_sdl2 ] + dxvk_extradep,")
    replace("src/vulkan/meson.build",
            "vkcommon_deps = [ thread_dep, wsi_dep, lib_vulkan ]",
            "vkcommon_deps = [ thread_dep, wsi_dep, lib_vulkan, lib_sdl2, lib_swappy ]")
    replace("src/vulkan/meson.build",
            """vkcommon_dep = declare_dependency(
  link_with           : [ vkcommon_lib ],
  include_directories : [ dxvk_include_path ])""",
            """vkcommon_dep = declare_dependency(
  link_with           : [ vkcommon_lib ],
  dependencies        : [ lib_swappy ],
  include_directories : [ dxvk_include_path ])""")
    replace("src/dxvk/platform/dxvk_sdl2_exts.cpp",
                       "namespace dxvk {\n\n  DxvkPlatformExts DxvkPlatformExts::s_instance;",
                       "namespace dxvk {\n\n#if defined(__ANDROID__)\n  static SDL_Window* s_androidWindow = nullptr;\n\n  extern \"C\" void dxvkSetSdl2Window(SDL_Window* window) {\n    s_androidWindow = window;\n  }\n#endif\n\n  DxvkPlatformExts DxvkPlatformExts::s_instance;")
    replace("src/dxvk/platform/dxvk_sdl2_exts.cpp",
                       '''    SDL_Window* window = SDL_CreateWindow(
      "Dummy Window",
      SDL_WINDOWPOS_UNDEFINED,
      SDL_WINDOWPOS_UNDEFINED,
      1, 1,
      SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);''',
                       '''    SDL_Window* window = nullptr;
#if defined(__ANDROID__)
    if (s_androidWindow == nullptr)
      throw DxvkError("SDL2 WSI: Application window was not supplied on Android");
    window = s_androidWindow;
#else
    window = SDL_CreateWindow(
      "Dummy Window",
      SDL_WINDOWPOS_UNDEFINED,
      SDL_WINDOWPOS_UNDEFINED,
      1, 1,
      SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);
#endif''')
    replace("src/dxvk/platform/dxvk_sdl2_exts.cpp",
                       "    SDL_DestroyWindow(window);\n\n    return names;",
                       "#if !defined(__ANDROID__)\n    SDL_DestroyWindow(window);\n#endif\n\n    return names;")
    replace("src/d3d9/d3d9_main.cpp",
                       "#include \"../dxvk/dxvk_instance.h\"",
                       "#include <cstdio>\n#include \"../dxvk/dxvk_instance.h\"")
    replace("src/d3d9/d3d9_main.cpp",
                       """    dxvk::CreateD3D9(false, &pDirect3D);

    return pDirect3D;""",
                       """    try {
      dxvk::CreateD3D9(false, &pDirect3D);
    } catch (const dxvk::DxvkError& e) {
      std::fprintf(stderr, \"DXVK v1 Direct3DCreate9: %s\\n\", e.message().c_str());
    } catch (...) {
      std::fprintf(stderr, \"DXVK v1 Direct3DCreate9: unknown native exception\\n\");
    }

    return pDirect3D;""")
    # The fork assumes desktop D3D9 features and enables several Vulkan bits
    # unconditionally. Mobile Vulkan 1.1 drivers may expose valid D3D9 paths
    # without all of them; enable only what the adapter reports.
    for feature in ("depthClamp", "depthBiasClamp", "fillModeNonSolid",
                    "sampleRateShading", "shaderClipDistance",
                    "textureCompressionBC", "multiViewport", "independentBlend"):
        replace("src/d3d9/d3d9_device.cpp",
                           "enabled.core.features.%s = VK_TRUE;" % feature,
                           "enabled.core.features.%s = supported.core.features.%s;" % (feature, feature))
    replace("src/d3d9/d3d9_device.cpp", "DxvkDeviceFeatures enabled = {};",
                       """DxvkDeviceFeatures enabled = {};
    if (!supported.core.features.shaderInt64) Logger::warn(\"Android DXVK v1 missing feature: shaderInt64\");
    if (!supported.core.features.textureCompressionBC) Logger::warn(\"Android DXVK v1 missing feature: textureCompressionBC\");
    if (!supported.core.features.multiDrawIndirect) Logger::warn(\"Android DXVK v1 missing feature: multiDrawIndirect\");
    if (!supported.core.features.multiViewport) Logger::warn(\"Android DXVK v1 missing feature: multiViewport\");
    if (!supported.core.features.shaderClipDistance) Logger::warn(\"Android DXVK v1 missing feature: shaderClipDistance\");
    if (!supported.core.features.shaderCullDistance) Logger::warn(\"Android DXVK v1 missing feature: shaderCullDistance\");""")
    write_new("src/util/platform/util_env_android.cpp", r'''#include "../util_env.h"

#include <cerrno>
#include <filesystem>
#include <string>
#include <sys/prctl.h>
#include <unistd.h>

namespace dxvk::env {

std::string getExePath() {
  char path[4096] = { };
  const ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  return len > 0 ? std::string(path, size_t(len)) : std::string();
}

void setThreadName(const std::string& name) {
  std::string truncated = name.substr(0, 15);
  prctl(PR_SET_NAME, truncated.c_str(), 0, 0, 0);
}

bool createDirectory(const std::string& path) {
  std::error_code error;
  std::filesystem::create_directories(path, error);
  return !error;
}

}
''')
    write_new("src/util/platform/util_string_android.cpp", r'''#include "util_string.h"

#include <cstdint>
#include <string>

namespace dxvk::str {

std::string fromws(const WCHAR* ws) {
  if (!ws) return {};
  std::string out;
  for (const uint32_t* p = reinterpret_cast<const uint32_t*>(ws); *p; ++p) {
    uint32_t cp = *p;
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
    if (cp <= 0x7f) out.push_back(char(cp));
    else if (cp <= 0x7ff) { out.push_back(char(0xc0 | (cp >> 6))); out.push_back(char(0x80 | (cp & 63))); }
    else if (cp <= 0xffff) { out.push_back(char(0xe0 | (cp >> 12))); out.push_back(char(0x80 | ((cp >> 6) & 63))); out.push_back(char(0x80 | (cp & 63))); }
    else { out.push_back(char(0xf0 | (cp >> 18))); out.push_back(char(0x80 | ((cp >> 12) & 63))); out.push_back(char(0x80 | ((cp >> 6) & 63))); out.push_back(char(0x80 | (cp & 63))); }
  }
  return out;
}

void tows(const char* mbs, WCHAR* wcs, size_t wcsLen) {
  if (!wcs || !wcsLen) return;
  size_t out = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(mbs ? mbs : "");
  while (*p && out + 1 < wcsLen) {
    const unsigned char lead = *p++;
    int extra = lead < 0x80 ? 0 : lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
    uint32_t cp = extra == 0 ? lead : extra == 1 ? (lead & 0x1f) : extra == 2 ? (lead & 0x0f) : (lead & 7);
    if (extra && (!*p || (*p & 0xc0) != 0x80)) cp = 0xfffd, extra = 0;
    for (int i = 0; i < extra; ++i) { if ((p[i] & 0xc0) != 0x80) { extra = -1; break; } cp = (cp << 6) | (p[i] & 63); }
    if (extra < 0) { cp = 0xfffd; ++p; } else p += extra;
    wcs[out++] = static_cast<WCHAR>(cp > 0x10ffff ? 0xfffd : cp);
  }
  wcs[out] = 0;
}

}
''')
    write_new("src/util/platform/util_luid_android.cpp", r'''#include "../util_luid.h"

#include "../log/log.h"

namespace dxvk {
LUID GetAdapterLUID(UINT) {
  static bool warned = false;
  if (!warned) { Logger::warn("GetAdapterLUID: Android has no native LUID; using zero"); warned = true; }
  return LUID();
}
}
''')
    replace("src/util/meson.build", "elif dxvk_platform == 'darwin'\n  util_src += util_src_darwin\nelse",
                       "elif dxvk_platform == 'darwin'\n  util_src += util_src_darwin\nelif dxvk_platform == 'android'\n  util_src += ['platform/util_env_android.cpp', 'platform/util_string_android.cpp', 'platform/util_luid_android.cpp', 'platform/thread_native.cpp']\nelse")
    # The old fork has no android_sdl2_include option; add one for the SDL
    # headers supplied by the application and use it in its native dependency.
    replace("meson_options.txt", "option('enable_tests',", "option('android_sdl2_include', type: 'string', value: '')\noption('android_sdl2_lib', type: 'string', value: '')\noption('android_swappy_include', type: 'string', value: '')\noption('android_swappy_lib', type: 'string', value: '')\noption('enable_tests',")
if variant == "v2":
    patch_v2()
elif variant == "v2-xr":
    patch_v2()
    patch_v2_xr()
elif variant == "native":
    patch_v1()
