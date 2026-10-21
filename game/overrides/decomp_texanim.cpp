// manager_sub_010A9EA8: a texture's scroll and palette (CLUT) animation,
// once a frame for every animated texture (3.4% of the Vita's game thread
// recompiled, throttled attract demo). The native renderer reads what it
// writes (the scroll offsets and the animated palette), so it is rewritten,
// not skipped.
//
// a0: the texture; +0x20 its animation record r, f12 the frame step.
// r+0xc the state s: s[0] / s[2] the u / v offsets (12 bits), s[1] / s[3]
// the linear parts, s[4] the palette rotation, s[5] its frame counter,
// s+0xc the palette in use, s+0x40 the source palette, s+0x44 + 4 id the
// destination one for display-list buffer id (iosDlGetWorkBufId).
// r+0x9c the time (+30 x step); r+0x20 / +0x24 phase and speed; r+0x30 /
// +0x34 the u / v wave amplitudes, +0x38 / +0x3c their wobble, +0x40 /
// +0x44 its period, +0x48 / +0x4c the wave periods, +0x50 / +0x54 /
// +0x58 / +0x5c the period wobble; r+0x28 / +0x2c the linear scroll speeds;
// r+0x1c the texture format (low 6 bits; the palette size from the table
// at 0x1192280, 0x13 / 0x1b go through the CSM1 swizzle at 0x1192370);
// r+0x60 / +0x64 the rotated range, +0x68 the frames per step, +0x6c the
// step, +0x70 blend between steps.
//
// The recompiled code's operations in the same order (FPU_* rounding and
// clamping), on locals; iosGetCosf / iosGetSinf stay guest calls (only for
// textures with a wave), iosDlGetWorkBufId is read directly (it returns
// *(0x2132f0), v1 = 0x2132d8). v0 and f0 as the original leaves them.
// Pointers outside plain RAM go to the recompiled code.
#include "sotc_decomp.h"

#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <cstring>

namespace decomp_texanim
{
    using sotc_decomp::returnTo;

    constexpr uint32_t kTexAnim = 0x010A9EA8u;
    constexpr uint32_t kSizes = 0x01192280u;   // palette entries per format
    constexpr uint32_t kSwizzle = 0x01192370u; // CSM1 entry order
    constexpr uint32_t kGetWorkBufId = 0x001C0138u;

    float bits(uint32_t b)
    {
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }
    uint32_t fbits(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }

    struct Ram
    {
        uint8_t *rdram;
        uint8_t *p(uint32_t a) const { return rdram + (a & PS2_RAM_MASK); }
        uint32_t u32(uint32_t a) const
        {
            uint32_t v;
            std::memcpy(&v, p(a), 4);
            return v;
        }
        int32_t s32(uint32_t a) const { return static_cast<int32_t>(u32(a)); }
        float f32(uint32_t a) const { return bits(u32(a)); }
        uint16_t u16(uint32_t a) const
        {
            uint16_t v;
            std::memcpy(&v, p(a), 2);
            return v;
        }
        void put16(uint32_t a, uint32_t v) const
        {
            const uint16_t h = static_cast<uint16_t>(v);
            std::memcpy(p(a), &h, 2);
        }
        void put32(uint32_t a, uint32_t v) const { std::memcpy(p(a), &v, 4); }
        uint8_t u8(uint32_t a) const { return *p(a); }
        void put8(uint32_t a, uint32_t v) const { *p(a) = static_cast<uint8_t>(v); }
    };

    bool plain(uint32_t a, uint32_t size)
    {
        return !Ps2IsSpecialAddress(a) && !Ps2IsSpecialAddress(a + size - 1u) && (a & PS2_RAM_MASK) + size <= PS2_RAM_SIZE;
    }

