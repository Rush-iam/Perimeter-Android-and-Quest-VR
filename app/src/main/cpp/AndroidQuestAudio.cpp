#include <jni.h>

#include <SDL_mixer.h>

extern "C" JNIEXPORT void JNICALL
Java_com_queststoredb_perimeter_QuestGameActivity_nativeSetAudioPaused(
    JNIEnv*, jobject, jboolean paused) {
    if (Mix_QuerySpec(nullptr, nullptr, nullptr) > 0) {
        // SDL's activity pause reaches the mixer only when the game pumps its
        // next event. Stop playback immediately on the Android lifecycle thread.
        Mix_PauseAudio(paused == JNI_TRUE ? 1 : 0);
    }
}
