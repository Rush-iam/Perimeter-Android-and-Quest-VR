package com.queststoredb.perimeter

import android.content.Context
import org.libsdl.app.SDLSurface

/** The engine Activity registered as immersive on Quest. */
class QuestGameActivity : MainActivity() {
    private var activityResumed = false

    private external fun nativeSetAudioPaused(paused: Boolean)

    private fun setAudioPaused(paused: Boolean) {
        if (!mBrokenLibraries && mSDLThread != null) {
            nativeSetAudioPaused(paused)
        }
    }

    override fun onPause() {
        activityResumed = false
        setAudioPaused(true)
        super.onPause()
    }

    override fun onResume() {
        super.onResume()
        activityResumed = true
        if (hasWindowFocus()) {
            setAudioPaused(false)
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        if (!hasFocus) {
            setAudioPaused(true)
        }
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus && activityResumed) {
            setAudioPaused(false)
        }
    }

    override fun createSDLSurface(context: Context): SDLSurface =
        SDLSurface(context).apply {
            // SDL's logical window drives UI layout, panel rendering, and pointer mapping.
            // Keep it independent of the headset's combined display dimensions and the
            // per-eye OpenXR targets, which use the runtime's recommended resolution.
            holder.setFixedSize(1024, 768)
        }
}
