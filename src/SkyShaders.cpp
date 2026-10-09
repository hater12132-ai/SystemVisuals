#include "bactro/SkyShaders.hpp"
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

#define SKY_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SystemVisuals", __VA_ARGS__)

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
constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_STENCIL_TEST = 0x0B90;
constexpr GLenum GL_VIEWPORT = 0x0BA2;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_ONE = 1;
constexpr GLenum GL_FUNC_ADD = 0x8006;
constexpr GLenum GL_FALSE = 0;
constexpr GLenum GL_TRUE = 1;
constexpr GLenum GL_COLOR_WRITEMASK = 0x0C23;
constexpr GLenum GL_DEPTH_WRITEMASK = 0x0B72;
constexpr GLenum GL_CURRENT_PROGRAM = 0x8B8D;
constexpr GLenum GL_ARRAY_BUFFER_BINDING = 0x8894;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER_BINDING = 0x8895;
constexpr GLenum GL_ACTIVE_TEXTURE = 0x84E0;
constexpr GLenum GL_TEXTURE_BINDING_2D = 0x8069;
constexpr GLenum GL_TEXTURE0 = 0x84C0;

namespace bactro::skyshaders {
namespace {

// One active sky at a time: -1 = none, 0..6 = mode
enum SkyId : int {
    SkyNone = -1,
    SkyMidnight = 0,
    SkyPlasma = 1,
    SkyAurora = 2,
    SkyWater = 3,
    SkyCaustic = 4,
    SkyThunder = 5,
    SkyPulsar = 6,
    SkyCount = 7
};

std::atomic<int> g_active{SkyNone};
std::atomic<float> g_speed{1.0f};
std::atomic<float> g_opacity{1.0f}; // 1 = full sky cover (visible), lower = blend with world
std::atomic<float> g_intensity{1.2f};
std::atomic<int> g_drawCount{0};
std::atomic_bool g_loggedDraw{false};

std::mutex g_glMu;
bool g_glReady = false;
bool g_glFailed = false;
GLuint g_vbo = 0;
GLuint g_ibo = 0;
GLuint g_progs[SkyCount]{};
bool g_progOk[SkyCount]{};
bool g_progTried[SkyCount]{};
GLint g_aPosLoc[SkyCount];

using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
EglSwapBuffersFn g_swapOriginal = nullptr;
bool g_swapHooked = false;

using PFN_glCreateShader = GLuint (*)(GLenum);
using PFN_glShaderSource = void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glCompileShader = void (*)(GLuint);
using PFN_glGetShaderiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glGetShaderInfoLog = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glCreateProgram = GLuint (*)(void);
using PFN_glAttachShader = void (*)(GLuint, GLuint);
using PFN_glBindAttribLocation = void (*)(GLuint, GLuint, const GLchar*);
using PFN_glLinkProgram = void (*)(GLuint);
using PFN_glGetProgramiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glGetProgramInfoLog = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glGetAttribLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glGetUniformLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glGenBuffers = void (*)(GLsizei, GLuint*);
using PFN_glBindBuffer = void (*)(GLenum, GLuint);
using PFN_glBufferData = void (*)(GLenum, GLsizei, const void*, GLenum);
using PFN_glUseProgram = void (*)(GLuint);
using PFN_glUniform1f = void (*)(GLint, GLfloat);
using PFN_glUniform2f = void (*)(GLint, GLfloat, GLfloat);
using PFN_glEnableVertexAttribArray = void (*)(GLuint);
using PFN_glDisableVertexAttribArray = void (*)(GLuint);
using PFN_glVertexAttribPointer = void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using PFN_glDrawElements = void (*)(GLenum, GLsizei, GLenum, const void*);
using PFN_glDisable = void (*)(GLenum);
using PFN_glEnable = void (*)(GLenum);
using PFN_glGetIntegerv = void (*)(GLenum, GLint*);
using PFN_glDeleteShader = void (*)(GLuint);
using PFN_glBlendFunc = void (*)(GLenum, GLenum);
using PFN_glBlendEquation = void (*)(GLenum);
using PFN_glDepthMask = void (*)(GLboolean);
using PFN_glColorMask = void (*)(GLboolean, GLboolean, GLboolean, GLboolean);
using PFN_glGetBooleanv = void (*)(GLenum, GLboolean*);
using PFN_glIsEnabled = GLboolean (*)(GLenum);
using PFN_glActiveTexture = void (*)(GLenum);
using PFN_glBindTexture = void (*)(GLenum, GLuint);
using PFN_glViewport = void (*)(GLint, GLint, GLsizei, GLsizei);

PFN_glCreateShader p_glCreateShader = nullptr;
PFN_glShaderSource p_glShaderSource = nullptr;
PFN_glCompileShader p_glCompileShader = nullptr;
PFN_glGetShaderiv p_glGetShaderiv = nullptr;
PFN_glGetShaderInfoLog p_glGetShaderInfoLog = nullptr;
PFN_glCreateProgram p_glCreateProgram = nullptr;
PFN_glAttachShader p_glAttachShader = nullptr;
PFN_glBindAttribLocation p_glBindAttribLocation = nullptr;
PFN_glLinkProgram p_glLinkProgram = nullptr;
PFN_glGetProgramiv p_glGetProgramiv = nullptr;
PFN_glGetProgramInfoLog p_glGetProgramInfoLog = nullptr;
PFN_glGetAttribLocation p_glGetAttribLocation = nullptr;
PFN_glGetUniformLocation p_glGetUniformLocation = nullptr;
PFN_glGenBuffers p_glGenBuffers = nullptr;
PFN_glBindBuffer p_glBindBuffer = nullptr;
PFN_glBufferData p_glBufferData = nullptr;
PFN_glUseProgram p_glUseProgram = nullptr;
PFN_glUniform1f p_glUniform1f = nullptr;
PFN_glUniform2f p_glUniform2f = nullptr;
PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray = nullptr;
PFN_glDisableVertexAttribArray p_glDisableVertexAttribArray = nullptr;
PFN_glVertexAttribPointer p_glVertexAttribPointer = nullptr;
PFN_glDrawElements p_glDrawElements = nullptr;
PFN_glDisable p_glDisable = nullptr;
PFN_glEnable p_glEnable = nullptr;
PFN_glGetIntegerv p_glGetIntegerv = nullptr;
PFN_glDeleteShader p_glDeleteShader = nullptr;
PFN_glBlendFunc p_glBlendFunc = nullptr;
PFN_glBlendEquation p_glBlendEquation = nullptr;
PFN_glDepthMask p_glDepthMask = nullptr;
PFN_glColorMask p_glColorMask = nullptr;
PFN_glGetBooleanv p_glGetBooleanv = nullptr;
PFN_glIsEnabled p_glIsEnabled = nullptr;
PFN_glActiveTexture p_glActiveTexture = nullptr;
PFN_glBindTexture p_glBindTexture = nullptr;
PFN_glViewport p_glViewport = nullptr;

void logLine(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    SKY_LOGI("%s", buf);
}

void* glProc(const char* name) {
    if (void* p = reinterpret_cast<void*>(eglGetProcAddress(name))) return p;
    void* lib = dlopen("libGLESv2.so", RTLD_NOW);
    if (!lib) lib = dlopen("libGLESv3.so", RTLD_NOW);
    if (!lib) lib = dlopen("libGLESv3.so.2", RTLD_NOW);
    return lib ? dlsym(lib, name) : nullptr;
}

#define LOAD(name) p_##name = reinterpret_cast<decltype(p_##name)>(glProc(#name))

bool loadGles() {
    LOAD(glCreateShader);
    LOAD(glShaderSource);
    LOAD(glCompileShader);
    LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog);
    LOAD(glCreateProgram);
    LOAD(glAttachShader);
    LOAD(glBindAttribLocation);
    LOAD(glLinkProgram);
    LOAD(glGetProgramiv);
    LOAD(glGetProgramInfoLog);
    LOAD(glGetAttribLocation);
    LOAD(glGetUniformLocation);
    LOAD(glGenBuffers);
    LOAD(glBindBuffer);
    LOAD(glBufferData);
    LOAD(glUseProgram);
    LOAD(glUniform1f);
    LOAD(glUniform2f);
    LOAD(glEnableVertexAttribArray);
    LOAD(glDisableVertexAttribArray);
    LOAD(glVertexAttribPointer);
    LOAD(glDrawElements);
    LOAD(glDisable);
    LOAD(glEnable);
    LOAD(glGetIntegerv);
    LOAD(glDeleteShader);
    LOAD(glBlendFunc);
    LOAD(glBlendEquation);
    LOAD(glDepthMask);
    LOAD(glColorMask);
    LOAD(glGetBooleanv);
    LOAD(glIsEnabled);
    LOAD(glActiveTexture);
    LOAD(glBindTexture);
    LOAD(glViewport);
    const bool ok = p_glCreateShader && p_glUseProgram && p_glDrawElements && p_glUniform1f &&
                    p_glGenBuffers && p_glVertexAttribPointer && p_glViewport;
    if (!ok) logLine("SkyShaders: missing GLES procs");
    return ok;
}

// ---- Vertex: fullscreen quad → world ray (yaw/pitch rotate for motion) ----
static constexpr const char* kVS = R"(
attribute vec2 aPosition;
varying vec3 vRay;
varying vec2 vUV;
uniform float uYaw;
uniform float uPitch;
uniform float uAspect;
void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vUV = aPosition * 0.5 + 0.5;
    // FOV ~70 deg
    float tanV = 0.7002;
    vec3 rayV = normalize(vec3(aPosition.x * tanV * uAspect, aPosition.y * tanV, 1.0));
    float cy = cos(uYaw);
    float sy = sin(uYaw);
    float cp = cos(uPitch);
    float sp = sin(uPitch);
    // R = rotY(yaw) * rotX(pitch)
    vRay = vec3(
        cy * rayV.x + sy * sp * rayV.y + sy * cp * rayV.z,
        cp * rayV.y - sp * rayV.z,
        -sy * rayV.x + cy * sp * rayV.y + cy * cp * rayV.z
    );
}
)";

