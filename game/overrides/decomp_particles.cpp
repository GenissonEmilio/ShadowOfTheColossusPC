// One particle's step (manager_sub_0113CA58 and the functions only it
// calls: 0113D280, 01141918, 01141968, 01141B70), run for every live
// particle of every emitter each frame: ~9% of the Vita's game thread
// recompiled in the attract demo, which is full of particles. The renderer
// draws what this writes (position, colour, size, rotation: ptclVtxHook).
//
// Arguments (all guest pointers): a0 the particle (0xb0 bytes: +0x00
// position, +0x10 velocity, +0x20 / +0x30 more vectors, +0x40 life, +0x44
// age, +0x4c.. size, colour, rotation, +0xa4 "has hit", +0xa8 next), a1 the
// emitter's list (+0x124 count / head / tail, +0x150 / +0x154 floor and
// ceiling), a2 / t0 / t1 the emitter's parameters, a3 its kind (+0x0 space,
// +0xc shape, +0x104 collision, +0x11c jitter, +0xc8 spin). *0x118cfe8 is
// the frame step: +0 dt, +4 a second step, +0xc VBlanks this frame.
//
// Done natively: a live particle without the collision test. Left to the
// recompiled code from the start (nothing written yet): a particle whose
// age passes its life (unlinked to the free list), one that would go
// through the collision test (0113D5F0), shape 0 (a trap), pointers outside
// main RAM. Called as guest code on the way: iosGetRandom (VU0's R
// register), iosGetReflectVector, and 0113C8C8 when the damage test is on.
//
// Every operation is the recompiler's own (FPU_* / PS2_V* macros, the EE's
// round-toward-zero, clamping and operand alignment), in the original order.
// VU0's vf / ACC registers are not left as the original leaves them (GCC
// uses them as scratch inside inline asm only).
#include "sotc_decomp.h"

#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <cstring>

namespace decomp_particles
{
    using sotc_decomp::returnTo;

    constexpr uint32_t kUpdate = 0x0113CA58u;
    constexpr uint32_t kStepPtr = 0x0118CFE8u;     // -> dt, dt2, ?, VBlanks
    constexpr uint32_t kDamageOn = 0x0118CFECu;    // 0113C8C8's first test
    constexpr uint32_t kSpinY = 0x01216ECCu;       // added to y when spin == 2
    constexpr uint32_t kDamage = 0x0113C8C8u;

    struct Guest
    {
        uint8_t *rdram;
        R5900Context *ctx;
        PS2Runtime *runtime;

        uint8_t *at(uint32_t a) const { return rdram + (a & PS2_RAM_MASK); }
        uint32_t u32(uint32_t a) const
        {
            uint32_t v;
            std::memcpy(&v, at(a), 4);
            return v;
        }
        float f32(uint32_t a) const
        {
            float v;
            std::memcpy(&v, at(a), 4);
            return v;
        }
        void put(uint32_t a, float v) const { std::memcpy(at(a), &v, 4); }
        void put32(uint32_t a, uint32_t v) const { std::memcpy(at(a), &v, 4); }
        __m128 q(uint32_t a) const
        {
            __m128 v;
            std::memcpy(&v, at(a), 16);
            return v;
        }
        void putq(uint32_t a, __m128 v) const { std::memcpy(at(a), &v, 16); }

        uint32_t step() const { return u32(kStepPtr); }
        float dt() const { return f32(step()); }

        // A guest function run to its return at ra, with sp as the original's frame.
        void call(uint32_t target, uint32_t ra, uint32_t sp) const { sotc_decomp::callGuest(rdram, ctx, runtime, target, ra, sp); }
        // iosGetRandom / iosGetReflectVector(out, in, normal) are in the boot
        // ELF: their jal sites are relocated (the target read from the code).
        float random(uint32_t site, uint32_t sp) const
        {
            call(sotc_decomp::jalTarget(rdram, site), site + 8u, sp);
            return ctx->f[0];
        }
        void reflect(uint32_t v, uint32_t normal, uint32_t site, uint32_t sp) const
        {
            SET_GPR_U32(ctx, 4, v);
            SET_GPR_U32(ctx, 5, v);
            SET_GPR_U32(ctx, 6, normal);
            call(sotc_decomp::jalTarget(rdram, site), site + 8u, sp);
        }
    };

