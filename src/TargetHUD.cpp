#include "bactro/TargetHUD.hpp"
#include "bactro/Signatures.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <EGL/egl.h>
#include <android/log.h>
#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define TH_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SystemVisuals", __VA_ARGS__)

// ---- minimal GLES types ----
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLfloat = float;
using GLchar = char;

constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_VIEWPORT = 0x0BA2;
constexpr GLenum GL_FALSE = 0;
constexpr GLenum GL_TRUE = 1;

namespace bactro::targethud {
namespace {

constexpr const char* kModuleId = "bactro.targethud";

std::atomic_bool g_enabled{false};
std::atomic_bool g_playersOnly{true};
std::atomic_bool g_showAbsor{true};
std::atomic_bool g_showDamage{true};
std::atomic<float> g_scale{1.0f};
std::atomic<float> g_range{12.f};

using ClientInstanceUpdateFn = void (*)(void*, void*);
using GetLocalPlayerFn = void* (*)(void*);
using LevelGetHitResultFn = void* (*)(void*);
using HitResultGetEntityFn = void* (*)(void*);
using ActorIsPlayerFn = bool (*)(void*);
using ActorGetNameTagFn = void* (*)(void*);
// Attribute system (Bedrock)
using ActorGetHealthFn = int (*)(void*);           // Actor::getHealth() const
using ActorGetMaxHealthFn = int (*)(void*);        // Actor::getMaxHealth() const
using ActorGetAttributeFn = void* (*)(void*, void*); // AttributeInstance* getAttribute(Attribute const&)
using AttrInstGetFloatFn = float (*)(void*);       // AttributeInstance getters

ActorGetHealthFn g_getHealth = nullptr;
ActorGetMaxHealthFn g_getMaxHealth = nullptr;
ActorGetAttributeFn g_getAttribute = nullptr;
AttrInstGetFloatFn g_attrCurrent = nullptr;
AttrInstGetFloatFn g_attrMax = nullptr;

ClientInstanceUpdateFn g_ciUpdateOrig = nullptr;
GetLocalPlayerFn g_getLocalPlayer = nullptr;
LevelGetHitResultFn g_levelGetHit = nullptr;
LevelGetHitResultFn g_levelGetHitOrig = nullptr;
HitResultGetEntityFn g_hitGetEntity = nullptr;
HitResultGetEntityFn g_hitGetEntityOrig = nullptr;
ActorIsPlayerFn g_isPlayer = nullptr;
ActorGetNameTagFn g_getNameTag = nullptr;

void* g_clientInstance = nullptr;
void* g_level = nullptr;
// atomic — HitResultGetEntity can run on a game thread; avoid mutex there
std::atomic<void*> g_crosshairActor{nullptr};

using LevelInitFn = void (*)(void*, void*, void*, void*, void*, void*);
LevelInitFn g_levelInitOrig = nullptr;

std::mutex g_targetMu;

void logLine(const char* fmt, ...); // defined below

void* hitResultGetEntityDetour(void* hit) {
    void* ent = g_hitGetEntityOrig ? g_hitGetEntityOrig(hit) : nullptr;
    // Always store last non-null entity pointer (game just returned it — valid for this call)
    if (ent)
        g_crosshairActor.store(ent, std::memory_order_relaxed);
    return ent;
}

void levelInitDetour(void* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    g_level = self;
    if (g_levelInitOrig) g_levelInitOrig(self, a1, a2, a3, a4, a5);
}

struct TargetState {
    void* actor = nullptr;
    std::string name;
    float health = -1.f;
    float maxHealth = 20.f;
    float absorption = 0.f;
    float displayHealth = -1.f; // smoothed bar
    float recentDamage = 0.f;
    float flash = 0.f; // head/card hit flash 0..1
    double lastSeen = 0.0;
    bool valid = false;
};
TargetState g_target;

struct HealthSample {
    float health = -1.f;
    float maxHealth = 20.f;
    float absor = 0.f;
    double t = 0.0;
};
std::mutex g_cacheMu;
std::unordered_map<uintptr_t, HealthSample> g_healthCache;

void logLine(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    TH_LOGI("%s", buf);
}

double nowSec() {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

// ---- Real attribute HP (AttributeInstance / Actor::getHealth) ----
// AttributeInstance typical layout (varies by build; we try several):
//   +0x00 Attribute*
//   +0x08.. vector of modifiers
//   +0x20 / +0x24 / +0x28 / +0x2C  floats: default/min/max/current (order varies)
// BaseAttributeMap holds instances; Actor::getAttributes() -> map*

bool readHealthFromAttributeInstance(void* inst, float& hp, float& maxHp) {
    if (!inst) return false;
    auto* base = reinterpret_cast<unsigned char*>(inst);
    // Prefer function getters if resolved
    if (g_attrCurrent && g_attrMax) {
        try {
            float c = g_attrCurrent(inst);
            float m = g_attrMax(inst);
            if (std::isfinite(c) && std::isfinite(m) && m >= 1.f && m <= 1024.f && c >= 0.f && c <= m + 1.f) {
                hp = c; maxHp = m; return true;
            }
        } catch (...) {}
    }
    // Layout scan: find float pair (current, max) or (max, current)
    static const int kFloatOff[] = {0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C};
    for (int i = 0; i < (int)(sizeof(kFloatOff)/sizeof(kFloatOff[0])) - 1; ++i) {
        float a = *reinterpret_cast<float*>(base + kFloatOff[i]);
        float b = *reinterpret_cast<float*>(base + kFloatOff[i + 1]);
        if (!std::isfinite(a) || !std::isfinite(b)) continue;
        // current, max
        if (b >= 1.f && b <= 1024.f && a >= 0.f && a <= b + 0.5f && (b == 20.f || b == 40.f || (b >= 2.f && b <= 100.f))) {
            hp = a; maxHp = b; return true;
        }
        // max, current
        if (a >= 1.f && a <= 1024.f && b >= 0.f && b <= a + 0.5f && (a == 20.f || a == 40.f || (a >= 2.f && a <= 100.f))) {
            maxHp = a; hp = b; return true;
        }
    }
    return false;
}

// Walk Actor for BaseAttributeMap* then AttributeInstance records containing health.
// SAFE health read: only use resolved getHealth/getMaxHealth.
// AttributeMap memory walks were removed — they caused SIGSEGV on invalid pointers.
// When getHealth sig is missing, HUD still shows name; HP shows as unknown until sig is fixed.
bool readHealthFromAttributeMap(void* /*actor*/, float& /*hp*/, float& /*maxHp*/, float& /*absor*/) {
    return false; // disabled: unsafe without verified AttributeInstance layout
}

// AttributeInstance floats (1.26.51.1): current @ +0x18, max @ +0x1c
// The unique "getHealth" sigs are methods on an object that HAS AttributeInstance* at +0x38 —
// they are NOT Actor methods. Calling them with Actor* crashes. Do not call them on Actor.
// HP disabled for stability — memory scans on Actor caused SIGSEGV.
// Name card only until we have a verified Actor->AttributeInstance call path.
bool readHealth(void* /*actor*/, float& hp, float& maxHp, float& absor) {
    hp = -1.f;
    maxHp = 20.f;
    absor = 0.f;
    return false;
}

std::string readNameTag(void* actor) {
    if (!actor || !g_getNameTag) return "Player";
    void* ret = nullptr;
    try {
        ret = g_getNameTag(actor);
    } catch (...) {
        return "Player";
    }
    if (!ret) return "Player";
    char* data = nullptr;
    auto* asPtr = *reinterpret_cast<char**>(ret);
    if (asPtr && reinterpret_cast<uintptr_t>(asPtr) > 0x10000ULL)
        data = asPtr;
    else
        data = reinterpret_cast<char*>(ret);
    if (!data) return "Player";
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 48; ++i) {
        char c = data[i];
        if (c == 0) break;
        if (c >= 32 && c < 127) out.push_back(c);
        else break;
    }
    return out.empty() ? "Player" : out;
}



void updateTargetFromWorld() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;

