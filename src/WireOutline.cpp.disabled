#include "bactro/WireOutline.hpp"
#include "bactro/RenderPhase.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <EGL/egl.h>
#include <android/log.h>
#include <dlfcn.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define WO_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

namespace bactro::wireoutline {
namespace {

constexpr const char* kModuleId = "bactro.wireoutline";

std::atomic_bool g_enabled{false};
std::atomic_bool g_handItems{true};
std::atomic_bool g_armor{true};
std::atomic_bool g_players{true};
std::atomic_bool g_crystals{true};
std::atomic_bool g_draw3d{true};
std::atomic_bool g_outlineSelf{true};
std::atomic<float> g_lineWidth{2.0f};

using ActorIsPlayerFn = bool (*)(void*);
ActorIsPlayerFn g_isPlayer = nullptr;
void* g_localPlayer = nullptr;
void* g_clientInstance = nullptr;
using GetLocalPlayerFn = void* (*)(void*);
GetLocalPlayerFn g_getLocalPlayer = nullptr;

std::mutex g_vpMu;
float g_viewProj[16]{};
std::atomic_bool g_vpValid{false};
std::atomic_int g_vpHits{0};

struct Box {
    float minx, miny, minz, maxx, maxy, maxz;
};
std::mutex g_boxMu;
std::vector<Box> g_boxes;

void logLine(const char* fmt, ...) {
    char buf[240];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    WO_LOGI("%s", buf);
}

bool finite3(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

bool looksLikeWorldPos(float x, float y, float z) {
    if (!finite3(x, y, z)) return false;
    if (std::fabs(x) > 300000.f || std::fabs(z) > 300000.f) return false;
    if (y < -80.f || y > 400.f) return false;
    if (std::fabs(x) < 1e-3f && std::fabs(y) < 1e-3f && std::fabs(z) < 1e-3f) return false;
    return true;
}

bool looksLikeViewProj(const float* m) {
    if (!m) return false;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    bool id = true;
    for (int i = 0; i < 16; ++i) {
        float expect = (i % 5 == 0) ? 1.f : 0.f;
        if (std::fabs(m[i] - expect) > 1e-4f) {
            id = false;
            break;
        }
    }
    if (id) return false;
    return (std::fabs(m[11]) + std::fabs(m[14]) + std::fabs(m[15] - 1.f)) > 0.05f;
}

bool probeActorBox(void* actor, Box& out) {
    if (!actor) return false;
    auto* base = reinterpret_cast<unsigned char*>(actor);
    static const int kPosOff[] = {0x48, 0x50, 0x68, 0x70, 0x88, 0x90, 0xA0, 0xB0, 0xC8, 0xD0,
                                  0x100, 0x108, 0x120, 0x128, 0x148, 0x150, 0x168, 0x190,
                                  0x1A0, 0x1C0, 0x1E0, 0x200, 0x220, 0x240, 0x280, 0x2A0,
                                  0x2C0, 0x300, 0x340, 0x380, 0x3C0, 0x400};
    for (int off : kPosOff) {
        auto* f = reinterpret_cast<float*>(base + off);
        if (finite3(f[0], f[1], f[2]) && finite3(f[3], f[4], f[5])) {
            float dx = f[3] - f[0], dy = f[4] - f[1], dz = f[5] - f[2];
            if (dx > 0.05f && dy > 0.05f && dz > 0.05f && dx < 8.f && dy < 12.f && dz < 8.f) {
                float cx = (f[0] + f[3]) * 0.5f, cy = (f[1] + f[4]) * 0.5f, cz = (f[2] + f[5]) * 0.5f;
                if (looksLikeWorldPos(cx, cy, cz)) {
                    out = {f[0], f[1], f[2], f[3], f[4], f[5]};
                    return true;
                }
            }
        }
        if (looksLikeWorldPos(f[0], f[1], f[2])) {
            float x = f[0], y = f[1], z = f[2];
            out = {x - 0.3f, y, z - 0.3f, x + 0.3f, y + 1.8f, z + 0.3f};
            return true;
        }
    }
    return false;
}

// ---- glUniformMatrix4fv (absolute hook via pl::memory::hook, same as eglSwapBuffers) ----
using PFN_glUniformMatrix4fv = void (*)(int, int, unsigned char, const float*);
PFN_glUniformMatrix4fv g_glUniformMatrix4fvOrig = nullptr;
std::atomic_bool g_glHooked{false};

void glUniformMatrix4fvDetour(int location, int count, unsigned char transpose, const float* value) {
    if (value && count >= 1 && looksLikeViewProj(value)) {
        {
            std::lock_guard<std::mutex> lock(g_vpMu);
            if (transpose) {
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        g_viewProj[c * 4 + r] = value[r * 4 + c];
            } else {
                std::memcpy(g_viewProj, value, 16 * sizeof(float));
            }
        }
        g_vpValid.store(true, std::memory_order_release);
        int n = g_vpHits.fetch_add(1, std::memory_order_relaxed);
        if (n < 8)
            logLine("WireOutline: VP captured loc=%d (#%d)", location, n);
    }
    if (g_glUniformMatrix4fvOrig)
        g_glUniformMatrix4fvOrig(location, count, transpose, value);
}

void* resolveGlUniformMatrix4fv() {
    if (void* p = reinterpret_cast<void*>(eglGetProcAddress("glUniformMatrix4fv")))
        return p;
    void* h = dlopen("libGLESv2.so", RTLD_NOW);
    if (!h) h = dlopen("libGLESv3.so", RTLD_NOW);
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW);
    return h ? dlsym(h, "glUniformMatrix4fv") : nullptr;
}

bool installGlHook() {
    if (g_glHooked.load(std::memory_order_acquire)) return true;
    void* target = resolveGlUniformMatrix4fv();
    if (!target) {
        logLine("WireOutline: glUniformMatrix4fv not found");
        return false;
    }
    logLine("WireOutline: glUniformMatrix4fv @%p", target);
    void* o = nullptr;
    // Same absolute-address hook MotionBlur uses for eglSwapBuffers (0 = success)
    if (pl::memory::hook(target, reinterpret_cast<void*>(&glUniformMatrix4fvDetour), &o) == 0 && o) {
        g_glUniformMatrix4fvOrig = reinterpret_cast<PFN_glUniformMatrix4fv>(o);
        g_glHooked.store(true, std::memory_order_release);
        logLine("WireOutline: glUniformMatrix4fv HOOKED (VP capture active)");
        return true;
    }
    logLine("WireOutline: glUniformMatrix4fv hook FAIL");
    return false;
}

// ---- NormalTick: local player ----
using NormalTickFn = void (*)(void*);
NormalTickFn g_tickOriginal = nullptr;
bool g_tickHooked = false;

void normalTickDetour(void* self) {
    if (g_tickOriginal) g_tickOriginal(self);
    if (!g_enabled.load(std::memory_order_relaxed) || !self) return;
    if (g_isPlayer) {
        bool ok = false;
        try {
            ok = g_isPlayer(self);
        } catch (...) {
            ok = false;
        }
        if (ok) g_localPlayer = self;
    }
}

using RenderFirstPersonFn = void (*)(void*, void*, void*, void*, void*, void*);
RenderFirstPersonFn g_renderFpOriginal = nullptr;
bool g_fpHooked = false;

void renderFirstPersonDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!g_renderFpOriginal) return;
    const bool want = g_enabled.load(std::memory_order_relaxed) && g_handItems.load(std::memory_order_relaxed);
    if (want) bactro::phase::inFirstPersonHand.store(true, std::memory_order_release);
    g_renderFpOriginal(self, a1, a2, a3, a4, a5);
    if (want) bactro::phase::inFirstPersonHand.store(false, std::memory_order_release);
}

