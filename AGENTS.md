# Perimeter Android - Project Context

## Project Goals
- **Primary Goal:** Port the classic C++ RTS game **Perimeter** (originally Windows/Linux) to Android.
- **Status:** Currently in the initial phase of getting a working Android build.
- **Future Goal:** Port to **Quest 3 VR Standalone**. Architectural decisions should favor abstractions that can eventually support VR/XR.

## Project Structure
- `/app`: The Android wrapper application (Kotlin/Java & JNI).
- `/Perimeter`: The core C++ engine (Upstream repository). **IMPORTANT:** Minimize changes here to allow for clean upstream merges.
- `/app/src/main/cpp`: Android-specific native code, CMake configuration, and JNI bridges.
- `/docs`: Design, plan, progress, and investigation documents; see the Documentation Map.
- `/scripts/frame-pacing`: PowerShell capture and analysis scripts for the frame-pacing workflow.
- `/dxvkSmoke`, `/xrSmoke`: Standalone DXVK and OpenXR verification apps; not shipped and not covered by the flavor tasks above.
- `/captures`, `/.tmp`: Generated working artifacts. Both are gitignored and must never be committed.

### Git Submodule and Upstream Sync
- `/Perimeter` is a Git submodule pointing at the `Rush-iam/Perimeter` fork (see `.gitmodules`), not vendored source. On a fresh clone run `git submodule update --init --recursive` before building; an empty `/Perimeter` directory means the submodule is not initialized.
- Make engine commits on the submodule's `android` branch only, and never rewrite or rebase its published history.
- The fork is the integration point for upstream merges: the original KD-lab `Perimeter` project is the upstream remote, and upstream releases are merged inside the fork so the `android` branch carries the Android changes on top. Keeping `/Perimeter` edits additive and guarded is what keeps that merge clean.

## Documentation Map
- `/docs` holds the project's design and evidence record. Treat these documents as the source of truth for architecture and status, and update the relevant document when a change affects it.
- `quest3-immersive-vr-port-plan.md` is the Quest 3 VR architecture, scope, and phase-gate plan; `quest3-immersive-vr-progress.md` tracks what has actually been built, observed, and decided, and owns the phase tracker. A planned gate is not complete until its evidence is linked in the progress document.
- `android-release-performance-plan.md` covers sustained Android release performance; `dxvk1-frame-pacing-plan.md` and `dxvk1-frame-pacing-benchmark.md` cover the frame-pacing work and its reproducible results; `dxvk1-d60-performance-benchmark.md` defines the DXVK 1 performance-mode matrix.
- `android-dxvk-memory-investigation.md` records the DXVK memory findings; `possible-render-release-bugs.md` lists candidate debug/release rendering differences.
- Terrain rendering has a three-document set: `terrain-lod-design-analysis.md` (problem analysis), `terrain-lod-reimplementation-plan.md` (reimplementation decision), and `tilemap-rebuild-stepping-plan.md` (rebuild-work reduction goal).

## Build & Run
- **Build System:** Gradle (for Android) + CMake (for Native Engine).
- **Gradle Tasks:**
  - `./gradlew :app:assembleMobileReleaseBenchmark`: Build the release-optimized,
    diagnostics-accessible development APK.
  - `./gradlew :app:assembleMobileRelease`: Build the production Android APK.
  - `./gradlew :app:installMobileReleaseBenchmark`: Install the Android development APK.
  - `./gradlew :app:assembleQuestRelease`: Build the production Quest VR APK.
  - `./gradlew :app:installQuestReleaseBenchmark`: Install the Quest VR development APK.
- Android and Quest VR are separate `mobile` and `quest` product flavors. The
  Quest flavor is VR-only: it compiles with `ANDROID_XR`, includes OpenXR, and
  initializes XR without a `quest_vr` runtime argument. Keep XR dependencies,
  manifest entries, CMake options, and native code isolated to that flavor.
- **ABI Filter:** Currently targeting `arm64-v8a`.
- **Min SDK:** 29 (Android 10).
- **Target SDK:** 37 (Android 15).

### Additional Gradle Modules
- `:dxvkSmoke` and `:xrSmoke` (see `settings.gradle.kts`) are standalone verification apps that exercise the DXVK and OpenXR paths independently of the game. They are not shipped and are not covered by the flavor tasks above.
- Build the DXVK smoke app with `./gradlew :dxvkSmoke:assembleDebug`; add `-PdxvkSmokeXr=true` to build its XR probe with `ANDROID_DXVK_XR=ON`.
- Build the XR smoke app with `./gradlew :xrSmoke:assembleDebug`.
- Both smoke modules pin `ndkVersion = "28.2.13676358"`; `:app` pins no NDK version and uses the Android Gradle Plugin default.

