// Metal shaders for the GPU GS rasterizer (included as a string by
// gs_metal_raster.mm). The fragment stage runs the same per-pixel algorithm as
// GSCpuBackend (coverage, interpolation, texturing, tests, blending) directly
// on the swizzled GS local memory, so both renderers produce the same bytes.
R"MSL(
#include <metal_stdlib>
using namespace metal;

#pragma METAL fp contract(off)

struct GpuPrim
{
    uint kind;      // 0 = triangle, 1 = sprite
    uint state;
    int x0, y0, x1, y1;   // inclusive pixel bounds, already clipped to the scissor
    float fx[3], fy[3];   // triangle vertices (offset applied); sprite: sx0, sy0, sx1, sy1 in fx[0..1], fy[0..1]
    float winding, invAbsDenom; // triangle; sprite: invDx, invDy
    float zhi[3], zlo[3]; // vertex Z as a float pair (hi + lo == double Z); sprite: z1 bits in zhi[0]
    uint rgba[3];
    float s[3], t[3], q[3];
    float u[3], v[3];     // UV as stored (12.4 fixed point values, as float)
    float fog[3];
    float su0, sv0, su1, sv1; // sprite texture coordinates
};

struct GpuState
{
    uint fbp, fbw, fpsm, fbmsk;
    uint zbp, zpsm, zmask, test;
    uint asel, bsel, csel, dsel;
    uint fix, abe, pabe, fba;
    uint fge, fogR, fogG, fogB;
    uint iip, tme, fst, linear;
    uint tbp0, tbw, tpsm, texW;
    uint texH, tcc, tfx, cpsm;
    uint csm, csa, wrapU, wrapV;
    uint minU, maxU, minV, maxV;
    uint ta0, aem, ta1, clut;
};

// Swizzle geometry of the eight layouts (C32, Z32, C16, C16S, Z16, Z16S, P8,
// P4): page, column and block extents plus the small block/column tables.
struct SwizzleInfo
{
    uint pw[8], ph[8], colW[8], colH[8], blkW[8], blkH[8];
    uchar block[8][32];
    ushort column[8][512];
};

// Pipeline specialization: the per-draw switches are compile-time constants,
// so each variant only contains the code its draws need.
constant uint FC_KIND [[function_constant(0)]];  // 0 = triangle, 1 = sprite
constant uint FC_FLAGS [[function_constant(1)]]; // see specializeState
constant uint FC_FPSM [[function_constant(2)]];
constant uint FC_ZPSM [[function_constant(3)]];
constant uint FC_TPSM [[function_constant(4)]];
constant uint FC_TEST [[function_constant(5)]];  // TEST bits except AREF
constant uint kTestSwitchMask = 0x7F00Fu;

inline GpuState specializeState(device const GpuState &in)
{
    GpuState st = in;
    st.tme = FC_FLAGS & 1u;
    st.fst = (FC_FLAGS >> 1) & 1u;
    st.linear = (FC_FLAGS >> 2) & 1u;
    st.iip = (FC_FLAGS >> 3) & 1u;
    st.fge = (FC_FLAGS >> 4) & 1u;
    st.abe = (FC_FLAGS >> 5) & 1u;
    st.pabe = (FC_FLAGS >> 6) & 1u;
    st.zmask = (FC_FLAGS >> 7) & 1u;
    st.fba = (FC_FLAGS >> 8) & 1u;
    st.tcc = (FC_FLAGS >> 9) & 1u;
    st.tfx = (FC_FLAGS >> 10) & 3u;
    if (((FC_FLAGS >> 12) & 1u) == 0u)
        st.fbmsk = 0u;
    st.fpsm = FC_FPSM;
    st.zpsm = FC_ZPSM;
    st.tpsm = FC_TPSM;
    st.test = (in.test & ~kTestSwitchMask) | FC_TEST;
    return st;
}

struct VOut
{
    float4 position [[position]];
    uint prim [[flat]];
};

