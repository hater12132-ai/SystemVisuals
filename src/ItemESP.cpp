#include "bactro/ItemESP.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>

#include <android/log.h>
#include <atomic>

#define IE_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SystemVisuals", __VA_ARGS__)

namespace bactro::itemesp {
namespace {

constexpr const char* kModuleId = "systemvisuals.itemesp";
std::atomic_bool g_enabled{false};
std::atomic<float> g_maxDistance{24.f};

// Design (native, not ProtoHax relay):
// 1) Resolve signatures for item actors / name tags (see Signatures.hpp Item* / ActorGetNameTag).
// 2) On render/HUD tick: for each nearby item entity in camera FOV, read display name.
// 3) Draw label with depth test ON so walls occlude (no wallhack).
// ProtoHax 2.2.0 source has no ItemESP module; hotfix APK used packet/entity list ESP.
// This pack implements the native path suitable for Levi preload-native.

} // namespace

void init() {
    IE_LOGI("ItemESP init (visual-only, FOV labels — hooks pending sig resolve)");
}

void registerModMenu() {
    pl::modmenu::ModuleBuilder(kModuleId, "Item ESP")
        .toggle("Enabled", &g_enabled)
        .slider("Max distance", &g_maxDistance, 4.f, 48.f)
        .build();
}

} // namespace bactro::itemesp
