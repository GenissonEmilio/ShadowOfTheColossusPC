// newlib's mktime (0x1dd190), called once a frame by iosGetClockSecond
// (0x1ad200) with the console clock: the year it passes is the clock's
// byte minus 1900 (tm_year ~ -1876, the year 24), so mktime's loop over
// the years down from 1969 runs ~1 900 times, each with two divisions
// (0.8% of the Vita's game thread). Here the days between the years are
// counted in closed form (proleptic Gregorian, as the loop's leap test).
//
// Our console clock reads all zeros (day 0, month -1, the year -1900):
// validate_structure (0x1dcdc0) takes the day back into a month before
// January (index -2, read before the table, as the game does) and comes out
// with the same fields every time.
#include "sotc_decomp.h"

#include "ps2_runtime_macros.h"

namespace decomp_time
{
    using sotc_decomp::returnTo;

    constexpr uint32_t kDaysInMonth = 0x001E4A08u;     // validate_structure's table
    constexpr uint32_t kDaysBeforeMonth = 0x001E4A38u; // mktime's

    // tm_year is years since 1900; the loops' test: y & 3, y % 100, (y + 1900) % 400.
    bool leap(int32_t y)
    {
        return (y & 3) == 0 && (y % 100 != 0 || (y + 1900) % 400 == 0);
    }

    int64_t floorDiv(int64_t a, int64_t b)
    {
        return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
    }

    // Days from 1 January 1970 to 1 January of tm_year y.
    int64_t daysFrom1970(int32_t y)
    {
        auto before = [](int64_t year) // days before 1 January of a Gregorian year
        {
            const int64_t z = year - 1;
            return 365 * z + floorDiv(z, 4) - floorDiv(z, 100) + floorDiv(z, 400);
        };
        return before(static_cast<int64_t>(y) + 1900) - before(1970);
    }

    int32_t daysInMonth(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int32_t mon, int32_t february)
    {
        return mon == 1 ? february : static_cast<int32_t>(READ32(kDaysInMonth + static_cast<uint32_t>(mon) * 4u));
    }

    // validate_structure (0x1dcdc0): brings seconds, minutes, hours and
    // months into range (div(): C's truncation, a negative remainder made
    // positive), then the day: below 1, months back (the month index can go
    // negative and read before the table, as the game's own code does);
    // past the month's days, months forward, the year with them.
    void validateStructure(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t tm)
    {
        auto carry = [&](uint32_t at, uint32_t next, int32_t range)
        {
            const int32_t value = static_cast<int32_t>(READ32(at));
            const int32_t quot = value / range, rem = value % range;
            const int32_t up = static_cast<int32_t>(READ32(next)) + quot;
            WRITE32(next, static_cast<uint32_t>(up));
            WRITE32(at, static_cast<uint32_t>(rem));
            if (rem < 0)
            {
                WRITE32(at, static_cast<uint32_t>(rem + range));
                WRITE32(next, static_cast<uint32_t>(up - 1));
            }
        };
        if (READ32(tm) >= 60u)
            carry(tm, tm + 4u, 60);
        if (READ32(tm + 4u) >= 60u)
            carry(tm + 4u, tm + 8u, 60);
        if (READ32(tm + 8u) >= 24u)
            carry(tm + 8u, tm + 12u, 24);
        if (static_cast<int32_t>(READ32(tm + 16u)) >= 12)
            carry(tm + 16u, tm + 20u, 12);
        int32_t february = leap(static_cast<int32_t>(READ32(tm + 20u))) ? 29 : 28;
        int32_t mday = static_cast<int32_t>(READ32(tm + 12u));
        int32_t mon = static_cast<int32_t>(READ32(tm + 16u));
        if (mday <= 0)
        {
            do
            {
                --mon;
                WRITE32(tm + 16u, static_cast<uint32_t>(mon));
                if (mon == -1)
                {
                    mon = 11;
                    WRITE32(tm + 16u, 11u);
                    const int32_t year = static_cast<int32_t>(READ32(tm + 20u)) - 1;
                    WRITE32(tm + 20u, static_cast<uint32_t>(year));
                    february = leap(year) ? 29 : 28;
                }
                mday += daysInMonth(rdram, ctx, runtime, mon, february);
                WRITE32(tm + 12u, static_cast<uint32_t>(mday));
            } while (mday <= 0);
            return;
        }
        while (daysInMonth(rdram, ctx, runtime, mon, february) < mday)
        {
            mday -= daysInMonth(rdram, ctx, runtime, mon, february);
            ++mon;
            WRITE32(tm + 12u, static_cast<uint32_t>(mday));
            WRITE32(tm + 16u, static_cast<uint32_t>(mon));
            if (mon == 12)
            {
                const int32_t year = static_cast<int32_t>(READ32(tm + 20u)) + 1;
                WRITE32(tm + 20u, static_cast<uint32_t>(year));
                WRITE32(tm + 16u, 0u);
                mon = 0;
                february = leap(year) ? 29 : 28;
            }
        }
    }

    void mktime(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t tm = GPR_U32(ctx, 4);
        validateStructure(rdram, ctx, runtime, tm);
        const uint32_t sec = READ32(tm), min = READ32(tm + 4u), hour = READ32(tm + 8u);
        const int32_t mday = static_cast<int32_t>(READ32(tm + 12u)), mon = static_cast<int32_t>(READ32(tm + 16u)),
                      year = static_cast<int32_t>(READ32(tm + 20u));
        const int64_t seconds = static_cast<int32_t>(hour * 3600u + min * 60u + sec);
        int64_t days = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(mday) - 1u)) +
                       static_cast<int32_t>(READ32(kDaysBeforeMonth + static_cast<uint32_t>(mon) * 4u));
        if (mon >= 2 && leap(year))
            ++days;
        WRITE32(tm + 0x1Cu, static_cast<uint32_t>(days)); // tm_yday
        const int32_t range = static_cast<int32_t>(static_cast<uint32_t>(year) + 10000u);
        if (static_cast<uint32_t>(range) >= 20001u)
        {
            SET_GPR_S64(ctx, 2, -1);
            SET_GPR_S64(ctx, 3, range);
            returnTo(ctx);
            return;
        }
        days += daysFrom1970(year);
        const int32_t wday = static_cast<int32_t>((days + 4) % 7);
        WRITE32(tm + 0x18u, static_cast<uint32_t>(wday < 0 ? wday + 7 : wday)); // tm_wday
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(seconds + days * 86400));
        SET_GPR_U64(ctx, 3, static_cast<uint64_t>(days * 45));
        returnTo(ctx);
    }
}

SOTC_DECOMP_FUNCTION(0x001DD190u, mktime, decomp_time::mktime);
