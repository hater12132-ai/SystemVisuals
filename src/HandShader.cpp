#include "bactro/HandShader.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>

#include <android/log.h>

#include <atomic>
#include <string>

#define HS_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SystemVisuals", __VA_ARGS__)

namespace bactro::handshader {
namespace {

constexpr const char* kModuleId = "bactro.handshader";
std::atomic_bool g_enabled{true};

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Hand Shader");
    b.description(
         "Legacy module. Phase-style outline is now in \"Phase Outline\" (native dual-pass). "
         "GLES glow was removed — it always leaked into world draws.")
        .defaultEnabled(false)
        .onToggle(onToggle);
    b.registerModule();
}

void onSignaturesReady() {
    // renderFirstPerson is owned by OutlinePass (dual-pass). Do not double-hook.
    HS_LOGI("HandShader: idle (outline moved to Phase Outline module)");
}

void onFrame() {}
void shutdown() { g_enabled.store(false, std::memory_order_release); }

} // namespace bactro::handshader