// ---- Plasma (Lexora plasma_sky) — primary, must work ----
static constexpr const char* kFS_Plasma = R"(
precision mediump float;
varying vec3 vRay;
uniform float uTime;
uniform float uIntensity;
uniform float uOpacity;
void main() {
    vec3 rd = normalize(vRay);
    float t = uTime * 0.4;
    // rotation matrix for seamless 3D noise (Lexora m3)
    mat3 m3 = mat3(
        0.36,  0.48, -0.80,
       -0.80,  0.60,  0.00,
        0.48,  0.64,  0.60
    );
    vec3 p = rd * 3.0;
    float flow = 0.0;
    float amp = 1.0;
    for (int i = 0; i < 5; i++) {
        p += t * 0.5;
        flow += amp * abs(sin(p.x) * cos(p.y) + sin(p.z));
        p = m3 * p * 1.3;
        amp *= 0.6;
    }
    flow = smoothstep(0.5, 2.5, flow);
    // deep space: purple -> pink -> cyan
    vec3 bg = vec3(0.05, 0.01, 0.10);
    vec3 c1 = vec3(0.60, 0.10, 0.40);
    vec3 c2 = vec3(0.10, 0.70, 0.80);
    vec3 col = mix(bg, c1, flow);
    col = mix(col, c2, smoothstep(0.6, 1.0, flow));
    col *= uIntensity;
    gl_FragColor = vec4(col, uOpacity);
}
)";

