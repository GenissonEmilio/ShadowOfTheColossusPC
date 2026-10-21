// manager_sub_010B8590: a skeleton's pose, bone by bone (recursive: first
// child, then next sibling). 2.8% of the Vita's game thread recompiled
// (throttled attract demo). For bone i (a0) of the skeleton a3 (+8 the
// bones, 0x40 each: +0x10 offset, +0x30 first child, +0x34 next sibling),
// with the local rotations t0 (quaternions), the scale t1, it pushes the
// matrix and quaternion stacks, turns the current quaternion by bone i's
// (normalised in place), builds that quaternion's rotation matrix and the
// offset x scale translation on the frame, applies them to the current
// matrix and stores it (a1 + 0x40 i) with the current quaternion,
// normalised (a2 + 0x10 i), then does the children and pops.
//
// The matrix / quaternion stack helpers (boot ELF: PushQuaternion,
// iosPushCurrentMatrix, MultiCurrentQuaternion, iosTransCurrentMatrix,
// iosMulCurrentMatrixR, iosGetCurrentMatrix, GetCurrentQuaternion,
// iosPopCurrentMatrix, PopQuaternion) are native too, on the context's VU0
// registers and VU0 memory (vi15, the matrix stack) as their recompiled
// code: 9 guest calls a bone were ~2.4% of the Vita's game thread in call
// overhead alone. Their jal sites are relocated: the targets are read from
// the code, and a site naming another function is called through the guest.
// The VU0 arithmetic of the pose itself is the recompiled code's own, on
// locals, written back to the context. Recursion is native.
#include "sotc_decomp.h"

#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <cstring>

namespace decomp_skeleton
{
    using sotc_decomp::returnTo;

    struct Ctx
    {
        uint8_t *rdram;
        R5900Context *ctx;
        PS2Runtime *runtime;

