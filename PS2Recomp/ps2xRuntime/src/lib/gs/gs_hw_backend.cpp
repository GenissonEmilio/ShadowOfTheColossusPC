#include "runtime/gs/gs_hw_backend.h"
#include "runtime/gs/gs_cpu_backend.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#define PS2X_GS_HW_GL 1
#endif

namespace
{
    std::function<void()> g_makeCurrent;
}

void GSHwBackend::setContextProvider(std::function<void()> makeCurrent) { g_makeCurrent = std::move(makeCurrent); }
bool GSHwBackend::available()
{
#if defined(PS2X_GS_HW_GL)
    return static_cast<bool>(g_makeCurrent);
#else
    return false;
#endif
}

#if defined(PS2X_GS_HW_GL)

namespace
{
    // Alpha inside the GPU is stored in PS2 units: 0x80 = 1.0.
    constexpr float kAlphaOne = 128.0f;

    bool isColorPsm(uint32_t psm) { return psm == GS_PSM_CT32 || psm == GS_PSM_CT24 || psm == GS_PSM_CT16 || psm == GS_PSM_CT16S; }
    uint32_t bytesPerPixel(uint32_t psm) { return (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S || psm == GS_PSM_Z16 || psm == GS_PSM_Z16S) ? 2u : 4u; }

    const char *kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;     // render-target pixels, depth 0..1
layout(location = 1) in vec3 aTex;     // s, t, q (or u, v, 1 in texels)
layout(location = 2) in vec4 aColor;   // 0..255
layout(location = 3) in float aFog;    // 0..255
uniform vec2 uTarget;
out vec3 vTex;
out vec4 vColor;
out float vFog;
out float vDepth;
void main()
{
    // Depth goes to gl_FragDepth as is: GS Z values are often tiny, and the
    // NDC mapping (z * 2 - 1) would round them together near -1.
    gl_Position = vec4(aPos.xy / uTarget * 2.0 - 1.0, 0.0, 1.0);
    vDepth = aPos.z;
    vTex = aTex;
    vColor = aColor;
    vFog = aFog;
}
)";

    const char *kFragmentShader = R"(#version 330 core
in vec3 vTex;
in vec4 vColor;
in float vFog;
in float vDepth;
uniform sampler2D uTexture;
uniform int uTme, uTfx, uTcc, uFst, uFge, uAtest, uAref, uAtestFailed;
uniform vec2 uTexSize;       // texels of the bound texture
uniform vec2 uUvScale;       // GS texture size / bound texture size
uniform float uAlphaFix;     // >= 0: texture alpha
uniform float uTexAlphaScale; // texture alpha read / scale -> PS2 alpha units of 128
uniform vec3 uFogColor;
out vec4 oColor;
void main()
{
    vec4 c = vColor;              // 0..255, 0x80 = 1.0 for modulation
    if (uTme != 0)
    {
        vec2 uv = (uFst != 0) ? vTex.xy / uTexSize : vTex.xy / vTex.z * uUvScale;
        vec4 t = texture(uTexture, uv);
        t.rgb *= 255.0;
        t.a *= 255.0 * uTexAlphaScale;
        if (uAlphaFix >= 0.0)
            t.a = uAlphaFix;
        vec3 rgb;
        float a;
        if (uTfx == 0)      { rgb = t.rgb * c.rgb / 128.0; a = (uTcc != 0) ? t.a * c.a / 128.0 : c.a; }   // MODULATE
        else if (uTfx == 1) { rgb = t.rgb;                  a = (uTcc != 0) ? t.a : c.a; }               // DECAL
        else if (uTfx == 2) { rgb = t.rgb * c.rgb / 128.0 + c.a; a = (uTcc != 0) ? t.a + c.a : c.a; }   // HIGHLIGHT
        else                { rgb = t.rgb * c.rgb / 128.0 + c.a; a = (uTcc != 0) ? t.a : c.a; }       // HIGHLIGHT2
        c = vec4(min(rgb, vec3(255.0)), min(a, 255.0));
    }
    if (uFge != 0)
        c.rgb = mix(uFogColor, c.rgb, vFog / 255.0);
    // Alpha test. uAtestFailed: this pass draws the pixels that fail it
    // (AFAIL modes other than KEEP still write part of the pixel).
    if (uAtest != 1)
    {
        float a = floor(c.a + 0.5);
        bool pass = uAtest == 0 ? false :
                    uAtest == 2 ? a < float(uAref) :
                    uAtest == 3 ? a <= float(uAref) :
                    uAtest == 4 ? a == float(uAref) :
                    uAtest == 5 ? a >= float(uAref) :
                    uAtest == 6 ? a > float(uAref) : a != float(uAref);
        if (pass == (uAtestFailed != 0))
            discard;
    }
    oColor = vec4(c.rgb / 255.0, c.a / 128.0);
    gl_FragDepth = vDepth;
}
)";

    GLuint compile(GLenum type, const char *source)
    {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[2048];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            std::cerr << "[gs:hw] shader error: " << log << std::endl;
        }
        return shader;
    }

    struct HwVertex
    {
        float x, y, z;
        float s, t, q;
        uint8_t r, g, b, a;
        float fog;
    };

    // Everything a draw's GPU state depends on; consecutive primitives with an
    // equal key go into one draw call.
    struct DrawKey
    {
        uint32_t fbp, fbw, fpsm, fbmsk;
        uint32_t zbp, zpsm, zmask;
        uint64_t test, alpha, clamp, tex1;
        uint32_t tme, fst, abe, fge, tfx, tcc;
        uint64_t texture; // GL texture name | (render target flag << 32)
        uint32_t texW, texH;
        float texAlphaScale;
        uint32_t gsW, gsH;   // GS texture size (2^TW, 2^TH): STQ coordinates are relative to it
        int32_t alphaFix;    // >= 0: texture alpha replaced by this (CT24 read of a drawn target)
        uint32_t sx0, sy0, sx1, sy1;
        uint32_t fogColor;
        uint32_t linear;
        uint32_t colclamp;
        uint64_t tex0; // raw TEX0 (trace only; equal for equal draws)
        bool operator==(const DrawKey &o) const { return std::memcmp(this, &o, sizeof(DrawKey)) == 0; }
    };

    struct RenderTarget
    {
        uint32_t fbp = 0, fbw = 0, psm = 0;
        uint32_t width = 0, height = 0;
        GLuint texture = 0;
        bool gpuDirty = false;       // GPU content newer than GS memory
        uint64_t syncedEpoch = 0;    // GS memory epoch when last synced
        uint32_t usedRows = 0;       // rows ever drawn (extent in GS memory)
        uint32_t loadedRows = 0;     // rows loaded from GS memory into the texture
        uint32_t dirtyY0 = ~0u, dirtyY1 = 0; // rows drawn since the last write-back
    };

    // GS memory pages (8 KiB) a buffer rectangle's rows [y0, y1] cover, as bytes.
    void pageDims(uint32_t psm, uint32_t &pw, uint32_t &ph)
    {
        switch (psm)
        {
        case GS_PSM_CT16: case GS_PSM_CT16S: case GS_PSM_Z16: case GS_PSM_Z16S: pw = 64; ph = 64; break;
        case GS_PSM_T8: pw = 128; ph = 64; break;
        case GS_PSM_T4: pw = 128; ph = 128; break;
        default: pw = 64; ph = 32; break;
        }
    }

    void rowPages(uint32_t baseBytes, uint32_t bufferWidth, uint32_t y0, uint32_t y1, uint32_t psm, uint64_t &begin, uint64_t &end)
    {
        uint32_t pw = 64, ph = 32;
        pageDims(psm, pw, ph);
        const uint64_t perRow = std::max<uint32_t>(1u, (std::max<uint32_t>(1u, bufferWidth) + pw - 1u) / pw);
        const uint64_t basePage = baseBytes / 8192u;
        begin = (basePage + (y0 / ph) * perRow) * 8192u;
        end = (basePage + (y1 / ph + 1u) * perRow + ((baseBytes % 8192u) ? 1u : 0u)) * 8192u;
    }

    struct DepthTarget
    {
        uint32_t zbp = 0;
        uint32_t width = 0, height = 0;
        GLuint texture = 0;
    };
}