// ---- Midnight ----
static constexpr const char* kFS_Midnight = R"(
precision mediump float;
varying vec3 vRay;
uniform float uTime;
uniform float uIntensity;
uniform float uOpacity;
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 19.19);
    return fract((p3.x + p3.y) * p3.z);
}
float noisyStar(vec2 p, float thr) {
    float v = hash12(p);
    return v >= thr ? pow((v - thr) / (1.0 - thr), 6.0) : 0.0;
}
float stableStar(vec2 p, float thr) {
    float fx = fract(p.x), fy = fract(p.y);
    vec2 fp = floor(p);
    return noisyStar(fp, thr) * (1.0 - fx) * (1.0 - fy)
         + noisyStar(fp + vec2(0.0, 1.0), thr) * (1.0 - fx) * fy
         + noisyStar(fp + vec2(1.0, 0.0), thr) * fx * (1.0 - fy)
         + noisyStar(fp + vec2(1.0, 1.0), thr) * fx * fy;
}
float hash11(float p) {
    vec3 p3 = fract(vec3(p) * 0.1031);
    p3 += dot(p3, p3.yzx + 19.19);
    return fract((p3.x + p3.y) * p3.z);
}
float smoothNoise13(vec3 x) {
    vec3 p = floor(x);
    vec3 f = smoothstep(0.0, 1.0, fract(x));
    float n = p.x + p.y * 57.0 + 113.0 * p.z;
    return mix(
        mix(mix(hash11(n), hash11(n + 1.0), f.x), mix(hash11(n + 57.0), hash11(n + 58.0), f.x), f.y),
        mix(mix(hash11(n + 113.0), hash11(n + 114.0), f.x), mix(hash11(n + 170.0), hash11(n + 171.0), f.x), f.y),
        f.z);
}
float fbm(vec3 p) {
    mat3 fbmMat = mat3(0.0, 1.6, 1.2, -1.6, 0.72, -0.96, -1.2, -0.96, 1.28);
    float f = 0.5 * smoothNoise13(p); p = fbmMat * p * 1.2;
    f += 0.25 * smoothNoise13(p); p = fbmMat * p * 1.3;
    f += 0.1666 * smoothNoise13(p); p = fbmMat * p * 1.4;
    f += 0.0834 * smoothNoise13(p);
    return f;
}
void main() {
    vec3 rd = normalize(vRay);
    vec3 col = vec3(0.02, 0.04, 0.12) * (abs(rd.y) * 0.5 + 0.5);
    // moon
    vec3 moonDir = normalize(vec3(0.3, 0.6, 0.7));
    float md = max(0.0, dot(rd, moonDir));
    col += vec3(0.7) * pow(md, 80.0);
    col += vec3(0.3, 0.35, 0.4) * pow(md, 6.0) * 0.25;
    // rotating stars
    vec2 starUV = rd.xz / max(abs(rd.y), 0.001);
    float angle = 0.0005 * uTime * 60.0 + atan(starUV.y, starUV.x);
    float len = length(starUV);
    vec2 samplePos = (0.5 * len * vec2(cos(angle), sin(angle)) + 0.5) * 900.0;
    col += vec3(stableStar(samplePos, 0.985));
    // drifting clouds
    float t = uTime * 0.05;
    vec3 fbmIn = vec3(rd.x / (abs(rd.y) + 0.1) - t, rd.z / (abs(rd.y) + 0.1), 0.0);
    col += vec3(0.5, 0.5, 0.75) * fbm(fbmIn) * 0.5;
    col *= uIntensity;
    gl_FragColor = vec4(col, uOpacity);
}
)";

