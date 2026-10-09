#pragma once

namespace bactro::wireoutline {

void registerModule();
void onSignaturesReady();
void shutdown();
void onPostFrame(); // GLES 3D/2D wire draw after frame (from MotionBlur)

} // namespace bactro::wireoutline
