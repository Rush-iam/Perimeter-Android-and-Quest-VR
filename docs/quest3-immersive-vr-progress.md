# Quest 3 immersive VR port progress

Started 2026-09-23. This document tracks implementation and measured results for the [Quest 3 immersive VR port plan](quest3-immersive-vr-port-plan.md). The plan owns the architecture, scope, and completion gates; this document records what has actually been built, observed, and decided. A planned gate is not complete until its evidence is linked here.

## Current position

Phase 0 done. Phase 1 todo. Update this block with current progress.

## Source plans and related work

| Document | Role in this effort |
| --- | --- |
| [Quest 3 immersive VR port plan](quest3-immersive-vr-port-plan.md) | Primary architecture, phase gates, XR/DXVK bridge, camera, input, lifecycle, and performance goals. |
| [Quest 3 Phase 1 reproduction notes](quest3-phase1-reproduction-notes.md) | Practical build, launch, Quest verification, diagnostic sweep, obstacles, and fixes for handing Phase 1 to another developer. |

## Phase tracker

Status values: **not started**, **in progress**, **blocked**, **complete**. Update a phase only after linking the relevant implementation and evidence below.

| Phase | Status | Evidence needed for completion |
| --- | --- | --- |
| 0. Headset feasibility | Complete | Quest build; shared OpenXR/DXVK 2 Vulkan device; distinct tracked D3D9 eye images; safe image synchronization and session stop/restart; capability and transfer-timing report. Real runtime loss is deferred to Phase 6. |
| 1. Engine stereo foundation | Not started | Small tracked stereo scene, correct asymmetric frusta, once-per-frame state updates, and preserved mono behavior; user confirmed the terrain looks correct during the dynamic Quest camera sweep. |
| 2. First playable tabletop | Not started | Fresh Tutorial playable in 6DoF with controllers, selection/move/build, UI panel, and stable terrain in both eyes. |
| 3. Main immersive god mode | Not started | Tutorial completed with immersive navigation and comfort defaults. |
| 4. Complete RTS usability | Not started | Representative mission completed entirely in-headset, including strategic controls and save/load. |
| 5. Sustained performance | Not started | Representative prolonged Quest runs meet a measured refresh target with stable memory and acceptable frame tails. |
| 6. Release hardening | Not started | Cold launch, resume, relaunch, content setup, and gameplay flows verified in a release artifact. |

The precise gates and ordering are in the [delivery sequence](quest3-immersive-vr-port-plan.md#delivery-sequence-and-gates). Performance observations should be collected from phase 0 onward, even though sustained performance is phase 5.

## Phase 0 work items

| ID | Status | Deliverable | Evidence to record |
| --- | --- | --- | --- |
| XR-01 | Complete | Quest launch configuration and Android OpenXR loader initialization; enumerate runtime, system, extensions, graphics requirements, formats, and recommended eye sizes. | `:xrSmoke:assembleDebug` succeeded; Quest report below includes a Vulkan 1.3 graphics session and swapchain formats. |
| XR-02 | Complete | Android-only DXVK 2 bootstrap that creates Vulkan instance/device through `XR_KHR_vulkan_enable2` and selects the runtime's physical device. | Quest run below: D3D9 `CreateDevice` and `xrCreateSession` succeeded with the same Vulkan handles. XR build option packages DXVK 2 only. |
| XR-03 | Complete | Render two distinct D3D9 eye targets and transfer them on-GPU to acquired OpenXR swapchain images. | Quest accepted two-color D3D9-to-Vulkan copies and stereo projection layers at both tested sizes; user confirmed correct left/right appearance and stability under head movement at both sizes. Timings, copy-tail attribution, and synchronization/failure-path review below. This is a graphics bridge gate, not engine stereo gameplay. |
| XR-04 | Complete for Phase 0 | Exercise focus, pause/resume, headset removal, session stop, and repeated launch/teardown. | Same-process relaunch, Home-exit, headset-removal/session-resume, synthetic-loss teardown, and a 60-second run followed by same-PID relaunch are below. Real session/instance loss is a separate Phase 6 hardening requirement, not a Phase 0 gate. |
| XR-05 | Complete | Summarize feasibility and decide the bridge needed for Phase 1. | Use the measured DXVK-to-OpenXR GPU copy as the Phase 1 reference bridge; default non-XR DXVK Smoke and main-app releaseBenchmark builds pass. The prototype uses teardown/relaunch on loss; actual loss remains untested. |

Phase 0 is complete only when a real Quest 3 displays distinct D3D9 images in both eyes through an OpenXR session, head tracking changes the submitted views, and the session can stop and restart without stale GPU resources. Record failures as findings rather than marking a work item complete.

## XR-03 synchronization contract

The current smoke bridge uses a single DXVK/OpenXR graphics queue and one command buffer at a time. DXVK clears both D3D9 eye textures, transitions their backing images to `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL`, flushes its command stream, and waits for both resources before external Vulkan access. The targets remain in transfer-source layout during the probe and return to their original DXVK layout before release.

For each requested frame, OpenXR acquisition is serialized with DXVK's submission lock, and the probe waits for each image before writing. Only `XR_SUCCESS` permits the copy; a positive-but-nonready or loss-pending result exits the frame path. Vulkan barriers move each runtime-owned destination from color-attachment layout to transfer-destination layout, copy the two images, then restore color-attachment layout. Queue submission is locked against DXVK and OpenXR queue users, and a fence completes before image release. The probe calls no D3D9 method while holding the submission lock. `xrBeginFrame`, `xrEndFrame`, `xrAcquireSwapchainImage`, and `xrReleaseSwapchainImage` all use that lock because the [OpenXR Vulkan binding permits those calls to access the bound queue](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_vulkan_enable-concurrency.html). The [runtime's Vulkan image-layout contract](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_vulkan_enable-swapchain-image-state.html) requires the destination to be color-attachment-compatible again at release.