// ---- Aurora (reduced loops for mobile GLES) ----
static constexpr const char* kFS_Aurora = R"(
precision mediump float;
varying vec3 vRay;
uniform float uTime;
uniform float uIntensity;
uniform float uOpacity;
mat2 mm2(float a) {
    float c = cos(a), s = sin(a);
    return mat2(c, s, -s, c);
}
float tri(float x) { return clamp(abs(fract(x) - 0.5), 0.01, 0.49); }
vec2 tri2(vec2 p) { return vec2(tri(p.x) + tri(p.y), tri(p.y + tri(p.x))); }
float triNoise2d(vec2 p, float spd) {
    float z = 1.8, z2 = 2.5, rz = 0.0;
    p *= mm2(p.x * 0.06);
    vec2 bp = p;
    mat2 m2 = mat2(0.95534, 0.29552, -0.29552, 0.95534);
    for (int i = 0; i < 5; i++) {
        vec2 dg = tri2(bp * 1.85) * 0.75;
        dg *= mm2(uTime * spd);
        p -= dg / z2;
        bp *= 1.3;
        z2 *= 0.45;
        z *= 0.42;
        p *= 1.21 + (rz - 1.0) * 0.02;
        rz += tri(p.x + tri(p.y)) * z;
        p *= -m2;
    }
    return clamp(1.0 / pow(rz * 29.0, 1.3), 0.0, 0.55);
}
float hash21(vec2 n) {
    return fract(sin(dot(n, vec2(12.9898, 4.1414))) * 43758.5453);
}
vec3 bg(vec3 rd) {
    float sd = dot(normalize(vec3(-0.5, -0.6, 0.9)), rd) * 0.5 + 0.5;
    sd = pow(sd, 5.0);
    return mix(vec3(0.05, 0.1, 0.2), vec3(0.1, 0.05, 0.2), sd) * 0.63;
}
void main() {
    vec3 rd = normalize(vRay);
    vec3 col = bg(rd);
    vec3 rrd = vec3(rd.x, abs(rd.y), rd.z);
    vec3 ro = vec3(0.0, 0.0, -6.7);
    vec4 aur = vec4(0.0);
    vec4 avgCol = vec4(0.0);
    for (int i = 0; i < 20; i++) {
        float fi = float(i);
        float of = 0.006 * hash21(rd.xz * 100.0) * smoothstep(0.0, 15.0, fi);
        float pt = ((0.8 + pow(fi, 1.4) * 0.002) - ro.y) / (rrd.y * 2.0 + 0.4);
        pt -= of;
        vec3 bpos = ro + pt * rrd;
        float rzt = triNoise2d(bpos.zx, 0.06);
        vec4 col2 = vec4((sin(1.0 - vec3(2.15, -0.5, 1.2) + fi * 0.043) * 0.5 + 0.5) * rzt, rzt);
        avgCol = mix(avgCol, col2, 0.5);
        aur += avgCol * exp2(-fi * 0.065 - 2.5) * smoothstep(0.0, 5.0, fi);
    }
    aur *= clamp(rrd.y * 15.0 + 0.4, 0.0, 1.0) * 1.8 * uIntensity;
    aur = smoothstep(0.0, 1.5, aur);
    // simple stars
    float sh = fract(sin(dot(floor(rd * 80.0), vec3(12.1, 78.2, 45.3))) * 43758.5);
    if (sh > 0.997) {
        float tw = 0.6 + 0.4 * sin(uTime * 4.0 + sh * 40.0);
        col += vec3(tw);
    }
    if (rd.y > -0.05) {
        col = col * (1.0 - aur.a) + aur.rgb;
    }
    gl_FragColor = vec4(col, uOpacity);
}
)";

// ---- Water ----
static constexpr const char* kFS_Water = R"(
precision mediump float;
varying vec3 vRay;
uniform float uTime;
uniform float uIntensity;
uniform float uOpacity;
void main() {
    vec3 rayW = normalize(vRay);
    vec3 p = rayW * 5.0;
    vec3 i = p;
    float c = 1.0;
    float inten = 0.01;
    for (int n = 0; n < 5; n++) {
        float t = uTime * (1.0 - (3.5 / float(n + 1)));
        i = p + vec3(
            cos(t - i.x) + sin(t + i.y),
            sin(t - i.y) + cos(t + i.z),
            cos(t - i.z) + sin(t + i.x)
        );
        vec3 sc = vec3(sin(i.x + t) / inten, cos(i.y + t) / inten, sin(i.z + t) / inten);
        c += 1.0 / length(p / sc);
    }
    c /= 5.0;
    c = 1.17 - pow(abs(c), 1.4);
    vec3 color = vec3(pow(abs(c), 8.0));
    vec3 base = vec3(0.2, 0.5, 0.9);
    color = clamp(color + base * 0.7, 0.0, 1.0);
    color = mix(base, color, 0.45) * uIntensity;
    gl_FragColor = vec4(color, uOpacity * clamp(c * 0.9 + 0.3, 0.0, 1.0));
}
)";

// ---- Caustic ----
static constexpr const char* kFS_Caustic = R"(
precision mediump float;
varying vec3 vRay;
uniform float uTime;
uniform float uIntensity;
uniform float uOpacity;
void main() {
    vec3 rayW = normalize(vRay);
    vec3 p = rayW * 5.0;
    vec3 i = p;
    float c = 1.0;
    vec3 p_inten = p * 0.01;
    for (int n = 0; n < 4; n++) {
        float t = uTime * (1.0 - (3.0 / float(n + 1)));
        i = p + vec3(
            cos(t - i.x) + sin(t + i.y),
            sin(t - i.y) + cos(t + i.z),
            cos(t - i.z) + sin(t + i.x)
        );
        vec3 sc = vec3(sin(i.x + t), cos(i.y + t), sin(i.z + t));
        c += 1.0 / length(p_inten / sc);
    }
    c /= 4.0;
    c = 1.5 - sqrt(max(c, 0.0));
    float brightness = c * c * c * c;
    vec3 uColor = vec3(0.15, 0.7, 0.85);
    vec3 color = uColor * brightness * 1.5 + uColor * 0.2;
    color *= uIntensity;
    gl_FragColor = vec4(color, uOpacity);
}
)";

