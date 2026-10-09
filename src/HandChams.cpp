#include "bactro/HandChams.hpp"
#include "bactro/RenderPhase.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <EGL/egl.h>
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define HC_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SystemVisuals", __VA_ARGS__)

namespace bactro::handchams {
namespace {

struct Color {
    float r, g, b, a;
};

constexpr const char* kModuleId = "bactro.handchams";

std::atomic_bool g_enabled{false};
std::atomic_bool g_handOnly{false}; // false = allow world actors when targets enabled
std::atomic_bool g_targetPlayers{true};
std::atomic_bool g_targetMobs{true};   // non-player actors
std::atomic_bool g_targetHand{true};   // FP hand / null actor (items, cosmetics-ish)
std::atomic<int> g_fpSticky{0};
std::atomic_bool g_boxEsp{false}; // REMOVED: screen boxes were drawn with depth test off (see-through). Never enabled.
std::atomic<float> g_outlineWidth{2.5f};   // glLineWidth
std::atomic<float> g_outlineGlow{0.6f};    // extra glow passes (0=off, 1=strong)
std::atomic<float> g_outlineAlpha{0.95f};  // line alpha
std::atomic_bool g_playersOnly{true};
std::atomic<float> g_r{1.00f};
std::atomic<float> g_g{1.00f};
std::atomic<float> g_b{1.00f};
std::atomic<float> g_opacity{0.85f};

Color g_chams{1.f, 1.f, 1.f, 0.85f};
Color g_outline{1.f, 1.f, 1.f, 1.f};

bool g_renderFpHooked = false;
bool g_hookedEntity = false;
bool g_hookedActor = false;
bool g_hookedGlint = false;

void logLine(const char* fmt, ...) {
    char buf[220];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    HC_LOGI("%s", buf);
}

void refresh() {
    float aa = g_opacity.load(std::memory_order_relaxed);
    if (aa < 0.05f) aa = 0.05f;
    if (aa > 1.0f) aa = 1.0f;
    g_chams = {g_r.load(std::memory_order_relaxed), g_g.load(std::memory_order_relaxed),
               g_b.load(std::memory_order_relaxed), aa};
}

void* glProc(const char* name) {
    if (void* p = reinterpret_cast<void*>(eglGetProcAddress(name))) return p;
    static void* h = dlopen("libGLESv2.so", RTLD_NOW);
    if (!h) h = dlopen("libGLESv3.so", RTLD_NOW);
    return h ? dlsym(h, name) : nullptr;
}


// ---- Optional box ESP (off by default; players only) ----
struct Box {
    float minx, miny, minz, maxx, maxy, maxz;
};
std::mutex g_boxMu;
std::vector<Box> g_boxes;
std::mutex g_vpMu;
float g_viewProj[16]{};
std::atomic_bool g_vpValid{false};

using ActorIsPlayerFn = bool (*)(void*);
ActorIsPlayerFn g_isPlayer = nullptr;

using PFN_glUniformMatrix4fv = void (*)(int, int, unsigned char, const float*);
PFN_glUniformMatrix4fv g_glUniformMatrix4fvOrig = nullptr;
bool g_glUniformHooked = false;
std::atomic_bool g_entityRenderArmed{false};

bool finite16(const float* m) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    return true;
}

bool looksLikeViewProj(const float* m) {
    if (!m || !finite16(m)) return false;
    float s = 0.f;
    for (int i = 0; i < 16; ++i) s += std::fabs(m[i]);
    if (s < 2.f || s > 1e5f) return false;
    return std::fabs(m[11]) > 0.05f || std::fabs(m[14]) > 0.05f;
}

bool looksLikeWorldModel(const float* m, float& tx, float& ty, float& tz) {
    if (!m || !finite16(m)) return false;
    if (std::fabs(m[15] - 1.f) > 0.15f) return false;
    tx = m[12];
    ty = m[13];
    tz = m[14];
    if (!std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(tz)) return false;
    // Solstice-style: keep nearby players (old xz<16 rejected everything close)
    float xz = std::sqrt(tx * tx + tz * tz);
    if (xz < 0.35f) return false;   // self / origin noise
    if (xz > 128.f) return false;    // too far
    if (ty < -64.f || ty > 400.f) return false;
    if (std::fabs(tx) > 300000.f || std::fabs(tz) > 300000.f) return false;
    return true;
}

void noteWorldBox(float x, float y, float z) {
    Box b{x - 0.35f, y - 0.05f, z - 0.35f, x + 0.35f, y + 1.85f, z + 0.35f};
    std::lock_guard<std::mutex> lock(g_boxMu);
    if (g_boxes.size() < 96) g_boxes.push_back(b);
}

void glUniformMatrix4fvDetour(int location, int count, unsigned char transpose, const float* value) {
    if (value && count >= 1 && g_boxEsp.load(std::memory_order_relaxed)) {
        float m[16];
        if (transpose) {
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) m[c * 4 + r] = value[r * 4 + c];
        } else {
            std::memcpy(m, value, sizeof(m));
        }
        if (looksLikeViewProj(m)) {
            std::lock_guard<std::mutex> lock(g_vpMu);
            std::memcpy(g_viewProj, m, sizeof(m));
            g_vpValid.store(true, std::memory_order_release);
        } else if (g_entityRenderArmed.load(std::memory_order_acquire)) {
            float tx, ty, tz;
            if (looksLikeWorldModel(m, tx, ty, tz)) noteWorldBox(tx, ty, tz);
        }
    }
    if (g_glUniformMatrix4fvOrig) g_glUniformMatrix4fvOrig(location, count, transpose, value);
}