    void* actor = g_crosshairActor.load(std::memory_order_relaxed);
    if (!actor) return;

    // Ultra-safe: do NOT call isPlayer / getLocalPlayer / getNameTag / getHealth.
    // Those have crashed in-world. Card shows a fixed label until we verify each call.
    TargetState ts{};
    ts.actor = actor;
    ts.name = "Target";
    ts.health = -1.f;
    ts.maxHealth = 20.f;
    ts.absorption = 0.f;
    ts.displayHealth = -1.f;
    ts.recentDamage = 0.f;
    ts.flash = 0.f;
    ts.lastSeen = nowSec();
    ts.valid = true;

    {
        std::lock_guard lock(g_targetMu);
        g_target = std::move(ts);
    }
}

void clientInstanceUpdateDetour(void* self, void* a1) {
    g_clientInstance = self;
    if (g_ciUpdateOrig) g_ciUpdateOrig(self, a1);
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    static double s_last = 0.0;
    double t = nowSec();
    if (t - s_last < 0.2) return; // 5 Hz
    s_last = t;
    updateTargetFromWorld();
}

// ---- GLES HUD (ProtoHax-style card: head | name + HP nums + bar) ----
std::mutex g_glMu;
bool g_glReady = false;
GLuint g_prog = 0, g_vbo = 0, g_ibo = 0;
GLint g_aPos = -1, g_aCol = -1;

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
GLDECL(void, glGenBuffers, GLsizei, GLuint*);
GLDECL(void, glBindBuffer, GLenum, GLuint);
GLDECL(void, glBufferData, GLenum, GLsizei, const void*, GLenum);
GLDECL(void, glEnableVertexAttribArray, GLuint);
GLDECL(void, glDisableVertexAttribArray, GLuint);
GLDECL(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
GLDECL(void, glDrawElements, GLenum, GLsizei, GLenum, const void*);
GLDECL(void, glEnable, GLenum);
GLDECL(void, glDisable, GLenum);
GLDECL(void, glBlendFunc, GLenum, GLenum);
GLDECL(void, glGetIntegerv, GLenum, GLint*);
GLDECL(void, glDeleteShader, GLuint);
GLDECL(void, glDepthMask, GLboolean);

struct Vtx {
    float x, y, r, g, b, a;
};

void pushQuad(std::vector<Vtx>& v, std::vector<unsigned short>& idx, float x0, float y0, float x1, float y1,
              float r, float g, float b, float a) {
    unsigned short base = static_cast<unsigned short>(v.size());
    v.push_back({x0, y0, r, g, b, a});
    v.push_back({x1, y0, r, g, b, a});
    v.push_back({x1, y1, r, g, b, a});
    v.push_back({x0, y1, r, g, b, a});
    idx.push_back(base);
    idx.push_back(static_cast<unsigned short>(base + 1));
    idx.push_back(static_cast<unsigned short>(base + 2));
    idx.push_back(base);
    idx.push_back(static_cast<unsigned short>(base + 2));
    idx.push_back(static_cast<unsigned short>(base + 3));
}

// Tiny 3x5 digit atlas in NDC units (procedural bars for numbers)
void pushDigit(std::vector<Vtx>& v, std::vector<unsigned short>& idx, float x, float y, float s, int d,
               float r, float g, float b, float a) {
    // 7-segment style
    auto seg = [&](float x0, float y0, float x1, float y1) {
        pushQuad(v, idx, x + x0 * s, y + y0 * s, x + x1 * s, y + y1 * s, r, g, b, a);
    };
    const float t = 0.12f;
    bool A = false, B = false, C = false, D = false, E = false, F = false, G = false;
    switch (d) {
    case 0: A = B = C = D = E = F = true; break;
    case 1: B = C = true; break;
    case 2: A = B = G = E = D = true; break;
    case 3: A = B = G = C = D = true; break;
    case 4: F = G = B = C = true; break;
    case 5: A = F = G = C = D = true; break;
    case 6: A = F = G = E = C = D = true; break;
    case 7: A = B = C = true; break;
    case 8: A = B = C = D = E = F = G = true; break;
    case 9: A = B = C = D = F = G = true; break;
    default: break;
    }
    if (A) seg(0.15f, 0.85f, 0.85f, 0.85f + t);           // top
    if (G) seg(0.15f, 0.45f, 0.85f, 0.45f + t);           // mid
    if (D) seg(0.15f, 0.05f, 0.85f, 0.05f + t);           // bot
    if (F) seg(0.05f, 0.50f, 0.05f + t, 0.90f);           // UL
    if (B) seg(0.85f, 0.50f, 0.85f + t, 0.90f);           // UR
    if (E) seg(0.05f, 0.10f, 0.05f + t, 0.50f);           // LL
    if (C) seg(0.85f, 0.10f, 0.85f + t, 0.50f);           // LR
}

void pushNumber(std::vector<Vtx>& v, std::vector<unsigned short>& idx, float x, float y, float s, int n,
                float r, float g, float b, float a) {
    if (n < 0) n = 0;
    if (n > 999) n = 999;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", n);
    float cx = x;
    for (char* p = buf; *p; ++p) {
        pushDigit(v, idx, cx, y, s, *p - '0', r, g, b, a);
        cx += s * 1.15f;
    }
}

bool loadGl() {
    if (g_glReady) return g_prog != 0;
    g_glReady = true;
#define L(n) p_##n = reinterpret_cast<PFN_##n>(glProc(#n))
    L(glCreateShader); L(glShaderSource); L(glCompileShader); L(glGetShaderiv);
    L(glCreateProgram); L(glAttachShader); L(glLinkProgram); L(glGetProgramiv);
    L(glUseProgram); L(glGetAttribLocation);
    L(glGenBuffers); L(glBindBuffer); L(glBufferData);
    L(glEnableVertexAttribArray); L(glDisableVertexAttribArray); L(glVertexAttribPointer);
    L(glDrawElements); L(glEnable); L(glDisable); L(glBlendFunc);
    L(glGetIntegerv); L(glDeleteShader); L(glDepthMask);
#undef L
    if (!p_glCreateShader) {
        logLine("TargetHUD: GLES load FAIL");
        return false;
    }
    const char* vs =
        "attribute vec2 aPos; attribute vec4 aCol; varying vec4 vCol;\n"
        "void main(){ vCol=aCol; gl_Position=vec4(aPos,0.0,1.0); }";
    const char* fs =
        "precision mediump float; varying vec4 vCol;\n"
        "void main(){ gl_FragColor=vCol; }";
    auto compile = [](GLenum type, const char* src) -> GLuint {
        GLuint sh = p_glCreateShader(type);
        p_glShaderSource(sh, 1, &src, nullptr);
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
    g_aCol = p_glGetAttribLocation(g_prog, "aCol");
    p_glGenBuffers(1, &g_vbo);
    p_glGenBuffers(1, &g_ibo);
    logLine("TargetHUD: GLES OK");
    return true;
}

void drawHud() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;

    TargetState ts;
    {
        std::lock_guard lock(g_targetMu);
        ts = g_target;
        if (ts.valid) {
            // smooth bar toward real HP
            if (ts.displayHealth < 0.f) ts.displayHealth = ts.health;
            float target = ts.health >= 0.f ? ts.health : ts.displayHealth;
            ts.displayHealth += (target - ts.displayHealth) * 0.18f;
            g_target.displayHealth = ts.displayHealth;
            g_target.flash = std::max(0.f, g_target.flash - 0.025f);
            g_target.recentDamage *= 0.96f;
            ts.flash = g_target.flash;
            ts.recentDamage = g_target.recentDamage;
        }
    }
    if (!ts.valid) return;
    // hold 1.5s after last look
    if (nowSec() - ts.lastSeen > 1.5) return;

    std::lock_guard lock(g_glMu);
    if (!loadGl() || !g_prog) return;

    GLint vp[4] = {0, 0, 1080, 2400};
    if (p_glGetIntegerv) p_glGetIntegerv(GL_VIEWPORT, vp);
    float aspect = (vp[3] > 0) ? (float)vp[2] / (float)vp[3] : 0.5f;
    float sc = g_scale.load(std::memory_order_relaxed);

    // Card layout in NDC (top-center-ish, ProtoHax style)
    // Width ~0.55 of screen height units, height ~0.14
    float cardW = 0.72f * sc;
    float cardH = 0.16f * sc;
    float cx = 0.0f;
    float cy = 0.55f; // upper third
    // aspect-correct X
    float x0 = cx - cardW * 0.5f * aspect;
    float x1 = cx + cardW * 0.5f * aspect;
    float y0 = cy - cardH * 0.5f;
    float y1 = cy + cardH * 0.5f;

    float flash = ts.flash;
    float bgR = 0.06f + flash * 0.25f;
    float bgG = 0.06f;
    float bgB = 0.08f;
    float bgA = 0.82f;

    std::vector<Vtx> verts;
    std::vector<unsigned short> inds;
    verts.reserve(256);
    inds.reserve(384);

    // Panel background
    pushQuad(verts, inds, x0, y0, x1, y1, bgR, bgG, bgB, bgA);
    // Thin border
    float bd = 0.004f;
    pushQuad(verts, inds, x0, y0, x1, y0 + bd, 1.f, 1.f, 1.f, 0.35f);
    pushQuad(verts, inds, x0, y1 - bd, x1, y1, 1.f, 1.f, 1.f, 0.35f);
    pushQuad(verts, inds, x0, y0, x0 + bd * aspect, y1, 1.f, 1.f, 1.f, 0.35f);
    pushQuad(verts, inds, x1 - bd * aspect, y0, x1, y1, 1.f, 1.f, 1.f, 0.35f);

    // Head slot (left) — placeholder face (no skin atlas without more RE)
    float headPad = 0.02f * sc;
    float headSize = (cardH - headPad * 2.f);
    float hx0 = x0 + headPad * aspect;
    float hx1 = hx0 + headSize * aspect;
    float hy0 = y0 + headPad;
    float hy1 = hy0 + headSize;
    // face base (skin tone)
    float fr = 0.82f + flash * 0.18f, fg = 0.65f, fb = 0.48f;
    pushQuad(verts, inds, hx0, hy0, hx1, hy1, fr, fg, fb, 1.f);
    // simple eyes / mouth so it reads as a "head"
    float ew = (hx1 - hx0) * 0.18f, eh = (hy1 - hy0) * 0.12f;
    pushQuad(verts, inds, hx0 + (hx1 - hx0) * 0.22f, hy0 + (hy1 - hy0) * 0.55f,
             hx0 + (hx1 - hx0) * 0.22f + ew, hy0 + (hy1 - hy0) * 0.55f + eh, 0.1f, 0.1f, 0.1f, 1.f);
    pushQuad(verts, inds, hx0 + (hx1 - hx0) * 0.60f, hy0 + (hy1 - hy0) * 0.55f,
             hx0 + (hx1 - hx0) * 0.60f + ew, hy0 + (hy1 - hy0) * 0.55f + eh, 0.1f, 0.1f, 0.1f, 1.f);
    pushQuad(verts, inds, hx0 + (hx1 - hx0) * 0.30f, hy0 + (hy1 - hy0) * 0.28f,
             hx0 + (hx1 - hx0) * 0.70f, hy0 + (hy1 - hy0) * 0.28f + eh * 0.7f, 0.45f, 0.2f, 0.2f, 1.f);
    // hit flash overlay on head
    if (flash > 0.05f)
        pushQuad(verts, inds, hx0, hy0, hx1, hy1, 1.f, 0.2f, 0.2f, flash * 0.45f);

    // Name bar (text approximated as small quads from first chars — full glyphs need font atlas;
    // we show HP numbers precisely and a name underline marker)
    float textX = hx1 + 0.025f * aspect * sc;
    float nameY = y1 - 0.045f * sc;
    // name placeholder bar length by name length
    float nameLen = std::min(12.f, (float)ts.name.size()) / 12.f;
    pushQuad(verts, inds, textX, nameY, textX + (x1 - textX - 0.02f * aspect) * nameLen, nameY + 0.012f * sc,
             0.95f, 0.95f, 0.98f, 0.9f);

    // HP numbers: current / max  — ProtoHax style top-right of bar
    float hpShow = ts.displayHealth >= 0.f ? ts.displayHealth : ts.health;
    if (hpShow < 0.f) hpShow = 0.f;
    int curI = (int)std::lround(hpShow);
    int maxI = (int)std::lround(ts.maxHealth > 0.f ? ts.maxHealth : 20.f);
    float numS = 0.028f * sc;
    float numY = nameY - 0.055f * sc;
    float numX = textX;
    // current HP white
    pushNumber(verts, inds, numX, numY, numS, curI, 1.f, 1.f, 1.f, 1.f);
    // count digits for spacing
    int digits = 1;
    for (int t = curI; t >= 10; t /= 10) ++digits;
    float afterCur = numX + digits * numS * 1.15f + numS * 0.2f;
    // slash
    pushQuad(verts, inds, afterCur, numY + numS * 0.15f, afterCur + numS * 0.35f, numY + numS * 0.25f, 0.8f, 0.8f,
             0.85f, 1.f);
    float afterSlash = afterCur + numS * 0.5f;
    // max HP — GOLD when absorption present (gapple), else muted white
    bool goldMax = g_showAbsor.load() && ts.absorption > 0.05f;
    if (goldMax)
        pushNumber(verts, inds, afterSlash, numY, numS, maxI, 1.f, 0.85f, 0.2f, 1.f);
    else
        pushNumber(verts, inds, afterSlash, numY, numS, maxI, 0.85f, 0.85f, 0.9f, 1.f);
    // +absorption extra
    if (goldMax) {
        int absI = (int)std::lround(ts.absorption);
        int d2 = 1;
        for (int t = maxI; t >= 10; t /= 10) ++d2;
        float ax = afterSlash + d2 * numS * 1.15f + numS * 0.15f;
        // small +N in gold
        pushQuad(verts, inds, ax, numY + numS * 0.35f, ax + numS * 0.35f, numY + numS * 0.45f, 1.f, 0.85f, 0.2f, 1.f);
        pushNumber(verts, inds, ax + numS * 0.4f, numY, numS * 0.85f, absI, 1.f, 0.85f, 0.2f, 1.f);
    }

    // HP bar track
    float barX0 = textX;
    float barX1 = x1 - 0.02f * aspect;
    float barY0 = y0 + 0.028f * sc;
    float barY1 = barY0 + 0.028f * sc;
    pushQuad(verts, inds, barX0, barY0, barX1, barY1, 0.12f, 0.12f, 0.14f, 0.95f);
    float ratio = 0.f;
    if (ts.maxHealth > 0.1f && hpShow >= 0.f) ratio = std::min(1.f, std::max(0.f, hpShow / ts.maxHealth));
    // green -> yellow -> red
    float hr = 0.2f, hg = 0.85f, hb = 0.3f;
    if (ratio < 0.5f) {
        float t = ratio / 0.5f;
        hr = 0.9f * (1.f - t) + 0.9f * t;
        hg = 0.2f * (1.f - t) + 0.85f * t;
        hb = 0.15f;
    }
    if (flash > 0.1f) {
        hr = std::min(1.f, hr + flash * 0.4f);
        hg *= (1.f - flash * 0.3f);
    }
    float fillX1 = barX0 + (barX1 - barX0) * ratio;
    if (ratio > 0.01f) pushQuad(verts, inds, barX0, barY0, fillX1, barY1, hr, hg, hb, 1.f);

    // absorption overlay bar (gold) on top of HP bar
    if (g_showAbsor.load() && ts.absorption > 0.05f && ts.maxHealth > 0.1f) {
        float ar = std::min(1.f, ts.absorption / ts.maxHealth);
        float ax1 = barX0 + (barX1 - barX0) * ar;
        pushQuad(verts, inds, barX0, barY1 - 0.008f * sc, ax1, barY1, 1.f, 0.85f, 0.15f, 0.85f);
    }

    // damage tick mark on bar
    if (ts.recentDamage > 0.15f && ts.maxHealth > 0.1f) {
        float dr = std::min(1.f, ts.recentDamage / ts.maxHealth);
        float mid = fillX1;
        float left = std::max(barX0, mid - (barX1 - barX0) * dr);
        pushQuad(verts, inds, left, barY0, mid, barY1, 1.f, 0.35f, 0.2f, 0.7f * std::min(1.f, ts.recentDamage));
    }

    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_CULL_FACE);
    if (p_glDepthMask) p_glDepthMask(GL_FALSE);
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    p_glUseProgram(g_prog);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(verts.size() * sizeof(Vtx)), verts.data(), GL_DYNAMIC_DRAW);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizei>(inds.size() * sizeof(unsigned short)), inds.data(),
                   GL_DYNAMIC_DRAW);
    p_glEnableVertexAttribArray(static_cast<GLuint>(g_aPos));
    p_glVertexAttribPointer(static_cast<GLuint>(g_aPos), 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), nullptr);
    p_glEnableVertexAttribArray(static_cast<GLuint>(g_aCol));
    p_glVertexAttribPointer(static_cast<GLuint>(g_aCol), 4, GL_FLOAT, GL_FALSE, sizeof(Vtx),
                            reinterpret_cast<void*>(sizeof(float) * 2));
    p_glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(inds.size()), GL_UNSIGNED_SHORT, nullptr);
    p_glDisableVertexAttribArray(static_cast<GLuint>(g_aPos));
    p_glDisableVertexAttribArray(static_cast<GLuint>(g_aCol));
    p_glUseProgram(0);
    if (p_glDepthMask) p_glDepthMask(GL_TRUE);

    static int s_log = 0;
    if (s_log < 6) {
        logLine("TargetHUD: draw %s hp=%.1f/%.1f abs=%.1f dmg=%.1f", ts.name.c_str(), ts.health, ts.maxHealth,
                ts.absorption, ts.recentDamage);
        ++s_log;
    }
}

