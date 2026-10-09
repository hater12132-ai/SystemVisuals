#include "bactro/OutlinePass.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"
#include "bactro/RenderPhase.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <android/log.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#define OL_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SystemVisuals", __VA_ARGS__)

namespace bactro::outline {
namespace {

// Bedrock Color { r, g, b, a }
struct Color {
    float r, g, b, a;
};

constexpr const char* kModuleId = "bactro.outline";

std::atomic_bool g_enabled{true};
std::atomic_bool g_handOnly{false}; // false = try all entity glint/overlay paths
std::atomic<float> g_intensity{1.25f};
std::atomic<float> g_alpha{0.85f};
std::atomic_int g_fpDual{0};
std::atomic_int g_overlayHits{0};

void logLine(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    OL_LOGI("%s", buf);
}

bool wantOutline() {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    if (bactro::phase::outlinePass.load(std::memory_order_acquire)) return true;
    if (g_handOnly.load(std::memory_order_relaxed))
        return bactro::phase::inFirstPersonHand.load(std::memory_order_acquire);
    // Global mode: always inject soft white overlay (third person / paper doll)
    return true;
}

Color makeOutlineOverlay() {
    const float i = g_intensity.load(std::memory_order_relaxed);
    const float a = g_alpha.load(std::memory_order_relaxed);
    // Bright white overlay — engine blends this as damage/hurt-style tint
    return {i, i, i, a};
}

// ---- renderFirstPerson dual pass ----

using RenderFirstPersonFn = void (*)(void* self, void* a1, void* a2, void* a3, void* a4, void* a5);
RenderFirstPersonFn g_renderFpOriginal = nullptr;
bool g_renderFpHooked = false;

void renderFirstPersonDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!g_renderFpOriginal) return;

    if (!g_enabled.load(std::memory_order_relaxed)) {
        bactro::phase::inFirstPersonHand.store(true, std::memory_order_release);
        g_renderFpOriginal(self, a1, a2, a3, a4, a5);
        bactro::phase::inFirstPersonHand.store(false, std::memory_order_release);
        return;
    }

    bactro::phase::inFirstPersonHand.store(true, std::memory_order_release);

    // PASS 1 — outline layer (setEntityConstants sees outlinePass=true → white overlay)
    bactro::phase::outlinePass.store(true, std::memory_order_release);
    g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    bactro::phase::outlinePass.store(false, std::memory_order_release);

    // PASS 2 — normal hand/item on top
    g_renderFpOriginal(self, a1, a2, a3, a4, a5);

    bactro::phase::inFirstPersonHand.store(false, std::memory_order_release);

    const int n = g_fpDual.fetch_add(1, std::memory_order_relaxed);
    if (n < 5) logLine("Outline: dual renderFirstPerson #%d", n);
}

// ---- ActorShaderManager::setEntityConstants — force white overlay on outline pass ----

using SetEntityConstantsFn = void (*)(
    void*, void*, const Color*, const void*, const void*, const Color*, const Color*, const Color*,
    const Color*, const void*, const void*, float, float, float, float);

SetEntityConstantsFn g_setEntityConstants = nullptr;
bool g_hookedEntity = false;

void setEntityConstantsDetour(
    void* entityConstants, void* renderContext, const Color* tileLightColor, const void* tileLightColorUV,
    const void* blockLightColor, const Color* overlay, const Color* changeColor, const Color* changeColor2,
    const Color* glintColor, const void* glintUVScale, const void* uvAnim, float uvOffset1, float uvOffset2,
    float uvRot1, float uvRot2) {
    if (!g_setEntityConstants) return;

    if (wantOutline()) {
        const Color white = makeOutlineOverlay();
        // Replace overlay with bright white — this is the "outline pass" tint
        g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                             &white, changeColor, changeColor2, glintColor, glintUVScale, uvAnim, uvOffset1,
                             uvOffset2, uvRot1, uvRot2);
        const int n = g_overlayHits.fetch_add(1, std::memory_order_relaxed);
        if (n < 8) logLine("Outline: white overlay inject #%d (fp=%d pass=%d)", n,
                           bactro::phase::inFirstPersonHand.load() ? 1 : 0,
                           bactro::phase::outlinePass.load() ? 1 : 0);
        return;
    }

    g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                         overlay, changeColor, changeColor2, glintColor, glintUVScale, uvAnim, uvOffset1,
                         uvOffset2, uvRot1, uvRot2);
}