bool installMatrixHook() {
    if (g_glUniformHooked) return true;
    void* target = glProc("glUniformMatrix4fv");
    if (!target) return false;
    void* o = nullptr;
    if (pl::memory::hook(target, reinterpret_cast<void*>(&glUniformMatrix4fvDetour), &o) == 0 && o) {
        g_glUniformMatrix4fvOrig = reinterpret_cast<PFN_glUniformMatrix4fv>(o);
        g_glUniformHooked = true;
        logLine("SnowChams: matrix HOOKED (box ESP)");
        return true;
    }
    return false;
}

bool worldToNdc(const float* vp, float x, float y, float z, float& ox, float& oy) {
    float clipX = vp[0] * x + vp[4] * y + vp[8] * z + vp[12];
    float clipY = vp[1] * x + vp[5] * y + vp[9] * z + vp[13];
    float clipW = vp[3] * x + vp[7] * y + vp[11] * z + vp[15];
    if (std::fabs(clipW) < 1e-4f) return false;
    ox = clipX / clipW;
    oy = clipY / clipW;
    return std::isfinite(ox) && std::isfinite(oy) && ox > -1.2f && ox < 1.2f && oy > -1.2f && oy < 1.2f;
}

using GLuint = unsigned int;
using GLint = int;
using GLenum = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLchar = char;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_LINES = 0x0001;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;