### App Identifiers and Entry Activities
- The application ID is `com.queststoredb.perimeter`. The `quest` flavor appends `.quest` and the `debug` and `releaseBenchmark` build types append `.debug`, so identifiers differ per variant (for example `com.queststoredb.perimeter.quest.debug`).
- `ContentActivity` is the shared entry activity. The launcher aliases are `.MobileActivity` (mobile) and `.QuestVrActivity` (Quest); the `quest` flavor additionally declares `.QuestGameActivity` and advertises `com.oculus.intent.category.VR`.
- Launch a specific activity by component for automation, for example `adb shell am start -n com.queststoredb.perimeter/.ContentActivity`.

### Android Studio-compatible Gradle builds
- To reuse Android Studio's Gradle and native CMake caches, set
  `GRADLE_USER_HOME` to `%USERPROFILE%\.gradle`; do not use the repository's
  `.gradle` directory for these builds.
- Preserve the project compiler-cache setting from `gradle.properties`
  (`androidCompilerCache=CCACHE`). Do not pass
  `-PandroidCompilerCache=OFF`, change `androidCompilerCacheDir`, or otherwise
  change the cache mode when matching Android Studio's native build.
- Ensure the host-installed `ccache` executable is available on `PATH` before
  building. The matching native configuration is keyed by the compiler-cache
  mode and directory, so changing either can trigger a complete CMake/DXVK
  rebuild even when Android Studio built the same variant recently.
- Use the normal Android Studio-compatible Release command for the release
  performance baseline:
  ```powershell
  $env:GRADLE_USER_HOME = Join-Path $env:USERPROFILE '.gradle'
  .\gradlew.bat "-Duser.home=$env:USERPROFILE" --no-daemon :app:assembleMobileRelease --console=plain
  ```
- Android native optimization defaults to `O2`. Build the Android-scoped O3
  candidate explicitly with `-PandroidOptimization=O3`; do not change the
  default when building the control. Pass `-Optimization O3` to
  `Capture-FramePacingRun.ps1` so the candidate level is recorded in capture
  metadata.
- ThinLTO is disabled by default. Build the Android-scoped ThinLTO candidate
  explicitly with `-PandroidThinLto=ON`; pass `-ThinLto ON` to
  `Capture-FramePacingRun.ps1` so the candidate state is recorded in capture
  metadata.
- If the Codex sandbox cannot execute the installed NDK toolchain or access
  `%USERPROFILE%\.gradle` / `%USERPROFILE%\.android`, rerun this same command
  with elevated execution. Do not switch to a repository-local Gradle cache or
  debug keystore merely to avoid the permission boundary.

### Gradle from the Codex Sandbox
- The sandbox PowerShell environment may expose Java's `user.home` as the filesystem root. Use the repository cache and quote the Java user-home argument so the Windows Gradle wrapper passes it through correctly:
  ```powershell
  $env:GRADLE_USER_HOME = Join-Path (Get-Location) '.gradle'
  .\gradlew.bat "-Duser.home=$env:USERPROFILE" --no-daemon :app:assembleMobileReleaseBenchmark --console=plain
  ```
- For a connected-device Android install, use the same environment and replace `:app:assembleMobileReleaseBenchmark` with `:app:installMobileReleaseBenchmark`. Use `:app:installQuestReleaseBenchmark` for the VR-only Quest flavor. Set `$env:HOME = $env:USERPROFILE` before invoking `adb` if it reports `Cannot mkdir '\.android': Permission denied`.
- Leave `ANDROID_USER_HOME` and `ANDROID_SDK_HOME` unset. `-Duser.home=$env:USERPROFILE` keeps Android tooling and debug signing pointed at the normal `%USERPROFILE%\.android` location; do not redirect them to a repository or sandbox-local keystore.
- If the sandbox denies access to `%USERPROFILE%\.android`, rerun the same Gradle command with elevated execution rather than redirecting Android's user directory or debug keystore.

