#include "bactro/ChamsESP.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>

#include <android/log.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>

#define CE_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SystemVisuals", __VA_ARGS__)

namespace bactro::chamsesp {
namespace {

constexpr const char* kModuleId = "bactro.chamsesp";

struct Color {
    float r, g, b, a;
};

std::atomic_bool g_enabled{false};
std::atomic<float> g_r{1.f}, g_g{1.f}, g_b{1.f}, g_a{0.85f};
std::atomic_bool g_playersOnly{true};

using ActorIsPlayerFn = bool (*)(void*);
ActorIsPlayerFn g_isPlayer = nullptr;

using SetupActorGlintFn = void (*)(void*, void*, void*, const Color*, const Color*, const Color*, const Color*,
                                   float, float, float, float, const void*);
SetupActorGlintFn g_setupActorGlint = nullptr;
bool g_hooked = false;

void logLine(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    CE_LOGI("%s", buf);
}

void setupActorGlintDetour(void* screenContext, void* entityContext, void* actor, const Color* overlay,
                           const Color* changeColor, const Color* changeColor2, const Color* glintColor,
                           float uvOffset1, float uvOffset2, float uvRot1, float uvRot2,
                           const void* lightEmissionColor) {
    if (!g_setupActorGlint) return;

    if (!g_enabled.load(std::memory_order_relaxed) || !actor) {
        g_setupActorGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor,
                          uvOffset1, uvOffset2, uvRot1, uvRot2, lightEmissionColor);
        return;
    }

    bool apply = true;
    if (g_playersOnly.load(std::memory_order_relaxed)) {
        if (!g_isPlayer) {
            apply = false;
        } else {
            try {
                apply = g_isPlayer(actor);
            } catch (...) {
                apply = false;
            }
        }
    }

    if (!apply) {
        g_setupActorGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor,
                          uvOffset1, uvOffset2, uvRot1, uvRot2, lightEmissionColor);
        return;
    }

    static Color fill{}, edge{};
    fill = {g_r.load(std::memory_order_relaxed), g_g.load(std::memory_order_relaxed),
            g_b.load(std::memory_order_relaxed), g_a.load(std::memory_order_relaxed)};
    edge = {1.f, 1.f, 1.f, 1.f};
    g_setupActorGlint(screenContext, entityContext, actor, &fill, &fill, &fill, &edge, uvOffset1, uvOffset2, uvRot1,
                      uvRot2, lightEmissionColor);
}

void tryInstall() {
    if (!g_isPlayer) {
        const auto addr = bactro::memory::resolve(bactro::memory::SignatureId::ActorIsPlayer);
        if (addr) {
            g_isPlayer = reinterpret_cast<ActorIsPlayerFn>(addr);
            logLine("ChamsESP: ActorIsPlayer @%p", reinterpret_cast<void*>(addr));
        }
    }
    if (g_hooked) return;

    void* o = nullptr;
    if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetupShaderParametersActorGlint,
                             reinterpret_cast<void*>(&setupActorGlintDetour), &o) &&
        o) {
        g_setupActorGlint = reinterpret_cast<SetupActorGlintFn>(o);
        g_hooked = true;
        logLine("ChamsESP: SetupActorGlint hooked");
    } else {
        logLine("ChamsESP: SetupActorGlint HOOK FAIL (if SnowChams hooked first, use that Players toggle)");
    }
}

// MUST match pl::modmenu: (std::string_view, bool) — NOT (bool)
void onToggle(std::string_view, bool on) {
    g_enabled.store(on, std::memory_order_relaxed);
    if (on) tryInstall();
    logLine(on ? "ChamsESP ON" : "ChamsESP OFF");
}

// MUST match pl::modmenu: (std::string_view, std::string_view, std::string_view)
void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "r")
            g_r.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "g")
            g_g.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "b")
            g_b.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "opacity")
            g_a.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "playersOnly")
            g_playersOnly.store(value == "true" || value == "1", std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Chams ESP");
    b.description("Visible player chams (in view only — not through walls).")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("r", "Red", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("g", "Green", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("b", "Blue", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("opacity", "Opacity", pl::modmenu::ConfigType::SliderFloat, "0.85", "0.2", "1", "");
    b.config("playersOnly", "Players only", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.registerModule();
}

void onSignaturesReady() {
    if (g_enabled.load(std::memory_order_relaxed))
        tryInstall();
    else
        logLine("ChamsESP: ready — enable for players in view (no wallhack)");
}

void shutdown() { g_enabled.store(false, std::memory_order_relaxed); }

} // namespace bactro::chamsesp
