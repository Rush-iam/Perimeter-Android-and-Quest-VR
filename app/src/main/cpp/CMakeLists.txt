cmake_minimum_required(VERSION 3.22.1)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AndroidCompilerCache.cmake")
project("perimeter_android")

find_package(games-frame-pacing REQUIRED CONFIG)

# The launcher needs to query Vulkan before the SDL/game process is started.
# Keep this probe independent from the engine and load Vulkan dynamically so
# devices without a Vulkan loader can still use the GLES fallback.
add_library(perimeter_vulkan_probe SHARED AndroidVulkanCapabilities.cpp)
target_compile_features(perimeter_vulkan_probe PRIVATE cxx_std_17)
target_link_libraries(perimeter_vulkan_probe PRIVATE log dl)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
    "Disable FetchContent updates after the initial population")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AndroidDependencyChecks.cmake")

set(PERIMETER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../Perimeter")

# Configure core project options for Android. DXVK is required for the Android
# build; Sokol remains available as a fallback for presentation.
set(OPTION_FFMPEG_MOVIES ON CACHE BOOL "")
set(OPTION_SOKOL ON CACHE BOOL "" FORCE)
set(OPTION_SOKOL_API "GLES3" CACHE STRING "" FORCE)
option(PERIMETER_ANDROID_XR "Build the Quest Android XR variant" OFF)
option(PERIMETER_ANDROID_STACKTRACE
    "Build Android stacktrace support and fetch Boost headers" ON)
if(PERIMETER_ANDROID_STACKTRACE)
    set(OPTION_DISABLE_STACKTRACE OFF CACHE BOOL "" FORCE)
else()
    set(OPTION_DISABLE_STACKTRACE ON CACHE BOOL "" FORCE)
endif()
# Android never ships the desktop script compiler or its host-only script
# processing targets.  Keep this Android-only switch explicit so upstream
# desktop configurations retain their existing defaults.
set(PERIMETER_ANDROID_SKIP_DEVELOPMENT_TARGETS ON CACHE BOOL
    "Skip Android-inapplicable Perimeter development targets")

# Create a proxy for 'GL' so the core project doesn't need modification
# On Android, 'GL' (desktop) doesn't exist, we use GLESv3
add_library(GL INTERFACE)
target_link_libraries(GL INTERFACE GLESv3)

if(PERIMETER_ANDROID_STACKTRACE)
    # XTool uses Boost.Stacktrace header-only; compile it with the Android
    # toolchain rather than depending on ABI-specific Boost binaries. Keep the
    # complete header tree for its transitive dependencies, without configuring
    # Boost's libraries.
    FetchContent_Declare(
            boost_headers
            URL https://archives.boost.io/release/1.85.0/source/boost_1_85_0.tar.bz2
            URL_HASH SHA256=7009fe1faa1697476bdc7027703a2badb84e849b7b0baad5086b087b971f8617
            SOURCE_SUBDIR perimeter_headers_only
    )
    FetchContent_MakeAvailable(boost_headers)
    # Supply the core project's find_package(Boost) with the fetched headers.
    set(Boost_NO_BOOST_CMAKE ON)
    set(Boost_NO_SYSTEM_PATHS ON)
    set(Boost_INCLUDE_DIR "${boost_headers_SOURCE_DIR}" CACHE PATH "Boost headers" FORCE)
endif()

FetchContent_Declare(
        SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-2.30.7
        GIT_SHALLOW TRUE
)
FetchContent_Declare(
        SDL2_net
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_net.git
        GIT_TAG release-2.2.0
        GIT_SHALLOW TRUE
)
FetchContent_Declare(
        SDL2_mixer
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_mixer.git
        GIT_TAG release-2.8.0
        GIT_SHALLOW TRUE
)
FetchContent_Declare(
        SDL2_image
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
        GIT_TAG release-2.8.2
        GIT_SHALLOW TRUE
)

# Disable SDL2 features not needed for engine logic during initial build
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)

# SDL2_mixer options
set(SDL2_MIXER_SAMPLES OFF CACHE BOOL "")
set(SDL2_MIXER_MOD OFF CACHE BOOL "")
set(SDL2_MIXER_MIDI OFF CACHE BOOL "")
set(SDL2_MIXER_OPUS OFF CACHE BOOL "")
set(SDL2_MIXER_FLAC OFF CACHE BOOL "")

