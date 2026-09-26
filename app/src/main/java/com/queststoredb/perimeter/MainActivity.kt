package com.queststoredb.perimeter

import android.content.pm.ActivityInfo
import android.os.PowerManager
import android.os.Build
import android.os.Bundle
import android.os.Process
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.window.OnBackInvokedDispatcher
import org.libsdl.app.SDL
import org.libsdl.app.SDLActivity
import org.libsdl.app.SDLSurface

open class MainActivity : SDLActivity() {
    private var mouseBackButtonHeld = false

    override fun createSDLSurface(context: android.content.Context): SDLSurface =
        ScaledSDLSurface(context)

    override fun onCreate(savedInstanceState: Bundle?) {
        configureSustainedPerformanceMode()
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT
            ) { sendEscapeClick() }
        }
    }

    private fun configureSustainedPerformanceMode() {
        val requested = GameLaunchOptions(this).sustainedPerformance()
        val powerManager = getSystemService(PowerManager::class.java)
        val supported = powerManager?.isSustainedPerformanceModeSupported == true
        if (supported) {
            window.setSustainedPerformanceMode(requested)
        }
        Log.i("PerimeterPower", "Sustained Performance requested=$requested supported=$supported")
    }

    override fun onDestroy() {
        super.onDestroy()
        if (isFinishing) {
            // Perimeter's native globals are not safe to initialize more than once in a
            // process. MainActivity runs in :game, so discard that process after SDL has
            // joined its native thread and completed nativeQuit().
            Process.killProcess(Process.myPid())
        }
    }

    override fun onPause() {
        setMouseBackButtonHeld(false)
        super.onPause()
    }

    @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
    override fun onBackPressed() {
        sendEscapeClick()
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode == KeyEvent.KEYCODE_BACK) {
            val fromRelativeMouse = event.isFromSource(InputDevice.SOURCE_MOUSE_RELATIVE)
            val fromMouse = event.isFromSource(InputDevice.SOURCE_MOUSE) || fromRelativeMouse
            if (fromMouse) {
                when (event.action) {
                    KeyEvent.ACTION_DOWN -> {
                        setMouseBackButtonHeld(true)
                    }
                    KeyEvent.ACTION_UP -> {
                        // In relative mode, ignore the early SOURCE_MOUSE key-up some mice
                        // emit while held. Outside relative mode, it is the actual release.
                        if (mouseBackButtonHeld &&
                            (fromRelativeMouse || !isRelativeMouseMode())) {
                            setMouseBackButtonHeld(false)
                        }
                    }
                }
            } else if (event.action == KeyEvent.ACTION_UP && !event.isCanceled) {
                sendEscapeClick()
            }
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onMouseBackButtonEvent(pressed: Boolean) {
        setMouseBackButtonHeld(pressed)
    }

    private fun setMouseBackButtonHeld(pressed: Boolean) {
        if (mouseBackButtonHeld == pressed || mBrokenLibraries) {
            return
        }
        mouseBackButtonHeld = pressed
        onNativeMouseButtonNoWarp(2, pressed)
    }

    private fun sendEscapeClick() {
        if (mBrokenLibraries) return
        onNativeKeyDown(KeyEvent.KEYCODE_ESCAPE)
        onNativeKeyUp(KeyEvent.KEYCODE_ESCAPE)
    }

    override fun setOrientationBis(w: Int, h: Int, resizable: Boolean, hint: String?) {
        // SDL's default can replace the manifest orientation with FULL_USER, honoring rotation lock.
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
    }

    override fun loadLibraries() {
        // The engine's global error handler requests SDL_GetPrefPath during library loading.
        SDL.setContext(this)
        super.loadLibraries()
    }

    override fun getMainSharedObject(): String {
        return "libperimeter.so"
    }

    override fun isAndroidMemoryMonitorEnabled(): Boolean =
        GameLaunchOptions(this).memoryMonitorEnabled()

    override fun getLibraries(): Array<String> {
        return arrayOf(
            "c++_shared",
            "ffmpeg",
            "SDL2",
            "SDL2_image",
            "SDL2_mixer",
            "SDL2_net",
            "perimeter"
        )
    }

    override fun getArguments(): Array<String> {
        val storage = GameContentStorage(this)
        val path = checkNotNull(storage.localFilesystemPath()) {
            "Grant all-files access and select the game folder before starting the engine"
        }
        val limitToHalfRefresh = FrameRateLimit.load(this)
        val questVr = BuildConfig.FLAVOR == "quest"
        val questTutorial = BuildConfig.DEBUG && questVr &&
            intent.getStringExtra("quest_test_mission") == "Tutorial"
        val arguments = GameLaunchOptions(this).arguments(path).filterNot {
            val key = it.removePrefix("tmp_").substringBefore('=')
            key == "android_vsync_interval" || key == "VSync" ||
                (questVr && (key == "graph" || key == "android_dxvk_version"))
        }
        return (arguments + listOfNotNull(
            "android_vsync_interval=2".takeIf { limitToHalfRefresh },
            "VSync=1",
            "graph=d3d9".takeIf { questVr },
            "android_dxvk_version=2".takeIf { questVr },
            "mainmenu=0".takeIf { questTutorial },
            "mission=Tutorial".takeIf { questTutorial },
            "disableBriefing=1".takeIf { questTutorial }
        )).toTypedArray()
    }
}
