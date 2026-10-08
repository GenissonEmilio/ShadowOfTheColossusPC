// Shadow of the Colossus: native version of the VU1 world-geometry loop.
//
// Microcode image f40e22b1ef541d18, program 0x05b0 (half of all VU1 work):
// 0x05b0 BAL vi14, 0x1140 runs a software-pipelined loop over a vertex batch
// (position, 12.4 texture coordinate, colour per vertex) that writes
// GIF-ready vertices [STQ, RGBA, XYZF2], marks triangles that are off screen
// (ADC) and counts those that cross the guard band. This code does what the
// microcode does from 0x05b0 up to 0x13b8, pair by pair with VU arithmetic
// (operands and results normalised, round toward zero: the caller has set
// it), and the microcode continues from 0x13b8: it stores the counters, runs
// its clipper when needed, XGKICKs and returns.
//
// The microcode relies on the VU interlocks, so in program order each read
// sees the latest write; the two lanes of a pair read before either writes.
// PS2X_VU1_NATIVE=verify compares this with the interpreter.

#include "ps2_runtime.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_native.h"

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace
{
    constexpr uint64_t kWorldImage = 0xf40e22b1ef541d18ull;
    constexpr uint32_t kWorldEntry = 0x05b0;
    constexpr uint32_t kWorldResume = 0x13b8;

    inline float norm(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t exponent = bits & 0x7F800000u;
        if (exponent == 0u)
            bits &= 0x80000000u;
        else if (exponent == 0x7F800000u)
            bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    inline int32_t ftoi(float value, float scale)
    {
        const double scaled = static_cast<double>(value) * static_cast<double>(scale);
        if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(scaled);
    }

    struct Machine
    {
        VU1State &S;
        uint8_t *data;
        uint32_t mask; // data size - 1

        float *vf(int r) { return S.vf[r]; }

        uint8_t *qword(int32_t index) { return data + ((static_cast<uint32_t>(index) * 16u) & mask); }
        void lq(float *dst, int32_t index) { std::memcpy(dst, qword(index), 16); }
        void sq(const float *src, int32_t index) { std::memcpy(qword(index), src, 16); }
        int32_t ilw(int32_t index, int lane)
        {
            uint32_t v;
            std::memcpy(&v, qword(index) + lane * 4, 4);
            return static_cast<int16_t>(v & 0xFFFFu);
        }
        void isw(int32_t index, int lane, int32_t value)
        {
            const uint32_t v = static_cast<uint16_t>(value & 0xFFFF);
            std::memcpy(qword(index) + lane * 4, &v, 4);
        }

        // ACC = vs * bc (all lanes)
        void mulaBc(int vs, float bc)
        {
            const float b = norm(bc);
            for (int c = 0; c < 4; ++c)
            {
                const float s = norm(S.vf[vs][c]);
                S.acc[c] = norm(s * b);
            }
        }
        // ACC += vs * bc
        void maddaBc(int vs, float bc)
        {
            const float b = norm(bc);
            for (int c = 0; c < 4; ++c)
            {
                const float a = norm(S.acc[c]);
                const float s = norm(S.vf[vs][c]);
                S.acc[c] = norm(a + s * b);
            }
        }
        // vd = ACC + vs * bc
        void maddBc(int vd, int vs, float bc, float out[4])
        {
            const float b = norm(bc);
            for (int c = 0; c < 4; ++c)
            {
                const float a = norm(S.acc[c]);
                const float s = norm(S.vf[vs][c]);
                out[c] = norm(a + s * b);
            }
            (void)vd;
        }
        void itof(float out[4], const float *in, float div)
        {
            for (int c = 0; c < 4; ++c)
            {
                int32_t iv;
                std::memcpy(&iv, &in[c], 4);
                out[c] = div == 1.0f ? static_cast<float>(iv) : static_cast<float>(iv) / div;
            }
        }
        void div(float den)
        {
            const float num = norm(S.vf[0][3]);
            const float d = norm(den);
            float result;
            if (d == 0.0f)
                result = std::signbit(num) != std::signbit(d) ? -FLT_MAX : FLT_MAX;
            else
                result = num / d;
            S.q = norm(result);
        }
        void clip(const float *v)
        {
            uint32_t wBits;
            std::memcpy(&wBits, &v[3], 4);
            const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
            auto exceeds = [limit](float value, uint32_t signMask)
            {
                uint32_t bits;
                std::memcpy(&bits, &value, 4);
                bits ^= signMask;
                int32_t ordered;
                std::memcpy(&ordered, &bits, 4);
                return ordered > limit;
            };
            uint32_t flags = 0;
            if (exceeds(v[0], 0u)) flags |= 0x01u;
            if (exceeds(v[0], 0x80000000u)) flags |= 0x02u;
            if (exceeds(v[1], 0u)) flags |= 0x04u;
            if (exceeds(v[1], 0x80000000u)) flags |= 0x08u;
            if (exceeds(v[2], 0u)) flags |= 0x10u;
            if (exceeds(v[2], 0x80000000u)) flags |= 0x20u;
            S.clip = ((S.clip << 6) | flags) & 0xFFFFFFu;
        }
    };

    uint32_t worldLoop(VU1State &S, uint8_t *data, uint32_t dataSize, uint64_t &cycles)
    {
        if (dataSize == 0 || (dataSize & (dataSize - 1)) != 0)
            return 0;
        Machine m{S, data, dataSize - 1u};
        int32_t *vi = S.vi;
        float *vf0 = S.vf[0];
        float t[4];

        // 0x05b0 BAL vi14, 0x1140 (delay slot 0x05b8: NOP)
        vi[14] = static_cast<int16_t>((kWorldEntry + 16u) / 8u);

        // Prologue 0x1140-0x1240
        vi[4] = static_cast<int32_t>(S.top & 0x3FFu);              // XTOP vi4
        vi[9] = 32767;                                             // IADDIU vi9, vi0, 32767
        vi[11] = static_cast<int16_t>(vi[4]);                      // IADDIU vi11, vi4, 0
        vi[3] = m.ilw(static_cast<uint16_t>(vi[4]), 0);            // ILWR.x vi3, (vi4)
        vi[6] = m.ilw(static_cast<uint16_t>(vi[4]), 3);            // ILWR.w vi6, (vi4)
        vi[2] = static_cast<int16_t>(vi[9] + 1);                   // IADDIU vi2, vi9, 1
        m.lq(S.vf[26], vi[4]);                                     // LQI vf26, (vi4++)
        vi[4] = static_cast<int16_t>(vi[4] + 1);
        vi[3] = vi[9] & vi[3];                                     // IAND vi3, vi9, vi3
        vi[5] = static_cast<int16_t>(vi[6]);                       // IADDIU vi5, vi6, 0
        vi[8] = 0;                                                 // IADDIU vi8, vi0, 0
        S.vf[29][3] = S.vf[17][0];                                 // MR32.w vf29, vf17
        vi[10] = 2;                                                // IADDI vi10, vi0, 2
        m.sq(S.vf[26], vi[5]);                                     // SQI vf26, (vi5++)
        vi[5] = static_cast<int16_t>(vi[5] + 1);
        m.lq(S.vf[18], vi[4]);                                     // LQ vf18, 0(vi4)
        m.lq(S.vf[21], vi[4] + 1);                                 // LQ vf21, 1(vi4)
        m.lq(S.vf[20], vi[4] + 2);                                 // LQ vf20, 2(vi4)
        m.mulaBc(1, S.vf[18][0]);                                  // MULAx ACC, vf1, vf18x
        m.maddaBc(2, S.vf[18][1]);                                 // MADDAy
        m.maddaBc(3, S.vf[18][2]);                                 // MADDAz
        m.maddBc(23, 4, vf0[3], t);                                // MADDw vf23, vf4, vf0w
        std::memcpy(S.vf[23], t, 16);
        m.itof(S.vf[25], S.vf[21], 4096.0f);                       // ITOF12 vf25, vf21
        m.mulaBc(5, S.vf[18][0]);                                  // MULAx ACC, vf5, vf18x
        m.maddaBc(6, S.vf[18][1]);                                 // MADDAy
        {
            const float den = S.vf[23][3];                         // DIV Q, vf0w, vf23w (same pair)
            m.maddaBc(7, S.vf[18][2]);                             // MADDAz
            m.div(den);
        }
        m.maddBc(28, 8, vf0[3], t);                                // MADDw vf28, vf8, vf0w | SQ vf25, 1(vi4)
        m.sq(S.vf[25], vi[4] + 1);
        std::memcpy(S.vf[28], t, 16);
        m.itof(S.vf[24], S.vf[20], 1.0f);                          // ITOF0 vf24, vf20 | LOI 255
        S.i = 255.0f;
        vi[4] = static_cast<int16_t>(vi[4] + 3);                   // IADDIU vi4, vi4, 3
        S.vf[25][2] = vf0[3];                                      // MR32.zw vf25, vf0
        S.vf[25][3] = vf0[0];
        m.sq(S.vf[24], vi[4] - 1);                                 // SQ vf24, -1(vi4)
        {
            const float q = norm(S.q);                             // MULq.xyz vf26, vf23, q | WAITQ
            for (int c = 0; c < 3; ++c)
                S.vf[26][c] = norm(norm(S.vf[23][c]) * q);
        }
        {
            const float q = norm(S.q);                             // MULq.xyz vf25, vf25, q | LQ vf18, 0(vi4)
            for (int c = 0; c < 3; ++c)
                S.vf[25][c] = norm(norm(S.vf[25][c]) * q);
            m.lq(S.vf[18], vi[4]);
        }
        S.acc[3] = norm(norm(S.vf[17][3]) * norm(vf0[3]));         // MULA.w ACC, vf17, vf0
        m.clip(S.vf[28]);                                          // CLIPw.xyz vf28, vf28w

        uint64_t vertices = 0;
        for (;;)
        {
            ++vertices;
            // 0x1248 MADDq.w vf26, vf29, q | LQ vf21, 1(vi4)
            S.vf[26][3] = norm(norm(S.acc[3]) + norm(S.vf[29][3]) * norm(S.q));
            m.lq(S.vf[21], vi[4] + 1);
            // 0x1250 MULAx ACC, vf1, vf18x | SQ vf20, 1(vi5)
            m.sq(S.vf[20], vi[5] + 1);
            m.mulaBc(1, S.vf[18][0]);
            // 0x1258 MADDAy | IADDI vi10, vi10, -1
            m.maddaBc(2, S.vf[18][1]);
            vi[10] = static_cast<int16_t>(vi[10] - 1);
            // 0x1260 MADDAz | FCAND vi1, 0x3ffff
            vi[1] = (S.clip & 0x3FFFFu) != 0u ? 1 : 0;
            m.maddaBc(3, S.vf[18][2]);
            // 0x1268 MADDw vf23, vf4, vf0w | LQ vf20, 2(vi4)
            m.maddBc(23, 4, vf0[3], t);
            m.lq(S.vf[20], vi[4] + 2);
            std::memcpy(S.vf[23], t, 16);
            // 0x1270 MINIi.w vf26, vf26, i | SQ vf25, 0(vi5)
            m.sq(S.vf[25], vi[5]);
            {
                const float iv = norm(S.i), sw = norm(S.vf[26][3]);
                S.vf[26][3] = sw < iv ? sw : iv;
            }
            // 0x1278 ITOF12 vf25, vf21 | IADDIU vi5, vi5, 3
            m.itof(S.vf[25], S.vf[21], 4096.0f);
            vi[5] = static_cast<int16_t>(vi[5] + 3);
            // 0x1280 MULAx ACC, vf5, vf18x | IADDIU vi4, vi4, 3
            m.mulaBc(5, S.vf[18][0]);
            vi[4] = static_cast<int16_t>(vi[4] + 3);
            // 0x1288 ITOF0 vf24, vf20 | DIV Q, vf0w, vf23w
            m.itof(S.vf[24], S.vf[20], 1.0f);
            m.div(S.vf[23][3]);
            // 0x1290 MAXx.w vf26, vf26, vf0x | IADDI vi3, vi3, -1
            {
                const float bc = norm(vf0[0]), sw = norm(S.vf[26][3]);
                S.vf[26][3] = sw > bc ? sw : bc;
            }
            vi[3] = static_cast<int16_t>(vi[3] - 1);
            // 0x1298 MADDAy ACC, vf6, vf18y | SQ vf25, -2(vi4)
            m.sq(S.vf[25], vi[4] - 2);
            m.maddaBc(6, S.vf[18][1]);
            // 0x12a0 MADDAz ACC, vf7, vf18z | MR32.zw vf25, vf0
            m.maddaBc(7, S.vf[18][2]);
            S.vf[25][2] = vf0[3];
            S.vf[25][3] = vf0[0];
            // 0x12a8 MADDw vf28, vf8, vf0w | SQ vf24, -1(vi4)
            m.maddBc(28, 8, vf0[3], t);
            m.sq(S.vf[24], vi[4] - 1);
            std::memcpy(S.vf[28], t, 16);
            // 0x12b0 FTOI4 vf27, vf26 | IBNE vi1, vi0, 0x12e8
            for (int c = 0; c < 4; ++c)
            {
                const int32_t iv = ftoi(norm(S.vf[26][c]), 16.0f);
                std::memcpy(&S.vf[27][c], &iv, 4);
            }
            const bool offScreen = vi[1] != 0;
            // 0x12b8 (delay slot) MULA.w ACC, vf17, vf0 | LQ vf18, 0(vi4)
            S.acc[3] = norm(norm(S.vf[17][3]) * norm(vf0[3]));
            m.lq(S.vf[18], vi[4]);

            if (!offScreen)
            {
                // 0x12c0 MULq.xyz vf26, vf23, q | WAITQ
                {
                    const float q = norm(S.q);
                    for (int c = 0; c < 3; ++c)
                        S.vf[26][c] = norm(norm(S.vf[23][c]) * q);
                }
                // 0x12c8 MULq.xyz vf25, vf25, q | IBNE vi3, vi0, 0x1248
                {
                    const float q = norm(S.q);
                    for (int c = 0; c < 3; ++c)
                        S.vf[25][c] = norm(norm(S.vf[25][c]) * q);
                }
                const bool more = vi[3] != 0;
                // 0x12d0 (delay slot) CLIPw.xyz vf28, vf28w | SQ vf27, -1(vi5)
                m.sq(S.vf[27], vi[5] - 1);
                m.clip(S.vf[28]);
                if (more)
                    continue;
                break; // 0x12d8 B 0x13b8
            }

            // 0x12e8 MTIR vi9, vf27w
            {
                uint32_t bits;
                std::memcpy(&bits, &S.vf[27][3], 4);
                vi[9] = static_cast<int16_t>(bits & 0xFFFFu);
            }
            // 0x12f0 FCAND vi1, 0x20820
            vi[1] = (S.clip & 0x20820u) != 0u ? 1 : 0;
            // 0x1300 MULq.xyz vf26, vf23, q | ISW.w vi1, -6(vi4)
            m.isw(vi[4] - 6, 3, vi[1]);
            {
                const float q = norm(S.q);
                for (int c = 0; c < 3; ++c)
                    S.vf[26][c] = norm(norm(S.vf[23][c]) * q);
            }
            // 0x1308 MULq.xyz vf25, vf25, q | IOR vi9, vi9, vi2
            {
                const float q = norm(S.q);
                for (int c = 0; c < 3; ++c)
                    S.vf[25][c] = norm(norm(S.vf[25][c]) * q);
            }
            vi[9] = vi[9] | vi[2];
            // 0x1310 IBGEZ vi10, 0x13a8 ; 0x1318 (delay slot) MFIR.w vf27, vi9
            const bool stripStart = static_cast<int16_t>(vi[10]) >= 0;
            {
                const int32_t v = static_cast<int16_t>(vi[9] & 0xFFFF);
                std::memcpy(&S.vf[27][3], &v, 4);
            }
            if (!stripStart)
            {
                // Triangle wholly outside one plane: stays ADC. Otherwise it
                // crosses the guard band: flag it for the clipper.
                static const uint32_t kPlanes[5] = {0xff7df7u, 0xffbefbu, 0xffdf7du, 0xffefbeu, 0xfdf7dfu};
                bool outside = false;
                for (uint32_t mask : kPlanes)
                {
                    vi[1] = ((S.clip | mask) == 0xFFFFFFu) ? 1 : 0; // FCOR vi1, mask
                    if (vi[1] != 0)
                    {
                        outside = true;
                        break;
                    }
                }
                if (!outside)
                {
                    m.isw(vi[5] - 3, 3, vi[2]);                     // ISW.w vi2, -3(vi5)
                    vi[8] = static_cast<int16_t>(vi[8] + 1);        // IADDIU vi8, vi8, 1
                }
            }
            // 0x13a8 IBNE vi3, vi0, 0x1248 ; 0x13b0 (delay slot) CLIPw | SQ vf27, -1(vi5)
            const bool more = vi[3] != 0;
            m.sq(S.vf[27], vi[5] - 1);
            m.clip(S.vf[28]);
            if (!more)
                break;
        }
        cycles = 24u + vertices * 13u;
        return kWorldResume;
    }

    vu1native::Registrar g_worldLoop(kWorldImage, kWorldEntry, worldLoop);
}
