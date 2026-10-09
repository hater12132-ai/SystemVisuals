#include "bactro/EntityOutline.hpp"
#include "bactro/RenderPhase.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <android/log.h>
#include <dlfcn.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#define EO_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SystemVisuals", __VA_ARGS__)

namespace bactro::entityoutline {
namespace {

constexpr const char* kModuleId = "bactro.entityoutline";

std::atomic_bool g_enabled{false};
std::atomic_bool g_playersOnly{false};
std::atomic_bool g_handOutline{true};
std::atomic<float> g_width{4.0f};
std::atomic<float> g_glow{0.8f};
std::atomic<float> g_r{1.f}, g_g{1.f}, g_b{1.f}, g_a{1.f};

using ActorIsPlayerFn = bool (*)(void*);
ActorIsPlayerFn g_isPlayer = nullptr;

struct Color {
    float r, g, b, a;
};

struct Box {
    float minx, miny, minz, maxx, maxy, maxz;
};

std::mutex g_boxMu;
std::vector<Box> g_boxes;
std::mutex g_vpMu;
float g_viewProj[16]{};
std::atomic_bool g_vpValid{false};

using SetupActorGlintFn = void (*)(void*, void*, void*, const Color*, const Color*, const Color*, const Color*, float,
                                   float, float, float, const void*);
SetupActorGlintFn g_setupActorGlint = nullptr;
bool g_glintHooked = false;

using PFN_glUniformMatrix4fv = void (*)(int, int, unsigned char, const float*);
PFN_glUniformMatrix4fv g_glUniformOrig = nullptr;
bool g_matrixHooked = false;
std::atomic_bool g_meshArmed{false};

// GLES line helpers
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
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

void* glProc(const char* name) {
    void* p = nullptr;
    void* gles = dlopen("libGLESv2.so", RTLD_NOLOAD);
    if (!gles) gles = dlopen("libGLESv3.so", RTLD_NOLOAD);
    if (!gles) gles = dlopen("libGLESv2.so", RTLD_NOW);
    if (gles) p = dlsym(gles, name);
    if (!p) {
        void* egl = dlopen("libEGL.so", RTLD_NOW);
        if (egl) {
            using GPA = void* (*)(const char*);
            auto gpa = reinterpret_cast<GPA>(dlsym(egl, "eglGetProcAddress"));
            if (gpa) p = gpa(name);
        }
    }
    return p;
}

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
bool g_lineReady = false;

void logLine(const char* fmt, ...) {
    char buf[240];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    EO_LOGI("%s", buf);
}

bool loadLineGl() {
    if (g_lineReady) return true;
#define L(n) d_##n = reinterpret_cast<decltype(d_##n)>(glProc(#n))
    L(glCreateShader);
    L(glShaderSource);
    L(glCompileShader);
    L(glCreateProgram);
    L(glAttachShader);
    L(glLinkProgram);
    L(glUseProgram);
    L(glGetAttribLocation);
    L(glGetUniformLocation);
    L(glUniform4f);
    L(glGenBuffers);
    L(glBindBuffer);
    L(glBufferData);
    L(glEnableVertexAttribArray);
    L(glVertexAttribPointer);
    L(glDrawArrays);
    L(glBlendFunc);
    L(glLineWidth);
    L(glEnable);
    L(glDisable);
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
    g_lineReady = g_prog != 0;
    return g_lineReady;
}

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
    float xz = std::sqrt(tx * tx + tz * tz);
    if (xz < 0.35f || xz > 128.f) return false;
    if (ty < -64.f || ty > 400.f) return false;
    return true;
}

bool worldToNdc(const float* vp, float x, float y, float z, float& ox, float& oy) {
    float clip[4];
    clip[0] = vp[0] * x + vp[4] * y + vp[8] * z + vp[12];
    clip[1] = vp[1] * x + vp[5] * y + vp[9] * z + vp[13];
    clip[2] = vp[2] * x + vp[6] * y + vp[10] * z + vp[14];
    clip[3] = vp[3] * x + vp[7] * y + vp[11] * z + vp[15];
    if (std::fabs(clip[3]) < 1e-5f) return false;
    ox = clip[0] / clip[3];
    oy = clip[1] / clip[3];
    return std::fabs(ox) <= 1.5f && std::fabs(oy) <= 1.5f;
}

void noteBox(float x, float y, float z) {
    Box b{x - 0.35f, y - 0.05f, z - 0.35f, x + 0.35f, y + 1.85f, z + 0.35f};
    std::lock_guard<std::mutex> lock(g_boxMu);
    if (g_boxes.size() < 96) g_boxes.push_back(b);
}

void glUniformMatrix4fvDetour(int location, int count, unsigned char transpose, const float* value) {
    if (value && count >= 1 && g_enabled.load(std::memory_order_relaxed)) {
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
        } else if (g_meshArmed.load(std::memory_order_acquire) ||
                   bactro::phase::entityMeshArmed.load(std::memory_order_acquire)) {
            float tx, ty, tz;
            if (looksLikeWorldModel(m, tx, ty, tz)) noteBox(tx, ty, tz);
        }
    }
    if (g_glUniformOrig) g_glUniformOrig(location, count, transpose, value);
}