// ---- Thunder ----
static constexpr const char* kFS_Thunder = R"(
precision mediump float;
varying vec3 vRay;
uniform float uTime;
uniform float uIntensity;
uniform float uOpacity;
float hash1(float n) { return fract(sin(n) * 43758.5453); }
float hash3(vec3 p) {
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}
float noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash3(i);
    float n100 = hash3(i + vec3(1.0, 0.0, 0.0));
    float n010 = hash3(i + vec3(0.0, 1.0, 0.0));
    float n110 = hash3(i + vec3(1.0, 1.0, 0.0));
    float n001 = hash3(i + vec3(0.0, 0.0, 1.0));
    float n101 = hash3(i + vec3(1.0, 0.0, 1.0));
    float n011 = hash3(i + vec3(0.0, 1.0, 1.0));
    float n111 = hash3(i + vec3(1.0, 1.0, 1.0));
    vec4 a = mix(vec4(n000, n010, n001, n011), vec4(n100, n110, n101, n111), f.x);
    vec2 b = mix(a.xz, a.yw, f.y);
    return mix(b.x, b.y, f.z);
}
float fbm(vec3 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 4; i++) {
        v += a * noise(p);
        p = p * 2.0 + vec3(100.0);
        a *= 0.5;
    }
    return v;
}
void main() {
    vec3 rayW = normalize(vRay);
    float cloudDensity = 0.0;
    if (rayW.y > -0.15) {
        vec3 p = rayW * 5.0;
        p.x += uTime * 0.08;
        p.z += uTime * 0.04;
        cloudDensity = fbm(p) * smoothstep(-0.15, 0.3, rayW.y);
    }
    float interval = 4.0;
    float timeIndex = floor(uTime / interval);
    float timeOffset = fract(uTime / interval);
    float strikeHash = hash1(timeIndex * 12.34);
    float flash = 0.0;
    float boltGlow = 0.0;
    if (strikeHash < 0.65) {
        float strikeAngle = hash1(timeIndex * 45.67) * 6.28318;
        vec3 boltStart = normalize(vec3(cos(strikeAngle), 1.0, sin(strikeAngle)));
        vec3 boltEnd = normalize(vec3(
            cos(strikeAngle + (hash1(timeIndex * 8.3) - 0.5) * 0.2),
            -0.2,
            sin(strikeAngle + (hash1(timeIndex * 8.3) - 0.5) * 0.2)));
        float t = timeOffset;
        if (t < 0.55) {
            float flicker = 0.8 + 0.2 * sin(uTime * 95.0) * cos(uTime * 135.0);
            flash = (exp(-t * 22.0) * 1.5 + exp(-abs(t - 0.15) * 25.0) + exp(-abs(t - 0.35) * 15.0) * 0.4) * flicker;
            float boltIntensity = max(exp(-t * 18.0), exp(-abs(t - 0.15) * 22.0));
            if (boltIntensity > 0.02) {
                vec3 segment = boltEnd - boltStart;
                float segLen = length(segment);
                vec3 segDir = segment / max(segLen, 0.001);
                float h = clamp(dot(rayW - boltStart, segDir), 0.0, segLen);
                vec3 projection = boltStart + segDir * h;
                float jagged = sin(h * 30.0 + timeIndex * 73.19) * 0.035
                             + cos(h * 70.0) * 0.015
                             + sin(h * 150.0) * 0.007;
                vec3 offsetProj = projection + vec3(jagged, 0.0, jagged * 0.6);
                float distToBolt = length(rayW - normalize(offsetProj));
                boltGlow = (exp(-distToBolt * 280.0) * 2.5 + exp(-distToBolt * 30.0) * 0.6) * boltIntensity * flicker;
            }
        }
    }
    vec3 stormBase = vec3(0.01, 0.01, 0.02);
    vec3 finalSky = stormBase + vec3(0.75, 0.82, 1.0) * flash * 0.25;
    vec3 cloudBase = mix(vec3(0.01), vec3(0.06, 0.07, 0.12), cloudDensity);
    vec3 cloudLit = mix(vec3(0.02, 0.025, 0.05), vec3(0.8, 0.85, 1.0), cloudDensity * flash * 0.7);
    vec3 finalCloud = mix(cloudBase, cloudLit, min(flash, 1.0));
    vec3 finalColor = mix(finalSky, finalCloud, smoothstep(0.18, 0.48, cloudDensity));
    finalColor += vec3(0.85, 0.92, 1.0) * boltGlow;
    finalColor *= (1.0 + uIntensity);
    gl_FragColor = vec4(finalColor, uOpacity);
}
)";