    const __m128 kXyz = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));

    // vmulx.xyz v, v, s (w kept)
    __m128 scaled(__m128 v, float s)
    {
        return PS2_VBLEND(v, PS2_VMUL(v, _mm_set1_ps(s)), kXyz);
    }
    // vadd.xyz a, a, b / vsub.xyz a, a, b (w of a kept)
    __m128 plus(__m128 a, __m128 b) { return PS2_VBLEND(a, PS2_VADD(a, b), kXyz); }
    __m128 minus(__m128 a, __m128 b) { return PS2_VBLEND(a, PS2_VSUB(a, b), kXyz); }

    float bitsToFloat(uint32_t b)
    {
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }
    // The game's constants as its code builds them (lui / ori): 0.001 is cut, not rounded.
    const float kHundredth = bitsToFloat(0x3C23D70Au), kThousandth = bitsToFloat(0x3A83126Eu);

    bool lt(float a, float b) { return FPU_C_OLT_S(a, b); }
    bool le(float a, float b) { return FPU_C_OLE_S(a, b); }
    bool eq(float a, float b) { return FPU_C_EQ_S(a, b); }
    float mul(float a, float b) { return FPU_MUL_S(a, b); }
    float add(float a, float b) { return FPU_ADD_S(a, b); }
    float sub(float a, float b) { return FPU_SUB_S(a, b); }

    // 01141918: f ^ VBlanks by repeated products (1 when f is 1).
    float decay(const Guest &g, float f)
    {
        float r = 1.0f;
        if (eq(f, 1.0f))
            return r;
        int32_t n = static_cast<int32_t>(g.u32(g.step() + 0xCu));
        if (n <= 0)
            return r;
        do
        {
            --n;
            r = mul(r, f);
        } while (n != 0);
        return r;
    }

    // 01141968: position += velocity dt + accel dt2 + list's drift x (+0x88 dt) [+ +0x30 vector x +0x48];
    // with a spin, += (axis x +0x80 dt) cross (position - centre), then the spin speed. f0 at the end.
    float move(const Guest &g, uint32_t p, __m128 accel, uint32_t list, uint32_t extra, uint32_t kind, uint32_t params)
    {
        float f0 = g.dt();
        const float f1 = mul(g.f32(p + 0x88u), f0);
        g.putq(p, plus(g.q(p), scaled(g.q(p + 0x10u), f0)));
        f0 = g.f32(g.step() + 4u);
        g.putq(p, plus(g.q(p), scaled(accel, f0)));
        g.putq(p, plus(g.q(p), scaled(g.q(list), f1)));
        const uint32_t space = g.u32(kind);
        if (space == 1u || space == 3u)
        {
            f0 = g.f32(p + 0x48u);
            g.putq(p, plus(g.q(p), scaled(g.q(extra), f0)));
        }
        if (g.u32(kind + 0xC8u) == 0u)
            return f0;
        const float s = mul(g.f32(p + 0x80u), g.dt());
        const __m128 axis = scaled(g.q(list + 0x30u), s);
        const __m128 r = minus(g.q(p), g.q(list + 0x10u));
        // vopmula.xyz ACC, axis, r; vopmsub.xyz c, r, axis
        const __m128 acc = PS2_VMUL(_mm_shuffle_ps(axis, axis, _MM_SHUFFLE(3, 0, 2, 1)), _mm_shuffle_ps(r, r, _MM_SHUFFLE(3, 1, 0, 2)));
        const __m128 cross = PS2_VSUB(acc, PS2_VMUL(_mm_shuffle_ps(r, r, _MM_SHUFFLE(3, 0, 2, 1)), _mm_shuffle_ps(axis, axis, _MM_SHUFFLE(3, 1, 0, 2))));
        g.putq(p, plus(g.q(p), cross));
        float spin;
        if (g.u32(kind + 0xC8u) == 1u)
            spin = add(g.f32(p + 0x80u), mul(g.f32(params + 0xD4u), g.dt()));
        else
        {
            const float f2 = g.f32(p + 0x80u);
            if (le(0.0f, f2))
                spin = add(f2, mul(g.f32(params + 0xD4u), g.dt()));
            else
                spin = sub(f2, mul(g.f32(params + 0xD4u), g.dt()));
        }
        g.put(p + 0x80u, spin);
        const float k = g.f32(params + 0xD8u);
        float factor = 1.0f;
        if (!eq(k, 1.0f))
        {
            int32_t n = static_cast<int32_t>(g.u32(g.step() + 0xCu));
            if (n > 0)
                do
                {
                    --n;
                    factor = mul(factor, k);
                } while (n != 0);
        }
        f0 = mul(g.f32(p + 0x80u), factor);
        g.put(p + 0x80u, f0);
        return f0;
    }

    // 01141B70: below the floor or above the ceiling (list +0x150 / +0x154,
    // widened by the size), put back on it and bounce. f0 at the end.
    float bounce(const Guest &g, uint32_t p, uint32_t list, uint32_t a2, uint32_t params, uint32_t sp)
    {
        const uint32_t frame = sp - 0x40u;
        float f1 = mul(g.f32(a2 + 0x134u), g.f32(p + 0x4Cu));
        const float floorY = sub(g.f32(list + 0x150u), f1);
        float f0 = g.f32(list + 0x154u);
        float y = g.f32(p + 4u);
        const float ceilingY = add(f0, f1);
        auto hit = [&](float to, uint32_t normal, uint32_t site1, uint32_t site2, uint32_t k)
        {
            g.put(p + 4u, to);
            g.reflect(p + 0x10u, normal, site1, frame);
            g.reflect(p + 0x20u, normal, site2, frame);
            g.putq(p + 0x10u, scaled(g.q(p + 0x10u), g.f32(params + k)));
            g.putq(p + 0x20u, scaled(g.q(p + 0x20u), g.f32(params + k + 4u)));
            f0 = mul(g.f32(p + 0x54u), g.f32(params + k + 8u));
            g.put(p + 0x54u, f0);
        };
        if (le(floorY, y))
        {
            hit(floorY, sotc_decomp::relocated(g.rdram, 0x01141BD4u, 0x01141BDCu), 0x01141BE0u, 0x01141BF0u, 0x130u);
            y = g.f32(p + 4u);
        }
        if (le(y, ceilingY))
            hit(ceilingY, sotc_decomp::relocated(g.rdram, 0x01141C48u, 0x01141C50u), 0x01141C64u, 0x01141C74u, 0x13Cu);
        return f0;
    }

    void particleUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const Guest g{rdram, ctx, runtime};
        const uint32_t p = GPR_U32(ctx, 4), list = GPR_U32(ctx, 5), a2 = GPR_U32(ctx, 6), kind = GPR_U32(ctx, 7);
        const uint32_t params = GPR_U32(ctx, 8), t1 = GPR_U32(ctx, 9);
        const uint32_t sp0 = GPR_U32(ctx, 29), ra0 = GPR_U32(ctx, 31);
        auto inRam = [](uint32_t a) { return a != 0u && a < PS2_RAM_SIZE - 0x200u; };
        const bool plain = inRam(p) && inRam(list) && inRam(a2) && inRam(kind) && inRam(params) && inRam(t1) && inRam(g.step()) &&
                           !lt(g.f32(p + 0x40u), add(g.f32(p + 0x44u), g.dt())) && g.u32(kind + 0xCu) != 0u &&
                           !(g.u32(kind + 0x104u) != 0u && g.u32(p + 0xA4u) == 0u);
        if (!plain)
        {
            sotc_decomp::recompiled(kUpdate)(rdram, ctx, runtime);
            return;
        }
        const uint32_t sp1 = sp0 - 0x90u, sp2 = sp1 - 0x90u;
        float f0, f1, f2;

        // Age.
        g.put(p + 0x44u, add(g.f32(p + 0x44u), g.dt()));

        // Local space: the four vectors (three unless shape 2) by the matrix at t1 + 0x10.
        if (g.u32(kind) - 2u < 2u)
        {
            const uint32_t m = t1 + 0x10u;
            const __m128 matrix[4] = {g.q(m), g.q(m + 0x10u), g.q(m + 0x20u), g.q(m + 0x30u)};
            const sotc_decomp::vu::Rows rows = sotc_decomp::vu::clampRows(matrix); // reloaded each time by the original: nothing writes it in between
            auto transform = [&](uint32_t at) { g.putq(at, sotc_decomp::vu::apply(rows, g.q(at))); };
            transform(p);
            transform(p + 0x10u);
            transform(p + 0x20u);
            if (g.u32(kind + 0xCu) == 2u)
                transform(p + 0x30u);
        }

        // The frame's buffer (the original's sp + 0 .. 0x24): the acceleration
        // and the decay factors for this age.
        __m128 accel;
        float velocityDecay, growth, growthDecay, turn, turnDecay;
        f1 = g.f32(params + 0x40u);
        if (le(f1, 0.0f))
        {
            accel = g.q(p + 0x20u);
            velocityDecay = g.f32(params + 0x3Cu);
        }
        else if (lt(g.f32(p + 0x44u), f1))
        {
            accel = scaled(g.q(p + 0x20u), g.f32(params + 0x38u));
            velocityDecay = g.f32(params + 0x3Cu);
        }
        else
        {
            accel = scaled(g.q(p + 0x20u), g.f32(params + 0x44u));
            velocityDecay = g.f32(params + 0x48u);
        }
        f1 = g.f32(params + 0x6Cu);
        if (le(f1, 0.0f) || lt(g.f32(p + 0x44u), f1))
        {
            growth = g.f32(params + 0x64u);
            growthDecay = g.f32(params + 0x68u);
        }
        else
        {
            growth = g.f32(params + 0x70u);
            growthDecay = g.f32(params + 0x74u);
        }
        f1 = g.f32(params + 0xECu);
        if (le(f1, 0.0f) || lt(g.f32(p + 0x44u), f1))
        {
            f1 = g.f32(params + 0xE4u);
            turnDecay = g.f32(params + 0xE8u);
        }
        else
        {
            f1 = g.f32(params + 0xF0u);
            turnDecay = g.f32(params + 0xF4u);
        }
        turn = mul(g.f32(p + 0xA0u), f1);

        // Size and its growth.
        f2 = g.f32(p + 0x50u);
        f1 = add(g.f32(p + 0x4Cu), mul(f2, g.dt()));
        g.put(p + 0x4Cu, f1);
        if (lt(f1, 0.0f))
            g.put(p + 0x4Cu, 0.0f);
        g.put(p + 0x50u, add(f2, mul(growth, g.dt())));
        f0 = decay(g, growthDecay);
        g.put(p + 0x50u, mul(g.f32(p + 0x50u), f0));

        // 0113D280: the move (01141968), jitter, the floor and ceiling (01141B70).
        move(g, p, accel, list, t1, kind, params);
        if (g.u32(kind + 0x11Cu) != 0u && lt(0.0f, g.dt()))
        {
            const float half = mul(g.f32(params + 0x118u), 0.5f);
            const uint32_t sites[3] = {0x0113D548u, 0x0113D564u, 0x0113D580u};
            for (uint32_t i = 0; i < 3u; ++i)
            {
                const float r = g.random(sites[i], sp2);
                g.put(p + i * 4u, add(g.f32(p + i * 4u), sub(mul(r, g.f32(params + 0x118u)), half)));
            }
        }
        bounce(g, p, list, a2, params, sp2);

        // Velocity: += acceleration dt (y less the particle's +0x8c), then its decay.
        alignas(16) float a[4];
        _mm_store_ps(a, accel);
        g.put(p + 0x10u, add(g.f32(p + 0x10u), mul(a[0], g.dt())));
        g.put(p + 0x14u, add(g.f32(p + 0x14u), mul(sub(a[1], g.f32(p + 0x8Cu)), g.dt())));
        g.put(p + 0x18u, add(g.f32(p + 0x18u), mul(a[2], g.dt())));
        f0 = decay(g, velocityDecay);
        if (!eq(f0, 1.0f))
            g.putq(p + 0x10u, scaled(g.q(p + 0x10u), f0));

        // Three channels in 0..1 (+0x68, +0x70, +0x78), each with a speed, its
        // change (params +0x88, +0xa0, +0xb8) and its decay.
        for (uint32_t c = 0; c < 3u; ++c)
        {
            const uint32_t at = p + 0x68u + c * 8u, k = params + 0x88u + c * 0x18u;
            const float speed = g.f32(at + 4u);
            f1 = add(g.f32(at), mul(speed, g.dt()));
            g.put(at, f1);
            if (lt(f1, 0.0f))
                g.put(at, 0.0f);
            if (lt(1.0f, g.f32(at)))
                g.put(at, 1.0f);
            g.put(at + 4u, add(speed, mul(g.f32(k), g.dt())));
            f0 = decay(g, g.f32(k + 4u));
            g.put(at + 4u, mul(g.f32(at + 4u), f0));
        }

        // Rotation (+0x90, speed +0x94), once past a2 +0xa0 when a2 +0x120 >= 2:
        // wraps at a2 +0x124 (a2 +0x8c == 1) or stops short of it.
        if (static_cast<int32_t>(g.u32(a2 + 0x120u)) >= 2 && le(g.f32(a2 + 0xA0u), g.f32(p + 0x44u)))
        {
            f2 = g.f32(p + 0x94u);
            const float angle = add(g.f32(p + 0x90u), mul(f2, g.dt()));
            g.put(p + 0x90u, angle);
            f1 = g.f32(a2 + 0x124u);
            const bool wraps = g.u32(a2 + 0x8Cu) == 1u;
            if (le(f1, angle))
                g.put(p + 0x90u, wraps ? sub(angle, f1) : sub(f1, kHundredth));
            else if (lt(angle, 0.0f))
                g.put(p + 0x90u, wraps ? add(angle, f1) : 0.0f);
            const float change = mul(g.f32(a2 + 0x98u), g.dt());
            g.put(p + 0x94u, g.u32(a2 + 0x88u) == 0u || le(0.0f, f2) ? add(f2, change) : sub(f2, change));
            f0 = decay(g, g.f32(a2 + 0x9Cu));
            g.put(p + 0x94u, mul(g.f32(p + 0x94u), f0));
        }

        // Spin (+0x98, speed +0x9c) by shape.
        const uint32_t shape = g.u32(kind + 0xCu);
        if (shape == 1u || shape == 4u)
        {
            const float speed = g.f32(p + 0x9Cu);
            g.put(p + 0x98u, add(g.f32(p + 0x98u), mul(speed, g.dt())));
            g.put(p + 0x9Cu, add(speed, mul(turn, g.dt())));
            f0 = decay(g, turnDecay);
            g.put(p + 0x9Cu, mul(g.f32(p + 0x9Cu), f0));
            if (g.u32(kind + 0xCu) == 4u && g.u32(kind + 0x174u) != 0u && lt(0.0f, g.dt()))
            {
                f0 = g.random(0x0113D150u, sp1);
                g.put(p + 0x30u, add(g.f32(params + 0x14Cu), mul(g.f32(params + 0x150u), f0)));
                f0 = g.random(0x0113D168u, sp1);
                f1 = mul(g.f32(params + 0x150u), f0);
                g.put(p + 0x38u, 0.0f);
                g.put(p + 0x3Cu, 0.0f);
                g.put(p + 0x34u, add(g.f32(params + 0x14Cu), f1));
            }
        }
        else if (shape == 2u)
        {
            const float speed = g.f32(p + 0x9Cu);
            f1 = add(g.f32(p + 0x98u), mul(speed, g.dt()));
            g.put(p + 0x98u, f1);
            if (lt(f1, kThousandth))
                g.put(p + 0x98u, kThousandth);
            g.put(p + 0x9Cu, add(speed, mul(g.f32(params + 0x108u), g.dt())));
            f0 = decay(g, g.f32(params + 0x10Cu));
            g.put(p + 0x9Cu, mul(g.f32(p + 0x9Cu), f0));
        }

        // 0113C8C8: the damage test, when it is on.
        if (g.u32(kDamageOn) != 0u && g.u32(a2 + 0xD8u) != 0u)
        {
            ctx->f[0] = f0;
            SET_GPR_U32(ctx, 4, p);
            SET_GPR_U32(ctx, 5, a2);
            g.call(kDamage, 0x0113D208u, sp1);
            f0 = ctx->f[0];
        }
        if (g.u32(a2 + 0xC8u) == 2u)
        {
            f0 = add(g.f32(p + 4u), g.f32(kSpinY));
            g.put(p + 4u, f0);
        }

        ctx->f[0] = f0;
        SET_GPR_U64(ctx, 2, 0u);
        SET_GPR_U32(ctx, 29, sp0);
        SET_GPR_U32(ctx, 31, ra0);
        returnTo(ctx);
    }
}

SOTC_DECOMP_FUNCTION(0x0113CA58u, particleUpdate, decomp_particles::particleUpdate);
