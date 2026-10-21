// isysWakeupSubThread (0x1c9a40) and isysSleepSubThread (0x1c9e4c): the
// game's sub-threads, coroutines on stacks of their own (1.85% of the
// Vita's game thread recompiled, throttled attract demo: ~0x2c0 bytes of
// registers saved and loaded one instruction at a time, twice a round trip).
//
// Wake (a0 = the object; its type at +8 picks the record, +0x20 or none,
// through the table at 0x1e42c0): unless the record's +0x1c says the
// sub-thread has ended, the caller's registers go into a 0x2c0 frame on its
// stack (f0-f31 words, the GPRs' 128 bits but at / k0 / k1 / gp / sp / ra,
// hi / lo / sa / hi1 / lo1), its sp to 0x270bb8; the record's stack (+0x10)
// and resume address (+4) to 0x270bbc / 0x270bc8; the sub-thread's
// registers come back from its frame and it is called at its resume
// address. When the sub-thread sleeps, the sleep saves its registers the
// same way, loads the caller's, records the resume address and the stack in
// the record and returns to the wake's caller (the recompiled code unwinds
// through the nested call: callGuest returns false). A sub-thread that
// returns instead goes on at 0x1c9e28 (the recompiled code: it marks the end
// and switches back).
//
// Natively: the same bytes in the same frames, the same registers as the
// recompiled code leaves them (the save uses v0 as its scratch; both loads
// give f5..f8 the word saved for f4, as the game's code does). Frames
// outside plain RAM go to the recompiled code.
#include "sotc_decomp.h"

#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <cstring>

namespace decomp_subthread
{
    using sotc_decomp::returnTo;

    constexpr uint32_t kWake = 0x001C9A40u, kSleep = 0x001C9E4Cu;
    constexpr uint32_t kMainSp = 0x00270BB8u, kSubSp = 0x00270BBCu, kEnded = 0x00270BC0u, kResume = 0x00270BC8u;
    constexpr uint32_t kFrame = 0x2C0u;

    bool plain(uint32_t a, uint32_t size)
    {
        return !Ps2IsSpecialAddress(a) && !Ps2IsSpecialAddress(a + size - 1u) && (a & PS2_RAM_MASK) + size <= PS2_RAM_SIZE;
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
        void put32(uint32_t a, uint32_t v) const { std::memcpy(p(a), &v, 4); }
        uint64_t u64(uint32_t a) const
        {
            uint64_t v;
            std::memcpy(&v, p(a), 8);
            return v;
        }
        void put64(uint32_t a, uint64_t v) const { std::memcpy(p(a), &v, 8); }
    };