struct GSHwBackend::Impl
{
    std::unique_ptr<GSCpuBackend> shadow = std::make_unique<GSCpuBackend>();
    uint8_t *vram = nullptr;
    uint32_t vramSize = 0;

    bool glReady = false;
    bool glFailed = false;
    std::thread::id glThread;
    GLuint program = 0, vao = 0, vbo = 0;
    std::unordered_map<std::string, GLint> uniforms;
    std::unordered_map<uint32_t, RenderTarget> targets;  // by fbp
    std::unordered_map<uint32_t, DepthTarget> depths;    // by zbp
    std::unordered_map<uint64_t, GLuint> framebuffers;   // (fbp << 32 | zbp+1)
    struct CachedTexture
    {
        GLuint texture = 0;
        uint64_t epoch = ~0ull;
        uint32_t width = 0, height = 0;
    };
    std::unordered_map<const void *, CachedTexture> textures;

    std::vector<HwVertex> pending;
    DrawKey pendingKey{};
    bool hasPending = false;
    uint64_t drawCalls = 0, primitives = 0;
    // PS2X_GS_HW_STATS=1: per-second counters.
    struct Stats
    {
        uint64_t writeBacks = 0, writeBackPixels = 0, reloads = 0, textureUploads = 0, rtTextures = 0;
        double writeBackMs = 0, reloadMs = 0;
        uint64_t presentWb = 0, textureWb = 0, transferWb = 0, clutWb = 0, discards = 0;
    } stats;
    const char *wbReason = "other";
    // PS2X_GS_HW_TRACE=N: list the draw calls between presents N and N+1.
    uint64_t presents = 0, tracedDraws = 0;
    bool traceDone = false;
    bool tracing() const
    {
        static const long frame = std::getenv("PS2X_GS_HW_TRACE") ? std::atol(std::getenv("PS2X_GS_HW_TRACE")) : -1;
        return frame >= 0 && !traceDone && presents >= static_cast<uint64_t>(frame);
    }
    void reportStats()
    {
        static const bool on = std::getenv("PS2X_GS_HW_STATS") != nullptr;
        static auto last = std::chrono::steady_clock::now();
        if (!on)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last).count() < 1.0)
            return;
        last = now;
        std::fprintf(stderr, "[gs:hw] draws=%llu prims=%llu wb=%llu (%.0f ms, %llu px; present %llu texture %llu transfer %llu clut %llu) reloads=%llu (%.0f ms) texUploads=%llu rtTex=%llu discards=%llu\n",
                     (unsigned long long)drawCalls, (unsigned long long)primitives, (unsigned long long)stats.writeBacks, stats.writeBackMs,
                     (unsigned long long)stats.writeBackPixels, (unsigned long long)stats.presentWb, (unsigned long long)stats.textureWb,
                     (unsigned long long)stats.transferWb, (unsigned long long)stats.clutWb, (unsigned long long)stats.reloads, stats.reloadMs,
                     (unsigned long long)stats.textureUploads, (unsigned long long)stats.rtTextures, (unsigned long long)stats.discards);
        stats = {};
        drawCalls = primitives = 0;
    }

    bool onGlThread() const { return glReady && std::this_thread::get_id() == glThread; }

    bool ensureGl()
    {
        if (glReady)
            return std::this_thread::get_id() == glThread;
        if (glFailed || !g_makeCurrent)
            return false;
        g_makeCurrent();
        glThread = std::this_thread::get_id();
        GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
        GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            glFailed = true;
            std::cerr << "[gs:hw] program link failed" << std::endl;
            return false;
        }
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(HwVertex), reinterpret_cast<void *>(offsetof(HwVertex, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(HwVertex), reinterpret_cast<void *>(offsetof(HwVertex, s)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(HwVertex), reinterpret_cast<void *>(offsetof(HwVertex, r)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(HwVertex), reinterpret_cast<void *>(offsetof(HwVertex, fog)));
        glUseProgram(program);
        glUniform1i(uniform("uTexture"), 0);
        glReady = true;
        std::cerr << "[gs:hw] OpenGL renderer: " << reinterpret_cast<const char *>(glGetString(GL_RENDERER)) << std::endl;
        return true;
    }

    GLint uniform(const char *name)
    {
        auto it = uniforms.find(name);
        if (it != uniforms.end())
            return it->second;
        const GLint location = glGetUniformLocation(program, name);
        uniforms.emplace(name, location);
        return location;
    }

    static void bufferRange(uint32_t baseBytes, uint32_t widthPixels, uint32_t heightPixels, uint32_t psm, uint64_t &begin, uint64_t &end)
    {
        begin = baseBytes;
        end = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(widthPixels) * heightPixels * bytesPerPixel(psm);
    }

    // ---- GS memory <-> GPU ------------------------------------------------

    void targetRange(const RenderTarget &rt, uint64_t &begin, uint64_t &end)
    {
        rowPages(rt.fbp * 8192u, rt.width, 0u, std::max(rt.usedRows, 1u) - 1u, rt.psm, begin, end);
    }

    void writeBack(RenderTarget &rt)
    {
        if (!rt.gpuDirty || !vram)
            return;
        flushPending();
        const auto t0 = std::chrono::steady_clock::now();
        ++stats.writeBacks;
        if (!std::strcmp(wbReason, "present")) ++stats.presentWb;
        else if (!std::strcmp(wbReason, "texture")) ++stats.textureWb;
        else if (!std::strcmp(wbReason, "transfer")) ++stats.transferWb;
        else if (!std::strcmp(wbReason, "clut")) ++stats.clutWb;
        const uint32_t y0 = std::min(rt.dirtyY0, rt.height - 1u);
        const uint32_t y1 = std::min(rt.dirtyY1, rt.height - 1u);
        const uint32_t rows = y1 >= y0 ? y1 - y0 + 1u : 0u;
        stats.writeBackPixels += static_cast<uint64_t>(rt.width) * rows;
        std::vector<uint8_t> pixels(static_cast<size_t>(rt.width) * std::max(rows, 1u) * 4u);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer(rt, nullptr));
        glReadPixels(0, static_cast<GLint>(y0), static_cast<GLsizei>(rt.width), static_cast<GLsizei>(rows), GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        {
        std::lock_guard<std::mutex> lock(shadow->vramMutex());
        for (uint32_t r = 0; r < rows; ++r)
        {
            const uint32_t y = y0 + r;
            const uint8_t *row = pixels.data() + static_cast<size_t>(r) * rt.width * 4u;
            for (uint32_t x = 0; x < rt.width; ++x)
            {
                const uint8_t *p = row + x * 4u;
                const uint32_t alpha = std::min(255u, (static_cast<uint32_t>(p[3]) * 128u + 127u) / 255u);
                uint32_t value = p[0] | (p[1] << 8) | (p[2] << 16) | (alpha << 24);
                if (rt.psm == GS_PSM_CT16 || rt.psm == GS_PSM_CT16S)
                    value = (p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10) | (alpha >= 0x40 ? 0x8000u : 0u);
                shadow->WriteVramUnlocked(rt.psm, rt.fbp * 32u, rt.fbw, x, y, value);
            }
        }
        }
        rt.dirtyY0 = ~0u;
        rt.dirtyY1 = 0;
        uint64_t begin = 0, end = 0;
        targetRange(rt, begin, end);
        shadow->noteExternalWrite(begin, end);
        rt.syncedEpoch = shadow->newestWrite(begin, end);
        rt.gpuDirty = false;
        stats.writeBackMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    void reloadFromVram(RenderTarget &rt, uint32_t firstRow = 0, uint32_t endRow = ~0u)
    {
        const auto t0 = std::chrono::steady_clock::now();
        ++stats.reloads;
        if (endRow == ~0u)
            endRow = std::max(rt.loadedRows, rt.usedRows);
        endRow = std::min(endRow, rt.height);
        if (endRow <= firstRow)
            return;
        const uint32_t rows = endRow - firstRow;
        std::vector<uint8_t> pixels(static_cast<size_t>(rt.width) * rows * 4u);
        std::unique_lock<std::mutex> lock(shadow->vramMutex());
        for (uint32_t r = 0; r < rows; ++r)
            for (uint32_t x = 0; x < rt.width; ++x)
            {
                const uint32_t y = firstRow + r;
                uint32_t v = shadow->ReadVramUnlocked(rt.psm, rt.fbp * 32u, rt.fbw, x, y);
                uint8_t *p = pixels.data() + (static_cast<size_t>(r) * rt.width + x) * 4u;
                if (rt.psm == GS_PSM_CT16 || rt.psm == GS_PSM_CT16S)
                {
                    p[0] = static_cast<uint8_t>((v & 0x1Fu) << 3);
                    p[1] = static_cast<uint8_t>(((v >> 5) & 0x1Fu) << 3);
                    p[2] = static_cast<uint8_t>(((v >> 10) & 0x1Fu) << 3);
                    v = (v & 0x8000u) ? 0x80000000u : 0u;
                }
                else
                {
                    p[0] = static_cast<uint8_t>(v);
                    p[1] = static_cast<uint8_t>(v >> 8);
                    p[2] = static_cast<uint8_t>(v >> 16);
                }
                const uint32_t raw = rt.psm == GS_PSM_CT24 ? 0x80u : (v >> 24);
                p[3] = static_cast<uint8_t>(std::min(255u, raw * 255u / 128u));
            }
        lock.unlock();
        glBindTexture(GL_TEXTURE_2D, rt.texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(firstRow), rt.width, rows, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        rt.loadedRows = std::max(rt.loadedRows, endRow);
        uint64_t begin = 0, end = 0;
        targetRange(rt, begin, end);
        rt.syncedEpoch = shadow->newestWrite(begin, end);
        stats.reloadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    // CPU-side users of [begin, end): bring GPU results into GS memory first.
    // GS memory the GPU drew and the CPU has not seen yet.
    bool dirtyOverlaps(RenderTarget &rt, uint64_t begin, uint64_t end)
    {
        if (!rt.gpuDirty || rt.dirtyY1 < rt.dirtyY0 || rt.dirtyY0 == ~0u)
            return false;
        uint64_t b = 0, e = 0;
        rowPages(rt.fbp * 8192u, rt.width, rt.dirtyY0, rt.dirtyY1, rt.psm, b, e);
        return begin < e && b < end;
    }

    void writeBackOverlapping(uint64_t begin, uint64_t end)
    {
        for (auto &[fbp, rt] : targets)
            if (dirtyOverlaps(rt, begin, end))
                writeBack(rt);
    }

    // The CPU overwrites [begin, end) (an upload): drawn targets there are
    // replaced, not merged; their rows reload from GS memory when drawn again.
    void discardOverlapping(uint64_t begin, uint64_t end, const GSTransferCommand &command)
    {
        const auto &b = command.bitbltbuf;
        for (auto &[fbp, rt] : targets)
        {
            if (!dirtyOverlaps(rt, begin, end))
                continue;
            const bool covers = b.dbp == rt.fbp * 32u && b.dpsm == rt.psm && b.dbw == rt.fbw &&
                                command.trxpos.dsax == 0u && command.trxreg.rrw >= rt.width &&
                                command.trxpos.dsay <= rt.dirtyY0 && command.trxpos.dsay + command.trxreg.rrh > rt.dirtyY1;
            if (!covers)
            {
                writeBack(rt); // a partial overwrite keeps the rest of the drawing
                continue;
            }
            ++stats.discards;
            rt.gpuDirty = false;
            rt.dirtyY0 = ~0u;
            rt.dirtyY1 = 0;
            rt.loadedRows = 0;
        }
    }

    RenderTarget &target(uint32_t fbp, uint32_t fbw, uint32_t psm, uint32_t neededHeight)
    {
        RenderTarget &rt = targets[fbp];
        const uint32_t width = std::max<uint32_t>(1u, fbw) * 64u;
        const uint32_t height = std::max<uint32_t>(rt.height, std::min<uint32_t>(2048u, (neededHeight + 63u) & ~63u));
        if (rt.texture == 0 || rt.width != width || rt.height < height || rt.psm != psm)
        {
            if (rt.texture != 0)
            {
                writeBack(rt);
                glDeleteTextures(1, &rt.texture);
                for (auto it = framebuffers.begin(); it != framebuffers.end();)
                {
                    if ((it->first >> 32) == fbp)
                    {
                        glDeleteFramebuffers(1, &it->second);
                        it = framebuffers.erase(it);
                    }
                    else
                        ++it;
                }
            }
            rt.fbp = fbp;
            rt.fbw = std::max<uint32_t>(1u, fbw);
            rt.psm = psm;
            rt.width = width;
            rt.height = std::max<uint32_t>(height, 64u);
            glGenTextures(1, &rt.texture);
            glBindTexture(GL_TEXTURE_2D, rt.texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rt.width, rt.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            rt.usedRows = rt.loadedRows = 0; // rows load from GS memory as draws reach them
            rt.gpuDirty = false;
        }
        else
        {
            uint64_t begin = 0, end = 0;
            targetRange(rt, begin, end);
            if (!rt.gpuDirty && shadow->newestWrite(begin, end) > rt.syncedEpoch)
                reloadFromVram(rt); // the CPU wrote into it (uploads, transfers)
        }
        return rt;
    }

    DepthTarget &depth(uint32_t zbp, uint32_t width, uint32_t height)
    {
        DepthTarget &d = depths[zbp];
        if (d.texture == 0 || d.width < width || d.height < height)
        {
            if (d.texture)
                glDeleteTextures(1, &d.texture);
            d.zbp = zbp;
            d.width = std::max(d.width, width);
            d.height = std::max(d.height, height);
            glGenTextures(1, &d.texture);
            glBindTexture(GL_TEXTURE_2D, d.texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, d.width, d.height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
            for (auto it = framebuffers.begin(); it != framebuffers.end();)
            {
                if ((it->first & 0xFFFFFFFFu) == zbp + 1u)
                {
                    glDeleteFramebuffers(1, &it->second);
                    it = framebuffers.erase(it);
                }
                else
                    ++it;
            }
        }
        return d;
    }

    GLuint framebuffer(RenderTarget &rt, DepthTarget *d)
    {
        const uint64_t key = (static_cast<uint64_t>(rt.fbp) << 32) | (d ? d->zbp + 1u : 0u);
        auto it = framebuffers.find(key);
        if (it != framebuffers.end())
            return it->second;
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.texture, 0);
        if (d)
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, d->texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            std::cerr << "[gs:hw] incomplete framebuffer fbp=" << rt.fbp << std::endl;
        framebuffers.emplace(key, fbo);
        return fbo;
    }

    // ---- Textures -----------------------------------------------------------

    // Returns the GL texture for the draw (0: none) and its alpha scale.
    GLuint textureFor(const GSDrawState &state, float &alphaScale, uint32_t &texW, uint32_t &texH, int32_t &alphaFix)
    {
        const auto &tex = state.context.tex0;
        texW = state.textureWidth;
        texH = state.textureHeight;
        alphaFix = -1;
        // A texture at the start of a render target the GPU drew: sample it there.
        for (auto &[fbp, rt] : targets)
        {
            const bool sameFormat = tex.psm == rt.psm || (tex.psm == GS_PSM_CT24 && rt.psm == GS_PSM_CT32);
            if (tex.tbp0 == fbp * 32u && sameFormat && rt.texture && tex.tbw * 64u == rt.width && rt.gpuDirty)
            {
                alphaScale = kAlphaOne / 255.0f; // targets keep alpha as A / 0x80
                if (tex.psm == GS_PSM_CT24)
                    alphaFix = state.texa.ta0; // (AEM's black-is-transparent not applied)
                texW = rt.width;
                texH = rt.height;
                ++stats.rtTextures;
                return rt.texture;
            }
        }
        // Otherwise decode from GS memory, after the GPU's results reach it.
        uint64_t begin = 0, end = 0;
        rowPages(tex.tbp0 * 256u, std::max<uint32_t>(state.textureWidth, tex.tbw * 64u), 0u, state.textureHeight - 1u, tex.psm, begin, end);
        wbReason = "texture";
        writeBackOverlapping(begin, end);
        wbReason = "other";
        uint32_t w = 0, h = 0;
        uint64_t epoch = 0;
        const std::vector<uint32_t> *texels = shadow->textureForHw(state, w, h, epoch);
        if (!texels || w == 0 || h == 0)
            return 0;
        CachedTexture &cached = textures[texels];
        if (cached.texture == 0)
            glGenTextures(1, &cached.texture);
        glBindTexture(GL_TEXTURE_2D, cached.texture);
        if (cached.epoch != epoch || cached.width != w || cached.height != h)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels->data());
            ++stats.textureUploads;
            cached.epoch = epoch;
            cached.width = w;
            cached.height = h;
        }
        alphaScale = 1.0f; // decoded texels carry the GS alpha as is
        texW = w;
        texH = h;
        return cached.texture;
    }

    // ---- Drawing -------------------------------------------------------------

    void applyBlend(const DrawKey &key)
    {
        if (!key.abe)
        {
            glDisable(GL_BLEND);
            return;
        }
        // (A - B) * C + D with A, B, D in {Cs, Cd, 0} and C in {As, Ad, FIX}.
        const uint32_t a = key.alpha & 3u, b = (key.alpha >> 2) & 3u, c = (key.alpha >> 4) & 3u, d = (key.alpha >> 6) & 3u;
        const GLenum cf = c == 0 ? GL_SRC_ALPHA : c == 1 ? GL_DST_ALPHA : GL_CONSTANT_ALPHA;
        const GLenum cinv = c == 0 ? GL_ONE_MINUS_SRC_ALPHA : c == 1 ? GL_ONE_MINUS_DST_ALPHA : GL_ONE_MINUS_CONSTANT_ALPHA;
        if (c == 2)
            glBlendColor(0, 0, 0, static_cast<float>((key.alpha >> 32) & 0xFFu) / kAlphaOne);
        glEnable(GL_BLEND);
        GLenum eq = GL_FUNC_ADD, src = GL_ONE, dst = GL_ZERO;
        enum { CS = 0, CD = 1, ZERO = 2 };
        if (a == CS && b == CD && d == CD) { src = cf; dst = cinv; }
        else if (a == CS && b == ZERO && d == CD) { src = cf; dst = GL_ONE; }
        else if (a == CS && b == ZERO && d == ZERO) { src = cf; dst = GL_ZERO; }
        else if (a == CS && b == CD && d == ZERO) { eq = GL_FUNC_SUBTRACT; src = cf; dst = cf; }
        else if (a == CD && b == CS && d == CS) { src = cinv; dst = cf; }
        else if (a == ZERO && b == CS && d == CD) { eq = GL_FUNC_REVERSE_SUBTRACT; src = cf; dst = GL_ONE; }
        else if (a == CD && b == ZERO && d == CS) { src = GL_ONE; dst = cf; }
        else if (a == CD && b == ZERO && d == ZERO) { src = GL_ZERO; dst = cf; }
        else if (a == b) { src = d == CS ? GL_ONE : GL_ZERO; dst = d == CD ? GL_ONE : GL_ZERO; }
        else if (a == CD && b == CS && d == CD) { eq = GL_FUNC_REVERSE_SUBTRACT; src = cf; dst = GL_ONE; } // approx: Cd - Cs*C
        else { src = cf; dst = cinv; }
        glBlendEquationSeparate(eq, GL_FUNC_ADD);
        // The frame buffer keeps the source alpha (the GS does not blend alpha).
        glBlendFuncSeparate(src, dst, GL_ONE, GL_ZERO);
    }

    void flushPending()
    {
        if (!hasPending || pending.empty())
        {
            pending.clear();
            hasPending = false;
            return;
        }
        const DrawKey &key = pendingKey;
        RenderTarget &rt = targets[key.fbp];
        const uint32_t ztst = (key.test >> 17) & 3u;
        const bool zte = ((key.test >> 16) & 1u) != 0u;
        const bool useDepth = zte && ztst != 1u ? true : (zte && !key.zmask);
        DepthTarget *d = useDepth ? &depth(key.zbp, rt.width, rt.height) : nullptr;
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer(rt, d));
        glViewport(0, 0, rt.width, rt.height);
        glEnable(GL_SCISSOR_TEST);
        glScissor(key.sx0, key.sy0, key.sx1 >= key.sx0 ? key.sx1 - key.sx0 + 1 : 0, key.sy1 >= key.sy0 ? key.sy1 - key.sy0 + 1 : 0);
        if (d)
        {
            glEnable(GL_DEPTH_TEST);
            static const GLenum funcs[4] = {GL_NEVER, GL_ALWAYS, GL_GEQUAL, GL_GREATER};
            glDepthFunc(funcs[ztst]);
        }
        else
            glDisable(GL_DEPTH_TEST);
        const uint32_t m = key.fbmsk;
        const bool writeR = (m & 0xFFu) != 0xFFu, writeG = (m & 0xFF00u) != 0xFF00u, writeB = (m & 0xFF0000u) != 0xFF0000u;
        const bool writeA = key.fpsm != GS_PSM_CT24 && (m & 0xFF000000u) != 0xFF000000u;
        const bool writeZ = d && !key.zmask;
        applyBlend(key);
        glUseProgram(program);
        glUniform2f(uniform("uTarget"), static_cast<float>(rt.width), static_cast<float>(rt.height));
        glUniform1i(uniform("uTme"), key.tme && key.texture ? 1 : 0);
        glUniform1i(uniform("uTfx"), static_cast<GLint>(key.tfx));
        glUniform1i(uniform("uTcc"), static_cast<GLint>(key.tcc));
        glUniform1i(uniform("uFst"), static_cast<GLint>(key.fst));
        glUniform1i(uniform("uFge"), static_cast<GLint>(key.fge));
        const bool ate = (key.test & 1u) != 0u;
        const uint32_t atst = ate ? static_cast<uint32_t>((key.test >> 1) & 7u) : 1u;
        const uint32_t afail = static_cast<uint32_t>((key.test >> 12) & 3u);
        glUniform1i(uniform("uAtest"), static_cast<GLint>(atst));
        glUniform1i(uniform("uAref"), static_cast<GLint>((key.test >> 4) & 0xFFu));
        glUniform2f(uniform("uTexSize"), static_cast<float>(key.texW), static_cast<float>(key.texH));
        glUniform1f(uniform("uTexAlphaScale"), key.texAlphaScale);
        glUniform1f(uniform("uAlphaFix"), key.tme ? static_cast<float>(key.alphaFix) : -1.0f);
        glUniform2f(uniform("uUvScale"), key.texW ? static_cast<float>(key.gsW) / key.texW : 1.0f,
                    key.texH ? static_cast<float>(key.gsH) / key.texH : 1.0f);
        glUniform3f(uniform("uFogColor"), static_cast<float>(key.fogColor & 0xFFu), static_cast<float>((key.fogColor >> 8) & 0xFFu),
                    static_cast<float>((key.fogColor >> 16) & 0xFFu));
        if (key.tme && key.texture)
        {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(key.texture));
            const GLint filter = key.linear ? GL_LINEAR : GL_NEAREST;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
            const uint32_t wms = key.clamp & 3u, wmt = (key.clamp >> 2) & 3u;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wms == 0 ? GL_REPEAT : GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wmt == 0 ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        }
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, pending.size() * sizeof(HwVertex), pending.data(), GL_STREAM_DRAW);
        if (tracing())
        {
            ++tracedDraws;
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (const auto &v : pending)
            {
                x0 = std::min(x0, v.x); y0 = std::min(y0, v.y); x1 = std::max(x1, v.x); y1 = std::max(y1, v.y);
            }
            std::fprintf(stderr, "[gs:trace] fbp=%u fbw=%u fpsm=%u fbmsk=%08x zbp=%u zmsk=%u test=%llx abe=%u alpha=%llx tme=%u tex=%llx tfx=%u tcc=%u fst=%u fge=%u gs=%ux%u bound=%ux%u afix=%d "
                         "verts=%zu bbox=(%.0f,%.0f)-(%.0f,%.0f) scissor=(%u,%u)-(%u,%u) c0=%02x%02x%02x%02x z0=%.9g zpsm=%u\n",
                         key.fbp, key.fbw, key.fpsm, key.fbmsk, key.zbp, key.zmask, (unsigned long long)key.test, key.abe, (unsigned long long)key.alpha,
                         key.tme, (unsigned long long)key.tex0, key.tfx, key.tcc, key.fst, key.fge, key.gsW, key.gsH, key.texW, key.texH, key.alphaFix,
                         pending.size(), x0, y0, x1, y1, key.sx0, key.sy0, key.sx1, key.sy1, pending[0].r, pending[0].g, pending[0].b, pending[0].a, pending[0].z, key.zpsm);
        }
        // Pass 1: pixels that pass the alpha test write everything enabled.
        // Pass 2 (AFAIL != KEEP): failing pixels write FB only, Z only, or RGB only.
        const GLsizei count = static_cast<GLsizei>(pending.size());
        if (atst != 0u)
        {
            glUniform1i(uniform("uAtestFailed"), 0);
            glColorMask(writeR, writeG, writeB, writeA);
            if (d)
                glDepthMask(writeZ ? GL_TRUE : GL_FALSE);
            glDrawArrays(GL_TRIANGLES, 0, count);
            ++drawCalls;
        }
        if (atst != 1u && afail != 0u)
        {
            const bool failColor = afail != 2u, failZ = afail == 2u && writeZ;
            if (failColor || failZ)
            {
                glUniform1i(uniform("uAtestFailed"), 1);
                glColorMask(failColor && writeR, failColor && writeG, failColor && writeB, afail == 1u && writeA);
                if (d)
                    glDepthMask(failZ ? GL_TRUE : GL_FALSE);
                glDrawArrays(GL_TRIANGLES, 0, count);
                ++drawCalls;
            }
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        rt.gpuDirty = true;
        pending.clear();
        hasPending = false;
    }

    void submit(const GSPrimitiveBatch &batch)
    {
        if (!ensureGl())
            return;
        const GSDrawState &state = batch.state;
        const GSContext &ctx = state.context;
        const auto type = state.prim.type;
        if (!isColorPsm(ctx.frame.psm))
            return; // drawing into Z or texture formats: not handled yet

        const float ofx = ctx.xyoffset.ofx / 16.0f, ofy = ctx.xyoffset.ofy / 16.0f;
        RenderTarget &rt = target(ctx.frame.fbp, ctx.frame.fbw, ctx.frame.psm, static_cast<uint32_t>(ctx.scissor.y1) + 1u);

        DrawKey key{};
        key.fbp = ctx.frame.fbp;
        key.fbw = ctx.frame.fbw;
        key.fpsm = ctx.frame.psm;
        key.fbmsk = ctx.frame.fbmsk;
        key.zbp = ctx.zbuf.zbp;
        key.zpsm = ctx.zbuf.psm;
        key.zmask = ctx.zbuf.zmask;
        key.test = ctx.test;
        key.alpha = ctx.alpha;
        key.clamp = ctx.clamp;
        key.tme = state.prim.tme;
        key.fst = state.prim.fst;
        key.abe = state.prim.abe;
        key.fge = state.prim.fge;
        key.tfx = ctx.tex0.tfx;
        key.tcc = ctx.tex0.tcc;
        key.sx0 = ctx.scissor.x0;
        key.sy0 = ctx.scissor.y0;
        key.sx1 = std::min<uint32_t>(ctx.scissor.x1, rt.width - 1u);
        key.sy1 = std::min<uint32_t>(ctx.scissor.y1, rt.height - 1u);
        key.fogColor = state.fogR | (state.fogG << 8) | (state.fogB << 16);
        key.linear = state.linearFilter;
        key.texAlphaScale = 1.0f;
        if (key.tme)
        {
            flushPendingIfTextureChanges(state);
            float alphaScale = 1.0f;
            uint32_t tw = 0, th = 0;
            int32_t alphaFix = -1;
            key.texture = textureFor(state, alphaScale, tw, th, alphaFix);
            key.alphaFix = alphaFix;
            key.tex0 = (static_cast<uint64_t>(ctx.tex0.tbp0)) | (static_cast<uint64_t>(ctx.tex0.tbw) << 14) | (static_cast<uint64_t>(ctx.tex0.psm) << 20) |
                       (static_cast<uint64_t>(ctx.tex0.cbp) << 37);
            key.gsW = state.textureWidth;
            key.gsH = state.textureHeight;
            key.texAlphaScale = alphaScale;
            key.texW = tw;
            key.texH = th;
        }
        if (hasPending && !(key == pendingKey))
            flushPending();
        pendingKey = key;
        hasPending = true;

        const float depthScale = (ctx.zbuf.psm == GS_PSM_Z16 || ctx.zbuf.psm == GS_PSM_Z16S) ? 65536.0f
                                 : ctx.zbuf.psm == GS_PSM_Z24 ? 16777216.0f : 4294967296.0f;
        auto make = [&](const GSVertex &v)
        {
            HwVertex o{};
            o.x = v.x - ofx + 0.5f;
            o.y = v.y - ofy + 0.5f;
            o.z = static_cast<float>(std::min(1.0, v.z / depthScale));
            if (state.prim.fst)
            {
                o.s = v.u / 16.0f;
                o.t = v.v / 16.0f;
                o.q = 1.0f;
            }
            else
            {
                o.s = v.s;
                o.t = v.t;
                o.q = v.q;
            }
            o.r = v.r;
            o.g = v.g;
            o.b = v.b;
            o.a = v.a;
            o.fog = v.fog;
            return o;
        };
        auto flatten = [&](HwVertex v, const GSVertex &last)
        {
            if (!state.prim.iip)
            {
                v.r = last.r;
                v.g = last.g;
                v.b = last.b;
                v.a = last.a;
            }
            return v;
        };
        const GSVertex *vs = batch.vertices.data();
        {
            // Rows this primitive can touch: load them from GS memory the first
            // time, and remember them for write-back.
            float ymin = 1e30f, ymax = -1e30f;
            for (int i = 0; i < std::max<int>(1, batch.vertexCount); ++i)
            {
                ymin = std::min(ymin, vs[i].y - ofy);
                ymax = std::max(ymax, vs[i].y - ofy);
            }
            const int lo = std::max<int>(static_cast<int>(key.sy0), static_cast<int>(std::floor(ymin)) - 1);
            const int hi = std::min<int>(static_cast<int>(key.sy1), static_cast<int>(std::ceil(ymax)) + 1);
            if (hi < lo)
                return;
            if (static_cast<uint32_t>(hi) + 1u > rt.loadedRows)
            {
                flushPending();
                reloadFromVram(rt, rt.loadedRows, static_cast<uint32_t>(hi) + 1u);
                pendingKey = key;
                hasPending = true;
            }
            rt.usedRows = std::max(rt.usedRows, static_cast<uint32_t>(hi) + 1u);
            rt.dirtyY0 = std::min(rt.dirtyY0, static_cast<uint32_t>(lo));
            rt.dirtyY1 = std::max(rt.dirtyY1, static_cast<uint32_t>(hi));
        }
        if ((type == GS_PRIM_TRIANGLE || type == GS_PRIM_TRISTRIP || type == GS_PRIM_TRIFAN) && batch.vertexCount >= 3)
        {
            for (int i = 0; i < 3; ++i)
                pending.push_back(flatten(make(vs[i]), vs[2]));
        }
        else if (type == GS_PRIM_SPRITE && batch.vertexCount >= 2)
        {
            HwVertex a = make(vs[0]), b = make(vs[1]);
            a.r = b.r; a.g = b.g; a.b = b.b; a.a = b.a; a.z = b.z; a.fog = b.fog;
            a.q = b.q;
            HwVertex c = a, d = a;
            c.x = b.x; c.s = b.s;          // top right
            d.y = b.y; d.t = b.t;          // bottom left
            pending.insert(pending.end(), {a, c, b, a, b, d});
        }
        else if ((type == GS_PRIM_LINE || type == GS_PRIM_LINESTRIP) && batch.vertexCount >= 2)
        {
            // Lines as one-pixel quads.
            HwVertex a = make(vs[0]), b = flatten(make(vs[1]), vs[1]);
            a = flatten(a, vs[1]);
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float len = std::max(1e-6f, std::sqrt(dx * dx + dy * dy));
            const float nx = -dy / len * 0.5f, ny = dx / len * 0.5f;
            HwVertex a0 = a, a1 = a, b0 = b, b1 = b;
            a0.x += nx; a0.y += ny; a1.x -= nx; a1.y -= ny;
            b0.x += nx; b0.y += ny; b1.x -= nx; b1.y -= ny;
            pending.insert(pending.end(), {a0, b0, b1, a0, b1, a1});
        }
        else if (type == GS_PRIM_POINT && batch.vertexCount >= 1)
        {
            HwVertex p = make(vs[0]);
            HwVertex a = p, b = p, c = p, d = p;
            a.x -= 0.5f; a.y -= 0.5f; b.x += 0.5f; b.y -= 0.5f; c.x += 0.5f; c.y += 0.5f; d.x -= 0.5f; d.y += 0.5f;
            pending.insert(pending.end(), {a, b, c, a, c, d});
        }
        ++primitives;
        if (pending.size() > 60000)
            flushPending();
    }

    // A draw sampling a render target the queued draws write: draw those first.
    void flushPendingIfTextureChanges(const GSDrawState &state)
    {
        if (hasPending && state.context.tex0.tbp0 == pendingKey.fbp * 32u)
            flushPending();
    }
};

GSHwBackend::GSHwBackend() : m_impl(std::make_unique<Impl>()) {}
GSHwBackend::~GSHwBackend() = default;

void GSHwBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    m_impl->vram = vram;
    m_impl->vramSize = vramSize;
    m_impl->shadow->Initialize(vram, vramSize);
}

