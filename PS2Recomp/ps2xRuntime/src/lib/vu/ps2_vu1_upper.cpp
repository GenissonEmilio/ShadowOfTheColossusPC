#include "runtime/ps2_vu1.h"
#include "ps2_vu1_detail.h"

#include <array>
#include <cmath>
#if defined(USE_SSE2NEON) || defined(__aarch64__) || defined(__arm64__)
#include "sse2neon.h"
#else
#include <immintrin.h>
#endif
#include <cstring>
#include <limits>

namespace
{
    // Vector form of VU1Interpreter::normalizeOperand: denormals become signed
    // zero and Inf/NaN become signed FLT_MAX.
    inline std::array<float, 4> normalizeOperands4(const float *in)
    {
        const __m128i bits = _mm_loadu_si128(reinterpret_cast<const __m128i *>(in));
        const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        const __m128i exponent = _mm_and_si128(_mm_srli_epi32(bits, 23), _mm_set1_epi32(0xFF));
        const __m128i isZeroExp = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
        const __m128i isMaxExp = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0xFF));
        __m128i result = bits;
        result = _mm_or_si128(_mm_andnot_si128(isZeroExp, result), _mm_and_si128(isZeroExp, sign));
        const __m128i clamped = _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF));
        result = _mm_or_si128(_mm_andnot_si128(isMaxExp, result), _mm_and_si128(isMaxExp, clamped));
        std::array<float, 4> out;
        _mm_storeu_si128(reinterpret_cast<__m128i *>(out.data()), result);
        return out;
    }

    int32_t vuFloatToInt(float value, float scale)
    {
        const double scaled = static_cast<double>(value) * static_cast<double>(scale);
        if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(scaled);
    }
}

