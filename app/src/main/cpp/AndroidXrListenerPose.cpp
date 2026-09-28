#include "AndroidXrListenerPose.h"

#if defined(ANDROID_XR)

#include "xmath.h"
#include <chrono>
#include <mutex>

namespace {

std::mutex listenerMutex;
MatXf listenerView = MatXf::ID;
std::chrono::steady_clock::time_point listenerUpdated;
bool listenerValid = false;

} // namespace

void androidXrPublishListenerView(const MatXf& view) {
    const std::lock_guard<std::mutex> lock(listenerMutex);
    listenerView = view;
    listenerUpdated = std::chrono::steady_clock::now();
    listenerValid = true;
}

bool androidXrReadListenerView(MatXf* view) {
    if (!view) return false;
    const std::lock_guard<std::mutex> lock(listenerMutex);
    constexpr auto maxAge = std::chrono::milliseconds(250);
    if (!listenerValid || std::chrono::steady_clock::now() - listenerUpdated > maxAge)
        return false;
    *view = listenerView;
    return true;
}

void androidXrClearListenerView() {
    const std::lock_guard<std::mutex> lock(listenerMutex);
    listenerValid = false;
}

#endif // ANDROID_XR