    // The 0x2c0 frame at f (16-byte aligned): f0-f31 at 0, the GPRs at 0x80 +
    // 16 r (v0..t9 and fp), hi / lo / sa / hi1 / lo1 from 0x280. v0 is the
    // recompiled code's scratch: its value in the frame is f31's word, it
    // ends with lo1.
    void save(const Ram &ram, R5900Context *ctx, uint32_t f)
    {
        std::memcpy(ram.p(f), ctx->f, 32u * 4u);
        uint32_t f31;
        std::memcpy(&f31, &ctx->f[31], 4);
        SET_GPR_U32(ctx, 2, f31); // mfc1 $v0, $f31
        std::memcpy(ram.p(f + 0xA0u), &ctx->r[2], 24u * 16u);
        std::memcpy(ram.p(f + 0x260u), &ctx->r[30], 16u);
        ram.put64(f + 0x280u, ctx->hi);
        ram.put64(f + 0x288u, ctx->lo);
        ram.put64(f + 0x290u, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(ctx->sa))));
        ram.put64(f + 0x298u, ctx->hi1);
        ram.put64(f + 0x2A0u, ctx->lo1);
        SET_GPR_U64(ctx, 2, ctx->lo1);
    }

    void load(const Ram &ram, R5900Context *ctx, uint32_t f)
    {
        // The game's own code has no lw for f5..f8: they get f4's word.
        uint32_t words[32];
        std::memcpy(words, ram.p(f), sizeof(words));
        words[5] = words[6] = words[7] = words[8] = words[4];
        std::memcpy(ctx->f, words, sizeof(words));
        std::memcpy(&ctx->r[2], ram.p(f + 0xA0u), 24u * 16u); // lq (v0 too: the lw's before are lost)
        std::memcpy(&ctx->r[30], ram.p(f + 0x260u), 16u);
        ctx->hi = ram.u64(f + 0x280u);
        ctx->lo = ram.u64(f + 0x288u);
        ctx->sa = static_cast<uint32_t>(ram.u64(f + 0x290u)) & 0x7Fu;
        ctx->hi1 = ram.u64(f + 0x298u);
        ctx->lo1 = ram.u64(f + 0x2A0u);
        SET_GPR_U64(ctx, 2, ctx->lo1);
    }

    void wake(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const Ram ram{rdram};
        const uint32_t a0 = GPR_U32(ctx, 4), sp0 = GPR_U32(ctx, 29), sp1 = sp0 - 0x10u;
        // The record (a3) by the object's type, as the jump table picks it.
        const uint8_t type = plain(a0 + 8u, 1u) ? ram.p(a0 + 8u)[0] : 0u;
        const uint32_t index = static_cast<uint32_t>(type) - 6u;
        uint32_t record = 0u;
        bool known = plain(a0 + 8u, 1u);
        if (index < 0x1Cu)
        {
            const uint32_t target = ram.u32(0x001E42C0u + index * 4u);
            if (target == 0x001C9A78u)
                record = a0 + 0x20u;
            else if (target != 0x001C9A80u)
                known = false;
        }
        const uint32_t sub = known && plain(record, 0x20u) ? ram.u32(record + 0x10u) : 0u;
        if (!known || !plain(record, 0x20u) || (sp1 & 0xFu) != 0u || !plain(sp1 - kFrame, kFrame + 0x10u) || (sub & 0xFu) != 0u ||
            !plain(sub, kFrame))
        {
            ctx->pc = kWake;
            return sotc_decomp::recompiled(kWake)(rdram, ctx, runtime);
        }

        SET_GPR_U32(ctx, 29, sp1);
        ram.put64(sp1, GPR_U64(ctx, 31)); // sd $ra, 0($sp)
        if (index < 0x1Cu)
        {
            SET_GPR_S32(ctx, 2, static_cast<int32_t>(index << 2));
            SET_GPR_S32(ctx, 3, static_cast<int32_t>(ram.u32(0x001E42C0u + index * 4u)));
        }
        else
        {
            SET_GPR_S32(ctx, 2, static_cast<int32_t>(index));
            SET_GPR_U64(ctx, 3, 0u);
        }
        SET_GPR_U64(ctx, 7, record);
        SET_GPR_S32(ctx, 3, static_cast<int32_t>(ram.u32(record + 0x1Cu)));
        SET_GPR_U64(ctx, 2, 1u);
        if (ram.u32(record + 0x1Cu) != 0u)
        {
            // Ended: v0 = 1, back to the caller.
            SET_GPR_U64(ctx, 31, ram.u64(sp1));
            SET_GPR_U32(ctx, 29, sp0);
            return returnTo(ctx);
        }
        ram.put32(0x001E1550u, a0);
        ram.put32(kMainSp, 0u);
        ram.put32(kEnded, 0u);
        SET_GPR_S32(ctx, 6, static_cast<int32_t>(kEnded));
        SET_GPR_S32(ctx, 4, 0x00270000);
        SET_GPR_S32(ctx, 5, static_cast<int32_t>(kSubSp));
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(ram.u32(record + 0x10u)));
        ram.put32(kSubSp, GPR_U32(ctx, 2));
        SET_GPR_S32(ctx, 3, static_cast<int32_t>(ram.u32(record + 4u)));
        ram.put32(kResume, GPR_U32(ctx, 3));

        const uint32_t frame = sp1 - kFrame;
        SET_GPR_U32(ctx, 29, frame);
        save(ram, ctx, frame);
        ram.put32(kMainSp, frame);
        const uint32_t subSp = ram.u32(kSubSp);
        SET_GPR_S32(ctx, 29, static_cast<int32_t>(subSp));
        std::memcpy(&ctx->r[31], ram.p((subSp + 0x270u) & ~0xFu), 16); // lq $ra, 0x270($sp)
        load(ram, ctx, subSp);
        SET_GPR_S32(ctx, 29, static_cast<int32_t>(subSp + kFrame));

        // jalr *0x270bc8, ra = 0x1c9e28
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(ram.u32(kResume)));
        const uint32_t target = GPR_U32(ctx, 2);
        SET_GPR_U32(ctx, 31, 0x001C9E28u);
        ctx->pc = target;
        if (!runtime->callGuest(rdram, ctx, target, 0x001C9E28u, 0x001C9E20u, PS2Runtime::GuestBranchKind::IndirectCall, "JALR"))
            return;
        // The sub-thread returned: the recompiled code marks it ended and switches back.
        ctx->pc = 0x001C9E28u;
        sotc_decomp::recompiled(kWake)(rdram, ctx, runtime);
    }

    void sleep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const Ram ram{rdram};
        const uint32_t sp0 = GPR_U32(ctx, 29), frame = sp0 - kFrame, main = ram.u32(kMainSp);
        if ((frame & 0xFu) != 0u || !plain(frame, kFrame) || (main & 0xFu) != 0u || !plain(main, kFrame + 0x10u))
        {
            ctx->pc = kSleep;
            return sotc_decomp::recompiled(kSleep)(rdram, ctx, runtime);
        }
        SET_GPR_U32(ctx, 29, frame);
        save(ram, ctx, frame);
        ram.put32(kSubSp, frame);
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(kMainSp));
        SET_GPR_S32(ctx, 29, static_cast<int32_t>(ram.u32(kMainSp)));
        const uint32_t m = GPR_U32(ctx, 29);
        load(ram, ctx, m);
        const uint32_t sp1 = m + kFrame;
        SET_GPR_U32(ctx, 29, sp1);
        // The caller's a1 / a2 / a3 as it saved them: 0x270bbc, 0x270bc0, the record.
        const uint32_t a3 = GPR_U32(ctx, 7), a1 = GPR_U32(ctx, 5), a2 = GPR_U32(ctx, 6);
        ram.put32(a3 + 4u, GPR_U32(ctx, 31));
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(ram.u32(a1)));
        ram.put32(a3 + 0x10u, GPR_U32(ctx, 2));
        ram.put32(GPR_U32(ctx, 2) + 0x270u, GPR_U32(ctx, 31));
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(ram.u32(a2)));
        ram.put32(a3 + 0x1Cu, GPR_U32(ctx, 2));
        SET_GPR_U64(ctx, 31, ram.u64(sp1));
        SET_GPR_U32(ctx, 29, sp1 + 0x10u);
        returnTo(ctx);
    }
}

SOTC_DECOMP_SWITCH_FUNCTION(0x001C9A40u, isysWakeupSubThread, decomp_subthread::wake);
SOTC_DECOMP_SWITCH_ONCE_FUNCTION(0x001C9E4Cu, isysSleepSubThread, decomp_subthread::sleep);
