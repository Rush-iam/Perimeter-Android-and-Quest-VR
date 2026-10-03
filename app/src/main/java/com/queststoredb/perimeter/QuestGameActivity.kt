package com.queststoredb.perimeter

import android.content.Context
import org.libsdl.app.SDLSurface

/** The engine Activity registered as immersive on Quest. */
class QuestGameActivity : MainActivity() {
    private external fun nativeSetXrPaused(paused: Boolean)

    override fun onPause() {
        super.onPause()
        if (!mBrokenLibraries && mSDLThread != null) nativeSetXrPaused(true)
    }

    override fun onResume() {
        if (!mBrokenLibraries && mSDLThread != null) nativeSetXrPaused(false)
        super.onResume()
    }

    override fun createSDLSurface(context: Context): SDLSurface =
        SDLSurface(context).apply {
            // SDL's logical window drives UI layout and pointer mapping. Native XR
            // renders the menu panel at 2x density and the gameplay panel at 1x.
            // Keep it independent of the headset's combined display dimensions and the
            // per-eye OpenXR targets, which use the runtime's recommended resolution.
            holder.setFixedSize(800, 600)
        }
}