### Signing and Device Installs
- Use Android Studio's default debug keystore at `%USERPROFILE%\.android\debug.keystore` for releaseBenchmark APKs installed on the configured Android device. The Quest flavor's debug benchmark variant is `questReleaseBenchmark`.
- The debug key alias is `androiddebugkey`. Keep the keystore outside the repository; do not substitute a repository-local or sandbox-local debug keystore.
- Do not set `ANDROID_USER_HOME` to a repository-local path when building or installing. The Android Gradle plugin derives the default debug keystore from that location, which would silently select `.android\debug.keystore` inside the repository and produce an APK that cannot update the Android Studio-signed install.
- If an isolated Gradle cache is needed, set `GRADLE_USER_HOME` only; leave `ANDROID_USER_HOME` unset so signing resolves to `%USERPROFILE%\.android\debug.keystore`.
- In shells where `adb` reports `Cannot mkdir '\.android': Permission denied`, the shell's `HOME` is empty. Set it from the Windows user profile before using `adb`: `$env:HOME = $env:USERPROFILE`. This keeps the device-auth directory at `%USERPROFILE%\.android`; do not point it at the repository. `ANDROID_USER_HOME` and `ANDROID_SDK_HOME` are not reliable fixes for this `adb` error.
- `assembleMobileRelease` and `assembleQuestRelease` are signed with the release keystore configured through `local.properties` (`RELEASE_STORE_FILE`, `RELEASE_STORE_PASSWORD`, `RELEASE_KEY_ALIAS`, `RELEASE_KEY_PASSWORD`). Keep that keystore outside the repository; the properties file is gitignored. Supply all four values, or the production release build type is unsigned.
- The `releaseBenchmark` and `debug` variants use the debug keystore and the `.debug` application ID suffix instead, so they install alongside a production release build.

