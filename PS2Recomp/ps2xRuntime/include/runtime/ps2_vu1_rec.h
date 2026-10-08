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
