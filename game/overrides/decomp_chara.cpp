// Character searches over the game's handle lists (3.4% of the Vita's game
// thread recompiled, throttled attract demo).
//
// A handle is a word in a list of 28-byte entries: its low 18 bits are the
// distance back to the owning table's pointer, bits 18-29 the index in it;
// the object is table +0x10 + index x the element size (table +8 -> +0xc),
// 0 when the table is missing or the empty one. The empty table's pointer
// and "this character" are boot-ELF data the module is linked to at run
// time (relocated lui / lw pairs, read from the code).
#include "sotc_decomp.h"

#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <cstring>

namespace decomp_chara
{
    using sotc_decomp::returnTo;

    constexpr uint32_t kGetCharacterClass = 0x010C0A70u;
    using sotc_decomp::relocated;

    struct Ram
    {
        uint8_t *rdram;
        uint32_t u32(uint32_t a) const
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4);
            return v;
        }
        // The table a handle at h belongs to (0: none).
        uint32_t table(uint32_t h) const
        {
            const uint32_t off = u32(h) & 0x3FFFFu;
            return off ? u32(h - off) : 0u;
        }
        uint32_t resolve(uint32_t h, uint32_t noTable) const
        {
            const uint32_t v = u32(h), t = table(h);
            if (t == 0u || t == u32(noTable))
                return 0u;
            return u32(t + 0x10u) + ((v >> 18) & 0xFFFu) * u32(u32(t + 8u) + 0xCu);
        }
    };

    // manager_sub_010CE558(a0, a1 target, f12): the character's link list
    // (*(chara +0x10ec): count +0xc, entries +0x10, 28 bytes each: two
    // handles, the link and the object it leads to). Links to a1 from
    // something other than this character: if that thing is one of the
    // character's 0x80-byte slots (+0x30c, count +0x3fc, slot +4 its handle
    // object, +0x44 == -2 an empty one), 010CE2E8(a0, slot, entry, f12),
    // stopping at the first that returns non-zero. If none was such a slot,
    // the links to a1 from the character itself go to 010CE138(a0, chara,
    // entry, f12).
    // Callee-visible frame/register correction adapted from HenryKun55/celosia
    // 77c962c945 (GPL-3.0). A nested script-animation call can copy a saved
    // s-register stack slot into an object: host-only locals are insufficient.
    // Mirror the SCUS prologue and live s0-s7/fp/f20 at guest calls, then reload
    // the original epilogue's values from that frame.
    void charaLinks(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const Ram ram{rdram};
        const uint64_t a0 = GPR_U64(ctx, 4); // fp receives a full daddu, not a truncated pointer
        const uint32_t target = GPR_U32(ctx, 5);
        const float f12 = ctx->f[12];
        const uint32_t sp0 = GPR_U32(ctx, 29), sp1 = sp0 - 0x70u;
        auto sx = [](uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); };
        for (int i = 0; i < 8; ++i)
            WRITE64(sp1 + 0x10u + i * 8u, GPR_U64(ctx, 16 + i));
        WRITE64(sp1 + 0x50u, GPR_U64(ctx, 30));
        WRITE64(sp1 + 0x58u, GPR_U64(ctx, 31));
        uint32_t savedF20;
        std::memcpy(&savedF20, &ctx->f[20], sizeof(savedF20));
        WRITE32(sp1 + 0x60u, savedF20);
        WRITE32(sp1, target);
        SET_GPR_U64(ctx, 30, a0);
        ctx->f[20] = FPU_MOV_S(f12);
        auto finish = [&](uint64_t v0)
        {
            SET_GPR_U64(ctx, 2, v0);
            for (int i = 0; i < 8; ++i)
                SET_GPR_U64(ctx, 16 + i, READ64(sp1 + 0x10u + i * 8u));
            SET_GPR_U64(ctx, 30, READ64(sp1 + 0x50u));
            const uint64_t restoredRa = READ64(sp1 + 0x58u);
            const uint32_t restoredF20 = READ32(sp1 + 0x60u);
            std::memcpy(&ctx->f[20], &restoredF20, sizeof(restoredF20));
            SET_GPR_U32(ctx, 29, sp0);
            SET_GPR_U64(ctx, 31, restoredRa);
            returnTo(ctx);
        };

        sotc_decomp::callGuest(rdram, ctx, runtime, kGetCharacterClass, 0x010CE598u, sp1);
        const uint32_t chara = GPR_U32(ctx, 2);
        SET_GPR_U64(ctx, 18, GPR_U64(ctx, 2));
        WRITE32(sp1 + 4u, 0u);
        const uint32_t self = ram.u32(relocated(rdram, 0x010CE5A8u, 0x010CE5B0u));
        SET_GPR_U64(ctx, 23, sx(self));
        const uint32_t list = ram.u32(chara + 0x10ECu);
        const uint32_t t = ram.table(list);
        const int32_t count = t != 0u && t != ram.u32(relocated(rdram, 0x010CE5D8u, 0x010CE5DCu)) ? static_cast<int32_t>(ram.u32(t + 0xCu)) : 0;
        const uint32_t base = t != 0u && t != ram.u32(relocated(rdram, 0x010CE620u, 0x010CE624u)) ? ram.u32(t + 0x10u) : 0u;
        SET_GPR_U64(ctx, 19, sx(static_cast<uint32_t>(count)));
        SET_GPR_U64(ctx, 20, sx(base));
        SET_GPR_U64(ctx, 16, sx(base));
        SET_GPR_U64(ctx, 17, 0u);
        if (count <= 0)
        {
            // v0: the lui of the empty table's pointer (0x10ce620).
            finish(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(sotc_decomp::codeWord(rdram, 0x010CE620u) << 16))));
            return;
        }
        SET_GPR_U64(ctx, 21, 0x3FFFFu);
        SET_GPR_U64(ctx, 22, sx((sotc_decomp::codeWord(rdram, 0x010CE64Cu) & 0xFFFFu) << 16));
        const uint32_t noTable1a = relocated(rdram, 0x010CE64Cu, 0x010CE678u), noTable1b = relocated(rdram, 0x010CE64Cu, 0x010CE6D0u);
        const uint32_t noTable2a = relocated(rdram, 0x010CE7A0u, 0x010CE7C8u), noTable2b = relocated(rdram, 0x010CE7A0u, 0x010CE820u);
        bool found = false;
        uint32_t a3 = 0u;
        uint32_t e = base;
        for (int32_t i = 0; i < count; ++i, e += 0x1Cu)
        {
            a3 = ram.resolve(e, noTable1a);
            if (a3 == self || ram.resolve(e + 4u, noTable1b) != target)
                continue;
            const int32_t slots = static_cast<int32_t>(ram.u32(chara + 0x3FCu));
            uint32_t slot = ram.u32(chara + 0x30Cu);
            int32_t index = -1;
            for (int32_t k = 0; k < slots; ++k, slot += 0x80u)
                if (ram.u32(slot + 0x44u) != 0xFFFFFFFEu && ram.u32(slot + 4u) == a3)
                {
                    index = k;
                    break;
                }
            if (index < 0)
                continue;
            found = true;
            WRITE32(sp1 + 4u, 1u);
            SET_GPR_U64(ctx, 16, sx(e));
            SET_GPR_U64(ctx, 17, sx(static_cast<uint32_t>(i)));
            SET_GPR_U64(ctx, 4, a0);
            SET_GPR_S32(ctx, 5, index);
            SET_GPR_U64(ctx, 6, sx(e));
            SET_GPR_U32(ctx, 7, a3);
            SET_GPR_U64(ctx, 2, 1u);
            ctx->f[12] = f12;
            sotc_decomp::callGuest(rdram, ctx, runtime, 0x010CE2E8u, 0x010CE768u, sp1);
            if (GPR_U64(ctx, 2) != 0u)
            {
                finish(GPR_U64(ctx, 2));
                return;
            }
        }
        if (found)
        {
            finish(0u);
            return;
        }
        // The second-loop call has no instruction assigning a3. Preserve what
        // the previous callee left there after the first call.
        SET_GPR_U32(ctx, 7, a3);
        SET_GPR_U64(ctx, 20, 0x3FFFFu);
        SET_GPR_U64(ctx, 19, sx((sotc_decomp::codeWord(rdram, 0x010CE7A0u) & 0xFFFFu) << 16));
        uint64_t v0 = 0u;
        e = base;
        for (int32_t i = count; i != 0; --i, e += 0x1Cu)
        {
            v0 = ram.resolve(e, noTable2a);
            if (v0 != self)
                continue;
            v0 = ram.resolve(e + 4u, noTable2b);
            if (v0 != target)
                continue;
            SET_GPR_U64(ctx, 16, sx(e));
            SET_GPR_U64(ctx, 17, sx(static_cast<uint32_t>(i)));
            SET_GPR_U64(ctx, 4, a0);
            SET_GPR_U64(ctx, 5, GPR_U64(ctx, 18));
            SET_GPR_U64(ctx, 6, sx(e));
            ctx->f[12] = f12;
            sotc_decomp::callGuest(rdram, ctx, runtime, 0x010CE138u, 0x010CE86Cu, sp1);
            v0 = GPR_U64(ctx, 2);
        }
        finish(v0);
    }

    // manager_sub_010CDD10(a0 handle, a1 key, a2 index): the table a0
    // belongs to has 0x4c-byte records, each led by a handle. Key 0: the
    // last record, if its handle leads to "this character"; otherwise the
    // first record whose handle leads to this character or to the key.
    // Returns the record's slot a2 (+4 + 8 a2), or 0. 0.9% of the Vita's
    // game thread recompiled.
    void findRecord(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const Ram ram{rdram};
        const uint32_t h = GPR_U32(ctx, 4), key = GPR_U32(ctx, 5), index = GPR_U32(ctx, 6);
        const uint32_t t = ram.table(h);
        const uint32_t base = t != 0u && t != ram.u32(relocated(rdram, 0x010CDD40u, 0x010CDD44u)) ? ram.u32(t + 0x10u) : 0u;
        uint32_t v0 = 0u;
        if (base != 0u)
        {
            const uint32_t count = t != 0u && t != ram.u32(relocated(rdram, 0x010CDD90u, 0x010CDD94u)) ? ram.u32(t + 0xCu) : 0u;
            if (key == 0u)
            {
                const uint32_t last = base + count * 76u - 0x4Cu;
                if (ram.resolve(last, relocated(rdram, 0x010CDEB0u, 0x010CDEB4u)) == ram.u32(relocated(rdram, 0x010CDEE4u, 0x010CDEE8u)))
                    v0 = last + index * 8u + 4u;
            }
            else if (count != 0u)
            {
                const uint32_t noTable = relocated(rdram, 0x010CDDC8u, 0x010CDDD0u), self = relocated(rdram, 0x010CDDCCu, 0x010CDDD4u);
                uint32_t e = base, slot = base + index * 8u + 4u;
                for (int32_t n = static_cast<int32_t>(count) - 1;; --n, e += 0x4Cu, slot += 0x4Cu)
                {
                    const uint32_t r = ram.resolve(e, noTable);
                    if (r == ram.u32(self) || r == key)
                    {
                        v0 = slot;
                        break;
                    }
                    if (n - 1 == -1)
                        break;
                }
            }
        }
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(v0));
        returnTo(ctx);
    }
}

SOTC_DECOMP_FUNCTION(0x010CE558u, charaLinks, decomp_chara::charaLinks);
SOTC_DECOMP_FUNCTION(0x010CDD10u, findRecord, decomp_chara::findRecord);
