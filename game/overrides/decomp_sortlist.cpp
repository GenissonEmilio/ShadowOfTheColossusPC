// The display's sort lists (DispDispObjGroupModel, AddSortObj): an array of
// buckets, each the head of a chain of objects linked at +8. The list:
// +0 the bucket array, +4 the bucket count, +0x10 the bucket being read.
// GetNextSortObj walks the buckets one by one past the empty ones, which
// the recompiled code did at ~15 host instructions a bucket (3% of the
// Vita's game thread in the attract demo, ClearSortList 1% more).
#include "sotc_decomp.h"

#include "ps2_runtime_macros.h"

namespace
{
    using sotc_decomp::returnTo;

    // GetNextSortObj(list, current) (0x110dd28): the object after current,
    // or the first object of the list when current is 0; 0 at the end.
    void getNextSortObj(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t list = GPR_U32(ctx, 4), current = GPR_U32(ctx, 5);
        int32_t result = 0;
        if (current != 0u)
        {
            result = static_cast<int32_t>(READ32(current + 8u));
            if (result == 0)
            {
                // The buckets after the one being read (the count read once,
                // the end found by equality as the game does).
                // The index is written once at the end (the game writes it
                // each bucket; nothing reads it in between).
                const int32_t count = static_cast<int32_t>(READ32(list + 4u));
                const uint32_t buckets = READ32(list);
                int32_t index = static_cast<int32_t>(READ32(list + 0x10u));
                for (;;)
                {
                    ++index;
                    if (index == count)
                        break;
                    result = static_cast<int32_t>(READ32(buckets + static_cast<uint32_t>(index) * 4u));
                    if (result != 0)
                        break;
                }
                WRITE32(list + 0x10u, static_cast<uint32_t>(index));
            }
        }
        else
        {
            const int32_t count = static_cast<int32_t>(READ32(list + 4u));
            if (0 < count)
            {
                WRITE32(list + 0x10u, 0u);
                const uint32_t buckets = READ32(list);
                result = static_cast<int32_t>(READ32(buckets));
                int32_t index = 0, read = 0; // read: the last bucket read (the index the game leaves)
                while (result == 0)
                {
                    ++index;
                    if (!(index < count))
                        break; // result stays 0
                    read = index;
                    result = static_cast<int32_t>(READ32(buckets + static_cast<uint32_t>(index) * 4u));
                }
                if (read != 0)
                    WRITE32(list + 0x10u, static_cast<uint32_t>(read));
            }
        }
        SET_GPR_S32(ctx, 2, result);
        returnTo(ctx);
    }

    // ClearSortList(list) (0x110de38): every bucket emptied, the reading
    // index back to 0 (the count and the array read again each bucket).
    void clearSortList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t list = GPR_U32(ctx, 4);
        // v0 as the game leaves it: the count when not above 0, else the
        // loop's last slt (0).
        int32_t v0 = static_cast<int32_t>(READ32(list + 4u));
        if (0 < v0)
        {
            for (int32_t i = 0; i < static_cast<int32_t>(READ32(list + 4u)); ++i)
                WRITE32(READ32(list) + static_cast<uint32_t>(i) * 4u, 0u);
            v0 = 0;
        }
        WRITE32(list + 0x10u, 0u);
        SET_GPR_S32(ctx, 2, v0);
        returnTo(ctx);
    }
}

SOTC_DECOMP_FUNCTION(0x0110DD28u, GetNextSortObj, getNextSortObj);
SOTC_DECOMP_FUNCTION(0x0110DE38u, ClearSortList, clearSortList);
