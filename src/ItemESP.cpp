#include "bactro/ItemESP.hpp"
#include "bactro/RenderPhase.hpp"
#include "bactro/Signatures.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <EGL/egl.h>
#include <android/log.h>
#include <dlfcn.h>

#include <algorithm>
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

#define IE_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SystemVisuals", __VA_ARGS__)

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
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
constexpr GLenum GL_DEPTH_WRITEMASK = 0x0B72;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_VIEWPORT = 0x0BA2;
constexpr GLenum GL_FALSE = 0;
constexpr GLenum GL_TRUE = 1;
constexpr GLenum GL_LESS = 0x0201;
constexpr GLenum GL_LEQUAL = 0x0203;

namespace bactro::itemesp {
namespace {

constexpr const char* kModuleId = "systemvisuals.itemesp";

std::atomic_bool g_enabled{true};
std::atomic<float> g_maxDistance{32.f};
std::atomic_bool g_filterPlayers{true};
std::atomic_bool g_worldLabels{true};
std::atomic_bool g_showList{false}; // list optional now; world labels primary
std::atomic_bool g_depthTest{true};

struct Color {
    float r, g, b, a;
};

using ClientInstanceUpdateFn = void (*)(void*, void*);
using ActorIsPlayerFn = bool (*)(void*);
using ActorGetNameTagFn = void* (*)(void*);
using SetupActorGlintFn = void (*)(void*, void*, void*, const Color*, const Color*, const Color*, const Color*,
                                   float, float, float, float, const void*);
using SetEntityConstantsFn = void (*)(void*, void*, void*, const void*); // soft — may not match; only hook glint

ClientInstanceUpdateFn g_ciUpdateOrig = nullptr;
ActorIsPlayerFn g_isPlayer = nullptr;
ActorGetNameTagFn g_getNameTagOrig = nullptr;
SetupActorGlintFn g_setupActorGlintOrig = nullptr;

// ---- matrix capture (same idea as EntityOutline) ----
using PFN_glUniformMatrix4fv = void (*)(int, int, unsigned char, const float*);
PFN_glUniformMatrix4fv g_glUniformOrig = nullptr;
bool g_matrixHooked = false;

float g_viewProj[16]{};
std::atomic_bool g_vpValid{false};
std::mutex g_vpMu;

std::atomic<void*> g_pendingActor{nullptr};
std::atomic_bool g_meshArmed{false};

struct WorldLabel {
    void* actor = nullptr;
    std::string name;
    float x = 0, y = 0, z = 0;
    float ndcX = 0, ndcY = 0, ndcZ = 0;
    double lastSeen = 0;
    bool projected = false;
};

std::mutex g_mu;
std::unordered_map<void*, WorldLabel> g_labels;

double nowSec() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

void logLine(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    IE_LOGI("%s", buf);
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
    if (xz < 0.15f || xz > 128.f) return false;
    if (ty < -64.f || ty > 400.f) return false;
    return true;
}

// Returns false if behind camera or outside soft FOV (clip w / NDC)
bool worldToNdc(const float* vp, float x, float y, float z, float& ox, float& oy, float& oz) {
    float clip[4];
    clip[0] = vp[0] * x + vp[4] * y + vp[8] * z + vp[12];
    clip[1] = vp[1] * x + vp[5] * y + vp[9] * z + vp[13];
    clip[2] = vp[2] * x + vp[6] * y + vp[10] * z + vp[14];
    clip[3] = vp[3] * x + vp[7] * y + vp[11] * z + vp[15];
    if (clip[3] <= 0.05f) return false; // behind / too close
    ox = clip[0] / clip[3];
    oy = clip[1] / clip[3];
    oz = clip[2] / clip[3];
    // FOV: must be roughly on screen
    if (std::fabs(ox) > 1.15f || std::fabs(oy) > 1.15f) return false;
    if (oz < -1.f || oz > 1.f) return false;
    return true;
}

std::string readNameFromReturn(void* ret) {
    if (!ret) return {};
    char* data = nullptr;
    auto* asPtr = *reinterpret_cast<char**>(ret);
    if (asPtr && reinterpret_cast<uintptr_t>(asPtr) > 0x10000ULL)
        data = asPtr;
    else
        data = reinterpret_cast<char*>(ret);
    if (!data) return {};
    std::string out;
    out.reserve(40);
    for (int i = 0; i < 64; ++i) {
        char c = data[i];
        if (c == 0) break;
        if (c >= 32 && c < 127) out.push_back(c);
        else break;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    return out;
}

bool looksLikeItemName(const std::string& n) {
    if (n.empty() || n.size() > 48) return false;
    if (n == "Player" || n == "Target") return false;
    return true;
}

bool isPlayerActor(void* actor) {
    if (!actor || !g_isPlayer) return false;
    try {
        return g_isPlayer(actor);
    } catch (...) {
        return false;
    }
}

void rememberName(void* actor, const std::string& name) {
    if (!actor || !looksLikeItemName(name)) return;
    if (g_filterPlayers.load(std::memory_order_relaxed) && isPlayerActor(actor)) return;
    std::lock_guard lock(g_mu);
    auto& lab = g_labels[actor];
    lab.actor = actor;
    lab.name = name;
    lab.lastSeen = nowSec();
}

void rememberPosition(void* actor, float x, float y, float z) {
    if (!actor) return;
    if (g_filterPlayers.load(std::memory_order_relaxed) && isPlayerActor(actor)) return;
    float dist = std::sqrt(x * x + z * z); // camera-relative model space often origin-relative
    const float maxD = g_maxDistance.load(std::memory_order_relaxed);
    if (dist > maxD) return;

    std::lock_guard lock(g_mu);
    auto it = g_labels.find(actor);
    if (it == g_labels.end()) {
        // position without name yet — keep placeholder
        WorldLabel lab;
        lab.actor = actor;
        lab.name = "Item";
        lab.x = x;
        lab.y = y;
        lab.z = z;
        lab.lastSeen = nowSec();
        g_labels[actor] = std::move(lab);
        return;
    }
    it->second.x = x;
    it->second.y = y;
    it->second.z = z;
    it->second.lastSeen = nowSec();
}

void* getNameTagDetour(void* actor) {
    void* ret = g_getNameTagOrig ? g_getNameTagOrig(actor) : nullptr;
    if (!g_enabled.load(std::memory_order_relaxed) || !actor) return ret;
    try {
        std::string name = readNameFromReturn(ret);
        if (!name.empty()) rememberName(actor, name);
    } catch (...) {
    }
    return ret;
}

void setupActorGlintDetour(void* sc, void* ec, void* actor, const Color* o, const Color* c1, const Color* c2,
                           const Color* g, float u1, float u2, float r1, float r2, const void* le) {
    if (g_enabled.load(std::memory_order_relaxed) && actor) {
        bool skip = g_filterPlayers.load(std::memory_order_relaxed) && isPlayerActor(actor);
        if (!skip) {
            g_pendingActor.store(actor, std::memory_order_release);
            g_meshArmed.store(true, std::memory_order_release);
            bactro::phase::entityMeshArmed.store(true, std::memory_order_release);
            // try name immediately if possible
            if (g_getNameTagOrig) {
                try {
                    void* ret = g_getNameTagOrig(actor);
                    std::string name = readNameFromReturn(ret);
                    if (!name.empty()) rememberName(actor, name);
                } catch (...) {
                }
            }
        }
    }
    if (g_setupActorGlintOrig) g_setupActorGlintOrig(sc, ec, actor, o, c1, c2, g, u1, u2, r1, r2, le);
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
            std::lock_guard lock(g_vpMu);
            std::memcpy(g_viewProj, m, sizeof(m));
            g_vpValid.store(true, std::memory_order_release);
        } else if (g_meshArmed.load(std::memory_order_acquire) ||
                   bactro::phase::entityMeshArmed.load(std::memory_order_acquire)) {
            float tx, ty, tz;
            if (looksLikeWorldModel(m, tx, ty, tz)) {
                void* actor = g_pendingActor.load(std::memory_order_acquire);
                if (actor) rememberPosition(actor, tx, ty, tz);
                g_meshArmed.store(false, std::memory_order_release);
            }
        }
    }
    if (g_glUniformOrig) g_glUniformOrig(location, count, transpose, value);
}

void clientInstanceUpdateDetour(void* self, void* a1) {
    if (g_ciUpdateOrig) g_ciUpdateOrig(self, a1);
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    const double t = nowSec();
    std::lock_guard lock(g_mu);
    for (auto it = g_labels.begin(); it != g_labels.end();) {
        if (t - it->second.lastSeen > 1.25) it = g_labels.erase(it);
        else ++it;
    }
}

void* glProc(const char* name) {
    void* p = eglGetProcAddress(name);
    if (p) return p;
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    if (egl) p = dlsym(egl, name);
    return p;
}

bool installMatrixHook() {
    if (g_matrixHooked) return true;
    void* p = glProc("glUniformMatrix4fv");
    if (!p) {
        logLine("ItemESP: glUniformMatrix4fv missing");
        return false;
    }
    void* o = nullptr;
    if (pl::memory::hook(p, reinterpret_cast<void*>(&glUniformMatrix4fvDetour), &o) != 0 || !o) {
        logLine("ItemESP: matrix HOOK FAIL");
        return false;
    }
    g_glUniformOrig = reinterpret_cast<PFN_glUniformMatrix4fv>(o);
    g_matrixHooked = true;
    logLine("ItemESP: matrix HOOKED (VP + model)");
    return true;
}

// ---- GLES text at NDC with depth ----
GLuint (*glCreateShader)(GLenum) = nullptr;
void (*glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
void (*glCompileShader)(GLuint) = nullptr;
void (*glGetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
GLuint (*glCreateProgram)() = nullptr;
void (*glAttachShader)(GLuint, GLuint) = nullptr;
void (*glLinkProgram)(GLuint) = nullptr;
void (*glGetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
void (*glUseProgram)(GLuint) = nullptr;
void (*glGenBuffers)(GLsizei, GLuint*) = nullptr;
void (*glBindBuffer)(GLenum, GLuint) = nullptr;
void (*glBufferData)(GLenum, GLsizei, const void*, GLenum) = nullptr;
void (*glEnableVertexAttribArray)(GLuint) = nullptr;
void (*glVertexAttribPointer)(GLuint, GLint, GLenum, unsigned char, GLsizei, const void*) = nullptr;
void (*glDrawElements)(GLenum, GLsizei, GLenum, const void*) = nullptr;
void (*glEnable)(GLenum) = nullptr;
void (*glDisable)(GLenum) = nullptr;
void (*glBlendFunc)(GLenum, GLenum) = nullptr;
void (*glDepthFunc)(GLenum) = nullptr;
void (*glDepthMask)(unsigned char) = nullptr;
void (*glGetIntegerv)(GLenum, GLint*) = nullptr;
void (*glDeleteShader)(GLuint) = nullptr;
GLint (*glGetAttribLocation)(GLuint, const GLchar*) = nullptr;

bool g_glReady = false;
GLuint g_prog = 0, g_vbo = 0, g_ibo = 0;
GLint g_aPos = 0, g_aCol = 0;
std::mutex g_glMu;

#define GL_LOAD(name) name = reinterpret_cast<decltype(name)>(glProc(#name))

bool loadGles() {
    GL_LOAD(glCreateShader);
    GL_LOAD(glShaderSource);
    GL_LOAD(glCompileShader);
    GL_LOAD(glGetShaderiv);
    GL_LOAD(glCreateProgram);
    GL_LOAD(glAttachShader);
    GL_LOAD(glLinkProgram);
    GL_LOAD(glGetProgramiv);
    GL_LOAD(glUseProgram);
    GL_LOAD(glGenBuffers);
    GL_LOAD(glBindBuffer);
    GL_LOAD(glBufferData);
    GL_LOAD(glEnableVertexAttribArray);
    GL_LOAD(glVertexAttribPointer);
    GL_LOAD(glDrawElements);
    GL_LOAD(glEnable);
    GL_LOAD(glDisable);
    GL_LOAD(glBlendFunc);
    GL_LOAD(glDepthFunc);
    GL_LOAD(glDepthMask);
    GL_LOAD(glGetIntegerv);
    GL_LOAD(glDeleteShader);
    GL_LOAD(glGetAttribLocation);
    return glCreateShader && glUseProgram && glDrawElements;
}

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    return ok ? s : 0;
}

bool ensureGl() {
    if (g_glReady) return true;
    if (!loadGles()) {
        logLine("ItemESP: GLES FAIL");
        return false;
    }
    // aPos is vec3 (x,y,z ndc) for depth
    const char* vs =
        "attribute vec3 aPos; attribute vec4 aCol; varying vec4 vCol;\n"
        "void main(){ vCol=aCol; gl_Position=vec4(aPos,1.0); }";
    const char* fs =
        "precision mediump float; varying vec4 vCol;\n"
        "void main(){ gl_FragColor=vCol; }";
    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return false;
    g_prog = glCreateProgram();
    glAttachShader(g_prog, v);
    glAttachShader(g_prog, f);
    glLinkProgram(g_prog);
    GLint ok = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(v);
    glDeleteShader(f);
    if (!ok) return false;
    g_aPos = glGetAttribLocation(g_prog, "aPos");
    g_aCol = glGetAttribLocation(g_prog, "aCol");
    glGenBuffers(1, &g_vbo);
    glGenBuffers(1, &g_ibo);
    g_glReady = true;
    logLine("ItemESP: GLES OK (depth labels)");
    return true;
}

struct Vtx {
    float x, y, z, r, g, b, a;
};

void pushQuad(std::vector<Vtx>& v, std::vector<unsigned short>& idx, float x0, float y0, float x1, float y1,
              float z, float r, float g, float b, float a) {
    auto base = static_cast<unsigned short>(v.size());
    v.push_back({x0, y0, z, r, g, b, a});
    v.push_back({x1, y0, z, r, g, b, a});
    v.push_back({x1, y1, z, r, g, b, a});
    v.push_back({x0, y1, z, r, g, b, a});
    idx.push_back(base);
    idx.push_back(base + 1);
    idx.push_back(base + 2);
    idx.push_back(base);
    idx.push_back(base + 2);
    idx.push_back(base + 3);
}

bool glyph(char c, int row, int col) {
    if (row < 0 || row > 4 || col < 0 || col > 2) return false;
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    auto rows = [&](unsigned r0, unsigned r1, unsigned r2, unsigned r3, unsigned r4) -> bool {
        unsigned r = r0;
        if (row == 1) r = r1;
        else if (row == 2) r = r2;
        else if (row == 3) r = r3;
        else if (row == 4) r = r4;
        return (r & (1u << (2 - col))) != 0;
    };
    switch (c) {
    case 'A': return rows(0b010, 0b101, 0b111, 0b101, 0b101);
    case 'B': return rows(0b110, 0b101, 0b110, 0b101, 0b110);
    case 'C': return rows(0b011, 0b100, 0b100, 0b100, 0b011);
    case 'D': return rows(0b110, 0b101, 0b101, 0b101, 0b110);
    case 'E': return rows(0b111, 0b100, 0b110, 0b100, 0b111);
    case 'F': return rows(0b111, 0b100, 0b110, 0b100, 0b100);
    case 'G': return rows(0b011, 0b100, 0b101, 0b101, 0b011);
    case 'H': return rows(0b101, 0b101, 0b111, 0b101, 0b101);
    case 'I': return rows(0b111, 0b010, 0b010, 0b010, 0b111);
    case 'J': return rows(0b001, 0b001, 0b001, 0b101, 0b010);
    case 'K': return rows(0b101, 0b101, 0b110, 0b101, 0b101);
    case 'L': return rows(0b100, 0b100, 0b100, 0b100, 0b111);
    case 'M': return rows(0b101, 0b111, 0b111, 0b101, 0b101);
    case 'N': return rows(0b101, 0b111, 0b111, 0b101, 0b101);
    case 'O': return rows(0b010, 0b101, 0b101, 0b101, 0b010);
    case 'P': return rows(0b110, 0b101, 0b110, 0b100, 0b100);
    case 'Q': return rows(0b010, 0b101, 0b101, 0b010, 0b001);
    case 'R': return rows(0b110, 0b101, 0b110, 0b101, 0b101);
    case 'S': return rows(0b011, 0b100, 0b010, 0b001, 0b110);
    case 'T': return rows(0b111, 0b010, 0b010, 0b010, 0b010);
    case 'U': return rows(0b101, 0b101, 0b101, 0b101, 0b010);
    case 'V': return rows(0b101, 0b101, 0b101, 0b010, 0b010);
    case 'W': return rows(0b101, 0b101, 0b111, 0b111, 0b101);
    case 'X': return rows(0b101, 0b101, 0b010, 0b101, 0b101);
    case 'Y': return rows(0b101, 0b101, 0b010, 0b010, 0b010);
    case 'Z': return rows(0b111, 0b001, 0b010, 0b100, 0b111);
    case '0': return rows(0b010, 0b101, 0b101, 0b101, 0b010);
    case '1': return rows(0b010, 0b110, 0b010, 0b010, 0b111);
    case '2': return rows(0b110, 0b001, 0b010, 0b100, 0b111);
    case '3': return rows(0b110, 0b001, 0b010, 0b001, 0b110);
    case '4': return rows(0b101, 0b101, 0b111, 0b001, 0b001);
    case '5': return rows(0b111, 0b100, 0b110, 0b001, 0b110);
    case '6': return rows(0b011, 0b100, 0b110, 0b101, 0b010);
    case '7': return rows(0b111, 0b001, 0b010, 0b010, 0b010);
    case '8': return rows(0b010, 0b101, 0b010, 0b101, 0b010);
    case '9': return rows(0b010, 0b101, 0b011, 0b001, 0b110);
    case '-': return rows(0b000, 0b000, 0b111, 0b000, 0b000);
    case '_': return rows(0b000, 0b000, 0b000, 0b000, 0b111);
    case '.': return rows(0b000, 0b000, 0b000, 0b000, 0b010);
    case ' ': return false;
    default: return rows(0b111, 0b101, 0b101, 0b101, 0b111);
    }
}

void pushText(std::vector<Vtx>& v, std::vector<unsigned short>& idx, float cx, float cy, float z, float s,
              const std::string& text, float r, float g, float b, float a) {
    float x = cx;
    for (char ch : text) {
        for (int row = 0; row < 5; ++row) {
            for (int col = 0; col < 3; ++col) {
                if (!glyph(ch, row, col)) continue;
                float x0 = x + col * s;
                float y0 = cy - row * s;
                pushQuad(v, idx, x0, y0 - s, x0 + s * 0.9f, y0, z, r, g, b, a);
            }
        }
        x += 4 * s;
    }
}

void projectLabels() {
    float vp[16];
    if (!g_vpValid.load(std::memory_order_acquire)) return;
    {
        std::lock_guard lock(g_vpMu);
        std::memcpy(vp, g_viewProj, sizeof(vp));
    }
    std::lock_guard lock(g_mu);
    for (auto& kv : g_labels) {
        auto& lab = kv.second;
        float ox, oy, oz;
        // lift label slightly above entity origin
        if (worldToNdc(vp, lab.x, lab.y + 0.45f, lab.z, ox, oy, oz)) {
            lab.ndcX = ox;
            lab.ndcY = oy;
            lab.ndcZ = oz;
            lab.projected = true;
        } else {
            lab.projected = false;
        }
    }
}

void drawWorldLabels() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    if (!g_worldLabels.load(std::memory_order_relaxed)) return;
    if (!ensureGl()) return;
    projectLabels();

    std::vector<WorldLabel> copy;
    {
        std::lock_guard lock(g_mu);
        for (auto& kv : g_labels)
            if (kv.second.projected && !kv.second.name.empty()) copy.push_back(kv.second);
    }
    if (copy.empty()) return;

    std::vector<Vtx> verts;
    std::vector<unsigned short> inds;
    verts.reserve(4096);
    inds.reserve(6144);

    for (const auto& lab : copy) {
        std::string line = lab.name;
        if (line.size() > 18) line = line.substr(0, 17) + ".";
        // scale by depth so far labels are smaller
        float depthScale = 0.018f * (1.f - std::min(std::max(lab.ndcZ, -0.5f), 0.95f));
        if (depthScale < 0.008f) depthScale = 0.008f;
        float textW = static_cast<float>(line.size()) * 4.f * depthScale;
        float x0 = lab.ndcX - textW * 0.5f;
        float y0 = lab.ndcY;
        // slight bias toward camera so label wins over entity mesh but loses to walls
        float z = lab.ndcZ - 0.002f;
        if (z < -0.999f) z = -0.999f;
        // background
        pushQuad(verts, inds, x0 - depthScale, y0 - 6.f * depthScale, x0 + textW + depthScale,
                 y0 + 2.f * depthScale, z + 0.0005f, 0.f, 0.f, 0.f, 0.55f);
        pushText(verts, inds, x0, y0, z, depthScale, line, 1.f, 1.f, 0.85f, 0.95f);
    }

    std::lock_guard glLock(g_glMu);
    if (g_depthTest.load(std::memory_order_relaxed)) {
        glEnable(GL_DEPTH_TEST);
        if (glDepthFunc) glDepthFunc(GL_LEQUAL);
        if (glDepthMask) glDepthMask(GL_FALSE); // don't pollute depth buffer
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_prog);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(verts.size() * sizeof(Vtx)), verts.data(),
                 GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizei>(inds.size() * sizeof(unsigned short)),
                 inds.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(static_cast<GLuint>(g_aPos));
    glVertexAttribPointer(static_cast<GLuint>(g_aPos), 3, GL_FLOAT, GL_FALSE, sizeof(Vtx),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(static_cast<GLuint>(g_aCol));
    glVertexAttribPointer(static_cast<GLuint>(g_aCol), 4, GL_FLOAT, GL_FALSE, sizeof(Vtx),
                          reinterpret_cast<void*>(sizeof(float) * 3));
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(inds.size()), GL_UNSIGNED_SHORT, nullptr);
    if (glDepthMask) glDepthMask(GL_TRUE);
}

void onToggle(std::string_view, bool on) {
    g_enabled.store(on, std::memory_order_release);
    logLine(on ? "ItemESP ON (world labels)" : "ItemESP OFF");
    if (on) installMatrixHook();
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "maxDistance") g_maxDistance.store(std::stof(std::string(value)));
        else if (key == "filterPlayers") g_filterPlayers.store(value == "true" || value == "1");
        else if (key == "worldLabels") g_worldLabels.store(value == "true" || value == "1");
        else if (key == "depthTest") g_depthTest.store(value == "true" || value == "1");
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Item ESP");
    b.description("Item names in world (FOV + depth). No wallhack.")
        .defaultEnabled(true)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    b.config("maxDistance", "Max distance", pl::modmenu::ConfigType::SliderFloat, "32", "4", "64", "");
    b.config("filterPlayers", "Hide players", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("worldLabels", "World labels", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("depthTest", "Depth test (no through walls)", pl::modmenu::ConfigType::Toggle, "true", "", "",
             "");
    b.registerModule();
}

void onSignaturesReady() {
    using bactro::memory::SignatureId;
    using bactro::memory::resolve;
    using bactro::memory::hook;

    if (auto a = resolve(SignatureId::ActorIsPlayer)) {
        g_isPlayer = reinterpret_cast<ActorIsPlayerFn>(a);
        logLine("ItemESP: ActorIsPlayer @%p", reinterpret_cast<void*>(a));
    }

    void* o = nullptr;
    if (hook(SignatureId::ActorGetNameTag, reinterpret_cast<void*>(&getNameTagDetour), &o) && o) {
        g_getNameTagOrig = reinterpret_cast<ActorGetNameTagFn>(o);
        logLine("ItemESP: ActorGetNameTag HOOKED");
    } else {
        logLine("ItemESP: ActorGetNameTag FAIL");
    }

    o = nullptr;
    if (hook(SignatureId::ActorShaderManagerSetupShaderParametersActorGlint,
             reinterpret_cast<void*>(&setupActorGlintDetour), &o) &&
        o) {
        g_setupActorGlintOrig = reinterpret_cast<SetupActorGlintFn>(o);
        logLine("ItemESP: setupActorGlint HOOKED (position arm)");
    } else {
        logLine("ItemESP: setupActorGlint FAIL (positions may be sparse)");
    }

    o = nullptr;
    if (hook(SignatureId::ClientInstanceUpdate, reinterpret_cast<void*>(&clientInstanceUpdateDetour), &o) &&
        o) {
        g_ciUpdateOrig = reinterpret_cast<ClientInstanceUpdateFn>(o);
        logLine("ItemESP: ClientInstanceUpdate hooked");
    }

    installMatrixHook();
    logLine("ItemESP: ready (world-projected + depth)");
}

void onPostFrame() {
    try {
        drawWorldLabels();
    } catch (...) {
    }
}

void shutdown() {
    g_enabled.store(false);
    std::lock_guard lock(g_mu);
    g_labels.clear();
}

} // namespace bactro::itemesp
