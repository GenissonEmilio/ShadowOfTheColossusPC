// Shadow of the Colossus: models drawn by the native renderer
// (runtime/native_renderer.h) straight from the game's own model data, read
// at modelDisp (docs/native_renderer_map.md). SOTC_NATIVE=1 shows that
// picture instead of the emulated one; the emulated drawing still runs.
//
// What is read: the model header (packets at +0x60/+0x64), the instance's
// LOD (bits 40-42 of the 64-bit word at instance + 0x250), the part
// visibility the clip left (LOD table at header + 0x30, part x 0x30 + 0x2c),
// and per packet its world batches (VIF: GIF tag, V3-32 positions, V2-16 ST,
// V4-8 colours, each batch ending in MSCNT; triangle strips). The matrix is
// camera (camera context + 0x5a0) x model (modelDisp's second argument).
// Textures: the surface's index (+0x2c) into the model's table (+0x40,
// 0x20 each; +0x10 the texture object, whose +0x20 is the "NTO2" header:
// +0x14 pixels, +0x18 CLUT, +0x1c format | log2 width << 16 | log2 height
// << 20; byte +0x1f bit 0: level 0 uploaded as PSMCT32, W/2 x H/2 (8-bit)
// or W/2 x H/4 (4-bit), so the pixels are in GS block order).
// Not yet: lighting (lit batches carry no colour), materials, skinned
// characters, the second texture and mip levels.

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/native_renderer.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/ps2_stats.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace sotc_native
{
    constexpr uint32_t kModelDisp = 0x01097118u, kGetCameraContext = 0x0108F518u;
    PS2Runtime::RecompiledFunction g_modelDisp = nullptr, g_getCameraContext = nullptr;
    uint32_t g_cameraContext = 0;
    uint64_t g_frame = 0;
    uint32_t g_frameDraws = 0;

    const uint8_t *g_ram = nullptr;
    uint32_t word(uint32_t addr)
    {
        uint32_t v;
        std::memcpy(&v, g_ram + (addr & 0x1FFFFFCu), 4);
        return v;
    }
    float real(uint32_t addr)
    {
        float v;
        std::memcpy(&v, g_ram + (addr & 0x1FFFFFCu), 4);
        return v;
    }

    // The packet's world batches as one triangle list (strips unrolled).
    void buildMesh(uint32_t addr, uint32_t size, std::vector<native_renderer::Vertex> &out)
    {
        struct In
        {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        std::vector<In> strip;
        auto flush = [&]
        {
            for (size_t i = 2; i < strip.size(); ++i)
            {
                const In *t[3] = {&strip[i - 2], &strip[i - 1], &strip[i]};
                const float ux = t[1]->x - t[0]->x, uy = t[1]->y - t[0]->y, uz = t[1]->z - t[0]->z;
                const float vx = t[2]->x - t[0]->x, vy = t[2]->y - t[0]->y, vz = t[2]->z - t[0]->z;
                float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (len <= 0.0f)
                    continue; // degenerate (strip joins)
                nx /= len;
                ny /= len;
                nz /= len;
                for (const In *v : t)
                    out.push_back({v->x, v->y, v->z, nx, ny, nz, v->u, v->v, v->r, v->g, v->b, v->a});
            }
            strip.clear();
        };
        const uint32_t end = addr + size;
        uint32_t p = addr;
        uint32_t positions = 0, colours = 0, uvs = 0, count = 0;
        while (p < end)
        {
            const uint32_t code = word(p);
            p += 4;
            const uint32_t cmd = (code >> 24) & 0x7Fu, num = (code >> 16) & 0xFFu;
            uint32_t words = 0;
            if ((cmd & 0x60u) == 0x60u)
            {
                const uint32_t vn = (cmd >> 2) & 3u, vl = cmd & 3u, n = num ? num : 256u;
                words = (n * (vn + 1u) * (32u >> vl) + 31u) / 32u;
                if ((cmd & 0x6Fu) == 0x68u) // V3-32 positions
                {
                    positions = p;
                    count = n;
                }
                else if ((cmd & 0x6Fu) == 0x6Eu) // V4-8 colours
                    colours = p;
                else if ((cmd & 0x6Fu) == 0x65u) // V2-16 ST, 12-bit fraction (VIF ROW taken as 0)
                    uvs = p;
            }
            else if (cmd == 0x20u)
                words = 1;
            else if (cmd == 0x30u || cmd == 0x31u)
                words = 4;
            else if (cmd == 0x4Au)
                words = (num ? num : 256u) * 2u;
            else if (cmd == 0x50u || cmd == 0x51u)
                words = ((code & 0xFFFFu) ? (code & 0xFFFFu) : 65536u) * 4u;
            else if (cmd == 0x14u || cmd == 0x15u || cmd == 0x17u) // the batch goes to VU1
            {
                if (positions)
                {
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        In v{real(positions + i * 12u), real(positions + i * 12u + 4u), real(positions + i * 12u + 8u), 0.0f, 0.0f, 128, 128, 128, 128};
                        if (uvs)
                        {
                            const uint32_t st = word(uvs + i * 4u);
                            v.u = static_cast<int16_t>(st & 0xFFFFu) / 4096.0f;
                            v.v = static_cast<int16_t>(st >> 16) / 4096.0f;
                        }
                        if (colours)
                        {
                            const uint32_t c = word(colours + i * 4u);
                            v.r = static_cast<uint8_t>(c);
                            v.g = static_cast<uint8_t>(c >> 8);
                            v.b = static_cast<uint8_t>(c >> 16);
                            v.a = static_cast<uint8_t>(c >> 24);
                        }
                        strip.push_back(v);
                    }
                    flush();
                }
                positions = colours = uvs = count = 0;
            }
            p += words * 4u;
        }
    }

    // An NTO2 texture (level 0) as RGBA8, on the GPU once under its header address.
    uint64_t texture(uint32_t header)
    {
        if (!header || word(header) != 0x324F544Eu) // "NTO2"
            return 0;
        if (native_renderer::hasTexture(header))
            return header;
        const uint32_t format = word(header + 0x1Cu), psm = format & 0x3Fu;
        const uint32_t width = 1u << ((format >> 16) & 0xFu), height = 1u << ((format >> 20) & 0xFu);
        if ((psm != 0x13u && psm != 0x14u) || width > 1024u || height > 1024u)
            return 0;
        const uint32_t pixels = header + word(header + 0x14u), clut = header + word(header + 0x18u);
        const bool swizzled = (g_ram[(header + 0x1Fu) & 0x1FFFFFFu] & 1u) != 0u;
        const uint32_t entries = psm == 0x14u ? 16u : 256u;
        uint32_t palette[256];
        for (uint32_t i = 0; i < entries; ++i)
        {
            // CSM1: of every 32 entries, 8-15 and 16-23 are swapped.
            const uint32_t from = entries == 256u ? ((i & ~0x18u) | ((i & 8u) << 1) | ((i & 16u) >> 1)) : i;
            palette[i] = word(clut + from * 4u);
        }
        std::vector<uint8_t> scratch;
        if (swizzled)
        {
            // Uploaded as PSMCT32 and read back as 8/4-bit: replay that.
            const uint32_t cw = width / 2u, ch = psm == 0x13u ? height / 2u : height / 4u;
            scratch.assign(static_cast<size_t>(width) * height + 0x4000u, 0);
            const uint32_t bw32 = std::max<uint32_t>(cw / 64u, 1u);
            for (uint32_t i = 0; i < cw * ch; ++i)
            {
                const uint32_t a = GSPSMCT32::addrPSMCT32(0, bw32, i % cw, i / cw);
                if (a + 4u <= scratch.size())
                    std::memcpy(&scratch[a], g_ram + ((pixels + i * 4u) & 0x1FFFFFFu), 4);
            }
        }
        const uint32_t tbw = std::max<uint32_t>(width / 64u, 2u);
        std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4u);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                uint32_t index;
                if (swizzled && psm == 0x14u)
                {
                    const uint32_t n = GSPSMT4::addrPSMT4(0, tbw, x, y);
                    const uint8_t b = (n >> 1) < scratch.size() ? scratch[n >> 1] : 0;
                    index = (n & 1u) ? (b >> 4) : (b & 15u);
                }
                else if (swizzled)
                {
                    const uint32_t a = GSPSMT8::addrPSMT8(0, tbw, x, y);
                    index = a < scratch.size() ? scratch[a] : 0u;
                }
                else if (psm == 0x14u)
                {
                    const uint8_t b = g_ram[(pixels + (y * width + x) / 2u) & 0x1FFFFFFu];
                    index = (x & 1u) ? (b >> 4) : (b & 15u);
                }
                else
                    index = g_ram[(pixels + y * width + x) & 0x1FFFFFFu];
                std::memcpy(&rgba[(static_cast<size_t>(y) * width + x) * 4u], &palette[index & (entries - 1u)], 4);
            }
        native_renderer::uploadTexture(header, width, height, rgba.data());
        return header;
    }

    uint64_t surfaceTexture(uint32_t header, uint32_t surface)
    {
        const uint32_t s = word(header + 0x50u) + surface * 0x120u;
        const uint32_t index = word(s + 0x2Cu);
        if (index >= word(header + 0x44u))
            return 0;
        const uint32_t object = word(word(header + 0x40u) + index * 0x20u + 0x10u);
        return object ? texture(word(object + 0x20u)) : 0;
    }

    void endFrame()
    {
        if (g_frame == 0)
            return;
        native_renderer::endFrame(512, 448);
        static const bool show = std::atoi(std::getenv("SOTC_NATIVE")) != 2; // 2: draw, keep the emulated picture
        native_renderer::setShown(show && g_frameDraws != 0);
    }

    void getCameraContextHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_getCameraContext(rdram, ctx, runtime);
        g_cameraContext = GPR_U32(ctx, 2);
    }

    void modelDispHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_ram = rdram;
        const uint64_t frame = ps2_stats::vif1Chains.load(std::memory_order_relaxed);
        if (frame != g_frame)
        {
            endFrame();
            native_renderer::beginFrame();
            g_frame = frame;
            g_frameDraws = 0;
        }
        const uint32_t instance = GPR_U32(ctx, 4), matrix = GPR_U32(ctx, 5);
        float model[16];
        for (int i = 0; i < 16; ++i)
            model[i] = real(matrix + i * 4u);
        g_modelDisp(rdram, ctx, runtime); // the emulated drawing goes on as before
        if (!g_cameraContext)
            return;

        float camera[16], clip[16];
        for (int i = 0; i < 16; ++i)
            camera[i] = real(g_cameraContext + 0x5A0u + i * 4u);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
            {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                    sum += camera[k * 4 + r] * model[c * 4 + k];
                clip[c * 4 + r] = sum;
            }

        const uint32_t header = word(word(instance) + 0x20u);
        uint64_t lodWord;
        std::memcpy(&lodWord, rdram + ((instance + 0x250u) & 0x1FFFFF8u), 8);
        uint32_t lod = static_cast<uint32_t>((lodWord >> 40) & 7u);
        if ((word(header + 0x18u) & 1u) && lod >= word(header + 0x1Cu) && word(header + 0x1Cu) != 0u)
            lod = word(header + 0x1Cu) - 1u;
        const uint32_t table = word(header + 0x30u + lod * 4u);
        const uint32_t packets = word(header + 0x60u), count = word(header + 0x64u);
        static std::vector<native_renderer::Vertex> vertices;
        for (uint32_t k = 0; k < count; ++k)
        {
            const uint32_t pk = packets + k * 32u;
            if (word(pk + 4u) != lod)
                continue;
            const uint32_t part = word(pk + 12u);
            if (part != 0xFFFFFFFFu && table)
            {
                const uint32_t visibility = word(table + part * 0x30u + 0x2Cu);
                if (visibility != 0u && visibility != 2u)
                    continue;
            }
            const uint32_t size = word(pk), data = word(pk + 16u);
            const uint64_t id = (static_cast<uint64_t>(size) << 32) | data;
            if (!native_renderer::hasMesh(id))
            {
                vertices.clear();
                buildMesh(data, size, vertices);
                native_renderer::uploadMesh(id, vertices.data(), static_cast<uint32_t>(vertices.size()));
            }
            native_renderer::draw(id, clip, surfaceTexture(header, word(pk + 8u)));
            ++g_frameDraws;
        }
    }
}

void installSotcNative(PS2Runtime &runtime)
{
    using namespace sotc_native;
    if (!std::getenv("SOTC_NATIVE") || !runtime.hasFunction(kModelDisp) || !runtime.hasFunction(kGetCameraContext))
        return;
    if (!native_renderer::available())
    {
        std::cerr << "[sotc:native] no native renderer on this platform" << std::endl;
        return;
    }
    g_modelDisp = runtime.lookupFunction(kModelDisp);
    runtime.registerFunction(kModelDisp, modelDispHook);
    g_getCameraContext = runtime.lookupFunction(kGetCameraContext);
    runtime.registerFunction(kGetCameraContext, getCameraContextHook);
    std::cerr << "[sotc:native] models drawn by the native renderer" << std::endl;
}