// ---- Pulsar ----
static constexpr const char* kFS_Pulsar = R"(
precision mediump float;
varying vec3 vRay;
uniform float uTime;
uniform float uIntensity;
uniform float uOpacity;
float hash3(vec3 p) {
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}
float noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash3(i);
    float n100 = hash3(i + vec3(1.0, 0.0, 0.0));
    float n010 = hash3(i + vec3(0.0, 1.0, 0.0));
    float n110 = hash3(i + vec3(1.0, 1.0, 0.0));
    float n001 = hash3(i + vec3(0.0, 0.0, 1.0));
    float n101 = hash3(i + vec3(1.0, 0.0, 1.0));
    float n011 = hash3(i + vec3(0.0, 1.0, 1.0));
    float n111 = hash3(i + vec3(1.0, 1.0, 1.0));
    vec4 a = mix(vec4(n000, n010, n001, n011), vec4(n100, n110, n101, n111), f.x);
    vec2 b = mix(a.xz, a.yw, f.y);
    return mix(b.x, b.y, f.z);
}
float fbm(vec3 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 4; i++) {
        v += a * noise(p);
        p = p * 2.0 + vec3(100.0);
        a *= 0.5;
    }
    return v;
}
void main() {
    vec3 rayW = normalize(vRay);
    float n1 = fbm(rayW * 2.0 + vec3(uTime * 0.003, 0.0, 0.0));
    float n2 = fbm(rayW * 4.5 - vec3(0.0, uTime * 0.002, 0.0));
    vec3 gasColor = mix(vec3(0.001, 0.003, 0.012), vec3(0.035, 0.01, 0.055), n1);
    gasColor = mix(gasColor, vec3(0.008, 0.025, 0.035), n2 * n1);
    float dust = smoothstep(0.3, 0.7, fbm(rayW * 6.0 + vec3(0.1)));
    vec3 col = mix(gasColor, vec3(0.0005, 0.0005, 0.0015), dust * 0.85);
    // stars
    vec3 gridPos = floor(rayW * 130.0);
    float starHash = hash3(gridPos);
    if (starHash > 0.992) {
        float twinkle = 0.55 + 0.45 * sin(uTime * 3.5 + starHash * 120.0);
        col += vec3(twinkle * 0.8);
    }
    // pulsar core + jets
    vec3 pulsarCenter = normalize(vec3(0.55, 0.65, -0.45));
    float frontMask = smoothstep(0.0, 0.4, dot(rayW, pulsarCenter));
    if (frontMask > 0.0) {
        vec3 tangent1 = normalize(cross(pulsarCenter, vec3(0.0, 1.0, 0.0)));
        vec3 tangent2 = cross(tangent1, pulsarCenter);
        vec2 p2d = vec2(dot(rayW, tangent1), dot(rayW, tangent2));
        float dCore = length(p2d);
        float pulse = 0.95 + 0.05 * sin(uTime * 8.0);
        float core = smoothstep(0.058 * pulse, 0.0, dCore);
        float halo = exp(-dCore * 16.0) * 3.2 + exp(-dCore * 3.5) * 0.9;
        float wobbleAngle = sin(uTime * 6.0) * 0.22;
        vec2 jetDir2d = vec2(sin(wobbleAngle), cos(wobbleAngle));
        float distAlong = dot(p2d, jetDir2d);
        float wave = sin(abs(distAlong) * 75.0 - uTime * 32.0) * 0.0055 * abs(distAlong);
        float distToJet = length(p2d - distAlong * jetDir2d) - wave;
        float jetIntensity = (exp(-distToJet * 320.0) * 3.0 + exp(-distToJet * 38.0) * 0.8);
        jetIntensity *= exp(-abs(distAlong) * 0.85);
        float jetFlicker = 0.82 + 0.18 * sin(uTime * 115.0) * cos(uTime * 145.0);
        vec3 pulsarCol = vec3(core);
        pulsarCol += vec3(0.18, 0.45, 1.0) * halo;
        pulsarCol += vec3(0.35, 0.68, 1.0) * jetIntensity * jetFlicker;
        col += pulsarCol * frontMask;
    }
    col *= (1.0 + uIntensity);
    gl_FragColor = vec4(col, uOpacity);
}
)";

const char* fragFor(int id) {
    switch (id) {
        case SkyMidnight: return kFS_Midnight;
        case SkyPlasma: return kFS_Plasma;
        case SkyAurora: return kFS_Aurora;
        case SkyWater: return kFS_Water;
        case SkyCaustic: return kFS_Caustic;
        case SkyThunder: return kFS_Thunder;
        case SkyPulsar: return kFS_Pulsar;
        default: return kFS_Plasma;
    }
}

const char* nameFor(int id) {
    switch (id) {
        case SkyMidnight: return "Midnight";
        case SkyPlasma: return "Plasma";
        case SkyAurora: return "Aurora";
        case SkyWater: return "Water";
        case SkyCaustic: return "Caustic";
        case SkyThunder: return "Thunder";
        case SkyPulsar: return "Pulsar";
        default: return "?";
    }
}

bool compileShader(GLenum type, const char* src, GLuint& out) {
    out = p_glCreateShader(type);
    if (!out) return false;
    p_glShaderSource(out, 1, &src, nullptr);
    p_glCompileShader(out);
    GLint ok = 0;
    p_glGetShaderiv(out, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        log[0] = 0;
        if (p_glGetShaderInfoLog) p_glGetShaderInfoLog(out, 512, nullptr, log);
        logLine("SkyShaders: %s shader FAIL: %s", type == GL_VERTEX_SHADER ? "VS" : "FS", log);
        return false;
    }
    return true;
}

bool buildProg(int id) {
    if (id < 0 || id >= SkyCount) return false;
    if (g_progOk[id]) return true;
    if (g_progTried[id]) return false;
    g_progTried[id] = true;

    GLuint vs = 0, fs = 0;
    if (!compileShader(GL_VERTEX_SHADER, kVS, vs)) return false;
    if (!compileShader(GL_FRAGMENT_SHADER, fragFor(id), fs)) {
        if (p_glDeleteShader) p_glDeleteShader(vs);
        return false;
    }
    GLuint prog = p_glCreateProgram();
    p_glAttachShader(prog, vs);
    p_glAttachShader(prog, fs);
    // Force attribute location 0 so we don't depend on driver assignment
    if (p_glBindAttribLocation) p_glBindAttribLocation(prog, 0, "aPosition");
    p_glLinkProgram(prog);
    GLint linked = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (p_glDeleteShader) {
        p_glDeleteShader(vs);
        p_glDeleteShader(fs);
    }
    if (!linked) {
        char log[512];
        log[0] = 0;
        if (p_glGetProgramInfoLog) p_glGetProgramInfoLog(prog, 512, nullptr, log);
        logLine("SkyShaders: link FAIL %s: %s", nameFor(id), log);
        return false;
    }
    g_progs[id] = prog;
    g_aPosLoc[id] = p_glGetAttribLocation ? p_glGetAttribLocation(prog, "aPosition") : 0;
    if (g_aPosLoc[id] < 0) g_aPosLoc[id] = 0;
    g_progOk[id] = true;
    logLine("SkyShaders: %s shader OK", nameFor(id));
    return true;
}

