#pragma once

// Support code for ahead-of-time recompiled VU1 microcode.
//
// tools generate one function per microcode image (VU1Interpreter::
// generateRecompiled). Each instruction pair becomes straight-line code that
// performs exactly the steps of VU1Interpreter::run() with the decode work
// resolved at generation time. Return values: 0 = cycle budget exhausted,
// 1 = program ended, 2 = reached a pc that was not compiled (the interpreter
// continues from the current state).

#include "runtime/ps2_vu1.h"
#include "runtime/ps2_stats.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace vu1rec
{
    enum : int
    {
        Budget = 0,
        Ended = 1,
        Bail = 2
    };

    struct Registrar
    {
        Registrar(uint64_t hash, VU1Interpreter::RecompiledFn fn)
        {
            VU1Interpreter::registerRecompiled(hash, fn);
        }
    };

    // VU1Interpreter::normalizeOperand / the value part of normalizeResult:
    // denormals flush to signed zero, Inf/NaN clamp to signed FLT_MAX.
    __attribute__((always_inline)) inline float norm(float value)
    {
#if defined(PS2X_VU1_TRUST_FPU)
        // The VU1 threads run with round-toward-zero and flush-to-zero
        // (ps2SetEeFloatingPointMode): the FPU itself gives denormals as
        // signed zero and overflow as +-FLT_MAX. Only Inf/NaN inputs, absent
        // from game data, would differ (PS2X_VU1_NATIVE=verify checks).
        return value;
#endif
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

    template <class Ready>
    inline bool stall(VU1Interpreter &I, uint64_t ready, uint64_t budgetEnd,
                      uint64_t &stallCycles, Ready &&recompute)
    {
        do
        {
            if (ready >= budgetEnd)
            {
                I.advanceTo(budgetEnd);
                return false;
            }
            stallCycles += ready - I.m_cycle;
            I.advanceTo(ready);
            ready = recompute();
        } while (ready > I.m_cycle);
        return true;
    }

    // End of a pair once a branch, halt or E bit is involved; mirrors the tail
    // of VU1Interpreter::run(). S.pc already holds the sequential next pc.
    // Returns true when the program ended.
    inline bool tail(VU1Interpreter &I, bool dBit, bool tBit, bool eBit, bool isBranch)
    {
        VU1State &S = I.m_state;
        if (S.branchPending)
        {
            if (S.branchDelay == 0u)
            {
                S.pc = S.branchTarget & I.microAddressMask();
                S.branchPending = false;
            }
            else
            {
                --S.branchDelay;
            }
        }

        const bool dHalt = dBit && S.dBitEnabled;
        const bool tHalt = tBit && S.tBitEnabled;
        const bool haltBit = dHalt || tHalt;
        const bool haltBranch = haltBit && isBranch;
        bool ended = false;
        if (S.haltAfterDelaySlot)
        {
            S.stoppedByD = I.m_pendingHaltD;
            S.stoppedByT = I.m_pendingHaltT;
            ended = true;
        }
        else if (S.ebit)
            ended = true;
        else if (haltBit && !haltBranch)
        {
            S.stoppedByD = dHalt;
            S.stoppedByT = tHalt;
            ended = true;
        }
        else if (eBit)
            S.ebit = true;
        else if (haltBranch)
        {
            S.haltAfterDelaySlot = true;
            I.m_pendingHaltD = dHalt;
            I.m_pendingHaltT = tHalt;
        }
        I.advanceOneCycle();
        return ended;
    }

    // Lower-instruction helpers, same semantics as VU1Interpreter::execLower.
    template <uint8_t DEST>
    inline void applyDest(float *dst, const float *src)
    {
        if (DEST & 0x8u) dst[0] = src[0];
        if (DEST & 0x4u) dst[1] = src[1];
        if (DEST & 0x2u) dst[2] = src[2];
        if (DEST & 0x1u) dst[3] = src[3];
    }

    template <uint8_t DEST>
    inline void loadQ(float *dst, const uint8_t *vuData, uint32_t dataSize, uint32_t qword)
    {
        const uint32_t addr = (qword * 16u) & (dataSize - 1u);
        if (addr + 16u <= dataSize)
        {
            float tmp[4];
            std::memcpy(tmp, vuData + addr, 16);
            applyDest<DEST>(dst, tmp);
        }
    }

    // Stores land immediately (see VU1Interpreter::queueStore; recompiled
    // code never runs in the exact pipeline mode).
    template <uint8_t DEST>
    inline void storeWords(VU1Interpreter &I, uint8_t *vuData, uint32_t dataSize, uint32_t qword, const uint32_t *words)
    {
        const uint32_t addr = (qword * 16u) & (dataSize - 1u);
        if (addr + 16u > dataSize)
            return;
        I.catchUpXgkick();
        uint8_t *dst = vuData + addr;
        if (DEST & 0x8u) std::memcpy(dst + 0, &words[0], 4);
        if (DEST & 0x4u) std::memcpy(dst + 4, &words[1], 4);
        if (DEST & 0x2u) std::memcpy(dst + 8, &words[2], 4);
        if (DEST & 0x1u) std::memcpy(dst + 12, &words[3], 4);
    }

    template <uint8_t DEST>
    inline void storeQ(VU1Interpreter &I, uint8_t *vuData, uint32_t dataSize, uint32_t qword, const float *src)
    {
        uint32_t words[4];
        std::memcpy(words, src, 16);
        storeWords<DEST>(I, vuData, dataSize, qword, words);
    }

    template <uint8_t DEST>
    inline void storeVi(VU1Interpreter &I, uint8_t *vuData, uint32_t dataSize, uint32_t qword, int32_t vi)
    {
        const uint32_t v = static_cast<uint32_t>(static_cast<uint16_t>(vi & 0xFFFF));
        const uint32_t words[4] = {v, v, v, v};
        storeWords<DEST>(I, vuData, dataSize, qword, words);
    }

    // Fast regions: CLIPw.xyz (fs against fs/ft w), DIV and RSQRT as the
    // interpreter computes them, written at once.
    inline uint32_t fastClip(uint32_t clip, const float *v, const float *wv)
    {
        uint32_t wBits;
        std::memcpy(&wBits, &wv[3], 4);
        const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
        auto ex = [limit](float value, uint32_t m)
        {
            uint32_t b;
            std::memcpy(&b, &value, 4);
            b ^= m;
            int32_t o;
            std::memcpy(&o, &b, 4);
            return o > limit;
        };
        uint32_t f = 0;
        if (ex(v[0], 0u)) f |= 1u;
        if (ex(v[0], 0x80000000u)) f |= 2u;
        if (ex(v[1], 0u)) f |= 4u;
        if (ex(v[1], 0x80000000u)) f |= 8u;
        if (ex(v[2], 0u)) f |= 16u;
        if (ex(v[2], 0x80000000u)) f |= 32u;
        return ((clip << 6) | f) & 0xFFFFFFu;
    }
    inline float fastDiv(float n, float d)
    {
        const float num = norm(n), den = norm(d);
        if (den == 0.0f)
            return std::signbit(num) != std::signbit(den) ? -3.40282347e38f : 3.40282347e38f;
        return norm(num / den);
    }
    inline float fastRsqrt(float n, float r)
    {
        const float num = norm(n), den = std::sqrt(std::fabs(norm(r)));
        if (den == 0.0f)
            return std::signbit(num) ? -3.40282347e38f : 3.40282347e38f;
        return norm(num / den);
    }

    // VU1Interpreter::cheapFmacFlags + updateFmacFlags: normalises the dest
    // lanes of res and gives the MAC and status (Z S U O) bits they set.
    template <uint8_t DEST>
    inline void fastFmacFlags(float *res, uint32_t &mac, uint32_t &status)
    {
        mac = 0u;
        status = 0u;
        for (uint32_t c = 0; c < 4u; ++c)
        {
            const uint32_t lane = 0x8u >> c;
            if ((DEST & lane) == 0u)
                continue;
            uint32_t bits;
            std::memcpy(&bits, &res[c], 4);
            const uint32_t sign = bits & 0x80000000u, magnitude = bits & 0x7FFFFFFFu, exponent = (bits >> 23) & 0xFFu;
            uint32_t flags = sign ? 0x2u : 0u;
            if (magnitude == 0u)
                flags |= 0x1u;
            else if (exponent == 0u)
            {
                flags |= 0x5u;
                bits = sign;
            }
            else if (exponent == 0xFFu)
            {
                flags |= 0x8u;
                bits = sign | 0x7F7FFFFFu;
            }
            if ((bits & 0x7FFFFFFFu) == 0x7F7FFFFFu)
                flags |= 0x8u;
            std::memcpy(&res[c], &bits, 4);
            if (flags & 0x1u) mac |= lane;
            if (flags & 0x2u) mac |= lane << 4;
            if (flags & 0x4u) mac |= lane << 8;
            if (flags & 0x8u) mac |= lane << 12;
            status |= flags;
        }
    }

    // MAC/status flags of a fast region, delivered as the interpreter's flag
    // pipeline does: an FMAC's 4 pairs after it, a divide's D/I bits with Q.
    // Pairs stand in for cycles (stalls only make the hardware later).
    struct FastFlags
    {
        struct Entry
        {
            uint64_t at;
            uint32_t mac, status;
        };
        Entry fmac[16];
        uint32_t head = 0, tail = 0;
        uint64_t diAt = 0;
        uint32_t di = 0;
        bool diValid = false;

        void push(uint64_t at, uint32_t mac, uint32_t status)
        {
            fmac[tail & 15u] = {at, mac, status};
            ++tail;
        }
        void pushDi(uint64_t at, uint32_t bits)
        {
            diAt = at;
            di = bits & 0x30u;
            diValid = true;
        }
        void commit(VU1State &S, uint64_t now)
        {
            while (head != tail && fmac[head & 15u].at <= now)
            {
                const Entry &e = fmac[head & 15u];
                S.mac = e.mac;
                const uint32_t current = e.status & 0xFu;
                S.status = (S.status & 0xFF0u) | current | (current << 6);
                ++head;
            }
            if (diValid && diAt <= now)
            {
                S.status = (S.status & 0xFCFu) | di | (di << 6);
                diValid = false;
            }
        }
    };

    inline uint32_t divStatus(float n, float d)
    {
        const float num = norm(n), den = norm(d);
        return den == 0.0f ? (num == 0.0f ? 0x10u : 0x20u) : 0u;
    }
    inline uint32_t rsqrtStatus(float n, float r)
    {
        const float num = norm(n), radicand = norm(r);
        const float den = std::sqrt(std::fabs(radicand));
        uint32_t di = radicand < 0.0f ? 0x10u : 0u;
        if (den == 0.0f)
            di = num == 0.0f ? 0x10u : 0x20u;
        return di;
    }

    // Fast regions (vu1fast_*.cpp) run from a program's start, before any
    // XGKICK: no transfer to catch up with. Same call shape as above.
    struct FastNoKick
    {
    };
    template <uint8_t DEST>
    inline void storeWords(FastNoKick, uint8_t *vuData, uint32_t dataSize, uint32_t qword, const uint32_t *words)
    {
        const uint32_t addr = (qword * 16u) & (dataSize - 1u);
        if (addr + 16u > dataSize)
            return;
        uint8_t *dst = vuData + addr;
        if (DEST & 0x8u) std::memcpy(dst + 0, &words[0], 4);
        if (DEST & 0x4u) std::memcpy(dst + 4, &words[1], 4);
        if (DEST & 0x2u) std::memcpy(dst + 8, &words[2], 4);
        if (DEST & 0x1u) std::memcpy(dst + 12, &words[3], 4);
    }
    template <uint8_t DEST>
    inline void storeQ(FastNoKick I, uint8_t *vuData, uint32_t dataSize, uint32_t qword, const float *src)
    {
        uint32_t words[4];
        std::memcpy(words, src, 16);
        storeWords<DEST>(I, vuData, dataSize, qword, words);
    }
    template <uint8_t DEST>
    inline void storeVi(FastNoKick I, uint8_t *vuData, uint32_t dataSize, uint32_t qword, int32_t vi)
    {
        const uint32_t v = static_cast<uint32_t>(static_cast<uint16_t>(vi & 0xFFFF));
        const uint32_t words[4] = {v, v, v, v};
        storeWords<DEST>(I, vuData, dataSize, qword, words);
    }

    inline int32_t loadVi(const uint8_t *vuData, uint32_t dataSize, uint32_t qword, uint32_t component, int32_t old)
    {
        const uint32_t addr = (qword * 16u) & (dataSize - 1u);
        if (addr + 16u > dataSize)
            return old;
        uint32_t v;
        std::memcpy(&v, vuData + addr + component * 4u, 4);
        return static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFF));
    }

    // VU1Interpreter's vuFloatToInt.
    inline int32_t floatToInt(float value, float scale)
    {
        const double scaled = static_cast<double>(value) * static_cast<double>(scale);
        if (scaled >= 2147483647.0)
            return 2147483647;
        if (scaled <= -2147483648.0)
            return static_cast<int32_t>(0x80000000u);
        return static_cast<int32_t>(scaled);
    }

    inline void resetZeroRegs(VU1State &S)
    {
        S.vf[0][0] = 0.0f;
        S.vf[0][1] = 0.0f;
        S.vf[0][2] = 0.0f;
        S.vf[0][3] = 1.0f;
        S.vi[0] = 0;
    }

    inline void flushStats(uint64_t instructions, uint64_t stallCycles)
    {
        ps2_stats::vu1Instr.fetch_add(instructions, std::memory_order_relaxed);
        if (stallCycles)
            ps2_stats::vu1StallCycles.fetch_add(stallCycles, std::memory_order_relaxed);
    }
}