void setupActorGlintDetour(void* sc, void* ec, void* actor, const Color* o, const Color* c1, const Color* c2,
                           const Color* g, float u1, float u2, float r1, float r2, const void* le) {
    if (g_enabled.load(std::memory_order_relaxed) && actor) {
        bool arm = true;
        if (g_playersOnly.load(std::memory_order_relaxed) && g_isPlayer) {
            try {
                arm = g_isPlayer(actor);
            } catch (...) {
                arm = true;
            }
        }
        if (arm) {
            g_meshArmed.store(true, std::memory_order_release);
            bactro::phase::entityMeshArmed.store(true, std::memory_order_release);
        }
    }
    if (g_setupActorGlint) g_setupActorGlint(sc, ec, actor, o, c1, c2, g, u1, u2, r1, r2, le);
}

void appendBoxLines(std::vector<float>& lines, const float* vp, const Box& b) {
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
    if (okn < 2) return;
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

void appendScreenRect(std::vector<float>& lines, float x0, float y0, float x1, float y1) {
    // rectangle in NDC
    lines.insert(lines.end(), {x0, y0, x1, y0, x1, y0, x1, y1, x1, y1, x0, y1, x0, y1, x0, y0});
}

void drawLines(const std::vector<float>& lines, float width, float r, float g, float b, float a) {
    if (lines.size() < 4 || !loadLineGl()) return;
    if (d_glDisable) d_glDisable(GL_DEPTH_TEST);
    if (d_glEnable) d_glEnable(GL_BLEND);
    if (d_glBlendFunc) d_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (d_glLineWidth) d_glLineWidth(width);
    d_glUseProgram(g_prog);
    if (d_glUniform4f && g_uColor >= 0) d_glUniform4f(g_uColor, r, g, b, a);
    d_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    d_glBufferData(GL_ARRAY_BUFFER, (long)(lines.size() * sizeof(float)), lines.data(), 0x88E4);
    d_glEnableVertexAttribArray((GLuint)g_aPos);
    d_glVertexAttribPointer((GLuint)g_aPos, 2, GL_FLOAT, 0, 0, nullptr);
    d_glDrawArrays(GL_LINES, 0, (GLsizei)(lines.size() / 2));
    d_glUseProgram(0);
}

void drawFrame() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;

    std::vector<float> lines;
    float vp[16];
    bool haveVp = false;
    {
        std::lock_guard<std::mutex> lock(g_vpMu);
        if (g_vpValid.load(std::memory_order_acquire)) {
            std::memcpy(vp, g_viewProj, sizeof(vp));
            haveVp = true;
        }
    }
    std::vector<Box> boxes;
    {
        std::lock_guard<std::mutex> lock(g_boxMu);
        boxes.swap(g_boxes);
    }
    if (haveVp) {
        for (const auto& b : boxes) appendBoxLines(lines, vp, b);
    }

    // First-person hand outline: fixed NDC frame where the hand usually sits
    if (g_handOutline.load(std::memory_order_relaxed) &&
        (bactro::phase::inFirstPersonHand.load(std::memory_order_acquire))) {
        // lower-right / center-right hand-ish region
        appendScreenRect(lines, 0.15f, -0.95f, 0.75f, -0.25f);
        appendScreenRect(lines, 0.20f, -0.90f, 0.70f, -0.30f); // inner for "glow"
    }

    if (lines.empty()) {
        g_meshArmed.store(false, std::memory_order_release);
        bactro::phase::entityMeshArmed.store(false, std::memory_order_release);
        return;
    }

    const float w = std::max(0.5f, std::min(12.f, g_width.load(std::memory_order_relaxed)));
    const float glow = std::max(0.f, std::min(1.f, g_glow.load(std::memory_order_relaxed)));
    const float r = g_r.load(std::memory_order_relaxed);
    const float g = g_g.load(std::memory_order_relaxed);
    const float b = g_b.load(std::memory_order_relaxed);
    const float a = g_a.load(std::memory_order_relaxed);

    // glow passes
    if (glow > 0.05f) {
        const int passes = 1 + static_cast<int>(glow * 3.f);
        for (int i = passes; i >= 1; --i) {
            drawLines(lines, w * (1.f + 0.55f * (float)i), r, g, b, a * (0.25f + 0.15f * (float)i));
        }
    }
    drawLines(lines, w, r, g, b, a);

    static int s_log = 0;
    if (s_log < 12) {
        logLine("EntityOutline: drew %d segs boxes=%d handFP=%d w=%.1f glow=%.2f", (int)(lines.size() / 2),
                (int)boxes.size(), bactro::phase::inFirstPersonHand.load() ? 1 : 0, w, glow);
        ++s_log;
    }

    g_meshArmed.store(false, std::memory_order_release);
    bactro::phase::entityMeshArmed.store(false, std::memory_order_release);
}