void onToggle(std::string_view, bool on) {
    g_enabled.store(on, std::memory_order_release);
    logLine(on ? "TargetHUD ON" : "TargetHUD OFF");
    if (!on) {
        std::lock_guard lock(g_targetMu);
        g_target = {};
    }
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "playersOnly")
            g_playersOnly.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "showAbsor")
            g_showAbsor.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "showDamage")
            g_showDamage.store(value == "true" || value == "1", std::memory_order_relaxed);
        else if (key == "scale")
            g_scale.store(std::stof(std::string(value)), std::memory_order_relaxed);
        else if (key == "range")
            g_range.store(std::stof(std::string(value)), std::memory_order_relaxed);
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Target HUD");
    b.description(
         "ProtoHax-style card: head slot + name + 20/20 HP (gold max when absorption) + "
         "smooth bar + damage flash. Crosshair HitResult target.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("playersOnly", "Players only", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("showAbsor", "Show absorption (gold)", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("showDamage", "Register damage", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("scale", "HUD scale", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.5", "2.0", "");
    b.config("range", "Max range (hint)", pl::modmenu::ConfigType::SliderFloat, "12", "3", "64", "");
    b.registerModule();
}

void onSignaturesReady() {
    using bactro::memory::SignatureId;
    using bactro::memory::resolve;
    using bactro::memory::hook;

    if (auto a = resolve(SignatureId::ClientInstanceGetLocalPlayer)) {
        g_getLocalPlayer = reinterpret_cast<GetLocalPlayerFn>(a);
        logLine("TargetHUD: GetLocalPlayer @%p", reinterpret_cast<void*>(a));
    }
    if (auto a = resolve(SignatureId::LevelGetHitResult)) {
        g_levelGetHit = reinterpret_cast<LevelGetHitResultFn>(a);
        logLine("TargetHUD: LevelGetHitResult @%p (not hooked — too short)", reinterpret_cast<void*>(a));
    }
    if (auto a = resolve(SignatureId::HitResultGetEntity)) {
        g_hitGetEntity = reinterpret_cast<HitResultGetEntityFn>(a);
        logLine("TargetHUD: HitResultGetEntity @%p", reinterpret_cast<void*>(a));
        void* o = nullptr;
        if (hook(SignatureId::HitResultGetEntity, reinterpret_cast<void*>(&hitResultGetEntityDetour), &o) && o) {
            g_hitGetEntityOrig = reinterpret_cast<HitResultGetEntityFn>(o);
            logLine("TargetHUD: HitResultGetEntity HOOKED (crosshair entity capture)");
        } else {
            logLine("TargetHUD: HitResultGetEntity hook FAIL");
        }
    }
    if (auto a = resolve(SignatureId::ActorIsPlayer)) {
        g_isPlayer = reinterpret_cast<ActorIsPlayerFn>(a);
        logLine("TargetHUD: ActorIsPlayer @%p", reinterpret_cast<void*>(a));
    }
    if (auto a = resolve(SignatureId::ActorGetNameTag)) {
        g_getNameTag = reinterpret_cast<ActorGetNameTagFn>(a);
        logLine("TargetHUD: ActorGetNameTag @%p", reinterpret_cast<void*>(a));
    }
    // Attribute / health
    if (auto a = resolve(SignatureId::ActorGetHealth)) {
        g_getHealth = reinterpret_cast<ActorGetHealthFn>(a);
        logLine("TargetHUD: ActorGetHealth @%p (layout ref, not called on Actor)", reinterpret_cast<void*>(a));
    } else {
        logLine("TargetHUD: ActorGetHealth MISSING (HP bar needs this sig)");
    }
    if (auto a = resolve(SignatureId::ActorGetMaxHealth)) {
        g_getMaxHealth = reinterpret_cast<ActorGetMaxHealthFn>(a);
        logLine("TargetHUD: ActorGetMaxHealth @%p", reinterpret_cast<void*>(a));
    }
    if (auto a = resolve(SignatureId::ActorGetAttribute)) {
        g_getAttribute = reinterpret_cast<ActorGetAttributeFn>(a);
        logLine("TargetHUD: ActorGetAttribute @%p", reinterpret_cast<void*>(a));
    }
    if (auto a = resolve(SignatureId::AttributeInstanceGetCurrentValue)) {
        g_attrCurrent = reinterpret_cast<AttrInstGetFloatFn>(a);
        logLine("TargetHUD: AttrCurrent @%p", reinterpret_cast<void*>(a));
    }
    if (auto a = resolve(SignatureId::AttributeInstanceGetMaxValue)) {
        g_attrMax = reinterpret_cast<AttrInstGetFloatFn>(a);
        logLine("TargetHUD: AttrMax @%p", reinterpret_cast<void*>(a));
    }

    void* o = nullptr;
    if (auto a = resolve(SignatureId::LevelInit)) {
        logLine("TargetHUD: LevelInit @%p", reinterpret_cast<void*>(a));
        if (hook(SignatureId::LevelInit, reinterpret_cast<void*>(&levelInitDetour), &o) && o) {
            g_levelInitOrig = reinterpret_cast<LevelInitFn>(o);
            logLine("TargetHUD: LevelInit hooked");
        } else {
            logLine("TargetHUD: LevelInit hook FAIL (OK — using LevelGetHitResult capture)");
        }
    } else {
        logLine("TargetHUD: LevelInit sig MISSING (unused)");
    }
    o = nullptr;
    if (hook(SignatureId::ClientInstanceUpdate, reinterpret_cast<void*>(&clientInstanceUpdateDetour), &o) && o) {
        g_ciUpdateOrig = reinterpret_cast<ClientInstanceUpdateFn>(o);
        logLine("TargetHUD: ClientInstanceUpdate hooked");
    } else {
        logLine("TargetHUD: ClientInstanceUpdate HOOK FAIL");
    }
    logLine("TargetHUD: ready");
}

void onPostFrame() {
    try {
        drawHud();
    } catch (...) {
    }
}

void shutdown() {
    g_enabled.store(false, std::memory_order_release);
    std::lock_guard lock(g_targetMu);
    g_target = {};
}

} // namespace bactro::targethud