using PFN_glCreateShader = GLuint (*)(GLenum);
using PFN_glShaderSource = void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glCompileShader = void (*)(GLuint);
using PFN_glCreateProgram = GLuint (*)(void);
using PFN_glAttachShader = void (*)(GLuint, GLuint);
using PFN_glLinkProgram = void (*)(GLuint);
using PFN_glUseProgram = void (*)(GLuint);
using PFN_glGetAttribLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glGetUniformLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glUniform4f = void (*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using PFN_glGenBuffers = void (*)(GLsizei, GLuint*);
using PFN_glBindBuffer = void (*)(GLenum, GLuint);
using PFN_glBufferData = void (*)(GLenum, long, const void*, GLenum);
using PFN_glEnableVertexAttribArray = void (*)(GLuint);
using PFN_glVertexAttribPointer = void (*)(GLuint, GLint, GLenum, unsigned char, GLsizei, const void*);
using PFN_glDrawArrays = void (*)(GLenum, GLint, GLsizei);
using PFN_glBlendFunc = void (*)(GLenum, GLenum);
using PFN_glLineWidth = void (*)(GLfloat);
using PFN_glEnable = void (*)(GLenum);
using PFN_glDisable = void (*)(GLenum);

PFN_glCreateShader d_glCreateShader = nullptr;
PFN_glShaderSource d_glShaderSource = nullptr;
PFN_glCompileShader d_glCompileShader = nullptr;
PFN_glCreateProgram d_glCreateProgram = nullptr;
PFN_glAttachShader d_glAttachShader = nullptr;
PFN_glLinkProgram d_glLinkProgram = nullptr;
PFN_glUseProgram d_glUseProgram = nullptr;
PFN_glGetAttribLocation d_glGetAttribLocation = nullptr;
PFN_glGetUniformLocation d_glGetUniformLocation = nullptr;
PFN_glUniform4f d_glUniform4f = nullptr;
PFN_glGenBuffers d_glGenBuffers = nullptr;
PFN_glBindBuffer d_glBindBuffer = nullptr;
PFN_glBufferData d_glBufferData = nullptr;
PFN_glEnableVertexAttribArray d_glEnableVertexAttribArray = nullptr;
PFN_glVertexAttribPointer d_glVertexAttribPointer = nullptr;
PFN_glDrawArrays d_glDrawArrays = nullptr;
PFN_glBlendFunc d_glBlendFunc = nullptr;
PFN_glLineWidth d_glLineWidth = nullptr;
PFN_glEnable d_glEnable = nullptr;
PFN_glDisable d_glDisable = nullptr;
GLuint g_prog = 0, g_vbo = 0;
GLint g_aPos = -1, g_uColor = -1;
bool g_lineGlReady = false;

bool loadLineGl() {
    if (g_lineGlReady) return true;
#define L(n) d_##n = reinterpret_cast<decltype(d_##n)>(glProc(#n))
    L(glCreateShader); L(glShaderSource); L(glCompileShader); L(glCreateProgram); L(glAttachShader);
    L(glLinkProgram); L(glUseProgram); L(glGetAttribLocation); L(glGetUniformLocation); L(glUniform4f);
    L(glGenBuffers); L(glBindBuffer); L(glBufferData); L(glEnableVertexAttribArray); L(glVertexAttribPointer);
    L(glDrawArrays); L(glBlendFunc); L(glLineWidth); L(glEnable); L(glDisable);
#undef L
    if (!d_glCreateShader || !d_glDrawArrays) return false;
    const char* vs = "attribute vec2 aPos; void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }";
    const char* fs = "precision mediump float; uniform vec4 uColor; void main(){ gl_FragColor = uColor; }";
    GLuint v = d_glCreateShader(GL_VERTEX_SHADER);
    GLuint f = d_glCreateShader(GL_FRAGMENT_SHADER);
    d_glShaderSource(v, 1, &vs, nullptr);
    d_glCompileShader(v);
    d_glShaderSource(f, 1, &fs, nullptr);
    d_glCompileShader(f);
    g_prog = d_glCreateProgram();
    d_glAttachShader(g_prog, v);
    d_glAttachShader(g_prog, f);
    d_glLinkProgram(g_prog);
    g_aPos = d_glGetAttribLocation(g_prog, "aPos");
    g_uColor = d_glGetUniformLocation(g_prog, "uColor");
    d_glGenBuffers(1, &g_vbo);
    g_lineGlReady = g_prog != 0;
    return g_lineGlReady;
}


// ---- Entity Outline style: glDrawElements wire + glow (width/glow controllable) ----
using PFN_glDrawElements = void (*)(unsigned int, int, unsigned int, const void*);
using PFN_glDepthMask = void (*)(unsigned char);
PFN_glDrawElements g_glDrawElementsOrig = nullptr;
PFN_glDepthMask d_glDepthMask = nullptr;
bool g_drawElementsHooked = false;
std::atomic_int g_wireDraws{0};

void glDrawElementsDetour(unsigned int mode, int count, unsigned int type, const void* indices) {
    if (g_glDrawElementsOrig)
        g_glDrawElementsOrig(mode, count, type, indices);

    if (!g_enabled.load(std::memory_order_relaxed) || !g_boxEsp.load(std::memory_order_relaxed))
        return;
    if (!g_entityRenderArmed.load(std::memory_order_acquire))
        return;
    // GL_TRIANGLES
    if (mode != 0x0004u) return;
    if (count < 6 || count > 200000) return;
    if (!g_glDrawElementsOrig) return;

    const float width = std::max(0.5f, std::min(12.f, g_outlineWidth.load(std::memory_order_relaxed)));
    const float glow = std::max(0.f, std::min(1.f, g_outlineGlow.load(std::memory_order_relaxed)));
    const float alpha = std::max(0.1f, std::min(1.f, g_outlineAlpha.load(std::memory_order_relaxed)));

    if (d_glEnable) d_glEnable(0x0BE2); // GL_BLEND
    if (d_glBlendFunc) d_glBlendFunc(0x0302, 0x0303);
    if (d_glDepthMask) d_glDepthMask(0);

    // Glow passes: thicker, softer lines first (behind core edge)
    if (glow > 0.05f && d_glLineWidth) {
        const int passes = 1 + static_cast<int>(glow * 3.f); // 1..4
        for (int p = passes; p >= 1; --p) {
            const float w = width * (1.f + 0.55f * static_cast<float>(p));
            d_glLineWidth(w);
            // still same draw — GLES has no per-line color without shader; width creates glow halo
            g_glDrawElementsOrig(0x0001u /*GL_LINES*/, count, type, indices);
        }
    }

    // Core outline
    if (d_glLineWidth) d_glLineWidth(width);
    g_glDrawElementsOrig(0x0001u /*GL_LINES*/, count, type, indices);

    if (d_glDepthMask) d_glDepthMask(1);
    g_wireDraws.fetch_add(1, std::memory_order_relaxed);
}

bool installDrawElementsHook() {
    if (g_drawElementsHooked) return true;
    void* p = glProc("glDrawElements");
    if (!p) {
        void* egl = dlopen("libEGL.so", RTLD_NOW);
        if (egl) {
            using EglGPA = void* (*)(const char*);
            auto gpa = reinterpret_cast<EglGPA>(dlsym(egl, "eglGetProcAddress"));
            if (gpa) p = gpa("glDrawElements");
        }
    }
    if (!p) {
        logLine("EntityOutline: glDrawElements not found");
        return false;
    }
    void* o = nullptr;
    // pl::memory::hook returns 0 on success (same as matrix hook)
    if (pl::memory::hook(p, reinterpret_cast<void*>(&glDrawElementsDetour), &o) != 0 || !o) {
        logLine("EntityOutline: glDrawElements HOOK FAIL");
        return false;
    }
    g_glDrawElementsOrig = reinterpret_cast<PFN_glDrawElements>(o);
    d_glDepthMask = reinterpret_cast<PFN_glDepthMask>(glProc("glDepthMask"));
    loadLineGl();
    g_drawElementsHooked = true;
    logLine("EntityOutline: glDrawElements HOOKED width=%.1f glow=%.2f",
            g_outlineWidth.load(), g_outlineGlow.load());
    return true;
}

// Sync width/glow into Electrocharge Entity Outline pack config (if installed)
void writeEntityOutlineConfig() {
    const char* paths[] = {
        "/sdcard/games/EntityOutline/config.json",
        "/storage/emulated/0/games/EntityOutline/config.json",
        nullptr,
    };
    const float w = g_outlineWidth.load(std::memory_order_relaxed);
    const float g = g_outlineGlow.load(std::memory_order_relaxed);
    const float a = g_outlineAlpha.load(std::memory_order_relaxed);
    char body[512];
    // Keys from libEntityOutline.so: width, strength, thick, alpha, outlineColor, masterEnabled
    std::snprintf(body, sizeof(body),
        "{\n"
        "  \"masterEnabled\": true,\n"
        "  \"enabled\": true,\n"
        "  \"width\": %.2f,\n"
        "  \"thick\": %.2f,\n"
        "  \"strength\": %.2f,\n"
        "  \"alpha\": %.2f,\n"
        "  \"outlineColor\": [%.2f, %.2f, %.2f, %.2f]\n"
        "}\n",
        w, w, g, a,
        g_r.load(std::memory_order_relaxed), g_g.load(std::memory_order_relaxed),
        g_b.load(std::memory_order_relaxed), a);
    for (int i = 0; paths[i]; ++i) {
        FILE* f = std::fopen(paths[i], "w");
        if (!f) continue;
        std::fputs(body, f);
        std::fclose(f);
        logLine("EntityOutline: wrote config %s (width=%.1f glow=%.2f)", paths[i], w, g);
        return;
    }
}

void drawBoxEsp() {
    if (!g_boxEsp.load(std::memory_order_relaxed)) return;
    if (!g_vpValid.load(std::memory_order_acquire)) return;
    std::vector<Box> boxes;
    {
        std::lock_guard<std::mutex> lock(g_boxMu);
        boxes.swap(g_boxes);
    }
    if (boxes.empty()) return;
    float vp[16];
    {
        std::lock_guard<std::mutex> lock(g_vpMu);
        std::memcpy(vp, g_viewProj, sizeof(vp));
    }
    std::vector<float> lines;
    for (const auto& b : boxes) {
        if ((b.maxx - b.minx) < 0.2f || (b.maxz - b.minz) < 0.2f) continue;
        float c[8][3] = {
            {b.minx, b.miny, b.minz}, {b.maxx, b.miny, b.minz}, {b.maxx, b.maxy, b.minz}, {b.minx, b.maxy, b.minz},
            {b.minx, b.miny, b.maxz}, {b.maxx, b.miny, b.maxz}, {b.maxx, b.maxy, b.maxz}, {b.minx, b.maxy, b.maxz},
        };
        float s[8][2];
        bool ok[8];
        int okn = 0;
        for (int i = 0; i < 8; ++i) {
            ok[i] = worldToNdc(vp, c[i][0], c[i][1], c[i][2], s[i][0], s[i][1]);
            if (ok[i]) ++okn;
        }
        if (okn < 2) continue;
        const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                  {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (auto& e : edges) {
            if (ok[e[0]] && ok[e[1]]) {
                lines.push_back(s[e[0]][0]);
                lines.push_back(s[e[0]][1]);
                lines.push_back(s[e[1]][0]);
                lines.push_back(s[e[1]][1]);
            }
        }
    }
    if (lines.size() < 4) return;
    if (!loadLineGl()) return;
    if (d_glDisable) d_glDisable(GL_DEPTH_TEST);
    if (d_glEnable) d_glEnable(GL_BLEND);
    if (d_glBlendFunc) d_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (d_glLineWidth) d_glLineWidth(std::max(0.5f, std::min(12.f, g_outlineWidth.load(std::memory_order_relaxed))));
    d_glUseProgram(g_prog);
    if (d_glUniform4f && g_uColor >= 0) d_glUniform4f(g_uColor, 0.2f, 0.95f, 1.f, 0.95f); // Solstice-ish cyan
    d_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    d_glBufferData(GL_ARRAY_BUFFER, (long)(lines.size() * sizeof(float)), lines.data(), 0x88E4);
    d_glEnableVertexAttribArray((GLuint)g_aPos);
    d_glVertexAttribPointer((GLuint)g_aPos, 2, GL_FLOAT, 0, 0, nullptr);
    d_glDrawArrays(GL_LINES, 0, (GLsizei)(lines.size() / 2));
    d_glUseProgram(0);
    static int s_draw = 0;
    if (s_draw < 8) {
        logLine("SolsticeESP: drew %d segs from %d boxes", (int)(lines.size() / 2), (int)boxes.size());
        ++s_draw;
    }
}

// ---- Thread probe (debug only) ------------------------------------------------------------------
// Question it answers: is the actor-setup hook (main thread?) on the SAME thread as the GL draw calls?
// If yes, "arm on setup, act on next draw" can identify entity draws. If not, it cannot, and a native
// GL outline needs a different design. Output goes to systemvisuals_status.txt (first 12 reports).
std::atomic_bool g_probe{false};
std::atomic<int> g_pSetupTid{0}, g_pDrawTid{0}, g_pSwapTid{0};
std::atomic<int> g_pSetups{0}, g_pActorSetups{0}, g_pDraws{0}, g_pDrawsSame{0};
std::atomic<int> g_pSince{0}, g_pMax{0}, g_pSum{0}, g_pN{0};
std::atomic<int> g_pFrames{0}, g_pLines{0};
PFN_glDrawElements g_probeDrawOrig = nullptr;
bool g_probeDrawHooked = false;

void probeNoteSetup(bool hasActor) {
    g_pSetups.fetch_add(1, std::memory_order_relaxed);
    if (!hasActor) return;
    g_pActorSetups.fetch_add(1, std::memory_order_relaxed);
    g_pSetupTid.store(static_cast<int>(gettid()), std::memory_order_relaxed);
    const int between = g_pSince.exchange(0, std::memory_order_relaxed);
    g_pSum.fetch_add(between, std::memory_order_relaxed);
    g_pN.fetch_add(1, std::memory_order_relaxed);
    int cur = g_pMax.load(std::memory_order_relaxed);
    while (between > cur && !g_pMax.compare_exchange_weak(cur, between, std::memory_order_relaxed)) {
    }
}

void probeDrawDetour(unsigned int mode, int count, unsigned int type, const void* indices) {
    if (g_probe.load(std::memory_order_relaxed)) {
        const int tid = static_cast<int>(gettid());
        g_pDrawTid.store(tid, std::memory_order_relaxed);
        g_pDraws.fetch_add(1, std::memory_order_relaxed);
        if (tid == g_pSetupTid.load(std::memory_order_relaxed)) {
            g_pDrawsSame.fetch_add(1, std::memory_order_relaxed);
            g_pSince.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (g_probeDrawOrig) g_probeDrawOrig(mode, count, type, indices);
}

bool installProbeDrawHook() {
    if (g_probeDrawHooked) return true;
    void* p = glProc("glDrawElements");
    if (!p) {
        logLine("Probe: glDrawElements not found");
        return false;
    }
    void* o = nullptr;
    if (pl::memory::hook(p, reinterpret_cast<void*>(&probeDrawDetour), &o) != 0 || !o) {
        logLine("Probe: glDrawElements HOOK FAIL");
        return false;
    }
    g_probeDrawOrig = reinterpret_cast<PFN_glDrawElements>(o);
    g_probeDrawHooked = true;
    logLine("Probe: glDrawElements hooked (pass-through)");
    return true;
}

void probeFrameTick() {
    if (!g_probe.load(std::memory_order_relaxed)) return;
    g_pSwapTid.store(static_cast<int>(gettid()), std::memory_order_relaxed);
    if (g_pFrames.fetch_add(1, std::memory_order_relaxed) % 120 != 119) return;
    if (g_pLines.load(std::memory_order_relaxed) >= 12) return;
    const int idx = g_pLines.fetch_add(1, std::memory_order_relaxed) + 1;
    const int n = g_pN.exchange(0, std::memory_order_relaxed);
    const int sum = g_pSum.exchange(0, std::memory_order_relaxed);
    logLine("Probe#%d tid setup=%d draw=%d swap=%d", idx, g_pSetupTid.load(), g_pDrawTid.load(),
            g_pSwapTid.load());
    logLine("Probe#%d actorSetups=%d draws=%d drawsOnSetupThread=%d", idx,
            g_pActorSetups.exchange(0), g_pDraws.exchange(0), g_pDrawsSame.exchange(0));
    logLine("Probe#%d draws between actor setups: avg=%.1f max=%d", idx, n > 0 ? (float)sum / (float)n : 0.f,
            g_pMax.exchange(0));
    g_pSetups.store(0, std::memory_order_relaxed);
}

// ---- FP hand only ----
using RenderFirstPersonFn = void (*)(void*, void*, void*, void*, void*, void*);
RenderFirstPersonFn g_renderFpOriginal = nullptr;

void renderFirstPersonDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!g_renderFpOriginal) return;
    bactro::phase::inFirstPersonHand.store(true, std::memory_order_release);
    g_fpSticky.store(32, std::memory_order_release); // longer window for constants + glint + edge
    g_renderFpOriginal(self, a1, a2, a3, a4, a5);
}

using SetEntityConstantsFn = void (*)(void*, void*, const Color*, const void*, const void*, const Color*,
                                      const Color*, const Color*, const Color*, const void*, const void*, float,
                                      float, float, float);
SetEntityConstantsFn g_setEntityConstants = nullptr;

void setEntityConstantsDetour(void* entityConstants, void* renderContext, const Color* tileLightColor,
                              const void* tileLightColorUV, const void* blockLightColor, const Color* overlay,
                              const Color* changeColor, const Color* changeColor2, const Color* glintColor,
                              const void* glintUVScale, const void* uvAnim, float uvOffset1, float uvOffset2,
                              float uvRot1, float uvRot2) {
    if (!g_setEntityConstants) return;
    static int s_enter = 0;
    if (s_enter < 8) {
        logLine("HandChams: setEntityConstants ENTER #%d en=%d", s_enter,
                g_enabled.load(std::memory_order_relaxed) ? 1 : 0);
        ++s_enter;
    }
    if (!g_enabled.load(std::memory_order_relaxed)) {
        g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                             overlay, changeColor, changeColor2, glintColor, glintUVScale, uvAnim, uvOffset1,
                             uvOffset2, uvRot1, uvRot2);
        return;
    }
    const bool fp = bactro::phase::inFirstPersonHand.load(std::memory_order_acquire);
    const int sticky = g_fpSticky.load(std::memory_order_acquire);
    const bool handWindow = fp || sticky > 0;
    // FX or normal chams: apply during hand window (or anytime FX wants item tint)
    const bool fxOn = false;
    if (!fxOn && (!g_targetHand.load(std::memory_order_relaxed) || !handWindow)) {
        g_setEntityConstants(entityConstants, renderContext, tileLightColor, tileLightColorUV, blockLightColor,
                             overlay, changeColor, changeColor2, glintColor, glintUVScale, uvAnim, uvOffset1,
                             uvOffset2, uvRot1, uvRot2);
        return;
    }
    refresh();
    // Aggressive hand force: fill + edge + overlay all bright (outline-like glint family)
    static Color s_fill{};
    static Color s_edge{};
    static Color s_white{};
    s_fill = g_chams;
    if (s_fill.a < 0.85f) s_fill.a = 0.85f;
    s_edge = {1.f, 1.f, 1.f, 1.f};
    s_white = {1.f, 1.f, 1.f, 1.f};
    const Color* tile = &s_fill;
    static int s_app = 0;
    if (s_app < 16) {
        logLine("HandChams: CONST APPLY #%d fp=%d sticky=%d (forced white edge)", s_app, fp ? 1 : 0, sticky);
        ++s_app;
    }
    // overlay/changeColor/glint all forced — max chance RD shows hand tint
    g_setEntityConstants(entityConstants, renderContext, tile, tileLightColorUV, blockLightColor, &s_white, &s_fill,
                         &s_fill, &s_edge, glintUVScale, uvAnim, uvOffset1, uvOffset2, uvRot1, uvRot2);
}

using SetupActorGlintFn = void (*)(void*, void*, void*, const Color*, const Color*, const Color*, const Color*,
                                   float, float, float, float, const void*);
SetupActorGlintFn g_setupActorGlint = nullptr;
SetupActorGlintFn g_setupGlint = nullptr;

void setupActorGlintDetour(void* screenContext, void* entityContext, void* actor, const Color* overlay,
                           const Color* changeColor, const Color* changeColor2, const Color* glintColor,
                           float uvOffset1, float uvOffset2, float uvRot1, float uvRot2,
                           const void* lightEmissionColor) {
    if (!g_setupActorGlint) return;

    static int s_ag = 0;
    if (s_ag < 6) {
        logLine("HandChams: setupActorGlint ENTER #%d en=%d actor=%p", s_ag,
                g_enabled.load(std::memory_order_relaxed) ? 1 : 0, actor);
        ++s_ag;
    }

    if (g_probe.load(std::memory_order_relaxed)) probeNoteSetup(actor != nullptr);

    // Solstice ESP: arm model-matrix capture for this actor draw
    if (actor) {
        // Shared arm for Entity Outline module (and optional box ESP)
        bool ok = true;
        if (g_playersOnly.load(std::memory_order_relaxed) && g_isPlayer) {
            try {
                ok = g_isPlayer(actor);
            } catch (...) {
                ok = true;
            }
        }
        if (ok) {
            bactro::phase::entityMeshArmed.store(true, std::memory_order_release);
            if (g_enabled.load(std::memory_order_relaxed) && g_boxEsp.load(std::memory_order_relaxed))
                g_entityRenderArmed.store(true, std::memory_order_release);
        }
    }

    // Snow chams: static Color only. Filter by Players / Mobs / Hand.
    if (g_enabled.load(std::memory_order_relaxed)) {
        const bool fp = bactro::phase::inFirstPersonHand.load(std::memory_order_acquire);
        const int sticky = g_fpSticky.load(std::memory_order_acquire);
        const bool handWin = fp || sticky > 0;

        bool doChams = false;
        const bool fxOn = false;
        if (actor == nullptr) {
            // First-person hand / held item only.
            doChams = g_targetHand.load(std::memory_order_relaxed) || fxOn || handWin;
        }
        // World actors (players/mobs) are intentionally NOT tinted here: this glint path is the one
        // armor / enchanted gear is drawn through, which is what made chams show up on armor.

        if (doChams) {
            refresh();
            static Color s_fill{};
            static Color s_edge{};
            s_fill = g_chams;
            // Normal-hook hand outline attempt: bright white glint edge
            // (same family as item_in_hand_glint material path)
            if (handWin || actor == nullptr) {
                s_edge = {1.f, 1.f, 1.f, 1.f};
                if (s_fill.a < 0.75f) s_fill.a = 0.75f;
            } else {
                s_edge = g_outline;
                s_edge.a = 1.f;
            }

            static int s_gapp = 0;
            if (s_gapp < 12) {
                logLine("HandChams: GLINT APPLY #%d fp=%d actor=%p edge=white", s_gapp, fp ? 1 : 0, actor);
                ++s_gapp;
            }
            g_setupActorGlint(screenContext, entityContext, actor, &s_fill, &s_fill, &s_fill, &s_edge, uvOffset1,
                              uvOffset2, uvRot1, uvRot2, lightEmissionColor);
            return;
        }
    }

    g_setupActorGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor,
                      uvOffset1, uvOffset2, uvRot1, uvRot2, lightEmissionColor);
}