bool initGl() {
    if (g_glReady) return true;
    if (g_glFailed) return false;
    if (!loadGles()) {
        g_glFailed = true;
        return false;
    }
    // Fullscreen triangle strip as two triangles (NDC)
    const GLfloat verts[] = {
        -1.f, -1.f,
         1.f, -1.f,
        -1.f,  1.f,
         1.f,  1.f,
    };
    const unsigned short idx[] = {0, 1, 2, 1, 3, 2};
    p_glGenBuffers(1, &g_vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    p_glGenBuffers(1, &g_ibo);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    g_glReady = true;
    logLine("SkyShaders: GL ready");
    return true;
}

float gameTime() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
}

void set1f(GLuint prog, const char* name, float v) {
    GLint loc = p_glGetUniformLocation(prog, name);
    if (loc >= 0) p_glUniform1f(loc, v);
}

void drawSky() {
    const int active = g_active.load(std::memory_order_relaxed);
    if (active < 0 || active >= SkyCount) return;

    // Must run on the GL thread (eglSwapBuffers is on it)
    std::lock_guard lock(g_glMu);
    if (!initGl()) return;
    if (!buildProg(active)) return;

    GLuint prog = g_progs[active];
    GLint aPos = g_aPosLoc[active];

    // Save minimal state we touch
    GLint prevProg = 0;
    GLint prevVbo = 0, prevIbo = 0;
    GLboolean depthWas = GL_TRUE;
    GLboolean blendWas = GL_FALSE;
    GLboolean depthMaskWas = GL_TRUE;
    if (p_glGetIntegerv) {
        p_glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
        p_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevVbo);
        p_glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevIbo);
    }
    if (p_glIsEnabled) {
        depthWas = p_glIsEnabled(GL_DEPTH_TEST);
        blendWas = p_glIsEnabled(GL_BLEND);
    }
    if (p_glGetBooleanv) {
        GLboolean dm = GL_TRUE;
        p_glGetBooleanv(GL_DEPTH_WRITEMASK, &dm);
        depthMaskWas = dm;
    }

    // CRITICAL: at swap time Minecraft often leaves a tiny UI viewport (corner of screen).
    // Always size from the EGL surface and set a full viewport — same fix MotionBlur uses.
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLSurface surf = eglGetCurrentSurface(EGL_DRAW);
    EGLint surfW = 0, surfH = 0;
    if (dpy != EGL_NO_DISPLAY && surf != EGL_NO_SURFACE) {
        eglQuerySurface(dpy, surf, EGL_WIDTH, &surfW);
        eglQuerySurface(dpy, surf, EGL_HEIGHT, &surfH);
    }
    if (surfW <= 0 || surfH <= 0) {
        GLint vp[4] = {0, 0, 1080, 1920};
        p_glGetIntegerv(GL_VIEWPORT, vp);
        surfW = std::max(1, vp[2]);
        surfH = std::max(1, vp[3]);
    }
    p_glViewport(0, 0, surfW, surfH);
    float aspect = static_cast<float>(surfW) / static_cast<float>(surfH);

    const float t = gameTime();
    const float speed = g_speed.load(std::memory_order_relaxed);
    const float opacity = g_opacity.load(std::memory_order_relaxed);
    const float intensity = g_intensity.load(std::memory_order_relaxed);
    // slow camera drift so the dome always feels alive even without real camera
    const float yaw = t * 0.05f * speed;
    const float pitch = 0.12f * std::sin(t * 0.15f * speed);

    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_CULL_FACE);
    p_glDisable(GL_SCISSOR_TEST);
    if (p_glIsEnabled && p_glIsEnabled(GL_STENCIL_TEST)) p_glDisable(GL_STENCIL_TEST);
    if (p_glDepthMask) p_glDepthMask(GL_FALSE);
    if (p_glColorMask) p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_glEnable(GL_BLEND);
    if (p_glBlendEquation) p_glBlendEquation(GL_FUNC_ADD);
    if (p_glBlendFunc) p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    p_glUseProgram(prog);
    set1f(prog, "uYaw", yaw);
    set1f(prog, "uPitch", pitch);
    set1f(prog, "uAspect", aspect);
    set1f(prog, "uTime", t * speed);
    set1f(prog, "uIntensity", intensity);
    set1f(prog, "uOpacity", opacity);

    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    p_glEnableVertexAttribArray(static_cast<GLuint>(aPos));
    p_glVertexAttribPointer(static_cast<GLuint>(aPos), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    p_glDisableVertexAttribArray(static_cast<GLuint>(aPos));

    // Restore
    p_glUseProgram(static_cast<GLuint>(prevProg));
    p_glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prevVbo));
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(prevIbo));
    if (p_glDepthMask) p_glDepthMask(depthMaskWas);
    if (depthWas) p_glEnable(GL_DEPTH_TEST); else p_glDisable(GL_DEPTH_TEST);
    if (blendWas) p_glEnable(GL_BLEND); else p_glDisable(GL_BLEND);

    const int n = g_drawCount.fetch_add(1) + 1;
    if (!g_loggedDraw.exchange(true) || n == 60 || n == 300) {
        logLine("SkyShaders: drawing %s frames=%d t=%.1f", nameFor(active), n, t);
    }
}