    void texAnim(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const Ram ram{rdram};
        uint32_t fcr31 = ctx->fcr31;
        auto cond = [&](bool c)
        {
            fcr31 = c ? (fcr31 | 0x800000u) : (fcr31 & ~0x800000u);
            return c;
        };
        const float zero = 0.0f;
        const float step = ctx->f[12];
        if (cond(FPU_C_EQ_S(step, zero)))
        {
            ctx->f[6] = zero;
            ctx->fcr31 = fcr31;
            return returnTo(ctx);
        }

        // Rare cases to the recompiled code, before anything is written.
        const uint32_t s3 = ram.u32(GPR_U32(ctx, 4) + 0x20u);
        const uint32_t r = s3 + 0x20u; // s0
        const bool inlineId = sotc_decomp::jalTarget(rdram, 0x010AA2ACu) == kGetWorkBufId;
        const uint32_t s1 = plain(s3, 0xA0u) ? ram.u32(s3 + 0xCu) : 0u;
        bool ok = plain(GPR_U32(ctx, 4) + 0x20u, 4u) && plain(s3, 0xA0u) && inlineId && plain(s1, 0x60u);
        if (ok)
        {
            const uint32_t id = ram.u32(0x002132F0u);
            const int32_t count = ram.s32(kSizes + (ram.u32(s3 + 0x1Cu) & 0x3Fu) * 4u);
            ok = id < 4u && count >= 0 && count <= 1024 && plain(ram.u32(s1 + 0x40u), 4u * 1024u) &&
                 plain(ram.u32(s1 + 0x44u + id * 4u), 4u * 1024u);
        }
        if (!ok)
            return sotc_decomp::recompiled(kTexAnim)(rdram, ctx, runtime);

        const uint32_t sp0 = GPR_U32(ctx, 29), ra0 = GPR_U32(ctx, 31), sp1 = sp0 - 0x50u;
        const float thirty = bits(0x41F00000u), piBits = bits(0x40490FDAu), two31 = bits(0x4F000000u);
        float f0 = ctx->f[0], f1, f2, f12;
        // guest iosGetCosf / iosGetSinf (f12 -> f0)
        auto trig = [&](uint32_t site, float x)
        {
            ctx->f[12] = x;
            ctx->fcr31 = fcr31;
            sotc_decomp::callGuest(rdram, ctx, runtime, sotc_decomp::jalTarget(rdram, site), site + 8u, sp1);
            fcr31 = ctx->fcr31;
            return ctx->f[0];
        };
        // cvt.w.s with the >= 2^31 case (sub.s, cvt.w.s, or 0x80000000)
        auto toWord = [&](float &v)
        {
            const bool big = cond(FPU_C_OLE_S(two31, v));
            if (!big)
                return static_cast<uint32_t>(FPU_CVT_W_S(v));
            v = FPU_SUB_S(v, two31);
            return static_cast<uint32_t>(FPU_CVT_W_S(v)) | 0x80000000u;
        };

        f0 = FPU_MUL_S(step, thirty);
        f2 = ram.f32(r + 0x7Cu);
        f1 = ram.f32(r);
        f0 = FPU_ADD_S(f2, f0);
        const float f3 = ram.f32(r + 4u);
        const uint32_t before = static_cast<uint32_t>(FPU_CVT_W_S(f2));
        f1 = FPU_ADD_S(f0, f1);
        ram.put32(r + 0x7Cu, fbits(f0));
        const uint32_t after = static_cast<uint32_t>(FPU_CVT_W_S(f0));
        const float f22 = FPU_MUL_S(f1, f3);
        const uint32_t frames = after - before; // s5
        f0 = FPU_ADD_S(f22, f22);
        const float f21 = FPU_MUL_S(f0, piBits);
        float f20 = ctx->f[20];

        // One wave: s[k] = (cos|sin)(phase / period) x amp + amp, 12 bits.
        auto wave = [&](uint32_t ampAt, uint32_t wobbleAt, uint32_t wobblePeriodAt, uint32_t periodAt, uint32_t periodWobbleAt,
                        uint32_t periodWobblePeriodAt, uint32_t sites[3]) -> uint32_t
        {
            uint32_t s2 = ram.u16(r + ampAt);
            bool loadedF0 = false;
            if (ram.u32(r + wobbleAt) != 0u)
            {
                f12 = ram.f32(r + wobblePeriodAt);
                if (cond(FPU_C_EQ_S(f12, zero)))
                {
                    f0 = ram.f32(r + periodWobbleAt);
                    loadedF0 = true;
                }
                else
                {
                    f12 = FPU_MUL_S(f12, thirty);
                    f12 = ps2FpuDivide(fcr31, f21, f12);
                    f0 = trig(sites[0], f12);
                    f1 = FPU_CVT_S_W(ram.s32(r + wobbleAt));
                    f2 = two31;
                    f0 = FPU_MUL_S(f1, f0);
                    const uint32_t w = toWord(f0);
                    s2 = (s2 + w) & 0xFFFFu;
                }
            }
            if (!loadedF0)
                f0 = ram.f32(r + periodWobbleAt);
            f2 = zero;
            f1 = ram.f32(r + periodAt);
            const bool noWobble = cond(FPU_C_EQ_S(f0, f2));
            f0 = thirty;
            f20 = FPU_MUL_S(f1, f0);
            if (!noWobble)
            {
                f12 = ram.f32(r + periodWobblePeriodAt);
                if (!cond(FPU_C_EQ_S(f12, f2)))
                {
                    f12 = FPU_MUL_S(f12, f0);
                    f12 = ps2FpuDivide(fcr31, f21, f12);
                    f0 = trig(sites[1], f12);
                    f1 = ram.f32(r + periodWobbleAt);
                    f1 = FPU_MUL_S(f1, f0);
                    f20 = FPU_ADD_S(f20, f1);
                }
            }
            f12 = ps2FpuDivide(fcr31, f21, f20);
            f0 = trig(sites[2], f12);
            f1 = FPU_CVT_S_W(static_cast<int32_t>(s2));
            f2 = two31;
            f0 = FPU_MUL_S(f0, f1);
            f0 = FPU_ADD_S(f0, f1);
            return toWord(f0) & 0xFFFu;
        };

        uint32_t cosSites[3] = {0x010A9F70u, 0x010A9FFCu, 0x010AA01Cu};
        uint32_t sinSites[3] = {0x010AA0B8u, 0x010AA144u, 0x010AA164u};
        if (ram.u32(r + 0x10u) == 0u)
            ram.put16(s1, 0u);
        else
        {
            const uint32_t v = wave(0x10u, 0x18u, 0x20u, 0x28u, 0x30u, 0x38u, cosSites);
            ram.put16(s1, v);
        }
        if (ram.u32(r + 0x14u) == 0u)
            ram.put16(s1 + 4u, 0u);
        else
        {
            if (ram.u32(r + 0x1Cu) != 0u)
                f0 = zero; // mtc1 $zero, $f0 before the wobble period test
            const uint32_t v = wave(0x14u, 0x1Cu, 0x24u, 0x2Cu, 0x34u, 0x3Cu, sinSites);
            ram.put16(s1 + 4u, v);
        }
        // Linear scrolls.
        auto linear = [&](uint32_t speedAt, uint32_t partAt, uint32_t offsetAt)
        {
            const int32_t speed = ram.s32(r + speedAt);
            if (speed == 0)
                return;
            f0 = FPU_MUL_S(FPU_CVT_S_W(speed), f22);
            f1 = two31;
            const uint32_t w = toWord(f0) & 0xFFFu;
            const uint32_t off = ram.u16(s1 + offsetAt);
            ram.put16(s1 + partAt, w);
            ram.put16(s1 + offsetAt, (off + w) & 0xFFFu);
        };
        linear(0x08u, 2u, 0u);
        linear(0x0Cu, 6u, 4u);

        uint32_t v0;
        auto finish = [&](uint32_t value)
        {
            SET_GPR_S32(ctx, 2, static_cast<int32_t>(value));
            ctx->f[0] = f0;
            ctx->fcr31 = fcr31;
            SET_GPR_U32(ctx, 29, sp0);
            SET_GPR_U32(ctx, 31, ra0);
            returnTo(ctx);
        };
        auto sizeOf = [&]() { return ram.u32(kSizes + (ram.u32(s3 + 0x1Cu) & 0x3Fu) * 4u); };
        {
            const uint32_t size = sizeOf();
            if (size == 0u)
                return finish(kSizes + (ram.u32(s3 + 0x1Cu) & 0x3Fu) * 4u);
            v0 = ram.u32(r + 0x4Cu);
            if (v0 == 0u || frames == 0u)
                return finish(v0);
        }

        // Palette animation.
        const uint32_t id = ram.u32(0x002132F0u); // iosDlGetWorkBufId
        const uint32_t dst = ram.u32(s1 + id * 4u + 0x44u); // t4
        const uint32_t src = ram.u32(s1 + 0x40u);           // t3
        uint32_t a1 = ram.u32(r + 0x40u);
        if (!(a1 < sizeOf()))
        {
            a1 = sizeOf() - 1u;
            ram.put32(r + 0x40u, a1);
        }
        uint32_t a0 = ram.u32(r + 0x44u);
        if (!(a0 < sizeOf()))
        {
            a0 = sizeOf() - 1u;
            ram.put32(r + 0x44u, a0);
        }
        const int32_t lo = static_cast<int32_t>(a0 < a1 ? a0 : a1); // t2
        const int32_t hi = static_cast<int32_t>(a0 < a1 ? a1 : a0); // t1
        const int32_t span = hi - lo + 1;                           // t0
        const uint32_t counter = ram.u16(s1 + 0xAu) + frames;       // v1
        ram.put16(s1 + 0xAu, counter);
        const int32_t perStep = ram.s32(r + 0x48u);
        const int32_t counter16 = static_cast<int32_t>(counter & 0xFFFFu);
        if (perStep == 0 || perStep < counter16)
        {
            ram.put16(s1 + 0xAu, counter - ram.u16(r + 0x48u));
            ram.put16(s1 + 8u, ram.u16(s1 + 8u) + ram.u16(r + 0x4Cu));
            f1 = zero;
        }
        else
        {
            f1 = FPU_CVT_S_W(counter16);
            f0 = FPU_CVT_S_W(perStep);
            f1 = ps2FpuDivide(fcr31, f1, f0);
        }
        auto wrap = [&](int32_t x)
        {
            while (x < lo)
                x += span;
            while (hi < x)
                x -= span;
            return x;
        };
        for (int32_t i = 0; i < static_cast<int32_t>(sizeOf()); ++i)
        {
            const uint32_t format = ram.u32(s3 + 0x1Cu) & 0x3Fu;
            const bool csm = format == 0x13u || format == 0x1Bu;
            if (i < lo || hi < i)
            {
                ram.put32(dst + 4u * i, ram.u32(src + 4u * i));
                continue;
            }
            const int32_t rot = static_cast<int16_t>(ram.u16(s1 + 8u));
            if (ram.u32(r + 0x50u) != 0u)
            {
                int32_t from = wrap(i + rot);
                int32_t to = wrap(static_cast<int32_t>(static_cast<uint32_t>(i + rot) + ram.u32(r + 0x4Cu)));
                uint32_t a2a, a1a, a3a;
                if (csm)
                {
                    a2a = src + 4u * ram.u8(kSwizzle + from);
                    a1a = src + 4u * ram.u8(kSwizzle + to);
                    a3a = dst + 4u * ram.u8(kSwizzle + i);
                }
                else
                {
                    a2a = src + 4u * from;
                    a1a = src + 4u * to;
                    a3a = dst + 4u * i;
                }
                for (uint32_t k = 0; k < 4u; ++k)
                {
                    const uint32_t a = ram.u8(a2a + k), b = ram.u8(a1a + k);
                    f0 = FPU_CVT_S_W(static_cast<int32_t>(b - a));
                    f0 = FPU_MUL_S(f0, f1);
                    ram.put8(a3a + k, a + static_cast<uint32_t>(FPU_CVT_W_S(f0)));
                }
            }
            else
            {
                const int32_t from = wrap(i + rot);
                uint32_t s, d;
                if (csm)
                {
                    d = dst + 4u * ram.u8(kSwizzle + i);
                    s = src + 4u * ram.u8(kSwizzle + from);
                }
                else
                {
                    d = dst + 4u * i;
                    s = src + 4u * from;
                }
                uint8_t w[4];
                for (uint32_t k = 0; k < 4u; ++k)
                    w[k] = ram.u8(s + k);
                for (uint32_t k = 0; k < 4u; ++k)
                    ram.put8(d + k, w[k]);
            }
        }
        // Keep the rotation in 0..span-1 (16 bits, signed).
        uint32_t rot = ram.u16(s1 + 8u);
        if (static_cast<int32_t>(rot << 16) < 0)
        {
            do
                rot += span;
            while (static_cast<int32_t>(rot << 16) < 0);
            ram.put16(s1 + 8u, rot);
        }
        if (!(static_cast<int16_t>(rot) < span))
        {
            do
                rot -= span;
            while (!(static_cast<int16_t>(rot) < span));
            ram.put16(s1 + 8u, rot);
        }
        ram.put32(s1 + 0xCu, dst);
        finish(1u);
    }
}

SOTC_DECOMP_FUNCTION(0x010A9EA8u, texAnim, decomp_texanim::texAnim);
