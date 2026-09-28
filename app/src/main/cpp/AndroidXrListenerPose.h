#pragma once

#if defined(ANDROID_XR)

class MatXf;

// The render path publishes a world-to-head view matrix in the sound camera
// convention. SoundQuant reads a complete snapshot on the logic thread,
// falling back when tracking is stale.
void androidXrPublishListenerView(const MatXf& view);
bool androidXrReadListenerView(MatXf* view);
void androidXrClearListenerView();

#endif // ANDROID_XR
