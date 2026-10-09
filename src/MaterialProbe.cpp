#include "bactro/MaterialProbe.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>

#include <android/log.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>

#define MP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SystemVisuals", __VA_ARGS__)

namespace bactro::material {
namespace {

void logLine(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    MP_LOGI("%s", buf);
}

} // namespace

void onSignaturesReady() {
    using bactro::memory::SignatureId;
    using bactro::memory::resolve;

    // Resolve-only — no hooks, no float writes (1.16.5 a1 write was wrong object)
    const auto hide = resolve(SignatureId::HideGlowOutlineQuery);
    const auto hand = resolve(SignatureId::ItemInHandShaderSetup);
    const auto mbin = resolve(SignatureId::MaterialBinPathBuilder);
    const auto miss = resolve(SignatureId::MaterialMissingError);
    const auto group = resolve(SignatureId::RenderMaterialGroupCommon);

    logLine("MaterialRE: HideGlowOutlineQuery %s @%p", hide ? "OK" : "MISS", reinterpret_cast<void*>(hide));
    logLine("MaterialRE: ItemInHandShaderSetup %s @%p", hand ? "OK" : "MISS", reinterpret_cast<void*>(hand));
    logLine("MaterialRE: MaterialBinPathBuilder %s @%p", mbin ? "OK" : "MISS", reinterpret_cast<void*>(mbin));
    logLine("MaterialRE: MaterialMissingError %s @%p", miss ? "OK" : "MISS", reinterpret_cast<void*>(miss));
    logLine("MaterialRE: RenderMaterialGroupCommon %s @%p", group ? "OK" : "MISS", reinterpret_cast<void*>(group));
    const auto matreg = resolve(SignatureId::MaterialRegistryInit);
    logLine("MaterialRE: MaterialRegistryInit (item_in_hand_glint+flat_color_line) %s @%p", matreg ? "OK" : "MISS", reinterpret_cast<void*>(matreg));
    logLine("MaterialRE: STOPPED experimental writes — a1 was not EDGE uniforms");
    logLine("MaterialRE: need MaterialFilter / custom material.bin bind for real chams");
}

} // namespace bactro::material