constant float2 kQuad[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1) };

vertex VOut gsVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                     device const GpuPrim *prims [[buffer(1)]],
                     constant uint &primBase [[buffer(6)]])
{
    const uint index = primBase + iid;
    const GpuPrim p = prims[index];
    const float2 corner = kQuad[vid];
    const float px = mix(float(p.x0), float(p.x1 + 1), corner.x);
    const float py = mix(float(p.y0), float(p.y1 + 1), corner.y);
    VOut out;
    out.position = float4(px / 1024.0 - 1.0, 1.0 - py / 1024.0, 0.0, 1.0);
    out.prim = index;
    return out;
}

// ---- GS local memory -------------------------------------------------------

constant uint kVramMask = 0x3FFFFFu;

inline int tableFor(uint psm)
{
    switch (psm)
    {
    case 0: case 1: case 27: case 36: case 44: return 0; // C32 layout
    case 48: case 49: return 1;                          // Z32
    case 2: return 2;                                    // C16
    case 10: return 3;                                   // C16S
    case 50: return 4;                                   // Z16
    case 58: return 5;                                   // Z16S
    case 19: return 6;                                   // P8
    case 20: return 7;                                   // P4
    default: return -1;
    }
}

// Same value as the CPU's page lookup table entry:
// (base % 32 + block id) * pixels per block + column id.
inline uint pixelAddress(constant SwizzleInfo &sw, int table, uint block, uint bw, uint x, uint y)
{
    const uint pw = sw.pw[table], ph = sw.ph[table];
    const uint cw = sw.colW[table], ch = sw.colH[table];
    const uint page = block / 32u + (y / ph) * ((bw * 64u) / pw) + x / pw;
    const uint lx = x % pw, ly = y % ph;
    const uint blockId = sw.block[table][((ly / ch) % sw.blkH[table]) * sw.blkW[table] + (lx / cw) % sw.blkW[table]];
    const uint columnId = sw.column[table][(ly % ch) * cw + (lx % cw)];
    return page * (pw * ph) + ((block % 32u) + blockId) * (cw * ch) + columnId;
}

inline uint readVram(device const uchar *vram, constant SwizzleInfo &sw,
                     uint psm, uint block, uint bw, uint x, uint y)
{
    const int table = tableFor(psm);
    if (table < 0)
        return 0u;
    const uint pixel = pixelAddress(sw, table, block, bw, x, y);
    switch (psm)
    {
    case 0: case 48:
        return *(device const uint *)(vram + ((pixel * 4u) & (kVramMask & ~3u)));
    case 1: case 49:
        return *(device const uint *)(vram + ((pixel * 4u) & (kVramMask & ~3u))) & 0x00FFFFFFu;
    case 2: case 10: case 50: case 58:
        return *(device const ushort *)(vram + ((pixel * 2u) & (kVramMask & ~1u)));
    case 19:
        return vram[pixel & kVramMask];
    case 27:
        return vram[(pixel * 4u + 3u) & kVramMask];
    case 20:
    {
        const uint bits = pixel * 4u;
        return (vram[(bits / 8u) & kVramMask] >> (bits % 8u)) & 0xFu;
    }
    case 36:
        return vram[(pixel * 4u + 3u) & kVramMask] & 0xFu;
    case 44:
        return (vram[(pixel * 4u + 3u) & kVramMask] >> 4u) & 0xFu;
    }
    return 0u;
}

// Frame and Z buffers (32/24/16-bit formats only; other formats stay on the CPU).
inline uint readTarget(device uchar *vram, constant SwizzleInfo &sw,
                       uint psm, uint block, uint bw, uint x, uint y)
{
    const int table = tableFor(psm);
    const uint pixel = pixelAddress(sw, table, block, bw, x, y);
    switch (psm)
    {
    case 0: case 48:
        return *(device uint *)(vram + ((pixel * 4u) & (kVramMask & ~3u)));
    case 1: case 49:
        return *(device uint *)(vram + ((pixel * 4u) & (kVramMask & ~3u))) & 0x00FFFFFFu;
    default:
        return *(device ushort *)(vram + ((pixel * 2u) & (kVramMask & ~1u)));
    }
}

