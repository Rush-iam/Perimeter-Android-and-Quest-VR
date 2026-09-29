package com.queststoredb.perimeter

import android.content.Context
import org.libsdl.app.SDLSurface

/** The engine Activity registered as immersive on Quest. */
class QuestGameActivity : MainActivity() {
    override fun createSDLSurface(context: Context): SDLSurface =
        SDLSurface(context).apply {
            // SDL's logical window drives UI layout, panel rendering, and pointer mapping.
            // Keep it independent of the headset's combined display dimensions and the
            // per-eye OpenXR targets, which use the runtime's recommended resolution.
            holder.setFixedSize(1280, 960)
        }
}