void collectBoxes() {
    std::vector<Box> next;
    if (g_outlineSelf.load(std::memory_order_relaxed) && g_localPlayer) {
        Box b{};
        if (probeActorBox(g_localPlayer, b)) {
            next.push_back(b);
            static int once = 0;
            if (once < 3) {
                logLine("WireOutline: self AABB (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)", b.minx, b.miny, b.minz,
                        b.maxx, b.maxy, b.maxz);
                ++once;
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_boxMu);
    g_boxes.swap(next);
}

// ---- GLES lines ----
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLfloat = float;
using GLchar = char;
using GLsizei = int;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_LINES = 0x0001;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;

void* glProc(const char* name) {
    if (void* p = reinterpret_cast<void*>(eglGetProcAddress(name))) return p;
    void* h = dlopen("libGLESv2.so", RTLD_NOW);
    if (!h) h = dlopen("libGLESv3.so", RTLD_NOW);
    return h ? dlsym(h, name) : nullptr;
}

#define GLDECL(ret, name, ...) \
    using PFN_##name = ret (*)(__VA_ARGS__); \
    PFN_##name p_##name = nullptr

GLDECL(GLuint, glCreateShader, GLenum);
GLDECL(void, glShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*);
GLDECL(void, glCompileShader, GLuint);
GLDECL(void, glGetShaderiv, GLuint, GLenum, GLint*);
GLDECL(GLuint, glCreateProgram);
GLDECL(void, glAttachShader, GLuint, GLuint);
GLDECL(void, glLinkProgram, GLuint);
GLDECL(void, glGetProgramiv, GLuint, GLenum, GLint*);
GLDECL(void, glUseProgram, GLuint);
GLDECL(GLint, glGetAttribLocation, GLuint, const GLchar*);
GLDECL(GLint, glGetUniformLocation, GLuint, const GLchar*);
GLDECL(void, glUniform4f, GLint, GLfloat, GLfloat, GLfloat, GLfloat);
GLDECL(void, glGenBuffers, GLsizei, GLuint*);
GLDECL(void, glBindBuffer, GLenum, GLuint);
GLDECL(void, glBufferData, GLenum, long, const void*, GLenum);
GLDECL(void, glEnableVertexAttribArray, GLuint);
GLDECL(void, glVertexAttribPointer, GLuint, GLint, GLenum, unsigned char, GLsizei, const void*);
GLDECL(void, glDrawArrays, GLenum, GLint, GLsizei);
GLDECL(void, glEnable, GLenum);
GLDECL(void, glDisable, GLenum);
GLDECL(void, glBlendFunc, GLenum, GLenum);
GLDECL(void, glLineWidth, GLfloat);
GLDECL(void, glDeleteShader, GLuint);

bool g_glReady = false;
GLuint g_prog = 0;
GLuint g_vbo = 0;
GLint g_aPos = -1;
GLint g_uColor = -1;
std::mutex g_glMu;
int g_drawLog = 0;

bool loadGl() {
    if (g_glReady) return g_prog != 0;
    g_glReady = true;
#define L(n) p_##n = reinterpret_cast<PFN_##n>(glProc(#n))
    L(glCreateShader); L(glShaderSource); L(glCompileShader); L(glGetShaderiv);
    L(glCreateProgram); L(glAttachShader); L(glLinkProgram); L(glGetProgramiv);
    L(glUseProgram); L(glGetAttribLocation); L(glGetUniformLocation); L(glUniform4f);
    L(glGenBuffers); L(glBindBuffer); L(glBufferData);
    L(glEnableVertexAttribArray); L(glVertexAttribPointer); L(glDrawArrays);
    L(glEnable); L(glDisable); L(glBlendFunc); L(glLineWidth); L(glDeleteShader);
#undef L
    if (!p_glCreateShader) return false;
    const char* vs = "attribute vec2 aPos; void main(){ gl_Position=vec4(aPos,0.0,1.0); }";
    const char* fs = "precision mediump float; uniform vec4 uColor; void main(){ gl_FragColor=uColor; }";
    auto compile = [](GLenum t, const char* s) -> GLuint {
        GLuint sh = p_glCreateShader(t);
        p_glShaderSource(sh, 1, &s, nullptr);
        p_glCompileShader(sh);
        GLint ok = 0;
        p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        return ok ? sh : 0;
    };
    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return false;
    g_prog = p_glCreateProgram();
    p_glAttachShader(g_prog, v);
    p_glAttachShader(g_prog, f);
    p_glLinkProgram(g_prog);
    GLint linked = 0;
    p_glGetProgramiv(g_prog, GL_LINK_STATUS, &linked);
    p_glDeleteShader(v);
    p_glDeleteShader(f);
    if (!linked) {
        g_prog = 0;
        return false;
    }
    g_aPos = p_glGetAttribLocation(g_prog, "aPos");
    g_uColor = p_glGetUniformLocation(g_prog, "uColor");
    p_glGenBuffers(1, &g_vbo);
    logLine("WireOutline: GLES line program OK");
    return true;
}

bool worldToNdc(const float vp[16], float x, float y, float z, float& ox, float& oy) {
    float clipX = vp[0] * x + vp[4] * y + vp[8] * z + vp[12];
    float clipY = vp[1] * x + vp[5] * y + vp[9] * z + vp[13];
    float clipW = vp[3] * x + vp[7] * y + vp[11] * z + vp[15];
    if (std::fabs(clipW) < 1e-5f) return false;
    ox = clipX / clipW;
    oy = clipY / clipW;
    return std::isfinite(ox) && std::isfinite(oy) && ox > -2.f && ox < 2.f && oy > -2.f && oy < 2.f;
}

void projectAndDraw(const std::vector<Box>& boxes) {
    if (boxes.empty() || !g_draw3d.load(std::memory_order_relaxed)) return;
    if (!g_vpValid.load(std::memory_order_acquire)) {
        if (g_drawLog < 5) {
            logLine("WireOutline: boxes=%d waiting VP (glHook=%d)", (int)boxes.size(),
                    g_glHooked.load() ? 1 : 0);
            ++g_drawLog;
        }
        return;
    }
    float vp[16];
    {
        std::lock_guard<std::mutex> lock(g_vpMu);
        std::memcpy(vp, g_viewProj, sizeof(vp));
    }
    std::vector<float> lines;
    for (const auto& b : boxes) {
        float c[8][3] = {
            {b.minx, b.miny, b.minz}, {b.maxx, b.miny, b.minz}, {b.maxx, b.maxy, b.minz}, {b.minx, b.maxy, b.minz},
            {b.minx, b.miny, b.maxz}, {b.maxx, b.miny, b.maxz}, {b.maxx, b.maxy, b.maxz}, {b.minx, b.maxy, b.maxz},
        };
        float s[8][2];
        bool ok[8];
        for (int i = 0; i < 8; ++i)
            ok[i] = worldToNdc(vp, c[i][0], c[i][1], c[i][2], s[i][0], s[i][1]);
        const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                  {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (auto& e : edges)
            if (ok[e[0]] && ok[e[1]]) {
                lines.push_back(s[e[0]][0]);
                lines.push_back(s[e[0]][1]);
                lines.push_back(s[e[1]][0]);
                lines.push_back(s[e[1]][1]);
            }
    }
    if (lines.empty()) return;

    std::lock_guard<std::mutex> lock(g_glMu);
    if (!loadGl() || !g_prog) return;
    p_glDisable(GL_DEPTH_TEST);
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (p_glLineWidth) p_glLineWidth(g_lineWidth.load());
    p_glUseProgram(g_prog);
    if (p_glUniform4f && g_uColor >= 0)
        p_glUniform4f(g_uColor, 1.f, 1.f, 1.f, 0.95f);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, (long)(lines.size() * sizeof(float)), lines.data(), 0x88E4);
    p_glEnableVertexAttribArray((GLuint)g_aPos);
    p_glVertexAttribPointer((GLuint)g_aPos, 2, GL_FLOAT, 0, 0, nullptr);
    p_glDrawArrays(GL_LINES, 0, (GLsizei)(lines.size() / 2));
    p_glUseProgram(0);
    if (g_drawLog < 12) {
        logLine("WireOutline: drew %d segs", (int)(lines.size() / 4));
        ++g_drawLog;
    }
}

void resolveAndHook() {
    if (!g_isPlayer) {
        auto a = bactro::memory::resolve(bactro::memory::SignatureId::ActorIsPlayer);
        if (a) {
            g_isPlayer = reinterpret_cast<ActorIsPlayerFn>(a);
            logLine("WireOutline: ActorIsPlayer @%p", reinterpret_cast<void*>(a));
        }
    }
    if (!g_tickHooked) {
        void* o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::NormalTick,
                                 reinterpret_cast<void*>(&normalTickDetour), &o) &&
            o) {
            g_tickOriginal = reinterpret_cast<NormalTickFn>(o);
            g_tickHooked = true;
            logLine("WireOutline: NormalTick hooked");
        } else
            logLine("WireOutline: NormalTick FAIL");
    }
    if (!g_fpHooked) {
        void* o = nullptr;
        if (bactro::memory::hook(bactro::memory::SignatureId::ItemInHandRendererRenderFirstPerson,
                                 reinterpret_cast<void*>(&renderFirstPersonDetour), &o) &&
            o) {
            g_renderFpOriginal = reinterpret_cast<RenderFirstPersonFn>(o);
            g_fpHooked = true;
            logLine("WireOutline: renderFirstPerson hooked");
        }
    }
    if (!g_getLocalPlayer) {
        auto a = bactro::memory::resolve(bactro::memory::SignatureId::ClientInstanceGetLocalPlayer);
        if (a) {
            g_getLocalPlayer = reinterpret_cast<GetLocalPlayerFn>(a);
            logLine("WireOutline: GetLocalPlayer @%p", reinterpret_cast<void*>(a));
        }
    }
    installGlHook();
    logLine("WireOutline: tick=%d fp=%d glHook=%d vp=%d getLP=%d", g_tickHooked ? 1 : 0, g_fpHooked ? 1 : 0,
            g_glHooked.load() ? 1 : 0, g_vpValid.load() ? 1 : 0, g_getLocalPlayer ? 1 : 0);
}

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    logLine("WireOutline %s", enabled ? "ON" : "OFF");
    if (enabled) resolveAndHook();
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "handItems")
            g_handItems.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "armor")
            g_armor.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "players")
            g_players.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "crystals")
            g_crystals.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "draw3d")
            g_draw3d.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "outlineSelf")
            g_outlineSelf.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "lineWidth")
            g_lineWidth.store(std::stof(std::string(value)), std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Wire Outline");
    b.description(
         "White 3D AABB wire. VP from glUniformMatrix4fv (same hook style as MotionBlur). "
         "Self-outline for debug (F5). Players/crystals next once nearby ABI is locked.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("handItems", "Hand / items / weapons", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("armor", "Armor", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("players", "Players", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("crystals", "Crystals", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("outlineSelf", "Outline self (F5 debug)", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("draw3d", "3D draw", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("lineWidth", "Line width", pl::modmenu::ConfigType::SliderFloat, "2.0", "1", "5", "");
    b.registerModule();
}

void onSignaturesReady() { logLine("WireOutline: ready"); }

void shutdown() {
    g_enabled.store(false, std::memory_order_release);
    g_localPlayer = nullptr;
    g_vpValid.store(false, std::memory_order_release);
}

void onPostFrame() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    if (!g_glHooked.load(std::memory_order_acquire))
        installGlHook();
    collectBoxes();
    std::vector<Box> copy;
    {
        std::lock_guard<std::mutex> lock(g_boxMu);
        copy = g_boxes;
    }
    projectAndDraw(copy);
}

} // namespace bactro::wireoutline