void setupGlintDetour(void* screenContext, void* entityContext, void* actor, const Color* overlay,
                      const Color* changeColor, const Color* changeColor2, const Color* glintColor, float uvOffset1,
                      float uvOffset2, float uvRot1, float uvRot2, const void* lightEmissionColor) {
    if (!g_setupGlint) return;
    const bool en = g_enabled.load(std::memory_order_relaxed);
    const bool handWin = bactro::phase::inFirstPersonHand.load(std::memory_order_acquire) ||
                         g_fpSticky.load(std::memory_order_acquire) > 0;
    if (en && g_targetHand.load(std::memory_order_relaxed) && (handWin || actor == nullptr)) {
        refresh();
        static Color fill{}, edge{};
        fill = g_chams;
        if (fill.a < 0.85f) fill.a = 0.85f;
        edge = {1.f, 1.f, 1.f, 1.f};
        static int n = 0;
        if (n < 10) {
            logLine("HandChams: SETUP_GLINT APPLY #%d (hand outline attempt)", n);
            ++n;
        }
        g_setupGlint(screenContext, entityContext, actor, &fill, &fill, &fill, &edge, uvOffset1, uvOffset2, uvRot1,
                     uvRot2, lightEmissionColor);
        return;
    }
    g_setupGlint(screenContext, entityContext, actor, overlay, changeColor, changeColor2, glintColor, uvOffset1,
                 uvOffset2, uvRot1, uvRot2, lightEmissionColor);
}