On abnormal exit, the probe waits for the graphics queue to become idle before destroying resources, then best-effort waits/releases any tracked eye-image lease. Teardown requests a finite image-wait timeout rather than an infinite one; the runtime may still block longer under contention. The injected failure validates the waited-image cleanup path; true device loss, a runtime loss event, and a failure after queue submission remain untested. A production bridge will need explicit recovery policy and should avoid the smoke probe's per-frame CPU fence wait once GPU dependencies are proven.

Session or instance loss is a separate, non-passing outcome. A loss-pending event or loss result stops the probe and follows the existing resource teardown path; it does not call `xrEndSession` or attempt to reuse the lost session. The tested synthetic switch takes that path after a complete frame, then requires activity relaunch. This conservative policy follows the [OpenXR session-state guidance](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrSessionState.html): a truly lost session's handles become unusable and recreation may require a new system and graphics resources. The synthetic test validates local control flow only, not runtime behavior during actual loss.

## Decisions and blockers

Record decisions only after the supporting evidence is available. In particular, phase 0 should settle whether the pinned DXVK 2 build and Quest runtime can share a Vulkan device, whether an SDL window can remain unused during XR rendering, what the GPU transfer costs, and whether the current SDL activity is suitable for XR lifecycle handling. The [plan's remaining decisions](quest3-immersive-vr-port-plan.md#decisions-remaining-after-the-prototype) also include direct image wrapping, terrain scope, and world scale.

| Date | Decision or blocker | Evidence | Follow-up |
| --- | --- | --- | --- |
| 2026-09-23 | The runtime's Vulkan 1.2 maximum tested version did not prevent Vulkan 1.3 creation on this Quest 3; the selected GPU reported Vulkan 1.3.295. | XR-01 Quest report and [Khronos graphics-requirements definition](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrGraphicsRequirementsVulkanKHR.html). | Check the full DXVK 2 device requirements in XR-02. |
| 2026-09-23 | Pinned DXVK 2 can create its Vulkan instance/device through the Quest runtime and share them with an XR graphics session. This resolves the startup compatibility question for this tested headset/runtime. | XR-02 `dxvk-smoke.txt` report: D3D9 `CreateDevice` and `xrCreateSession` both succeeded. | Begin XR-03 image transfer and frame-loop work; verify synchronization, swapchain formats, and repeatable session lifecycle separately. |
| 2026-09-23 | Use the OpenXR-mediated DXVK 2 device plus explicit GPU transfer as the phase-1 reference bridge. Distinct D3D9 eyes, headset presentation, and session stop/resume work on this Quest 3. This is a prototype decision, not a full-game performance claim. | XR-02/03/04 Quest reports, user visual checks at both sizes, and the synchronization review above. The recommended-size two-eye copy section had ~0.123 ms P50 and ~0.82 ms P95 in repeated runs; D3D9 scene rendering and whole-frame timing were not measured. | Integrate the bridge behind a Quest-only build path. Keep direct OpenXR-image wrapping as a later measured optimization; do not alter ordinary Android or PC rendering to support the smoke test. |

## Reproduce the XR-01 probe

Build with `:xrSmoke:assembleDebug` using the Studio-compatible Gradle user home. Install `xrSmoke/build/outputs/apk/debug/xrSmoke-debug.apk`, then launch `com.queststoredb.perimeter.xrsmoke/.ProbeActivity` on the Quest 3. The activity displays the report and writes `files/xr-capabilities.txt` in its app data. Retrieve it with `adb shell run-as com.queststoredb.perimeter.xrsmoke cat files/xr-capabilities.txt`. The probe creates and destroys a basic Vulkan graphics session; it does not create swapchain images or present to the headset.

## Reproduce the XR-03 visual check

Build `:dxvkSmoke:assembleDebug -PdxvkSmokeXr=true` using the Studio-compatible Gradle user home, install `dxvkSmoke/build/outputs/apk/debug/dxvkSmoke-debug.apk`, and launch `com.queststoredb.perimeter.dxvksmoke/.SmokeActivity` while wearing the Quest. The test submits a stereo projection layer for about ten seconds, with a red D3D9 target for the left eye and a blue target for the right. Record whether both colors appear in the expected eyes, whether either image is inverted or channel-swapped, and whether the compositor remains stable while moving the head. Retrieve the native report with `adb shell run-as com.queststoredb.perimeter.dxvksmoke cat files/dxvk-smoke.txt`. The probe is not gameplay; it draws static solid-color D3D9 targets at the runtime's recommended eye extents.

To repeat the control-flow-only loss check, launch with `adb shell am start -n com.queststoredb.perimeter.dxvksmoke/.SmokeActivity --ei xr_simulate_session_loss_after_frames 120`. The report must say `SIMULATED LOSS` and `INCOMPLETE`, not `PASS`; verify successful teardown, then launch normally and verify a completed probe. This switch never causes a real OpenXR or Vulkan loss.

For a longer headset-removal/focus test, launch with `adb shell am start -n com.queststoredb.perimeter.dxvksmoke/.SmokeActivity --ei xr_probe_seconds 30`, remove the headset briefly, then put it back on before the run ends. Record whether visuals resume or the activity exits, and retrieve the report immediately afterward. The optional duration accepts 1–60 seconds; do not treat Quest Home as an in-place pause because it destroys this activity on the tested runtime.
