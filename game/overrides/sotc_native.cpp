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
// Skinned parts (characters) are posed on the CPU every frame from the bone
// palette in their instance chain (readPalette).
// Lit batches (normals) are lit per vertex as the VU1's lit microcode does,
// from the lights in the instance's own packet.
// Not yet: the second texture and mip levels.

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/native_renderer.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/ps2_stats.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

    // A vertex as the batch gives it: model space, or bind pose with up to
    // three bones (palette matrices) and weights.
    struct BatchVertex
    {
        float x, y, z;
        float u, v;
        float nx, ny, nz; // lit batches
        uint8_t r, g, b, a;
        uint8_t bones;
        uint8_t bone[3];
        float weight[3];
    };
    using Strip = std::vector<BatchVertex>;

    // The packet's batches, one triangle strip each (they end in MSCNT).
    // STCYCL n+1 interleaves n fields per vertex; each UNPACK's address is
    // its field: 0 GIF tag, 1 position (V3-32), then by format
    //   V2-16 ST, V4-8 colour (unlit)
    //   V4-16 two ST pairs (two textures), V4-8 colour
    //   V3-32 normal, V2-16 ST, V4-8 colour (lit)
    //   V4-16 normal, V2-16 ST, V4-8 colour, V4-32 skin (skinned)
    // Skin: w = bone count, x/y/z = bone index + weight (the fraction; one
    // bone: full weight); the bone is palette matrix n (VU1 qwords 4n..4n+3).
    void readBatches(uint32_t addr, uint32_t size, std::vector<Strip> &out)
    {
        const uint32_t end = addr + size;
        uint32_t p = addr;
        uint32_t positions = 0, normals = 0, colours = 0, uvs = 0, pairs = 0, skin = 0, count = 0;
        while (p < end)
        {
            const uint32_t code = word(p);
            p += 4;
            const uint32_t cmd = (code >> 24) & 0x7Fu, num = (code >> 16) & 0xFFu, field = code & 0x3FFu;
            uint32_t words = 0;
            if ((cmd & 0x60u) == 0x60u)
            {
                const uint32_t vn = (cmd >> 2) & 3u, vl = cmd & 3u, n = num ? num : 256u;
                words = (n * (vn + 1u) * (32u >> vl) + 31u) / 32u;
                switch (cmd & 0x6Fu)
                {
                case 0x68u: // V3-32: position at field 1 (a normal elsewhere)
                    if (field == 1u)
                    {
                        positions = p;
                        count = n;
                    }
                    else
                        normals = p;
                    break;
                case 0x6Eu:
                    colours = p;
                    break;
                case 0x65u: // ST, 12-bit fraction (VIF ROW taken as 0)
                    uvs = p;
                    break;
                case 0x6Du: // two ST pairs, or a normal
                    pairs = p;
                    break;
                case 0x6Cu:
                    if (field != 0u)
                        skin = p;
                    break;
                }
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
                    Strip strip(count);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        BatchVertex &v = strip[i];
                        v = {real(positions + i * 12u), real(positions + i * 12u + 4u), real(positions + i * 12u + 8u), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 128, 128, 128, 128, 0, {0, 0, 0}, {0.0f, 0.0f, 0.0f}};
                        // V4-16 is the normal when the ST comes apart (V2-16), else two ST pairs.
                        const uint32_t st = uvs ? word(uvs + i * 4u) : (pairs ? word(pairs + i * 8u) : 0u);
                        if (normals)
                        {
                            v.nx = real(normals + i * 12u);
                            v.ny = real(normals + i * 12u + 4u);
                            v.nz = real(normals + i * 12u + 8u);
                        }
                        else if (pairs && uvs)
                        {
                            const uint32_t xy = word(pairs + i * 8u), z = word(pairs + i * 8u + 4u);
                            v.nx = static_cast<int16_t>(xy & 0xFFFFu) / 4096.0f;
                            v.ny = static_cast<int16_t>(xy >> 16) / 4096.0f;
                            v.nz = static_cast<int16_t>(z & 0xFFFFu) / 4096.0f;
                        }
                        v.u = static_cast<int16_t>(st & 0xFFFFu) / 4096.0f;
                        v.v = static_cast<int16_t>(st >> 16) / 4096.0f;
                        if (colours)
                        {
                            const uint32_t c = word(colours + i * 4u);
                            v.r = static_cast<uint8_t>(c);
                            v.g = static_cast<uint8_t>(c >> 8);
                            v.b = static_cast<uint8_t>(c >> 16);
                            v.a = static_cast<uint8_t>(c >> 24);
                        }
                        if (skin)
                        {
                            const uint32_t q = skin + i * 16u;
                            v.bones = static_cast<uint8_t>(std::min(word(q + 12u), 3u));
                            for (uint32_t k = 0; k < v.bones; ++k)
                            {
                                const float packed = real(q + k * 4u);
                                const float bone = std::floor(packed);
                                v.bone[k] = static_cast<uint8_t>(std::clamp(bone, 0.0f, 255.0f));
                                v.weight[k] = v.bones == 1 ? 1.0f : packed - bone;
                            }
                        }
                    }
                    out.push_back(std::move(strip));
                }
                positions = normals = colours = uvs = pairs = skin = count = 0;
            }
            p += words * 4u;
        }
    }

    bool lit(const std::vector<Strip> &strips)
    {
        for (const Strip &strip : strips)
            if (!strip.empty() && (strip[0].nx != 0.0f || strip[0].ny != 0.0f || strip[0].nz != 0.0f))
                return true;
        return false;
    }

    // Strips as a triangle list; `place` maps a batch vertex (position,
    // normal) to the mesh's space. Unlit strips get face normals.
    template <typename Place>
    void triangulate(const std::vector<Strip> &strips, Place place, std::vector<native_renderer::Vertex> &out)
    {
        std::vector<native_renderer::Vertex> placed;
        for (const Strip &strip : strips)
        {
            placed.resize(strip.size());
            for (size_t i = 0; i < strip.size(); ++i)
            {
                const BatchVertex &v = strip[i];
                native_renderer::Vertex &o = placed[i];
                place(v, o);
                o.u = v.u;
                o.v = v.v;
                o.r = v.r;
                o.g = v.g;
                o.b = v.b;
                o.a = v.a;
            }
            for (size_t i = 2; i < placed.size(); ++i)
            {
                native_renderer::Vertex t[3] = {placed[i - 2], placed[i - 1], placed[i]};
                const float ux = t[1].x - t[0].x, uy = t[1].y - t[0].y, uz = t[1].z - t[0].z;
                const float vx = t[2].x - t[0].x, vy = t[2].y - t[0].y, vz = t[2].z - t[0].z;
                float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (!(len > 0.0f))
                    continue; // degenerate (strip joins)
                for (native_renderer::Vertex &v : t)
                {
                    if (v.nx == 0.0f && v.ny == 0.0f && v.nz == 0.0f)
                    {
                        v.nx = nx / len;
                        v.ny = ny / len;
                        v.nz = nz / len;
                    }
                    out.push_back(v);
                }
            }
        }
    }

    bool skinned(const std::vector<Strip> &strips)
    {
        for (const Strip &strip : strips)
            if (!strip.empty() && strip[0].bones)
                return true;
        return false;
    }

    // An NTO2 texture (level 0) as RGBA8, on the GPU once under its header address.
    // The game streams data in and reuses memory when the area changes: what
    // is cached under an address is also keyed by a sample of its contents.
    uint64_t sample(uint32_t addr, uint32_t bytes, uint32_t samples)
    {
        uint64_t h = 0xcbf29ce484222325ull ^ addr;
        const uint32_t step = std::max<uint32_t>((bytes / samples) & ~3u, 4u);
        for (uint32_t off = 0; off + 4u <= bytes; off += step)
            h = (h ^ word(addr + off)) * 0x100000001b3ull;
        return h;
    }

    uint64_t texture(uint32_t header)
    {
        if (!header || word(header) != 0x324F544Eu) // "NTO2"
            return 0;
        const uint32_t format = word(header + 0x1Cu), psm = format & 0x3Fu;
        const uint32_t width = 1u << ((format >> 16) & 0xFu), height = 1u << ((format >> 20) & 0xFu);
        const uint32_t pixelBytes = psm == 0x14u ? width * height / 2u : width * height;
        const uint64_t key = sample(header + word(header + 0x14u), pixelBytes, 64u) ^
                             sample(header + word(header + 0x18u), psm == 0x14u ? 64u : 1024u, 16u) * 31u ^ format;
        if (native_renderer::hasTexture(key))
            return key;
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
        native_renderer::uploadTexture(key, width, height, rgba.data());
        return key;
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

    // modelGetDlLayer (0x1096a80): the display list layer of a surface; the
    // layers go to the GS in increasing order, so it is the draw order.
    uint32_t surfaceLayer(uint32_t header, uint32_t s, bool fading)
    {
        uint64_t flags;
        std::memcpy(&flags, g_ram + ((s + 0x18u) & 0x1FFFFF8u), 8);
        const uint32_t owner = word(header + 8u);
        const bool second = owner && word(owner + 0x38u) == 1u;
        const auto bit = [flags](int n) { return ((flags >> n) & 1u) != 0; };
        if (flags & 0x1020000u)
            return 8;
        if (bit(23))
            return 10;
        if (bit(25))
            return 16;
        if (bit(9))
            return 17;
        if (bit(10))
            return (bit(8) || fading) ? 13 : 12;
        if (bit(14))
            return 1;
        if (bit(15))
            return 18;
        if (bit(8) || fading)
            return second ? 6 : 3;
        return second ? 5 : 2;
    }

    // modelDisp's skips: bit 29 never drawn; bit 17 (boss effect) and bit 24
    // (shadow effect) only while getNowBossEffect (0x1186290) has bit 0 / 1;
    // bit 26 not while the underwater fog (getFogUnderWater, 0x1186328) is on.
    bool surfaceDrawn(uint32_t s)
    {
        uint64_t flags;
        std::memcpy(&flags, g_ram + ((s + 0x18u) & 0x1FFFFF8u), 8);
        const uint32_t boss = word(0x1186290u);
        if ((flags >> 29) & 1u)
            return false;
        if ((flags >> 17) & 1u)
            return (boss & 1u) != 0;
        if (((flags >> 24) & 1u) && !(boss & 2u))
            return false;
        return !(((flags >> 26) & 1u) && word(0x1186328u) != 0u);
    }

    // The ALPHA_1 register in the surface's A+D packet (+0x60): 0x48 adds.
    native_renderer::Blend surfaceBlend(uint32_t s)
    {
        for (uint32_t q = s + 0x60u; q < s + 0xC0u; q += 16u)
            if (word(q + 8u) == 0x42u)
                return (word(q) & 0xFFu) == 0x48u ? native_renderer::Blend::Additive : native_renderer::Blend::Normal;
        return native_renderer::Blend::Normal;
    }

    // Skinned parts: their instance chain (instance + 0xc: a NEXT tag past
    // the instance's own 15 qwords) ends in a RET packet that unpacks the
    // bone palette to VU1 qword 0 (modelSetMatrixTransTag), each matrix
    // bone x view (camera space, the projection comes after). Rigid parts
    // upload one matrix there (5 qwords). False when there is no palette.
    bool readPalette(uint32_t instance, std::vector<float> &vu)
    {
        const uint32_t own = word(instance + 0xCu);
        if (((word(own) >> 28) & 7u) != 2u)
            return false;
        const uint32_t ret = word(own + 4u), qwc = word(ret) & 0xFFFFu;
        if (qwc <= 5u)
            return false;
        vu.assign(1024u * 4u, 0.0f);
        const uint32_t end = ret + 16u + qwc * 16u;
        for (uint32_t p = ret + 8u; p < end;)
        {
            const uint32_t code = word(p);
            p += 4;
            const uint32_t cmd = (code >> 24) & 0x7Fu;
            if ((cmd & 0x6Fu) != 0x6Cu)
                continue; // NOP / STCYCL / MSCAL: one word
            const uint32_t n = ((code >> 16) & 0xFFu) ? ((code >> 16) & 0xFFu) : 256u, at = code & 0x3FFu;
            for (uint32_t q = 0; q < n && at + q < 1024u; ++q)
                for (uint32_t k = 0; k < 4u; ++k)
                    vu[(at + q) * 4u + k] = real(p + q * 16u + k * 4u);
            p += n * 16u;
        }
        return true;
    }

    std::unordered_map<uint64_t, std::vector<Strip>> g_skinnedPackets; // bind pose, by packet
    std::unordered_set<uint64_t> g_litPackets;                        // packets with normals
    uint64_t g_skinnedDraws = 0;                                       // this frame's skinned meshes

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

    // A frame of the game ends when the display list goes to the VIF1.
    void syncFrame(uint8_t *rdram)
    {
        g_ram = rdram;
        const uint64_t frame = ps2_stats::vif1Chains.load(std::memory_order_relaxed);
        if (frame != g_frame)
        {
            endFrame();
            native_renderer::beginFrame();
            g_frame = frame;
            g_frameDraws = 0;
            g_skinnedDraws = 0;
        }
    }

    // ---- 2D: sprites, fades, cinema bars ------------------------------------
    //
    // The 2D functions write GS registers with gsReg(reg, value) (0x1c5b00):
    // PRIM, RGBAQ, UV or ST, XYZF2 per vertex, ALPHA. They are read as they
    // are written, told apart by gsReg's return address: the recompiled
    // functions run in pieces the dispatcher continues after a call returns,
    // so a hook around the call sees none of the writes. Entry hooks note
    // what the registers do not carry (texture, display list layer).
    //   spriteDrawSprite(obj, z, layer)  0x10a32b8: triangle strip, layer 25 + layer
    //   spriteDrawSpriteTexNoSet(obj, z) 0x10a24c0: the same, texture already set
    //   spriteDrawSpriteUV / ST(.., .., obj)   0x10a45f0 / 0x10a4890: SPRITE, layer 26
    //   spriteDrawSpriteUV2 / ST2(obj, ..)     0x10a4b38 / 0x10a4ed0: SPRITE, layer 26
    //   fadeScreen(amount)        0x108a408: full screen, colour 0x1186340, layer 0x1186350
    //   drawDemoBar(amount, layer, height) 0x108a588: the cinema bars
    // A sprite object (0x50 bytes) holds its texture resource at [0]; the
    // resource's +0x20 is the NTO2 header. Positions are 12.4 on a 512 x 448
    // screen centred on 2048. Fades blend with ALPHA C = FIX: FIX is the alpha.
    constexpr uint32_t kGsReg = 0x001C5B00u;
    PS2Runtime::RecompiledFunction g_gsReg = nullptr;

    struct Emitter
    {
        uint32_t start, end; // the function, as gsReg's return address sees it
        int objectReg;       // GPR holding the sprite object at entry, -1: none
        int layer;           // fixed display list layer, -1: from the entry hook
        PS2Runtime::RecompiledFunction original;
    };
    Emitter g_emitters[] = {
        {0x010A24C0u, 0x010A32B8u, 4, 25, nullptr},  // spriteDrawSpriteTexNoSet
        {0x010A32B8u, 0x010A45F0u, 4, -1, nullptr},  // spriteDrawSprite
        {0x010A45F0u, 0x010A4890u, 6, 26, nullptr},  // spriteDrawSpriteUV
        {0x010A4890u, 0x010A4B38u, 6, 26, nullptr},  // spriteDrawSpriteST
        {0x010A4B38u, 0x010A4ED0u, 4, 26, nullptr},  // spriteDrawSpriteUV2
        {0x010A4ED0u, 0x010A5270u, 4, 26, nullptr},  // spriteDrawSpriteST2
        {0x0108A408u, 0x0108A588u, -1, -1, nullptr}, // fadeScreen
        {0x0108A588u, 0x0108A758u, -1, -1, nullptr}, // drawDemoBar
    };
    constexpr int kEmitters = static_cast<int>(sizeof(g_emitters) / sizeof(g_emitters[0]));

    struct Sprite
    {
        uint32_t layer = 25;
        uint32_t header = 0; // NTO2, 0: untextured
        uint64_t prim = 0, rgbaq = 0, alpha = 0x44;
        float s = 0.0f, t = 0.0f;
        uint32_t u = 0, v = 0;
        native_renderer::Vertex corners[4];
        uint32_t count = 0;
    } g_sprite;

    void spriteDraw(const native_renderer::Vertex *tris, uint32_t count)
    {
        const Sprite &sp = g_sprite;
        const uint64_t mesh = (1ull << 62) | g_skinnedDraws++;
        native_renderer::uploadMesh(mesh, tris, count);
        native_renderer::DrawState state;
        state.screen = true;
        state.texture = (sp.prim & 0x10u) ? texture(sp.header) : 0;
        const uint32_t mode = static_cast<uint32_t>(sp.alpha & 0xFFu);
        state.blend = !(sp.prim & 0x40u) ? native_renderer::Blend::Opaque
                      : (mode == 0x48u ? native_renderer::Blend::Additive : native_renderer::Blend::Normal);
        state.order = sp.layer;
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        native_renderer::draw(mesh, identity, state);
        ++g_frameDraws;
    }

    void spriteVertex(uint64_t xyz)
    {
        Sprite &sp = g_sprite;
        native_renderer::Vertex &o = sp.corners[sp.count & 3u];
        o.x = ((static_cast<float>(xyz & 0xFFFFu) / 16.0f) - 2048.0f) / 256.0f;
        o.y = -((static_cast<float>((xyz >> 16) & 0xFFFFu) / 16.0f) - 2048.0f) / 224.0f;
        o.z = 0.0f;
        o.nx = o.ny = 0.0f;
        o.nz = 1.0f;
        o.r = static_cast<uint8_t>(sp.rgbaq);
        o.g = static_cast<uint8_t>(sp.rgbaq >> 8);
        o.b = static_cast<uint8_t>(sp.rgbaq >> 16);
        o.a = static_cast<uint8_t>(sp.rgbaq >> 24);
        if (((sp.alpha >> 4) & 3u) == 2u) // C = FIX
            o.a = static_cast<uint8_t>(std::min<uint64_t>((sp.alpha >> 32) & 0xFFu, 0x80u));
        const uint32_t format = sp.header ? word(sp.header + 0x1Cu) : 0u;
        const float width = static_cast<float>(1u << ((format >> 16) & 0xFu)), height = static_cast<float>(1u << ((format >> 20) & 0xFu));
        if (sp.prim & 0x100u) // FST: UV in texels, 12.4
        {
            o.u = (static_cast<float>(sp.u) / 16.0f) / width;
            o.v = (static_cast<float>(sp.v) / 16.0f) / height;
        }
        else
        {
            o.u = sp.s;
            o.v = sp.t;
        }
        ++sp.count;
        const native_renderer::Vertex *c = sp.corners;
        if ((sp.prim & 7u) == 6u) // SPRITE: two opposite corners
        {
            if (sp.count < 2u)
                return;
            sp.count = 0;
            native_renderer::Vertex q[4] = {c[0], c[1], c[1], c[1]};
            q[1].y = c[0].y; q[1].v = c[0].v; // (x1, y0)
            q[2].x = c[0].x; q[2].u = c[0].u; // (x0, y1)
            const native_renderer::Vertex tris[6] = {q[0], q[1], q[2], q[1], q[3], q[2]};
            spriteDraw(tris, 6);
            return;
        }
        if (sp.count < 4u) // triangle strip of four
            return;
        sp.count = 0;
        const native_renderer::Vertex tris[6] = {c[0], c[1], c[2], c[1], c[3], c[2]};
        spriteDraw(tris, 6);
    }

    void gsRegHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = GPR_U32(ctx, 31);
        for (const Emitter &e : g_emitters)
        {
            if (ra < e.start || ra >= e.end)
                continue;
            syncFrame(rdram);
            const uint64_t value = GPR_U64(ctx, 5);
            switch (GPR_U32(ctx, 4) & 0xFFu)
            {
            case 0x00u: g_sprite.prim = value; g_sprite.count = 0; break;
            case 0x01u: g_sprite.rgbaq = value; break;
            case 0x02u:
                std::memcpy(&g_sprite.s, &value, 4);
                std::memcpy(&g_sprite.t, reinterpret_cast<const uint8_t *>(&value) + 4, 4);
                break;
            case 0x03u: g_sprite.u = value & 0x3FFFu; g_sprite.v = (value >> 16) & 0x3FFFu; break;
            case 0x04u: case 0x05u: spriteVertex(value); break;
            case 0x42u: g_sprite.alpha = value; break;
            }
            break;
        }
        g_gsReg(rdram, ctx, runtime);
    }

    template <int Index>
    void emitterHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        syncFrame(rdram);
        const Emitter &e = g_emitters[Index];
        g_sprite = Sprite{};
        if (e.objectReg >= 0)
        {
            const uint32_t resource = word(GPR_U32(ctx, e.objectReg));
            g_sprite.header = resource ? word(resource + 0x20u) : 0u;
        }
        if (e.layer >= 0)
            g_sprite.layer = static_cast<uint32_t>(e.layer);
        else if (e.start == 0x010A32B8u) // spriteDrawSprite(obj, z, layer)
            g_sprite.layer = 25u + std::min<uint32_t>(GPR_U32(ctx, 6), 5u);
        else if (e.start == 0x0108A408u) // fadeScreen
            g_sprite.layer = word(0x1186350u);
        else // drawDemoBar(amount, layer, height)
            g_sprite.layer = GPR_U32(ctx, 4);
        e.original(rdram, ctx, runtime);
    }

    template <int... I>
    void installEmitters(PS2Runtime &runtime, std::integer_sequence<int, I...>)
    {
        const PS2Runtime::RecompiledFunction hooks[] = {emitterHook<I>...};
        for (int i = 0; i < kEmitters; ++i)
            if (runtime.hasFunction(g_emitters[i].start))
            {
                g_emitters[i].original = runtime.lookupFunction(g_emitters[i].start);
                runtime.registerFunction(g_emitters[i].start, hooks[i]);
            }
    }

    void modelDispHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        syncFrame(rdram);
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
        const bool fading = real(instance + 0x244u) > 0.0f; // modelDisp: param_1[0x91]
        // modelDisp's instance opacity (the microcode's, at its packet + 0xdc).
        float opacity = 1.0f - real(instance + 0x244u);
        if (word(header + 0xCu) != 0u)
            opacity *= 1.0f - real(instance + 0x248u);
        if (!(opacity > 0.0f))
            return;
        opacity = std::min(opacity, 1.0f);
        uint64_t lodWord;
        std::memcpy(&lodWord, rdram + ((instance + 0x250u) & 0x1FFFFF8u), 8);
        uint32_t lod = static_cast<uint32_t>((lodWord >> 40) & 7u);
        if ((word(header + 0x18u) & 1u) && lod >= word(header + 0x1Cu) && word(header + 0x1Cu) != 0u)
            lod = word(header + 0x1Cu) - 1u;
        const uint32_t table = word(header + 0x30u + lod * 4u);
        const uint32_t packets = word(header + 0x60u), count = word(header + 0x64u);
        static std::vector<native_renderer::Vertex> vertices;
        static std::vector<Strip> strips;
        static std::vector<float> palette;
        const bool hasPalette = readPalette(instance, palette);
        float projection[16];
        for (int i = 0; i < 16; ++i)
            projection[i] = real(g_cameraContext + 0x40u + i * 4u);
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
            const uint32_t surface = word(pk + 8u), s = word(header + 0x50u) + surface * 0x120u;
            if (!surfaceDrawn(s))
                continue;
            const uint32_t size = word(pk), data = word(pk + 16u);
            const uint64_t id = ((static_cast<uint64_t>(size) << 32) | data) ^ (sample(data, size, 32u) & 0x7FFF000000000000ull);
            auto skinnedPacket = g_skinnedPackets.find(id);
            if (skinnedPacket == g_skinnedPackets.end() && !native_renderer::hasMesh(id))
            {
                strips.clear();
                readBatches(data, size, strips);
                if (skinned(strips) && lit(strips))
                    g_litPackets.insert(id);
                if (skinned(strips))
                    skinnedPacket = g_skinnedPackets.emplace(id, std::move(strips)).first;
                else
                {
                    vertices.clear();
                    if (lit(strips))
                        g_litPackets.insert(id);
                    triangulate(strips, [](const BatchVertex &v, native_renderer::Vertex &o)
                    {
                        o.x = v.x;
                        o.y = v.y;
                        o.z = v.z;
                        o.nx = v.nx;
                        o.ny = v.ny;
                        o.nz = v.nz;
                    }, vertices);
                    native_renderer::uploadMesh(id, vertices.data(), static_cast<uint32_t>(vertices.size()));
                }
            }
            uint64_t mesh = id;
            const float *transform = clip;
            if (skinnedPacket != g_skinnedPackets.end())
            {
                if (!hasPalette)
                    continue;
                // Posed on the CPU (camera space), one mesh per draw this frame.
                vertices.clear();
                triangulate(skinnedPacket->second, [](const BatchVertex &v, native_renderer::Vertex &o)
                {
                    o.x = o.y = o.z = o.nx = o.ny = o.nz = 0.0f;
                    for (uint32_t b = 0; b < v.bones; ++b)
                    {
                        const float *m = &palette[(v.bone[b] * 4u % 1024u) * 4u];
                        const float w = v.weight[b];
                        o.x += w * (m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12]);
                        o.y += w * (m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13]);
                        o.z += w * (m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14]);
                        o.nx += w * (m[0] * v.nx + m[4] * v.ny + m[8] * v.nz);
                        o.ny += w * (m[1] * v.nx + m[5] * v.ny + m[9] * v.nz);
                        o.nz += w * (m[2] * v.nx + m[6] * v.ny + m[10] * v.nz);
                    }
                }, vertices);
                mesh = (1ull << 63) | g_skinnedDraws++;
                native_renderer::uploadMesh(mesh, vertices.data(), static_cast<uint32_t>(vertices.size()));
                transform = projection;
            }
            native_renderer::DrawState state;
            state.texture = surfaceTexture(header, surface);
            state.blend = surfaceBlend(s);
            state.order = surfaceLayer(header, s, fading);
            state.opacity = opacity;
            if (g_litPackets.count(id))
            {
                // The instance's lights (its own 15 qwords, unpacked to VU1
                // 0x3d8): the light matrix, then colours and ambient.
                state.lit = true;
                const uint32_t own = word(instance + 0xCu);
                for (uint32_t i = 0; i < 32u; ++i)
                    state.light[i] = real(own + 0x10u + i * 4u);
            }
            native_renderer::draw(mesh, transform, state);
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
    if (runtime.hasFunction(kGsReg))
    {
        installEmitters(runtime, std::make_integer_sequence<int, kEmitters>{});
        g_gsReg = runtime.lookupFunction(kGsReg);
        runtime.registerFunction(kGsReg, gsRegHook);
    }
    std::cerr << "[sotc:native] models drawn by the native renderer" << std::endl;
}