using SetupFoilFn = void (*)(void*, void*, void*, void*, void*, void*, void*, void*);
SetupFoilFn g_setupFoil = nullptr;
bool g_hookedFoil = false;

void setupFoilDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    if (!g_setupFoil) return;
    static int s_f = 0;
    if (s_f < 5) {
        logLine("HandChams: SetupFoil ENTER #%d en=%d", s_f, g_enabled.load() ? 1 : 0);
        ++s_f;
    }
    // Always call through — foil is enchanted-item path; still useful signal
    g_setupFoil(a0, a1, a2, a3, a4, a5, a6, a7);
}



// ItemInHandShaderSetup — arm FP window (6-arg safe pass-through, matches renderFirstPerson style)
using ItemInHandSetupFn = void (*)(void*, void*, void*, void*, void*, void*);
ItemInHandSetupFn g_itemHandSetup = nullptr;
bool g_itemHandHooked = false;

void itemInHandSetupDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!g_itemHandSetup) return;
    if (g_enabled.load(std::memory_order_relaxed) && g_targetHand.load(std::memory_order_relaxed)) {
        bactro::phase::inFirstPersonHand.store(true, std::memory_order_release);
        g_fpSticky.store(32, std::memory_order_release);
        static int n = 0;
        if (n < 8) {
            logLine("HandChams: ItemInHandShaderSetup #%d FP armed", n);
            ++n;
        }
    }
    g_itemHandSetup(a0, a1, a2, a3, a4, a5);
}