inline void writeTarget(device uchar *vram, constant SwizzleInfo &sw,
                        uint psm, uint block, uint bw, uint x, uint y, uint value)
{
    const int table = tableFor(psm);
    const uint pixel = pixelAddress(sw, table, block, bw, x, y);
    switch (psm)
    {
    case 0: case 48:
        *(device uint *)(vram + ((pixel * 4u) & (kVramMask & ~3u))) = value;
        break;
    case 1: case 49:
    {
        device uint *word = (device uint *)(vram + ((pixel * 4u) & (kVramMask & ~3u)));
        *word = (*word & 0xFF000000u) | (value & 0x00FFFFFFu);
        break;
    }
    default:
        *(device ushort *)(vram + ((pixel * 2u) & (kVramMask & ~1u))) = ushort(value);
        break;
    }
}

// ---- Colour helpers (same as the CPU backend) ------------------------------

inline uint clampU8(int v) { return uint(clamp(v, 0, 255)); }

inline uint rgba5551To8888(uint c)
{
    return ((c & 0x1Fu) << 3) | (((c >> 5) & 0x1Fu) << 11) | (((c >> 10) & 0x1Fu) << 19) | (((c >> 15) & 1u) << 31);
}

inline uint rgba8888To5551(uint c)
{
    return ((c & 0xFFu) >> 3) | ((((c >> 8) & 0xFFu) >> 3) << 5) | ((((c >> 16) & 0xFFu) >> 3) << 10) | ((((c >> 24) & 0xFFu) >> 7) << 15);
}

inline uint applyTexa(thread const GpuState &st, uint psm, uint texel)
{
    if (psm == 0u)
        return texel;
    const bool rgbZero = (texel & 0x00FFFFFFu) == 0u;
    uint a = (texel >> 24) & 0xFFu;
    if (psm == 1u)
        a = (st.aem != 0u && rgbZero) ? 0u : st.ta0;
    else if (psm == 2u || psm == 10u)
        a = (a & 0x80u) != 0u ? st.ta1 : ((st.aem != 0u && rgbZero) ? 0u : st.ta0);
    return (texel & 0x00FFFFFFu) | (a << 24);
}

inline uint lookupClut(thread const GpuState &st, device const ushort *clut, uint index)
{
    const bool sixteenBit = st.cpsm == 2u || st.cpsm == 10u;
    const uint csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint clutBase = (st.csa & csaMask) << 4u;
    const bool fourBit = st.tpsm == 20u || st.tpsm == 36u || st.tpsm == 44u;
    const bool eightBit = st.tpsm == 19u || st.tpsm == 27u;
    const uint sourceIndex = fourBit ? (index & 0x0Fu) : index;
    uint clutIndex = (clutBase + sourceIndex) & (sixteenBit ? 0x1FFu : 0x0FFu);
    if (!sixteenBit && st.csm == 0u && eightBit)
    {
        const uint block = min((sourceIndex & 0xF0u) + clutBase, 240u);
        clutIndex = block + (sourceIndex & 0x0Fu);
    }
    if (st.cpsm == 0u)
        return applyTexa(st, 0u, uint(clut[clutIndex]) | (uint(clut[clutIndex + 256u]) << 16));
    if (st.cpsm == 1u)
        return applyTexa(st, 1u, (uint(clut[clutIndex]) | (uint(clut[clutIndex + 256u]) << 16)) & 0x00FFFFFFu);
    if (sixteenBit)
        return applyTexa(st, st.cpsm, rgba5551To8888(clut[clutIndex]));
    return 0xFFFF00FFu;
}

