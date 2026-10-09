#pragma once

namespace bactro::targethud {

void registerModule();
void onSignaturesReady();
void onPostFrame(); // draw via MotionBlur swap path (no second egl hook)
void shutdown();

} // namespace bactro::targethud