EGLBoolean swapDetour(EGLDisplay d, EGLSurface s) {
    // Draw AFTER the game finished the frame so our fullscreen sky is visible
    drawSky();
    return g_swapOriginal ? g_swapOriginal(d, s) : eglSwapBuffers(d, s);
}

bool installSwapHook() {
    if (g_swapHooked) return true;
    void* sym = reinterpret_cast<void*>(eglGetProcAddress("eglSwapBuffers"));
    if (!sym) {
        void* egl = dlopen("libEGL.so", RTLD_NOW);
        if (!egl) egl = dlopen("libEGL.so.1", RTLD_NOW);
        if (egl) sym = dlsym(egl, "eglSwapBuffers");
    }
    if (!sym) {
        logLine("SkyShaders: eglSwapBuffers symbol missing");
        return false;
    }
    void* o = nullptr;
    // pl::memory::hook returns 0 on success (same as MotionBlur)
    if (pl::memory::hook(sym, reinterpret_cast<void*>(&swapDetour), &o) != 0) {
        logLine("SkyShaders: eglSwapBuffers HOOK FAIL");
        return false;
    }
    g_swapOriginal = reinterpret_cast<EglSwapBuffersFn>(o);
    g_swapHooked = true;
    logLine("SkyShaders: eglSwapBuffers hooked");
    return true;
}

void activateSky(int id, bool on) {
    if (on) {
        g_active.store(id, std::memory_order_release);
        g_loggedDraw.store(false);
        g_drawCount.store(0);
        installSwapHook();
        // Reset compile attempt so a previous fail can retry after GL context is live
        if (id >= 0 && id < SkyCount) g_progTried[id] = false;
        logLine("SkyShaders: ON %s", nameFor(id));
    } else {
        // Only turn off if this sky is the active one
        int cur = g_active.load(std::memory_order_relaxed);
        if (cur == id) {
            g_active.store(SkyNone, std::memory_order_release);
            logLine("SkyShaders: OFF %s", nameFor(id));
        }
    }
}

void onToggleMidnight(std::string_view, bool on) { activateSky(SkyMidnight, on); }
void onTogglePlasma(std::string_view, bool on) { activateSky(SkyPlasma, on); }
void onToggleAurora(std::string_view, bool on) { activateSky(SkyAurora, on); }
void onToggleWater(std::string_view, bool on) { activateSky(SkyWater, on); }
void onToggleCaustic(std::string_view, bool on) { activateSky(SkyCaustic, on); }
void onToggleThunder(std::string_view, bool on) { activateSky(SkyThunder, on); }
void onTogglePulsar(std::string_view, bool on) { activateSky(SkyPulsar, on); }

void onGlobalConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "speed") {
            g_speed.store(std::stof(std::string(value)), std::memory_order_relaxed);
        } else if (key == "opacity") {
            g_opacity.store(std::stof(std::string(value)), std::memory_order_relaxed);
        } else if (key == "intensity") {
            g_intensity.store(std::stof(std::string(value)), std::memory_order_relaxed);
        }
    } catch (...) {
    }
}

void registerOne(const char* id, const char* title, void (*toggle)(std::string_view, bool)) {
    pl::modmenu::ModuleBuilder b(id, title);
    b.description("Lexora-style animated sky. Only one sky active at a time — last enabled wins.")
        .defaultEnabled(false)
        .onToggle(toggle);
    b.registerModule();
}

} // namespace

void registerModule() {
    // Shared settings module
    {
        pl::modmenu::ModuleBuilder b("bactro.sky.settings", "Sky Settings");
        b.description("Speed / opacity / intensity for whatever sky is active.")
            .defaultEnabled(true)
            .onConfigChanged(onGlobalConfig);
        b.config("speed", "Animation Speed", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.1", "3.0", "");
        b.config("opacity", "Sky Opacity (1=full cover)", pl::modmenu::ConfigType::SliderFloat, "1.0", "0.2", "1.0", "");
        b.config("intensity", "Brightness", pl::modmenu::ConfigType::SliderFloat, "1.2", "0.3", "3.0", "");
        b.registerModule();
    }
    registerOne("bactro.sky.plasma", "Sky: Plasma", onTogglePlasma);
    registerOne("bactro.sky.midnight", "Sky: Midnight", onToggleMidnight);
    registerOne("bactro.sky.aurora", "Sky: Aurora", onToggleAurora);
    registerOne("bactro.sky.water", "Sky: Water", onToggleWater);
    registerOne("bactro.sky.caustic", "Sky: Caustic", onToggleCaustic);
    registerOne("bactro.sky.thunder", "Sky: Thunder", onToggleThunder);
    registerOne("bactro.sky.pulsar", "Sky: Pulsar", onTogglePulsar);

    // Install hook early so first toggle already has a live path
    installSwapHook();
    logLine("SkyShaders: modules registered (7 skies)");
}

void onSignaturesReady() {
    installSwapHook();
}

void shutdown() {
    g_active.store(SkyNone, std::memory_order_release);
}

} // namespace bactro::skyshaders