FetchContent_Populate(SDL2)
perimeter_android_verify_git_revision(SDL2 "${sdl2_SOURCE_DIR}"
    9519b9916cd29a14587af0507292f2bd31dd5752)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AndroidDxvk.cmake")
android_prepare_sdl("${sdl2_SOURCE_DIR}")

# SDL_net, SDL_mixer, and SDL_image consume the SDL2 target configured from
# the disposable patched tree above.
FetchContent_MakeAvailable(SDL2_net SDL2_mixer SDL2_image)
perimeter_android_verify_git_revision(SDL2_net "${sdl2_net_SOURCE_DIR}"
    669e75b84632e2c6cc5c65974ec9e28052cb7a4e)
perimeter_android_verify_git_revision(SDL2_mixer "${sdl2_mixer_SOURCE_DIR}"
    a37e09f85d321a13dfcf0d4432827ee09beeb623)
perimeter_android_verify_git_revision(SDL2_image "${sdl2_image_SOURCE_DIR}"
    abcf63aa71b4e3ac32120fa9870a6500ddcdcc89)

android_add_dxvk("${android_sdl_source_dir}/include" games-frame-pacing::swappy_static)
if(PERIMETER_ANDROID_XR)
    find_package(OpenXR REQUIRED CONFIG)
endif()
# The renderer compiles against DXVK's stable D3D9 headers only. The
# generation-specific DSOs are packaged by android_dxvk_runtime and loaded
# by AndroidDxvkLoader after the engine command line is initialized.
set(PERIMETER_DXVK_D3D9_TARGET AndroidDxvk::Headers)
set(OPTION_D3D9 ON CACHE BOOL "" FORCE)

# FFmpeg Prebuilts for Android
FetchContent_Declare(
        ffmpeg_prebuilt
        GIT_REPOSITORY https://github.com/hzw1199/Android-FFmpeg-Prebuilt.git
        GIT_TAG 90231cc0105aef4f76926b911535f5eb73511b86
)
FetchContent_GetProperties(ffmpeg_prebuilt)
if(NOT ffmpeg_prebuilt_POPULATED)
    FetchContent_Populate(ffmpeg_prebuilt)
endif()

# The repo has versioned folders
set(FFMPEG_ROOT ${ffmpeg_prebuilt_SOURCE_DIR}/ffmpeg-9.0)
set(FFMPEG_DIR ${FFMPEG_ROOT})

# Define the unified FFmpeg library for Android
add_library(ffmpeg SHARED IMPORTED)
set_target_properties(ffmpeg PROPERTIES
        IMPORTED_LOCATION ${FFMPEG_ROOT}/libffmpeg.so
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_ROOT}/include"
)

# Include the core project
# We use EXCLUDE_FROM_ALL to prevent building desktop targets not needed for Android
add_subdirectory(${PERIMETER_ROOT} perimeter_engine EXCLUDE_FROM_ALL)
target_sources(Sound PRIVATE AndroidSoundLoader.cpp)

# The DXVK header staging tree is replaced when its patch changes. Render
# compiles against those generated headers, so it must wait for that refresh.
add_dependencies(Render android_dxvk_patch_v2)
add_dependencies(perimeter ${ANDROID_DXVK_RUNTIME_TARGET})

# RenderSokol compiles independently from the final shared library and needs
# Swappy's public headers while building its Android GLES presentation path.
target_link_libraries(RenderSokol PRIVATE games-frame-pacing::swappy_static)

# Core static libraries compile independently, so they cannot inherit SDL usage
# requirements from the final perimeter shared-library link. Keep those
# requirements scoped to the Android engine targets instead of restoring
# wrapper-global include directories. SDL 2 generates/copies its public headers
# at build time; make every SDL consumer wait for that complete header set before
# compiling, rather than merely before SDL itself links.
add_library(perimeter_android_sdl INTERFACE)
target_link_libraries(perimeter_android_sdl INTERFACE
        SDL2::SDL2
        SDL2_net::SDL2_net
        SDL2_mixer::SDL2_mixer
        SDL2_image::SDL2_image)

# Keep the release optimization experiment Android-owned. The upstream engine
# supplies the O2 baseline through the Android toolchain and its own release
# settings; O3 is appended only when explicitly selected so it overrides that
# baseline without changing the upstream project.
set(PERIMETER_ANDROID_OPTIMIZATION "O2" CACHE STRING
        "Android native optimization level (O2 or O3)")