// ============================================================================
// Upper instructions (FMAC pipeline)
// ============================================================================
void VU1Interpreter::execUpper(uint32_t instr)
{
    m_currentUpperInstruction = instr;
    uint8_t dest = DEST(instr);
    uint8_t ft = FT(instr);
    uint8_t fs = FS(instr);
    uint8_t fd = FD(instr);
    uint8_t op = instr & 0x3F;

    float *vd = m_state.vf[fd];
    float result[4];

    // Upper opcode decoding (bits 5:0 of upper word)
    switch (op)
    {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03: // ADDbc
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const float bc = normalizeOperand(m_state.vf[ft][op & 3u]);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07: // SUBbc
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const float bc = normalizeOperand(m_state.vf[ft][op & 3u]);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B: // MADDbc
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const auto acc = normalizeOperands4(m_state.acc);
        const float bc = normalizeOperand(m_state.vf[ft][op & 3u]);
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x0F: // MSUBbc
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const auto acc = normalizeOperands4(m_state.acc);
        const float bc = normalizeOperand(m_state.vf[ft][op & 3u]);
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13: // MAXbc
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const float bc = normalizeOperand(m_state.vf[ft][op & 3u]);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > bc) ? vs[c] : bc;
        applyDest(vd, result, dest);
        return;
    }
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17: // MINIbc
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const float bc = normalizeOperand(m_state.vf[ft][op & 3u]);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < bc) ? vs[c] : bc;
        applyDest(vd, result, dest);
        return;
    }
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B: // MULbc
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const float bc = normalizeOperand(m_state.vf[ft][op & 3u]);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * bc;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x1C: // MULq
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const float q = normalizeOperand(m_state.q);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * q;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x1D: // MAXi
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const float i = normalizeOperand(m_state.i);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > i) ? vs[c] : i;
        applyDest(vd, result, dest);
        return;
    }
    case 0x1E: // MULi
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const float i = normalizeOperand(m_state.i);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * i;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x1F: // MINIi
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const float i = normalizeOperand(m_state.i);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < i) ? vs[c] : i;
        applyDest(vd, result, dest);
        return;
    }
    case 0x20: // ADDq
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const float q = normalizeOperand(m_state.q);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + q;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x21: // MADDq
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const auto acc = normalizeOperands4(m_state.acc);
        const float q = normalizeOperand(m_state.q);
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * q;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x22: // ADDi
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const float i = normalizeOperand(m_state.i);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + i;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x23: // MADDi
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const auto acc = normalizeOperands4(m_state.acc);
        const float i = normalizeOperand(m_state.i);
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * i;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x24: // SUBq
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const float q = normalizeOperand(m_state.q);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - q;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x25: // MSUBq
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const auto acc = normalizeOperands4(m_state.acc);
        const float q = normalizeOperand(m_state.q);
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * q;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x26: // SUBi
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const float i = normalizeOperand(m_state.i);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - i;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x27: // MSUBi
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const auto acc = normalizeOperands4(m_state.acc);
        const float i = normalizeOperand(m_state.i);
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * i;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x28: // ADD
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const auto vt = normalizeOperands4(m_state.vf[ft]);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] + vt[c];
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x29: // MADD
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const auto vt = normalizeOperands4(m_state.vf[ft]);
        const auto acc = normalizeOperands4(m_state.acc);
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] + vs[c] * vt[c];
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x2A: // MUL
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const auto vt = normalizeOperands4(m_state.vf[ft]);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] * vt[c];
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x2B: // MAX
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const auto vt = normalizeOperands4(m_state.vf[ft]);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] > vt[c]) ? vs[c] : vt[c];
        applyDest(vd, result, dest);
        return;
    }
    case 0x2C: // SUB
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const auto vt = normalizeOperands4(m_state.vf[ft]);
        for (int c = 0; c < 4; c++)
            result[c] = vs[c] - vt[c];
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x2D: // MSUB
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const auto vt = normalizeOperands4(m_state.vf[ft]);
        const auto acc = normalizeOperands4(m_state.acc);
        for (int c = 0; c < 4; c++)
            result[c] = acc[c] - vs[c] * vt[c];
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x2E: // OPMSUB
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const auto vt = normalizeOperands4(m_state.vf[ft]);
        const auto acc = normalizeOperands4(m_state.acc);
        result[0] = acc[0] - vs[1] * vt[2];
        result[1] = acc[1] - vs[2] * vt[0];
        result[2] = acc[2] - vs[0] * vt[1];
        result[3] = 0.0f;
        applyFmacDest(vd, result, dest);
        return;
    }
    case 0x2F: // MINI
    {
        const auto vs = normalizeOperands4(m_state.vf[fs]);
        const auto vt = normalizeOperands4(m_state.vf[ft]);
        for (int c = 0; c < 4; c++)
            result[c] = (vs[c] < vt[c]) ? vs[c] : vt[c];
        applyDest(vd, result, dest);
        return;
    }

    // Upper special group (low op 0x3C..0x3F).
    // Like lower1 special, the real selector is not just bits 5:0.  Dobie decodes:
    //   op = (instr & 0x3) | ((instr >> 4) & 0x7C)
    // Several instructions in this group also use FT as the destination, not FD.
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F:
    {
        const uint8_t specialOp = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
        float *vtDest = m_state.vf[ft];

        switch (specialOp)
        {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03: // ADDAbc
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const float bc = normalizeOperand(m_state.vf[ft][specialOp & 3u]);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07: // SUBAbc
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const float bc = normalizeOperand(m_state.vf[ft][specialOp & 3u]);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B: // MADDAbc
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const auto acc = normalizeOperands4(m_state.acc);
            const float bc = normalizeOperand(m_state.vf[ft][specialOp & 3u]);
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x0C:
        case 0x0D:
        case 0x0E:
        case 0x0F: // MSUBAbc
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const auto acc = normalizeOperands4(m_state.acc);
            const float bc = normalizeOperand(m_state.vf[ft][specialOp & 3u]);
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x10: // ITOF0
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv);
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x11: // ITOF4
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv) / 16.0f;
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x12: // ITOF12
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv) / 4096.0f;
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x13: // ITOF15
            for (int c = 0; c < 4; c++)
            {
                int32_t iv;
                std::memcpy(&iv, &m_state.vf[fs][c], 4);
                result[c] = static_cast<float>(iv) / 32768.0f;
            }
            applyDest(vtDest, result, dest);
            return;
        case 0x14: // FTOI0
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = vuFloatToInt(vs[c], 1.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        }
        case 0x15: // FTOI4
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = vuFloatToInt(vs[c], 16.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        }
        case 0x16: // FTOI12
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = vuFloatToInt(vs[c], 4096.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        }
        case 0x17: // FTOI15
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            for (int c = 0; c < 4; c++)
            {
                int32_t iv = vuFloatToInt(vs[c], 32768.0f);
                std::memcpy(&result[c], &iv, 4);
            }
            applyDest(vtDest, result, dest);
            return;
        }
        case 0x18:
        case 0x19:
        case 0x1A:
        case 0x1B: // MULAbc
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const float bc = normalizeOperand(m_state.vf[ft][specialOp & 3u]);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * bc;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x1C: // MULAq
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const float q = normalizeOperand(m_state.q);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * q;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x1D: // ABS
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            for (int c = 0; c < 4; c++)
                result[c] = std::fabs(vs[c]);
            applyDest(vtDest, result, dest);
            return;
        }
        case 0x1E: // MULAi
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const float i = normalizeOperand(m_state.i);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * i;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x1F: // CLIP
        {
            uint32_t wBits = 0u;
            std::memcpy(&wBits, &m_state.vf[ft][3], sizeof(wBits));
            const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;

            const auto exceedsClipPlane = [limit](float value, uint32_t signMask)
            {
                uint32_t bits = 0u;
                std::memcpy(&bits, &value, sizeof(bits));
                bits ^= signMask;
                int32_t orderedBits = 0;
                std::memcpy(&orderedBits, &bits, sizeof(orderedBits));
                return orderedBits > limit;
            };

            uint32_t flags = 0u;
            if (exceedsClipPlane(m_state.vf[fs][0], 0x00000000u))
                flags |= 0x01u;
            if (exceedsClipPlane(m_state.vf[fs][0], 0x80000000u))
                flags |= 0x02u;
            if (exceedsClipPlane(m_state.vf[fs][1], 0x00000000u))
                flags |= 0x04u;
            if (exceedsClipPlane(m_state.vf[fs][1], 0x80000000u))
                flags |= 0x08u;
            if (exceedsClipPlane(m_state.vf[fs][2], 0x00000000u))
                flags |= 0x10u;
            if (exceedsClipPlane(m_state.vf[fs][2], 0x80000000u))
                flags |= 0x20u;
            queueClip(flags);
            return;
        }
        case 0x20: // ADDAq
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const float q = normalizeOperand(m_state.q);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + q;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x21: // MADDAq
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const auto acc = normalizeOperands4(m_state.acc);
            const float q = normalizeOperand(m_state.q);
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * q;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x22: // ADDAi
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const float i = normalizeOperand(m_state.i);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + i;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x23: // MADDAi
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const auto acc = normalizeOperands4(m_state.acc);
            const float i = normalizeOperand(m_state.i);
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * i;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x24: // SUBAq
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const float q = normalizeOperand(m_state.q);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - q;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x25: // MSUBAq
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const auto acc = normalizeOperands4(m_state.acc);
            const float q = normalizeOperand(m_state.q);
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * q;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x26: // SUBAi
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const float i = normalizeOperand(m_state.i);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - i;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x27: // MSUBAi
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const auto acc = normalizeOperands4(m_state.acc);
            const float i = normalizeOperand(m_state.i);
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * i;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x28: // ADDA
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const auto vt = normalizeOperands4(m_state.vf[ft]);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] + vt[c];
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x29: // MADDA
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const auto vt = normalizeOperands4(m_state.vf[ft]);
            const auto acc = normalizeOperands4(m_state.acc);
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] + vs[c] * vt[c];
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x2A: // MULA
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const auto vt = normalizeOperands4(m_state.vf[ft]);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] * vt[c];
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x2C: // SUBA
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const auto vt = normalizeOperands4(m_state.vf[ft]);
            for (int c = 0; c < 4; c++)
                result[c] = vs[c] - vt[c];
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x2D: // MSUBA
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const auto vt = normalizeOperands4(m_state.vf[ft]);
            const auto acc = normalizeOperands4(m_state.acc);
            for (int c = 0; c < 4; c++)
                result[c] = acc[c] - vs[c] * vt[c];
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x2E: // OPMULA
        {
            const auto vs = normalizeOperands4(m_state.vf[fs]);
            const auto vt = normalizeOperands4(m_state.vf[ft]);
            result[0] = vs[1] * vt[2];
            result[1] = vs[2] * vt[0];
            result[2] = vs[0] * vt[1];
            result[3] = 0.0f;
            applyFmacDestAcc(result, dest);
            return;
        }
        case 0x2F:
        case 0x30: // NOP
            return;
        default:
            reportReservedInstruction(true, instr);
            return;
        }
    }

    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    default:
        reportReservedInstruction(true, instr);
        return;
    }
}