bool installMatrixHook() {
    if (g_matrixHooked) return true;
    void* p = glProc("glUniformMatrix4fv");
    if (!p) return false;
    void* o = nullptr;
    if (pl::memory::hook(p, reinterpret_cast<void*>(&glUniformMatrix4fvDetour), &o) != 0 || !o) {
        logLine("EntityOutline: matrix HOOK FAIL");
        return false;
    }
    g_glUniformOrig = reinterpret_cast<PFN_glUniformMatrix4fv>(o);
    g_matrixHooked = true;
    logLine("EntityOutline: matrix HOOKED (box + hand outline)");
    return true;
}

bool installGlintArm() {
    if (g_glintHooked) return true;
    if (!g_isPlayer) {
        auto addr = bactro::memory::resolve(bactro::memory::SignatureId::ActorIsPlayer);
        if (addr) g_isPlayer = reinterpret_cast<ActorIsPlayerFn>(addr);
    }
    void* o = nullptr;
    if (bactro::memory::hook(bactro::memory::SignatureId::ActorShaderManagerSetupShaderParametersActorGlint,
                             reinterpret_cast<void*>(&setupActorGlintDetour), &o) &&
        o) {
        g_setupActorGlint = reinterpret_cast<SetupActorGlintFn>(o);
        g_glintHooked = true;
        logLine("EntityOutline: setupActorGlint ARMED");
        return true;
    }
    logLine("EntityOutline: setupActorGlint FAIL (using shared phase arm from HandChams)");
    return false;
}

void tryInstall() {
    installMatrixHook();
    installGlintArm();
    loadLineGl();
}

void onToggle(std::string_view, bool on) {
    g_enabled.store(on, std::memory_order_release);
    logLine(on ? "EntityOutline ON" : "EntityOutline OFF");
    if (on) tryInstall();
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "width")
            g_width.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "glow")
            g_glow.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "r")
            g_r.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "g")
            g_g.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "b")
            g_b.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "alpha")
            g_a.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "playersOnly")
            g_playersOnly.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "handOutline")
            g_handOutline.store(value == "true" || value == "1", std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Entity Outline");
    b.description("Visible GLES outlines: entity boxes + optional FP hand frame. Width + glow.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("width", "Outline width", pl::modmenu::ConfigType::SliderFloat, "4.0", "0.5", "12", "");
    b.config("glow", "Outline glow", pl::modmenu::ConfigType::SliderFloat, "0.8", "0", "1", "");
    b.config("r", "Color R", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("g", "Color G", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("b", "Color B", pl::modmenu::ConfigType::SliderFloat, "1.00", "0", "1", "");
    b.config("alpha", "Alpha", pl::modmenu::ConfigType::SliderFloat, "1.00", "0.1", "1", "");
    b.config("playersOnly", "Players only", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.config("handOutline", "Hand outline (FP frame)", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.registerModule();
}

void onSignaturesReady() {
    logLine("EntityOutline: ready — enable for visible box+hand outlines");
    if (g_enabled.load(std::memory_order_relaxed)) tryInstall();
}

void onPostFrame() { drawFrame(); }

void shutdown() {
    g_enabled.store(false, std::memory_order_release);
    g_meshArmed.store(false, std::memory_order_release);
}

} // namespace bactro::entityoutline