// ---- setupActorGlint — has actor pointer; good for third-person / paper doll ----

using SetupActorGlintFn = void (*)(
    void*, void*, void*, const Color*, const Color*, const Color*, const Color*, float, float, float,
    float, const void*, const void*, float, std::uint8_t, const void*);

SetupActorGlintFn g_setupActorGlint = nullptr;
bool g_hookedActor = false;

void setupActorGlintDetour(
    void* screenContext, void* entityContext, void* actor, const Color* overlay, const Color* changeColor,
    const Color* changeColor2, const Color* glintColor, float uvOffset1, float uvOffset2, float uvRot1,
    float uvRot2, const void* glintUVScale, const void* uvAnim, float br, std::uint8_t lightEmission,
    const void* lightEmissionColor) {
    if (!g_setupActorGlint) return;

    if (wantOutline()) {
        const Color white = makeOutlineOverlay();
        g_setupActorGlint(screenContext, entityContext, actor, &white, changeColor, changeColor2, glintColor,
                          uvOffset1, uvOffset2, uvRot1, uvRot2, glintUVScale, uvAnim, br, lightEmission,
                          lightEmissionColor);
        return;
    }

    g_setupActorGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor,
                      uvOffset1, uvOffset2, uvRot1, uvRot2, glintUVScale, uvAnim, br, lightEmission,
                      lightEmissionColor);
}

void tryInstallHooks() {
    void* o = nullptr;

    if (!g_renderFpHooked) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ItemInHandRendererRenderFirstPerson,
                                 reinterpret_cast<void*>(&renderFirstPersonDetour), &o)) {
            g_renderFpOriginal = reinterpret_cast<RenderFirstPersonFn>(o);
            g_renderFpHooked = true;
            logLine("Outline: renderFirstPerson dual-pass hooked");
        } else {
            logLine("Outline: renderFirstPerson hook FAILED");
        }
    }

    if (!g_hookedEntity) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetEntityConstants,
                                 reinterpret_cast<void*>(&setEntityConstantsDetour), &o)) {
            g_setEntityConstants = reinterpret_cast<SetEntityConstantsFn>(o);
            g_hookedEntity = true;
            logLine("Outline: setEntityConstants hooked");
        } else {
            logLine("Outline: setEntityConstants hook FAILED");
        }
    }

    if (!g_hookedActor) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetupShaderParametersActorGlint,
                                 reinterpret_cast<void*>(&setupActorGlintDetour), &o)) {
            g_setupActorGlint = reinterpret_cast<SetupActorGlintFn>(o);
            g_hookedActor = true;
            logLine("Outline: setupActorGlint hooked");
        } else {
            logLine("Outline: setupActorGlint hook FAILED");
        }
    }

    logLine("Outline: hooks fp=%d entity=%d actor=%d", g_renderFpHooked ? 1 : 0, g_hookedEntity ? 1 : 0,
            g_hookedActor ? 1 : 0);
}

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    OL_LOGI("Outline %s", enabled ? "ON" : "OFF");
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "intensity")
            g_intensity.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "alpha")
            g_alpha.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "handOnly")
            g_handOnly.store(value == "true" || value == "1", std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Phase Outline");
    b.description(
         "Native second-pass outline: dual first-person render + white overlay inject on entity "
         "shader constants. Closest to Phase silhouette without scaling the mesh (scale needs "
         "model-matrix access). Turn Hand Only OFF for paper-doll / third-person.")
        .defaultEnabled(true)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("intensity", "Outline intensity", pl::modmenu::ConfigType::SliderFloat, "1.25", "0.3", "3", "");
    b.config("alpha", "Outline alpha", pl::modmenu::ConfigType::SliderFloat, "0.85", "0.1", "1", "");
    b.config("handOnly", "Hand only (1st person)", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.registerModule();
}

void onSignaturesReady() { tryInstallHooks(); }

void shutdown() {
    g_enabled.store(false, std::memory_order_release);
    bactro::phase::outlinePass.store(false, std::memory_order_release);
}

} // namespace bactro::outline