inline int wrapCoord(int c, int size, uint mode, uint minV, uint maxV)
{
    switch (mode & 3u)
    {
    case 0: return int(uint(c) & uint(size - 1));
    case 1: return clamp(c, 0, size - 1);
    case 2: return min(max(c, int(minV)), int(maxV));
    default: return int((uint(c) & minV) | maxV);
    }
}

inline uint samplePoint(thread const GpuState &st, device const uchar *vram, constant SwizzleInfo &sw, device const ushort *clut, int su, int sv)
{
    su = wrapCoord(su, int(st.texW), st.wrapU, st.minU, st.maxU);
    sv = wrapCoord(sv, int(st.texH), st.wrapV, st.minV, st.maxV);
    const uint raw = readVram(vram, sw, st.tpsm, st.tbp0, st.tbw, uint(su), uint(sv));
    switch (st.tpsm)
    {
    case 0: case 48: case 1: case 49:
        return applyTexa(st, st.tpsm, raw);
    case 2: case 10: case 50: case 58:
        return applyTexa(st, st.tpsm, rgba5551To8888(raw));
    case 19: case 27: case 20: case 36: case 44:
        return lookupClut(st, clut, raw & 0xFFu);
    }
    return 0xFFFF00FFu;
}

inline float interp3(float a, float b, float c, float w0, float w1, float w2)
{
    return fma(c, w2, fma(b, w1, a * w0));
}

inline float lerpf(float a, float b, float t) { return fma(b - a, t, a); }

inline float fabsQ(float q) { return fabs(q) > 1.0e-8f ? q : 1.0f; }

inline uint lerpChannel(uint c00, uint c10, uint c01, uint c11, float fx, float fy)
{
    const float top = lerpf(float(c00), float(c10), fx);
    const float bottom = lerpf(float(c01), float(c11), fx);
    return clampU8(int(round(lerpf(top, bottom, fy))));
}

inline uint sampleTexture(thread const GpuState &st, device const uchar *vram, constant SwizzleInfo &sw, device const ushort *clut, float s, float t, float q, uint u, uint v)
{
    float texUf, texVf;
    if (st.fst != 0u)
    {
        texUf = float(u) / 16.0f;
        texVf = float(v) / 16.0f;
    }
    else
    {
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * float(st.texW);
        texVf = t * invQ * float(st.texH);
    }
    if (st.linear == 0u)
        return samplePoint(st, vram, sw, clut, int(texUf), int(texVf));
    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    const int u0 = int(floor(sampleU));
    const int v0 = int(floor(sampleV));
    const float fx = sampleU - float(u0);
    const float fy = sampleV - float(v0);
    const uint c00 = samplePoint(st, vram, sw, clut, u0, v0);
    const uint c10 = samplePoint(st, vram, sw, clut, u0 + 1, v0);
    const uint c01 = samplePoint(st, vram, sw, clut, u0, v0 + 1);
    const uint c11 = samplePoint(st, vram, sw, clut, u0 + 1, v0 + 1);
    uint out = 0u;
    for (uint shift = 0u; shift < 32u; shift += 8u)
        out |= lerpChannel((c00 >> shift) & 0xFFu, (c10 >> shift) & 0xFFu, (c01 >> shift) & 0xFFu, (c11 >> shift) & 0xFFu, fx, fy) << shift;
    return out;
}

inline uint4 combineTexture(thread const GpuState &st, uint4 c, uint texel)
{
    const uint tr = texel & 0xFFu, tg = (texel >> 8) & 0xFFu, tb = (texel >> 16) & 0xFFu, ta = texel >> 24;
    const bool hasAlpha = st.tcc != 0u;
    switch (st.tfx)
    {
    case 0:
        return uint4(clampU8(int((tr * c.r) >> 7)), clampU8(int((tg * c.g) >> 7)), clampU8(int((tb * c.b) >> 7)),
                     hasAlpha ? clampU8(int((ta * c.a) >> 7)) : c.a);
    case 2:
        return uint4(clampU8(int(((tr * c.r) >> 7) + c.a)), clampU8(int(((tg * c.g) >> 7) + c.a)), clampU8(int(((tb * c.b) >> 7) + c.a)),
                     hasAlpha ? clampU8(int(ta + c.a)) : c.a);
    case 3:
        return uint4(clampU8(int(((tr * c.r) >> 7) + c.a)), clampU8(int(((tg * c.g) >> 7) + c.a)), clampU8(int(((tb * c.b) >> 7) + c.a)),
                     hasAlpha ? ta : c.a);
    default:
        return uint4(tr, tg, tb, hasAlpha ? ta : c.a);
    }
}