## Coding Standards & Guidelines
### Native Code (C++)
- **Standard:** C++17.
- **Minimal Invasive Changes:** Do not modify core logic in `/Perimeter` unless absolutely necessary. Use `Exodus` (the engine's platform abstraction layer) or add Android-specific files in `/app/src/main/cpp` (do not prefix names with "Exodus").
- **Platform Abstraction:** Use SDL2 for input, sound, and networking where possible.
- **Graphics:** Prefer **DirectX via DXVK** (primary path); **Sokol** is used only as a fallback.
- **Logging:** Use the Android log redirector (see `AndroidLog.cpp`) for native logs.
- **XR isolation:** Guard every XR-specific change in `/Perimeter` and
  `/app/src/main/cpp` with `#if defined(ANDROID_XR)`. Do not use `__ANDROID__`
  alone for XR code: ordinary Android builds must not compile or execute XR
  paths. Define `ANDROID_XR` only for the XR-enabled native targets, and add
  XR-only sources, headers, dependencies, and link settings conditionally.

### Android Development
- **UI:** The wrapper uses standard Android Views/Activity for now; Jetpack Compose is preferred for any new Android-specific UI.
- **Namespace:** `com.queststoredb.perimeter`.

### Repository Conventions
- The engine version is parsed from `Perimeter/CMakeLists.txt` (`project(perimeter VERSION ...)`) into `BuildConfig.PERIMETER_VERSION`; keep the two in agreement when the engine version changes.
- There is no committed formatter or lint configuration (no `.clang-format` or `.editorconfig`). Match the surrounding file's existing formatting and line endings instead of introducing a new style, and do not reformat untouched lines.

### Frame-Pacing Benchmark Workflow
- Use the Android Studio-compatible `mobileReleaseBenchmark` build for release
  performance captures and the established scripts under `scripts/frame-pacing/`.
  `mobileReleaseBenchmark` is initialized from `Release` and is debuggable only so
  `run-as` can retrieve native timing files; do not treat the ordinary
  non-debuggable `Release` APK as capture-ready.
- After restarting `ContentActivity` with the mobileRelease/mobileReleaseBenchmark build, wait **2 seconds** for the launcher, tap **Play**, then wait **7 seconds after that tap** for the game main menu. Do not run `Start-TutorialBenchmark.ps1` while the launcher is still visible.
- Obtain the current **Play** button bounds from `adb shell uiautomator dump /data/local/tmp/perimeter-ui.xml` before an automated tap, and tap their center. The launcher can appear in portrait or landscape, so fixed screen coordinates are unreliable.
- Start `Start-TutorialBenchmark.ps1` from the game's top menu with `-InitialMenuSeconds 0 -TransitionSeconds 2`; use the established mission-load wait and verify the gameplay HUD before capture. The script's menu navigation must not begin from the launcher or a submenu.
- `Capture-FramePacingRun.ps1` defaults to a **10-second warm-up**. The canonical controlled zoom case is 1.5 seconds out, 1.5 seconds in, repeated 21 times for a 63-second window; capture it from a fresh Tutorial mission. The seven-cycle form remains a short diagnostic.
- The standard warm-up then applies a 3-second all-core load pulse followed by
  a **2-second post-load settling wait** before the camera measurement. Earlier
  captures using the 250 ms gap remain valid historical results.
- Record native DXVK timing and, when analyzing presentation, run the passive SurfaceFlinger sampler concurrently.
- `Capture-FramePacingRun.ps1` writes runs under `captures/frame-pacing/` by default (`-OutputRoot`), and `captures/` is gitignored. Keep capture output and other working artifacts, including commit-plan documents, under the gitignored `captures/` or `.tmp/` directories rather than in tracked paths.

### Commit Plan Workflow
- The commit title and description conventions below apply to every commit created in this repository, including direct commit requests that do not use a commit-splitting plan.
- When asked to prepare a commit-splitting plan, inspect the Git repository named by the user. If the path is a nested repository or submodule, run Git from that repository's root; do not substitute the parent repository.
- Respect the requested change state exactly: use `git diff --cached` for staged changes and `git diff` for unstaged tracked changes. Do not stage, unstage, commit, or otherwise alter the index while preparing the plan.
- If the requested repository/state has no changes, report that fact instead of analyzing another repository or change state.
- Write or update a plan document in the inspected repository, with proposed commits grouped and sorted by category. Use these category icons consistently: `🛠️` build, `🐛` bugfix, `🕹️` controls, `🎨` graphics, `📝` logging, `🚀` optimization, `♻️` refactoring, `🧹` cleanup, `📚` docs, `🧪` testing, `🧰` tooling, and `📊` benchmark.
- Review every proposed commit for correctness, regressions, and bugs before presenting the plan. Inspect the complete diff assigned to each commit, including interactions across commit boundaries and nested repositories; record findings in the plan and revise the proposed boundaries or descriptions when needed.
- Store commit-splitting plan documents in the main repository's `.tmp/` directory, including plans for nested repositories; they are working artifacts and must never be included in a proposed commit or committed to a repository.
- Prefix every title with its category icon and lowercase category tag in square brackets, for example `📚 [docs] Add user-facing documentation`. Capitalized `[Android]` and `[VR]` are additional scope tags, not replacements for the category tag. For Android-specific commits inside a mixed-scope repository, including the core `/Perimeter` project, put `[Android]` after the category tag, for example `🕹️ [controls] [Android] Add Android-only controls`. Commits made in this repository's root (the main Android project), including commits that only update a submodule gitlink, must omit `[Android]`. Add `[VR]` after the category tag only when every change in the commit is confined to the VR implementation, tooling, or documentation and does not affect standard Android builds or the core PC project, for example `🧰 [tooling] [VR] Add Quest OpenXR capability probe`. If both scope tags apply, order them `[Android] [VR]`.
- Make every commit description standalone and describe the completed change. Do not include instructions about what to perform, test, stage, validate, or fix, and do not make a commit description depend on another commit's description.
- Write each commit description in imperative mood, beginning with a command such as `Add`, `Update`, `Remove`, or `Fix` rather than a third-person form such as `Adds` or `Updates`.
- In the plan document, make the first paragraph under each proposed commit the exact standalone commit description; do not add a label such as `Standalone description:` before it.
- Include file and line-number references for every proposed commit. Split mixed-purpose files at hunk level, and exclude whitespace-only or no-op changes from functional commits.
- Keep dependency-aware ordering, cross-repository prerequisites, and review notes outside commit descriptions. Treat the described changes as already tested unless the user explicitly asks for a testing plan.

#### Approved Plan Commit Process
- Once the user approves the plan, recheck the inspected repository from its own root: confirm the intended branch, review the current worktree and index, and reconcile any changes made since the plan was prepared. Do not assume the plan is still an exact match.
- Commits in the core `/Perimeter` submodule must always be made on its `android` branch. Confirm that branch before committing and stop if the submodule is on another branch.
- For other repositories, commit only on the branch explicitly requested by the user. Do not switch branches, push, or modify the parent repository unless the user requests it.
- Follow the plan's dependency-aware order. For each commit, stage only the listed files and hunks; preserve unrelated staged, unstaged, untracked, whitespace-only, and no-op changes.
- Before each commit, inspect `git diff --cached --stat`, `git diff --cached --name-only`, and `git diff --cached`. Run `git diff --cached --check` with the repository's line-ending configuration so existing CRLF files are not rewritten merely to satisfy the check.
- Use the approved title and standalone description from the plan verbatim unless the user explicitly requests a description change. Create one commit at a time and verify its result before proceeding to the next.
- After the final commit, verify the branch, recent commit order, staged state, and worktree status. Report any intentionally preserved changes and confirm that nothing was pushed unless pushing was explicitly requested.

## Architecture Notes
- The engine is integrated as a library via `add_subdirectory(${PERIMETER_ROOT})` in the native `CMakeLists.txt`.
- Native dependencies (SDL2, SDL2_net, SDL2_mixer, SDL2_image, FFmpeg) are managed via `FetchContent` in CMake.
- Builds must not require or default to tools, sources, or caches under `.artifacts`; use host-installed tools, CMake/FetchContent fallbacks, or explicitly supplied external paths instead.
- Avoid tight coupling between the Android Activity and the Engine logic to facilitate the future Quest 3 (VR) port.