void GSHwBackend::Reset() { m_impl->shadow->Reset(); }

void GSHwBackend::Submit(const GSPrimitiveBatch &batch) { m_impl->submit(batch); }

void GSHwBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    if (m_impl->onGlThread())
    {
        // The CLUT comes from GS memory, which the GPU may have drawn.
        m_impl->flushPending();
        m_impl->wbReason = "clut";
        m_impl->writeBackOverlapping(tex0.cbp * 256u, tex0.cbp * 256u + 1024u);
        m_impl->wbReason = "other";
    }
    m_impl->shadow->LoadClut(tex0, texclut);
}

void GSHwBackend::BeginTransfer(const GSTransferCommand &command)
{
    if (m_impl->onGlThread())
    {
        m_impl->flushPending();
        // Uploads and copies go through GS memory: make the GPU's results land
        // there first so partial overwrites keep the rest.
        uint64_t begin = 0, end = 0;
        const auto &b = command.bitbltbuf;
        const uint32_t rows = std::max<uint32_t>(1u, command.trxreg.rrh);
        rowPages(b.dbp * 256u, std::max<uint32_t>(1u, b.dbw) * 64u, command.trxpos.dsay, command.trxpos.dsay + rows - 1u, b.dpsm, begin, end);
        m_impl->wbReason = "transfer";
        if (command.direction == 0u)
            m_impl->discardOverlapping(begin, end, command); // host -> GS upload
        else
            m_impl->writeBackOverlapping(begin, end);
        if (command.direction != 0u)
        {
            rowPages(b.sbp * 256u, std::max<uint32_t>(1u, b.sbw) * 64u, command.trxpos.ssay, command.trxpos.ssay + rows - 1u, b.spsm, begin, end);
            m_impl->writeBackOverlapping(begin, end);
        }
        m_impl->wbReason = "other";
    }
    m_impl->shadow->BeginTransfer(command);
}

void GSHwBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes) { m_impl->shadow->UploadImage(data, sizeBytes); }

void GSHwBackend::Flush()
{
    if (m_impl->onGlThread())
    {
        m_impl->flushPending();
        glFlush();
    }
}

void GSHwBackend::TextureFlush()
{
    if (m_impl->onGlThread())
        m_impl->flushPending();
    m_impl->shadow->TextureFlush();
}

void GSHwBackend::Sync(GSSyncReason reason) { m_impl->shadow->Sync(reason); }

PresentationFrame GSHwBackend::Present(const GSPresentationRequest &request)
{
    if (m_impl->onGlThread())
    {
        m_impl->flushPending();
        // The display circuits read GS memory: bring the displayed targets there.
        for (const uint64_t dispfb : {request.dispfb1, request.dispfb2})
        {
            const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
            auto it = m_impl->targets.find(fbp);
            m_impl->wbReason = "present";
            if (it != m_impl->targets.end())
                m_impl->writeBack(it->second);
            m_impl->wbReason = "other";
        }
        m_impl->reportStats();
        if (m_impl->tracing())
            std::fprintf(stderr, "[gs:trace] present pmode=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx\n", (unsigned long long)request.pmode, (unsigned long long)request.dispfb1, (unsigned long long)request.display1, (unsigned long long)request.dispfb2, (unsigned long long)request.display2);
        if (m_impl->tracing() && m_impl->tracedDraws > 0)
            m_impl->traceDone = true; // one whole frame with draws
        ++m_impl->presents;
    }
    return m_impl->shadow->Present(request);
}