// ---- Double precision Z as a float pair ------------------------------------

struct DF { float hi, lo; };

inline DF twoSum(float a, float b)
{
    const float s = a + b;
    const float bb = s - a;
    return DF{s, (a - (s - bb)) + (b - bb)};
}

inline DF dfAdd(DF a, DF b)
{
    DF s = twoSum(a.hi, b.hi);
    s.lo += a.lo + b.lo;
    return twoSum(s.hi, s.lo);
}

// w * (hi + lo), exact enough for 32-bit Z.
inline DF dfMul(float w, float hi, float lo)
{
    const float p = w * hi;
    const float e = fma(w, hi, -p);
    return twoSum(p, e + w * lo);
}

inline uint dfToU32Rounded(DF z)
{
    DF h = dfAdd(z, DF{0.5f, 0.0f});
    const float fh = floor(h.hi);
    const float rem = (h.hi - fh) + h.lo;
    long value = long(fh) + long(floor(rem));
    return uint(clamp(value, 0l, long(0xFFFFFFFFu)));
}

// ---- Pixel pipeline (WritePixel) --------------------------------------------

inline void writePixel(thread const GpuState &st, device uchar *target, constant SwizzleInfo &sw,
                       uint x, uint y, uint z, uint4 c, uint fog)
{
    uint r = c.r, g = c.g, b = c.b, a = c.a;
    if (st.fge != 0u)
    {
        const uint inv = 255u - fog;
        r = ((fog * r) >> 8) + ((inv * st.fogR) >> 8);
        g = ((fog * g) >> 8) + ((inv * st.fogG) >> 8);
        b = ((fog * b) >> 8) + ((inv * st.fogB) >> 8);
    }

    // Alpha test (TEST.ATE/ATST/AREF) and its fail mode (AFAIL).
    bool writeRgb = true, writeAlpha = true, writeDepth = true;
    if ((st.test & 1u) != 0u)
    {
        const uint atst = (st.test >> 1) & 7u, aref = (st.test >> 4) & 0xFFu;
        bool pass;
        switch (atst)
        {
        case 0: pass = false; break;
        case 1: pass = true; break;
        case 2: pass = a < aref; break;
        case 3: pass = a <= aref; break;
        case 4: pass = a == aref; break;
        case 5: pass = a >= aref; break;
        case 6: pass = a > aref; break;
        default: pass = a != aref; break;
        }
        if (!pass)
        {
            switch ((st.test >> 12) & 3u)
            {
            case 1: writeDepth = false; break;
            case 2: writeRgb = false; writeAlpha = false; break;
            case 3:
                if (st.fpsm == 0u) { writeAlpha = false; writeDepth = false; }
                else writeDepth = false;
                break;
            default: return;
            }
        }
    }
    const bool writesFb = writeRgb || writeAlpha;
    if (!writesFb && !writeDepth)
        return;

    const bool preserveDstAlpha = writeRgb && !writeAlpha && st.fpsm == 0u;
    const bool date = ((st.test >> 14) & 1u) != 0u && (st.fpsm == 0u || st.fpsm == 2u || st.fpsm == 10u);
    const bool frame16 = st.fpsm == 2u || st.fpsm == 10u;
    const bool frmw = date || (writesFb && (st.fbmsk != 0u || st.abe != 0u || preserveDstAlpha));

    uint raw = 0u, fb = 0u;
    if (frmw)
    {
        raw = readTarget(target, sw, st.fpsm, st.fbp, st.fbw, x, y);
        fb = frame16 ? rgba5551To8888(raw) : (st.fpsm == 1u ? (raw | 0x80000000u) : raw);
    }
    if (date)
    {
        const bool datm = ((st.test >> 15) & 1u) != 0u;
        const bool msb = st.fpsm == 0u ? ((raw >> 31) & 1u) != 0u : ((raw >> 15) & 1u) != 0u;
        if (msb != datm)
            return;
    }

    switch ((st.test >> 17) & 3u)
    {
    case 0:
        return;
    case 2:
        if (!(z >= readTarget(target, sw, st.zpsm, st.zbp, st.fbw, x, y)))
            return;
        break;
    case 3:
        if (!(z > readTarget(target, sw, st.zpsm, st.zbp, st.fbw, x, y)))
            return;
        break;
    default:
        break;
    }

    if (writesFb)
    {
        if (st.abe != 0u && !(st.pabe != 0u && (a & 0x80u) == 0u))
        {
            const uint dr = fb & 0xFFu, dg = (fb >> 8) & 0xFFu, db = (fb >> 16) & 0xFFu, da = fb >> 24;
            const int ca = st.csel == 0u ? int(a) : (st.csel == 1u ? int(da) : int(st.fix));
            const int3 cs = int3(int(r), int(g), int(b));
            const int3 cd = int3(int(dr), int(dg), int(db));
            const int3 A = st.asel == 0u ? cs : (st.asel == 1u ? cd : int3(0));
            const int3 B = st.bsel == 0u ? cs : (st.bsel == 1u ? cd : int3(0));
            const int3 D = st.dsel == 0u ? cs : (st.dsel == 1u ? cd : int3(0));
            const int3 o = ((A - B) * ca >> 7) + D;
            r = clampU8(o.x);
            g = clampU8(o.y);
            b = clampU8(o.z);
        }
        if (writeAlpha && st.fba != 0u)
            a |= 0x80u;
        uint pixel = r | (g << 8) | (b << 16) | (a << 24);
        if (st.fbmsk != 0u)
            pixel = (pixel & ~st.fbmsk) | (fb & st.fbmsk);
        if (preserveDstAlpha)
            pixel = (pixel & 0x00FFFFFFu) | (fb & 0xFF000000u);
        if (frame16)
            pixel = rgba8888To5551(pixel);
        writeTarget(target, sw, st.fpsm, st.fbp, st.fbw, x, y, pixel);
    }
    if (writeDepth && st.zmask == 0u)
        writeTarget(target, sw, st.zpsm, st.zbp, st.fbw, x, y, z);
}