        uint32_t u32(uint32_t a) const
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4);
            return v;
        }
        __m128 q(uint32_t a) const
        {
            __m128 v;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK & ~0xFu), 16);
            return v;
        }
        void putq(uint32_t a, __m128 v) const { std::memcpy(rdram + (a & PS2_RAM_MASK & ~0xFu), &v, 16); }
        void call(uint32_t site, uint32_t sp) const { sotc_decomp::callGuest(rdram, ctx, runtime, sotc_decomp::jalTarget(rdram, site), site + 8u, sp); }
        uint8_t *vu0(uint32_t vi) const { return runtime->memory().getVU0Data() + ((vi & 0xFFu) << 4); }

        // The boot ELF's matrix / quaternion stack helpers, natively (each
        // as its recompiled code: the same VU0 operations, the registers it
        // leaves). Called through the guest when the jal site does not name
        // the expected function, or for PushQuaternion's reset case.
        bool is(uint32_t site, uint32_t target) const { return sotc_decomp::jalTarget(rdram, site) == target; }
        static constexpr uint32_t kQuatTop = 0x001E0F60u, kQuatStack = 0x00202AC0u;
        void pushQuaternion(uint32_t site, uint32_t sp) const
        {
            const int32_t n0 = static_cast<int32_t>(u32(kQuatTop));
            if (!is(site, 0x001BBB48u) || n0 < 0)
                return call(site, sp);
            int32_t n = n0 + 1;
            put32(kQuatTop, static_cast<uint32_t>(n));
            if (!(n < 0x40))
            {
                n = 0x3F;
                put32(kQuatTop, 0x3Fu);
            }
            const uint32_t a0 = (static_cast<uint32_t>(n) << 4) + kQuatStack;
            const __m128i v = _mm_castps_si128(q(a0 - 0x10u));
            putq(a0, _mm_castsi128_ps(v));
            SET_GPR_VEC(ctx, 2, v);
            SET_GPR_S32(ctx, 3, static_cast<int32_t>(kQuatStack));
            SET_GPR_S32(ctx, 4, static_cast<int32_t>(a0));
            SET_GPR_S32(ctx, 5, static_cast<int32_t>(a0));
        }
        void pushMatrix(uint32_t site, uint32_t sp) const
        {
            if (!is(site, 0x001B8638u))
                return call(site, sp);
            for (int r = 1; r <= 4; ++r)
            {
                std::memcpy(vu0(ctx->vi[15]), &ctx->vu0_vf[r], 16);
                ctx->vi[15] = static_cast<uint16_t>(ctx->vi[15] + 1u);
            }
        }
        void popMatrix(uint32_t site, uint32_t sp) const
        {
            if (!is(site, 0x001B8650u))
                return call(site, sp);
            for (int r = 4; r >= 1; --r)
            {
                ctx->vi[15] = static_cast<uint16_t>(ctx->vi[15] - 1u);
                std::memcpy(&ctx->vu0_vf[r], vu0(ctx->vi[15]), 16);
            }
        }
        void popQuaternion(uint32_t site, uint32_t sp) const
        {
            if (!is(site, 0x001BBC08u))
                return call(site, sp);
            int32_t n = static_cast<int32_t>(u32(kQuatTop)) - 1;
            const bool negative = n < 0;
            if (negative)
                n = 0;
            SET_GPR_S32(ctx, 2, static_cast<int32_t>(kQuatTop));
            SET_GPR_S32(ctx, 3, n);
            SET_GPR_U64(ctx, 4, negative ? 1u : 0u);
            put32(kQuatTop, static_cast<uint32_t>(n));
        }
        uint32_t currentQuaternion() const
        {
            const uint32_t v0 = kQuatStack + (u32(kQuatTop) << 4);
            SET_GPR_S32(ctx, 1, static_cast<int32_t>(kQuatStack));
            SET_GPR_S32(ctx, 2, static_cast<int32_t>(v0));
            SET_GPR_S32(ctx, 3, 0x001E0000);
            return v0;
        }
        void getCurrentQuaternion(uint32_t site, uint32_t sp) const
        {
            if (!is(site, 0x001BB868u))
                return call(site, sp);
            currentQuaternion();
        }
        void multiCurrentQuaternion(uint32_t site, uint32_t sp, uint32_t a0) const
        {
            if (!is(site, 0x001BB8A8u))
            {
                SET_GPR_U32(ctx, 4, a0);
                return call(site, sp);
            }
            const uint32_t v0 = currentQuaternion();
            __m128 *vf = ctx->vu0_vf;
            const __m128 xyz = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1)), x = _mm_castsi128_ps(_mm_set_epi32(0, 0, 0, -1)),
                         w = _mm_castsi128_ps(_mm_set_epi32(-1, 0, 0, 0));
            auto ln = [](__m128 v, int l)
            {
                switch (l)
                {
                case 0: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
                case 1: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
                case 2: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));
                default: return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3));
                }
            };
            auto yzx_ = [](__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 0, 2, 1)); };
            auto zxy_ = [](__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 1, 0, 2)); };
            vf[11] = q(v0);
            vf[12] = q(a0);
            vf[13] = PS2_VMUL(vf[11], vf[12]);
            vf[13] = _mm_blendv_ps(vf[13], PS2_VADD(vf[13], ln(vf[13], 1)), x);
            vf[13] = _mm_blendv_ps(vf[13], PS2_VADD(vf[13], ln(vf[13], 2)), x);
            vf[13] = _mm_blendv_ps(vf[13], PS2_VSUB(vf[13], ln(vf[13], 0)), w);
            vf[14] = _mm_blendv_ps(vf[14], PS2_VMUL(vf[12], ln(vf[11], 3)), xyz);
            vf[15] = _mm_blendv_ps(vf[15], PS2_VMUL(vf[11], ln(vf[12], 3)), xyz);
            ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, PS2_VMUL(yzx_(vf[12]), zxy_(vf[11])), xyz);
            vf[16] = _mm_blendv_ps(vf[16], PS2_VSUB(ctx->vu0_acc, PS2_VMUL(yzx_(vf[11]), zxy_(vf[12]))), xyz);
            vf[13] = _mm_blendv_ps(vf[13], PS2_VADD(vf[14], vf[15]), xyz);
            vf[13] = _mm_blendv_ps(vf[13], PS2_VADD(vf[13], vf[16]), xyz);
            putq(v0, vf[13]);
            SET_GPR_U32(ctx, 4, a0);
        }
        // ACC = m1 x v.x + m2 x v.y + m3 x v.z; returns ACC + m4 x v.w (vmulax / vmadday / vmaddaz / vmaddw).
        __m128 column(__m128 v, __m128 w4) const
        {
            const __m128 *vf = ctx->vu0_vf;
            __m128 acc = PS2_VMUL(vf[1], _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0)));
            acc = PS2_VADD(acc, PS2_VMUL(vf[2], _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1))));
            acc = PS2_VADD(acc, PS2_VMUL(vf[3], _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2))));
            ctx->vu0_acc = acc;
            return PS2_VADD(acc, PS2_VMUL(vf[4], w4));
        }
        void transCurrentMatrix(uint32_t site, uint32_t sp, uint32_t a0) const
        {
            SET_GPR_U32(ctx, 4, a0);
            if (!is(site, 0x001B8878u))
                return call(site, sp);
            __m128 *vf = ctx->vu0_vf;
            vf[8] = q(a0);
            vf[4] = column(vf[8], _mm_shuffle_ps(vf[0], vf[0], _MM_SHUFFLE(3, 3, 3, 3)));
        }
        void mulCurrentMatrixR(uint32_t site, uint32_t sp, uint32_t a0) const
        {
            SET_GPR_U32(ctx, 4, a0);
            if (!is(site, 0x001B8CD0u))
                return call(site, sp);
            __m128 *vf = ctx->vu0_vf;
            for (int c = 0; c < 4; ++c)
                vf[14 + c] = q(a0 + 0x10u * c);
            for (int c = 0; c < 4; ++c)
                vf[14 + c] = column(vf[14 + c], _mm_shuffle_ps(vf[14 + c], vf[14 + c], _MM_SHUFFLE(3, 3, 3, 3)));
            for (int c = 0; c < 4; ++c)
                vf[1 + c] = vf[14 + c];
        }
        void getCurrentMatrix(uint32_t site, uint32_t sp, uint32_t a0) const
        {
            SET_GPR_U32(ctx, 4, a0);
            if (!is(site, 0x001B8C90u))
                return call(site, sp);
            for (int c = 0; c < 4; ++c)
                putq(a0 + 0x10u * c, ctx->vu0_vf[1 + c]);
        }
        void put32(uint32_t a, uint32_t v) const { std::memcpy(rdram + (a & PS2_RAM_MASK), &v, 4); }
    };

    const __m128 kX = _mm_castsi128_ps(_mm_set_epi32(0, 0, 0, -1)), kY = _mm_castsi128_ps(_mm_set_epi32(0, 0, -1, 0)),
                 kZ = _mm_castsi128_ps(_mm_set_epi32(0, -1, 0, 0)), kW = _mm_castsi128_ps(_mm_set_epi32(-1, 0, 0, 0)),
                 kXyz = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1)), kYz = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, 0));

    template <int L>
    __m128 lane(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(L, L, L, L)); }
    __m128 yzx(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 0, 2, 1)); }
    __m128 zxy(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 1, 0, 2)); }

    // vmul vf15, vf14, vf14; vaddy / vaddz / vaddw .x; vrsqrt Q, vf0w, vf15x; vmulq vf15, vf14, Q.
    __m128 normalised(R5900Context *ctx, __m128 v, __m128 &vf15)
    {
        const __m128 vf0 = ctx->vu0_vf[0];
        vf15 = PS2_VMUL(v, v);
        vf15 = _mm_blendv_ps(vf15, PS2_VADD(vf15, lane<1>(vf15)), kX);
        vf15 = _mm_blendv_ps(vf15, PS2_VADD(vf15, lane<2>(vf15)), kX);
        vf15 = _mm_blendv_ps(vf15, PS2_VADD(vf15, lane<3>(vf15)), kX);
        ctx->vu0_q = ps2VuRsqrt(ctx->vu0_status, _mm_cvtss_f32(lane<3>(vf0)), _mm_cvtss_f32(vf15));
        vf15 = PS2_VMUL(v, _mm_set1_ps(ctx->vu0_q));
        return vf15;
    }

    void bone(const Ctx &g, uint32_t i, uint32_t matrices, uint32_t quats, uint32_t skeleton, uint32_t locals, uint32_t scale, uint32_t sp0)
    {
        R5900Context *ctx = g.ctx;
        const uint32_t sp = sp0 - 0xB0u;
        const uint32_t b = g.u32(skeleton + 8u) + i * 0x40u;
        const uint32_t local = locals + i * 16u;
        const uint32_t sibling = g.u32(b + 0x34u);
        g.pushQuaternion(0x010B85F0u, sp);
        g.pushMatrix(0x010B85F8u, sp);
        const uint32_t child = g.u32(b + 0x30u);
        g.multiCurrentQuaternion(0x010B8604u, sp, local);

        __m128 vf15;
        const __m128 vf0 = ctx->vu0_vf[0];
        g.putq(local, normalised(ctx, g.q(local), vf15));
        // The rotation matrix of the quaternion (rows vf14..vf16, w 0).
        __m128 vf12 = _mm_blendv_ps(ctx->vu0_vf[12], PS2_VADD(vf0, lane<3>(vf0)), kXyz);
        __m128 vf11 = g.q(local);
        const __m128 mr32w = _mm_shuffle_ps(vf0, vf0, _MM_SHUFFLE(0, 3, 2, 1));
        __m128 vf14 = _mm_blendv_ps(ctx->vu0_vf[14], mr32w, kW);
        vf15 = _mm_blendv_ps(vf15, mr32w, kW);
        __m128 vf16 = _mm_blendv_ps(ctx->vu0_vf[16], mr32w, kW);
        __m128 acc = _mm_blendv_ps(ctx->vu0_acc, PS2_VMUL(yzx(vf11), zxy(vf12)), kXyz);
        vf11 = _mm_blendv_ps(vf11, PS2_VADD(acc, PS2_VMUL(vf0, vf0)), kXyz);
        const __m128 vf10 = _mm_blendv_ps(ctx->vu0_vf[10], PS2_VADD(acc, PS2_VMUL(vf11, vf12)), kXyz);
        const __m128 vf13 = PS2_VBLEND(ctx->vu0_vf[13], PS2_VMUL(vf11, vf10), kXyz);
        acc = _mm_blendv_ps(acc, PS2_VMUL(yzx(vf11), zxy(vf10)), kXyz);
        vf15 = _mm_blendv_ps(vf15, PS2_VADD(acc, PS2_VMUL(vf10, lane<3>(vf11))), kXyz);
        vf16 = _mm_blendv_ps(vf16, PS2_VSUB(acc, PS2_VMUL(vf10, lane<3>(vf11))), kXyz);
        acc = _mm_blendv_ps(acc, PS2_VMUL(yzx(vf13), zxy(vf12)), kXyz);
        __m128 vf17 = _mm_blendv_ps(ctx->vu0_vf[17], PS2_VADD(acc, PS2_VMUL(vf13, vf12)), kXyz);
        acc = _mm_blendv_ps(acc, PS2_VMUL(yzx(vf15), zxy(vf12)), kXyz);
        vf15 = _mm_blendv_ps(vf15, PS2_VADD(acc, PS2_VMUL(vf0, vf0)), kXyz);
        vf14 = PS2_VBLEND(vf14, PS2_VSUB(vf12, vf17), kXyz);
        vf17 = _mm_blendv_ps(vf17, vf14, kYz);
        vf14 = _mm_blendv_ps(vf14, vf16, kY);
        vf16 = _mm_blendv_ps(vf16, vf15, kY);
        vf15 = _mm_blendv_ps(vf15, vf17, kY);
        vf14 = _mm_blendv_ps(vf14, vf15, kZ);
        vf15 = _mm_blendv_ps(vf15, vf16, kZ);
        vf16 = _mm_blendv_ps(vf16, vf17, kZ);
        g.putq(sp, vf14);
        g.putq(sp + 0x10u, vf15);
        g.putq(sp + 0x20u, vf16);
        g.putq(sp + 0x30u, vf0);
        __m128 vf5 = g.q(b + 0x10u);
        const __m128 vf6 = g.q(scale);
        vf5 = PS2_VBLEND(vf5, PS2_VMUL(vf5, vf6), kXyz);
        g.putq(sp + 0x40u, vf5);
        const float one = 1.0f;
        std::memcpy(g.rdram + ((sp + 0x4Cu) & PS2_RAM_MASK), &one, 4);
        ctx->f[0] = one;
        ctx->vu0_vf[5] = vf5;
        ctx->vu0_vf[6] = vf6;
        ctx->vu0_vf[10] = vf10;
        ctx->vu0_vf[11] = vf11;
        ctx->vu0_vf[12] = vf12;
        ctx->vu0_vf[13] = vf13;
        ctx->vu0_vf[14] = vf14;
        ctx->vu0_vf[15] = vf15;
        ctx->vu0_vf[16] = vf16;
        ctx->vu0_vf[17] = vf17;
        ctx->vu0_acc = acc;

        g.transCurrentMatrix(0x010B86C0u, sp, sp + 0x40u); // offset x scale
        g.mulCurrentMatrixR(0x010B86C8u, sp, sp);           // rotation
        g.getCurrentMatrix(0x010B86D8u, sp, matrices + i * 0x40u);
        g.getCurrentQuaternion(0x010B86E0u, sp);
        __m128 n15;
        g.putq(quats + i * 16u, normalised(ctx, g.q(GPR_U32(ctx, 2)), n15));
        ctx->vu0_vf[14] = g.q(GPR_U32(ctx, 2));
        ctx->vu0_vf[15] = n15;

        auto recurse = [&](uint32_t j, uint32_t ra)
        {
            SET_GPR_U32(ctx, 4, j);
            SET_GPR_U32(ctx, 5, matrices);
            SET_GPR_U32(ctx, 6, quats);
            SET_GPR_U32(ctx, 7, skeleton);
            SET_GPR_U32(ctx, 8, locals);
            SET_GPR_U32(ctx, 9, scale);
            SET_GPR_U32(ctx, 31, ra);
            SET_GPR_U32(ctx, 29, sp);
            bone(g, j, matrices, quats, skeleton, locals, scale, sp);
        };
        if (child != 0xFFFFFFFFu)
            recurse(child, 0x010B8734u);
        g.popMatrix(0x010B8734u, sp);
        g.popQuaternion(0x010B873Cu, sp);
        if (sibling != 0xFFFFFFFFu)
            recurse(sibling, 0x010B8768u);
    }

    void skeleton(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const Ctx g{rdram, ctx, runtime};
        const uint32_t sp0 = GPR_U32(ctx, 29), ra0 = GPR_U32(ctx, 31);
        bone(g, GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7), GPR_U32(ctx, 8), GPR_U32(ctx, 9), sp0);
        SET_GPR_U32(ctx, 29, sp0);
        SET_GPR_U32(ctx, 31, ra0);
        returnTo(ctx);
    }
}

SOTC_DECOMP_FUNCTION(0x010B8590u, skeletonPose, decomp_skeleton::skeleton);
