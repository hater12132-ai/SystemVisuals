#pragma once

namespace bactro::handchams {

void registerModule();
void onSignaturesReady();
void shutdown();
void onPostFrame(); // no-op (kept so MotionBlur still links)

} // namespace bactro::handchams