inline uint4 unpackRgba(uint c) { return uint4(c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu, c >> 24); }

fragment void gsFragment(VOut in [[stage_in]],
                         device uchar *target [[buffer(0), raster_order_group(0)]],
                         device const GpuPrim *prims [[buffer(1)]],
                         device const GpuState *states [[buffer(2)]],
                         constant SwizzleInfo &sw [[buffer(3)]],
                         device const ushort *cluts [[buffer(4)]],
                         device const uchar *vram [[buffer(7)]])
{
    const uint x = uint(in.position.x);
    const uint y = uint(in.position.y);
    device const GpuPrim &p = prims[in.prim];
    const GpuState st = specializeState(states[p.state]);
    device const ushort *clut = cluts + st.clut * 512u;

    if (FC_KIND == 0u)
    {
        const float px = float(x) + 0.5f, py = float(y) + 0.5f;
        const float fx0 = p.fx[0], fx1 = p.fx[1], fx2 = p.fx[2];
        const float fy0 = p.fy[0], fy1 = p.fy[1], fy2 = p.fy[2];
        const float w0 = (fma(fy1 - fy2, px - fx2, (fx2 - fx1) * (py - fy2)) * p.winding) * p.invAbsDenom;
        const float w1 = (fma(fy2 - fy0, px - fx2, (fx0 - fx2) * (py - fy2)) * p.winding) * p.invAbsDenom;
        const float w2 = 1.0f - w0 - w1;
        const float eps = 1.0e-4f;
        if (w0 < -eps || w1 < -eps || w2 < -eps)
            return;

        const DF zsum = dfAdd(dfAdd(dfMul(w0, p.zhi[0], p.zlo[0]), dfMul(w1, p.zhi[1], p.zlo[1])), dfMul(w2, p.zhi[2], p.zlo[2]));
        const uint z = dfToU32Rounded(zsum);

        uint4 c;
        if (st.iip != 0u)
        {
            const uint4 c0 = unpackRgba(p.rgba[0]), c1 = unpackRgba(p.rgba[1]), c2 = unpackRgba(p.rgba[2]);
            c = uint4(clampU8(int(interp3(float(c0.r), float(c1.r), float(c2.r), w0, w1, w2))),
                      clampU8(int(interp3(float(c0.g), float(c1.g), float(c2.g), w0, w1, w2))),
                      clampU8(int(interp3(float(c0.b), float(c1.b), float(c2.b), w0, w1, w2))),
                      clampU8(int(interp3(float(c0.a), float(c1.a), float(c2.a), w0, w1, w2))));
        }
        else
        {
            c = unpackRgba(p.rgba[2]);
        }

        if (st.tme != 0u)
        {
            uint texel;
            if (st.fst != 0u)
            {
                const float fu = interp3(p.u[0], p.u[1], p.u[2], w0, w1, w2);
                const float fv = interp3(p.v[0], p.v[1], p.v[2], w0, w1, w2);
                // Like the CPU's float -> uint16 conversion: negatives saturate to 0.
                texel = sampleTexture(st, vram, sw, clut, 0.0f, 0.0f, 1.0f, uint(max(fu, 0.0f)) & 0xFFFFu, uint(max(fv, 0.0f)) & 0xFFFFu);
            }
            else
            {
                const float is = interp3(p.s[0], p.s[1], p.s[2], w0, w1, w2);
                const float it = interp3(p.t[0], p.t[1], p.t[2], w0, w1, w2);
                const float iq = interp3(p.q[0], p.q[1], p.q[2], w0, w1, w2);
                texel = sampleTexture(st, vram, sw, clut, is, it, iq, 0u, 0u);
            }
            c = combineTexture(st, c, texel);
        }
        const uint fog = clampU8(int(interp3(p.fog[0], p.fog[1], p.fog[2], w0, w1, w2)));
        writePixel(st, target, sw, x, y, z, c, fog);
    }
    else
    {
        // Sprite: coverage is the bounds; texture coordinates from the exact positions.
        const float sx0 = p.fx[0], sy0 = p.fy[0];
        const float invDx = p.winding, invDy = p.invAbsDenom;
        uint4 c = unpackRgba(p.rgba[0]);
        if (st.tme != 0u)
        {
            const float ty = (float(y) - sy0) * invDy;
            const float texVf = lerpf(p.sv0, p.sv1, ty);
            const float tx = (float(x) - sx0) * invDx;
            const float texUf = lerpf(p.su0, p.su1, tx);
            uint texel;
            if (st.fst != 0u)
            {
                const int fixedU = int((texUf * 16.0f) + 0.5f);
                const int fixedV = int((texVf * 16.0f) + 0.5f);
                texel = sampleTexture(st, vram, sw, clut, 0.0f, 0.0f, 1.0f, uint(clamp(fixedU, 0, 0xFFFF)), uint(clamp(fixedV, 0, 0xFFFF)));
            }
            else
            {
                texel = sampleTexture(st, vram, sw, clut, texUf / float(st.texW), texVf / float(st.texH), 1.0f, 0u, 0u);
            }
            c = combineTexture(st, c, texel);
        }
        writePixel(st, target, sw, x, y, as_type<uint>(p.zhi[0]), c, uint(p.fog[0]));
    }
}
)MSL"