void tryInstallHooks() {
    void* o = nullptr;

    if (!g_isPlayer) {
        std::uintptr_t addr = bactro::memory::resolve(bactro::memory::SignatureId::ActorIsPlayer);
        if (addr) {
            g_isPlayer = reinterpret_cast<ActorIsPlayerFn>(addr);
            logLine("SnowChams: ActorIsPlayer @%p", reinterpret_cast<void*>(addr));
        }
    }

    // Matrix hook only if box ESP wanted (avoids extra work / crash surface on launch)
    if (g_boxEsp.load(std::memory_order_relaxed)) installMatrixHook();

    if (!g_renderFpHooked) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ItemInHandRendererRenderFirstPerson,
                                 reinterpret_cast<void*>(&renderFirstPersonDetour), &o) &&
            o) {
            g_renderFpOriginal = reinterpret_cast<RenderFirstPersonFn>(o);
            g_renderFpHooked = true;
            logLine("HandChams: renderFirstPerson hooked");
        } else
            logLine("HandChams: renderFirstPerson FAIL");

    if (!g_itemHandHooked) {
        void* o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ItemInHandShaderSetup,
                                 reinterpret_cast<void*>(&itemInHandSetupDetour), &o) &&
            o) {
            g_itemHandSetup = reinterpret_cast<ItemInHandSetupFn>(o);
            g_itemHandHooked = true;
            logLine("HandChams: ItemInHandShaderSetup hooked");
        } else
            logLine("HandChams: ItemInHandShaderSetup FAIL");
    }

    }
    if (!g_hookedEntity) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetEntityConstants,
                                 reinterpret_cast<void*>(&setEntityConstantsDetour), &o) &&
            o) {
            g_setEntityConstants = reinterpret_cast<SetEntityConstantsFn>(o);
            g_hookedEntity = true;
            logLine("HandChams: setEntityConstants hooked");
        } else
            logLine("HandChams: setEntityConstants FAIL");
    }
    if (!g_hookedActor) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetupShaderParametersActorGlint,
                                 reinterpret_cast<void*>(&setupActorGlintDetour), &o) &&
            o) {
            g_setupActorGlint = reinterpret_cast<SetupActorGlintFn>(o);
            g_hookedActor = true;
            logLine("HandChams: setupActorGlint hooked");
        } else
            logLine("HandChams: setupActorGlint FAIL");
    }
    if (!g_hookedGlint) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetupShaderParametersGlint,
                                 reinterpret_cast<void*>(&setupGlintDetour), &o) &&
            o) {
            g_setupGlint = reinterpret_cast<SetupActorGlintFn>(o);
            g_hookedGlint = true;
            logLine("HandChams: setupGlint hooked");
        } else
            logLine("HandChams: setupGlint FAIL");
    }
    if (!g_hookedFoil) {
        o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetupFoilShaderParameters,
                                 reinterpret_cast<void*>(&setupFoilDetour), &o) &&
            o) {
            g_setupFoil = reinterpret_cast<SetupFoilFn>(o);
            g_hookedFoil = true;
            logLine("HandChams: setupFoil hooked");
        } else
            logLine("HandChams: setupFoil FAIL");
    }
    logLine("SnowChams: hooks fp=%d entity=%d actor=%d glint=%d foil=%d itemHand=%d matrix=%d", g_renderFpHooked ? 1 : 0, g_hookedEntity ? 1 : 0, g_hookedActor ? 1 : 0, g_hookedGlint ? 1 : 0, g_hookedFoil ? 1 : 0, g_itemHandHooked ? 1 : 0, g_glUniformHooked ? 1 : 0);
}

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    logLine("SnowChams %s", enabled ? "ON" : "OFF");
    if (enabled) {
        tryInstallHooks();
        if (g_boxEsp.load(std::memory_order_relaxed)) installMatrixHook();
    }
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "r")
            g_r.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "g")
            g_g.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "b")
            g_b.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "opacity")
            g_opacity.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "handOnly")
            g_handOnly.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "targetPlayers")
            g_targetPlayers.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "targetMobs")
            g_targetMobs.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "probe") {
            const bool on = (value == "true" || value == "1");
            g_probe.store(on, std::memory_order_relaxed);
            if (on) {
                tryInstallHooks();
                installProbeDrawHook();
            }
            logLine(on ? "Probe ON (join a world with players/mobs nearby, wait ~30s)" : "Probe OFF");
        }
        else if (key == "targetHand")
            g_targetHand.store(value == "true" || value == "1", std::memory_order_relaxed);
                else if (key == "boxEsp") {
            g_boxEsp.store(false, std::memory_order_relaxed); // ignored: see-through ESP removed
        } else if (key == "playersOnly")
            g_playersOnly.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "outlineWidth")
            g_outlineWidth.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "outlineGlow")
            g_outlineGlow.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "outlineAlpha")
            g_outlineAlpha.store(std::stof(std::string(value)), std::memory_order_relaxed);
        if (key == "outlineWidth" || key == "outlineGlow" || key == "outlineAlpha" ||
            key == "r" || key == "g" || key == "b")
            writeEntityOutlineConfig();
        refresh();
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Hand Chams");
    b.description("Tint on first-person hand / held item. No armor tint, no through-wall ESP.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("r", "Red", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("g", "Green", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("b", "Blue", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("opacity", "Opacity", pl::modmenu::ConfigType::SliderFloat, "0.85", "0.05", "1.0", "");
    b.config("targetHand", "Hand / items / cosmetics", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("probe", "Thread probe (debug)", pl::modmenu::ConfigType::Toggle, "false", "", "", "");

    b.registerModule();
}

void onSignaturesReady() {
    logLine("SnowChams: ready — join world, then enable");
    if (g_enabled.load(std::memory_order_relaxed)) {
        logLine("SnowChams: was ON — installing hooks now");
        tryInstallHooks();
    }
}

void shutdown() { g_enabled.store(false, std::memory_order_release); }

void onPostFrame() {
    probeFrameTick();
    if (g_enabled.load(std::memory_order_relaxed)) drawBoxEsp();
    static int s_wlog = 0;
    const int wd = g_wireDraws.exchange(0, std::memory_order_relaxed);
    if (wd > 0 && s_wlog < 10) {
        logLine("EntityOutline: wire draws this frame=%d", wd);
        ++s_wlog;
    }
    g_entityRenderArmed.store(false, std::memory_order_release);
    bactro::phase::inFirstPersonHand.store(false, std::memory_order_release);
    {
        int s = g_fpSticky.load(std::memory_order_relaxed);
        if (s > 0) g_fpSticky.store(s - 1, std::memory_order_relaxed);
    }
}

} // namespace bactro::handchams