set_property(CACHE PERIMETER_ANDROID_OPTIMIZATION PROPERTY STRINGS O2 O3)
if(NOT PERIMETER_ANDROID_OPTIMIZATION STREQUAL "O2" AND
   NOT PERIMETER_ANDROID_OPTIMIZATION STREQUAL "O3")
    message(FATAL_ERROR
            "PERIMETER_ANDROID_OPTIMIZATION must be O2 or O3, got '${PERIMETER_ANDROID_OPTIMIZATION}'")
endif()
add_library(perimeter_android_optimization INTERFACE)
if(PERIMETER_ANDROID_OPTIMIZATION STREQUAL "O3")
    target_compile_options(perimeter_android_optimization INTERFACE
            "$<$<COMPILE_LANGUAGE:C,CXX>:-O3>")
endif()

# Keep ThinLTO optional and Android-owned. Only the engine targets below are
# included; third-party and prebuilt dependencies remain ordinary native
# libraries and are not rebuilt with LTO.
option(PERIMETER_ANDROID_THIN_LTO
        "Enable ThinLTO for Android-owned engine targets" OFF)
add_library(perimeter_android_thin_lto INTERFACE)
if(PERIMETER_ANDROID_THIN_LTO)
    target_compile_options(perimeter_android_thin_lto INTERFACE
            "$<$<COMPILE_LANGUAGE:C,CXX>:-flto=thin>")
    target_link_options(perimeter_android_thin_lto INTERFACE
            "-flto=thin")
endif()

set(_perimeter_android_native_targets
        perimeter
        XTool
        XPrmLib
        PluginMAX
        Terra
        tx3d
        Render
        RenderSokol
        Sound
        Network
        Util
        Units
        UserInterface
        Physics
        AI
        GeoLogic
        HT
        Integrations
        Exodus)

# Keep the generated-header ordering edge in this directory. CMake's Ninja
# generator otherwise weakens a cross-directory dependency on SDL's utility
# target to a directory-level order-only edge for static-library objects.
add_custom_target(perimeter_android_sdl_headers
        DEPENDS sdl_headers_copy)
foreach(_perimeter_android_sdl_consumer IN LISTS _perimeter_android_native_targets)
    target_link_libraries(${_perimeter_android_sdl_consumer} PRIVATE
            perimeter_android_sdl)
    target_link_libraries(${_perimeter_android_sdl_consumer} PRIVATE
            perimeter_android_optimization)
    target_link_libraries(${_perimeter_android_sdl_consumer} PRIVATE
            perimeter_android_thin_lto)
    add_dependencies(${_perimeter_android_sdl_consumer}
            perimeter_android_sdl_headers)
    if(PERIMETER_ANDROID_XR)
        # XR adds fields to shared engine classes such as cCamera and
        # GameShell. Keep their layouts consistent in every engine target.
        target_compile_definitions(${_perimeter_android_sdl_consumer} PRIVATE
                ANDROID_XR=1)
    endif()
endforeach()
unset(_perimeter_android_sdl_consumer)
unset(_perimeter_android_native_targets)

# Add Android-specific functions to the engine target
target_sources(perimeter PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/AndroidLog.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/AndroidFrameTiming.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/AndroidTouchInput.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/AndroidDxvkLoader.cpp")
target_link_libraries(perimeter PRIVATE AndroidDxvk::Headers)
if(PERIMETER_ANDROID_XR)
    target_sources(perimeter PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/AndroidXrBootstrap.cpp")
    target_link_libraries(perimeter PRIVATE OpenXR::openxr_loader)
    target_link_libraries(perimeter PRIVATE vulkan)
    target_include_directories(perimeter PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/android-dxvk-v2/patched-source/src/d3d9")
    target_include_directories(perimeter PRIVATE
        "${PERIMETER_ROOT}/Source/Render/D3D")
endif()
target_link_libraries(perimeter PRIVATE dl)
target_include_directories(perimeter PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(Render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(RenderSokol PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(UserInterface PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Link the core engine library and dependencies
target_link_libraries(
        perimeter PRIVATE
        ffmpeg
        games-frame-pacing::swappy_static
        android
        EGL
        log)