bool GSHwBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba) { return m_impl->shadow->ClearFramebuffer(context, rgba); }
uint32_t GSHwBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) { return m_impl->shadow->ConsumeLocalToHostBytes(dst, maxBytes); }
uint32_t GSHwBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const { return m_impl->shadow->ReadVram(psm, base, bw, x, y); }
void GSHwBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) { m_impl->shadow->WriteVram(psm, base, bw, x, y, value); }
void GSHwBackend::SnapshotVram(std::vector<uint8_t> &out) const { m_impl->shadow->SnapshotVram(out); }
GSTransferSnapshot GSHwBackend::GetTransferSnapshot() const { return m_impl->shadow->GetTransferSnapshot(); }

#else // no GL on this platform: forward everything to the CPU backend

struct GSHwBackend::Impl
{
    std::unique_ptr<GSCpuBackend> shadow = std::make_unique<GSCpuBackend>();
};
GSHwBackend::GSHwBackend() : m_impl(std::make_unique<Impl>()) {}
GSHwBackend::~GSHwBackend() = default;
void GSHwBackend::Initialize(uint8_t *vram, uint32_t vramSize) { m_impl->shadow->Initialize(vram, vramSize); }
void GSHwBackend::Reset() { m_impl->shadow->Reset(); }
void GSHwBackend::Submit(const GSPrimitiveBatch &batch) { m_impl->shadow->Submit(batch); }
void GSHwBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) { m_impl->shadow->LoadClut(tex0, texclut); }
void GSHwBackend::BeginTransfer(const GSTransferCommand &command) { m_impl->shadow->BeginTransfer(command); }
void GSHwBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes) { m_impl->shadow->UploadImage(data, sizeBytes); }
void GSHwBackend::Flush() { m_impl->shadow->Flush(); }
void GSHwBackend::TextureFlush() { m_impl->shadow->TextureFlush(); }
void GSHwBackend::Sync(GSSyncReason reason) { m_impl->shadow->Sync(reason); }
PresentationFrame GSHwBackend::Present(const GSPresentationRequest &request) { return m_impl->shadow->Present(request); }
bool GSHwBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba) { return m_impl->shadow->ClearFramebuffer(context, rgba); }
uint32_t GSHwBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) { return m_impl->shadow->ConsumeLocalToHostBytes(dst, maxBytes); }
uint32_t GSHwBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const { return m_impl->shadow->ReadVram(psm, base, bw, x, y); }
void GSHwBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) { m_impl->shadow->WriteVram(psm, base, bw, x, y, value); }
void GSHwBackend::SnapshotVram(std::vector<uint8_t> &out) const { m_impl->shadow->SnapshotVram(out); }
GSTransferSnapshot GSHwBackend::GetTransferSnapshot() const { return m_impl->shadow->GetTransferSnapshot(); }

#endif
