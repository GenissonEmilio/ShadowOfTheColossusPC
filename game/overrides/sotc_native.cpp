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
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
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
            g_skinnedDraws = 0;
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
        const bool fading = real(instance + 0x244u) > 0.0f; // modelDisp: param_1[0x91]
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
            const uint64_t id = (static_cast<uint64_t>(size) << 32) | data;
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
    std::cerr << "[sotc:native] models drawn by the native renderer" << std::endl;
}
