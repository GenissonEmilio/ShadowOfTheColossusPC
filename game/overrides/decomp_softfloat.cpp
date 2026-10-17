// The EE has no double-precision FPU: the game's C doubles go through the
// SDK's software routines (an old gcc fp-bit). Decompiled here bit for bit:
//
//  - values unpacked to {class, sign, exponent, fraction}: class 0/1 NaN
//    (signalling/quiet), 2 zero, 3 normal, 4 infinity; doubles keep 8 guard
//    bits (implicit 1 at bit 60), floats 7 (implicit at bit 30);
//  - a denormal is read as zero and an underflow written as zero;
//  - NO rounding anywhere: the guard bits are dropped when packing (only
//    the add's alignment keeps a sticky bit). Host IEEE math rounds to
//    nearest: it agrees when the result is exact (most of the game's
//    operands come from floats), 1 ulp above now and then otherwise;
//  - the float pack does not look at the class (a NaN or infinity double
//    becomes an infinity) and has its own case for the exponent 128.
//
// Doubles travel as 64-bit integers in a0 / a1 and v0; floats in f12 and
// f0; the "long" of litodp / dptoli is 32 bits.
#include "sotc_decomp.h"

#include "ps2_runtime_macros.h"

#include <cstring>

namespace
{
    using sotc_decomp::returnTo;

    enum : uint32_t
    {
        kSignallingNan = 0,
        kQuietNan = 1,
        kZero = 2,
        kNormal = 3,
        kInfinity = 4,
    };
    constexpr uint64_t kImplicit1 = 1ull << 60, kImplicit2 = 1ull << 61;

    struct Parts
    {
        uint32_t cls = kZero;
        uint32_t sign = 0;
        int32_t exp = 0;
        uint64_t fraction = 0;
    };
    const Parts kNan{kSignallingNan, 0, 0, 0}; // the SDK's constant (0x139238): inf - inf, 0 x inf, 0 / 0

    Parts unpackD(uint64_t b) // __unpack_d (0x1271b0)
    {
        Parts p;
        const uint32_t e = static_cast<uint32_t>(b >> 52) & 0x7FFu;
        const uint64_t m = b & 0xFFFFFFFFFFFFFull;
        p.sign = static_cast<uint32_t>(b >> 63);
        if (e == 0u)
        {
            p.cls = kZero;
            return p;
        }
        if (e != 0x7FFu)
        {
            p.fraction = (m << 8) | kImplicit1;
            p.exp = static_cast<int32_t>(e) - 0x3FF;
            p.cls = kNormal;
            return p;
        }
        p.exp = 0x3FF;
        if (m == 0u)
        {
            p.fraction = kImplicit1;
            p.cls = kInfinity;
            return p;
        }
        p.cls = (b & 0x8000000000000ull) ? kQuietNan : kSignallingNan;
        p.fraction = m;
        return p;
    }

    uint64_t packD(const Parts &p) // __pack_d (0x1270d8)
    {
        uint64_t e = 0, f = 0;
        if (p.cls < 2u)
        {
            e = 0x7FF;
            f = p.fraction | 0x8000000000000ull;
        }
        else if (p.cls == kInfinity)
            e = 0x7FF;
        else if (p.cls != kZero && p.fraction != 0u && p.exp >= -0x3FE)
        {
            if (p.exp < 0x400)
            {
                e = static_cast<uint64_t>(p.exp + 0x3FF);
                f = p.fraction >> 8;
            }
            else
                e = 0x7FF;
        }
        return (f & 0xFFFFFFFFFFFFFull) | ((e & 0x7FFu) << 52) | (static_cast<uint64_t>(p.sign) << 63);
    }

    Parts unpackF(uint32_t b) // __unpack_f (0x127048): no infinity or NaN for floats
    {
        Parts p;
        const uint32_t e = (b >> 23) & 0xFFu;
        p.sign = b >> 31;
        if (e == 0u)
        {
            p.cls = kZero;
            return p;
        }
        p.exp = static_cast<int32_t>(e) - 0x7F;
        p.cls = kNormal;
        p.fraction = ((b & 0x7FFFFFu) << 7) | 0x40000000u;
        return p;
    }

    uint32_t packF(const Parts &p) // __pack_f (0x127390), fraction 32 bits
    {
        const uint32_t fraction = static_cast<uint32_t>(p.fraction);
        uint32_t e = 0, f = 0;
        if (p.cls != kZero && fraction != 0u && p.exp >= -0x7E)
        {
            if (p.exp < 0x81)
            {
                e = static_cast<uint32_t>(p.exp + 0x7F);
                if (p.exp != 0x80 || static_cast<int32_t>(fraction) >= 0)
                    f = fraction >> 7;
                else
                    e = 0xFF;
            }
            else
                e = 0xFF;
        }
        return (f & 0x7FFFFFu) | ((e & 0xFFu) << 23) | (p.sign << 31);
    }

    Parts addParts(const Parts &a, const Parts &b) // 0x125080
    {
        if (a.cls < 2u)
            return a;
        if (b.cls < 2u)
            return b;
        if (a.cls == kInfinity)
            return (b.cls == kInfinity && a.sign != b.sign) ? kNan : a;
        if (b.cls == kInfinity)
            return b;
        if (b.cls == kZero)
        {
            if (a.cls != kZero)
                return a;
            Parts r = a;
            r.sign = a.sign & b.sign;
            return r;
        }
        if (a.cls == kZero)
            return b;
        int32_t ae = a.exp, be = b.exp;
        uint64_t af = a.fraction, bf = b.fraction;
        int32_t diff = ae - be;
        if (diff < 0)
            diff = -diff;
        int32_t exp;
        if (diff < 64)
        {
            if (ae > be)
            {
                for (int32_t i = ae - be; i != 0; --i)
                    bf = (bf & 1u) | (bf >> 1);
                exp = ae;
            }
            else
            {
                while (ae < be)
                {
                    ++ae;
                    af = (af & 1u) | (af >> 1);
                }
                exp = ae;
            }
        }
        else if (be < ae)
        {
            bf = 0;
            exp = ae;
        }
        else
        {
            af = 0;
            exp = be;
        }
        Parts r;
        r.cls = kNormal;
        r.exp = exp;
        if (a.sign == b.sign)
        {
            r.sign = a.sign;
            r.fraction = af + bf;
        }
        else
        {
            const int64_t d = a.sign == 0u ? static_cast<int64_t>(af - bf) : static_cast<int64_t>(bf - af);
            r.sign = d < 0 ? 1u : 0u;
            r.fraction = d < 0 ? static_cast<uint64_t>(-d) : static_cast<uint64_t>(d);
            while (r.fraction - 1u < 0xFFFFFFFFFFFFFFFull)
            {
                r.fraction <<= 1;
                --r.exp;
            }
        }
        if (r.fraction > 0x1FFFFFFFFFFFFFFFull)
        {
            r.fraction = (r.fraction & 1u) | (r.fraction >> 1);
            ++r.exp;
        }
        return r;
    }

    Parts mulParts(Parts a, Parts b) // dpmul (0x1253a0)
    {
        const uint32_t sign = a.sign != b.sign ? 1u : 0u;
        if (a.cls < 2u)
        {
            a.sign = sign;
            return a;
        }
        if (b.cls >= 2u)
        {
            if (a.cls == kInfinity)
            {
                if (b.cls == kZero)
                    return kNan;
                a.sign = sign;
                return a;
            }
            if (b.cls == kInfinity)
            {
                if (a.cls == kZero)
                    return kNan;
            }
            else
            {
                if (a.cls == kZero)
                {
                    a.sign = sign;
                    return a;
                }
                if (b.cls != kZero)
                {
                    const unsigned __int128 product = static_cast<unsigned __int128>(b.fraction) * a.fraction;
                    uint64_t high = static_cast<uint64_t>(product >> 64), low = static_cast<uint64_t>(product);
                    Parts r;
                    r.cls = kNormal;
                    r.sign = sign;
                    r.exp = a.exp + b.exp + 4;
                    while (high > 0x1FFFFFFFFFFFFFFFull)
                    {
                        const uint64_t bit = high & 1u;
                        high >>= 1;
                        ++r.exp;
                        if (bit)
                            low = (low >> 1) | 0x8000000000000000ull;
                    }
                    while (high < kImplicit1)
                    {
                        high <<= 1;
                        --r.exp;
                        if (low & 0x8000000000000000ull)
                            high |= 1u;
                        low <<= 1;
                    }
                    r.fraction = high; // the guard bits stay: packing drops them
                    return r;
                }
            }
        }
        b.sign = sign;
        return b;
    }

    Parts divParts(Parts a, const Parts &b) // dpdiv (0x125628)
    {
        if (a.cls < 2u)
            return a;
        if (b.cls < 2u)
            return b;
        a.sign ^= b.sign;
        if (a.cls == kInfinity || a.cls == kZero)
            return a.cls == b.cls ? kNan : a;
        if (b.cls == kInfinity)
        {
            a.fraction = 0;
            a.exp = 0;
            return a;
        }
        if (b.cls == kZero)
        {
            a.cls = kInfinity;
            return a;
        }
        a.exp -= b.exp;
        bool below = false;
        uint64_t n = a.fraction;
        if (n < b.fraction)
        {
            --a.exp;
            n <<= 1;
            below = n < b.fraction;
        }
        uint64_t bit = kImplicit1, q = 0;
        do
        {
            uint64_t t = n << 1;
            if (!below)
            {
                q |= bit;
                t = (n - b.fraction) * 2u;
            }
            bit >>= 1;
            below = t < b.fraction;
            n = t;
        } while (bit != 0u);
        a.fraction = q;
        return a;
    }

    int32_t cmpParts(const Parts &a, const Parts &b) // __fpcmp_parts_d (0x127270)
    {
        if (a.cls < 2u || b.cls < 2u)
            return 1;
        if (a.cls == kInfinity)
        {
            if (b.cls == kInfinity)
                return static_cast<int32_t>(b.sign - a.sign);
            return a.sign == 0u ? 1 : -1;
        }
        if (b.cls == kInfinity)
            return b.sign == 0u ? -1 : 1;
        if (a.cls == kZero)
        {
            if (b.cls == kZero)
                return 0;
            return b.sign != 0u ? 1 : -1;
        }
        if (b.cls == kZero)
            return a.sign == 0u ? 1 : -1;
        if (a.sign == b.sign)
        {
            if (a.exp <= b.exp)
            {
                if (b.exp <= a.exp)
                {
                    if (b.fraction < a.fraction)
                        return a.sign == 0u ? 1 : -1;
                    if (b.fraction <= a.fraction)
                        return 0;
                }
                return a.sign == 0u ? -1 : 1;
            }
        }
        return a.sign == 0u ? 1 : -1;
    }

    uint64_t litodpBits(int32_t v) // litodp (0x1257b8)
    {
        Parts p;
        p.sign = v < 0 ? 1u : 0u;
        if (v == 0)
        {
            p.cls = kZero;
            return packD(p);
        }
        if (v == INT32_MIN)
            return 0xC1E0000000000000ull;
        p.cls = kNormal;
        p.fraction = static_cast<uint64_t>(v < 0 ? -static_cast<int64_t>(v) : static_cast<int64_t>(v));
        p.exp = 60;
        while (p.fraction < kImplicit1)
        {
            p.fraction <<= 1;
            --p.exp;
        }
        return packD(p);
    }

    uint32_t dptofpBits(uint64_t d) // dptofp (0x1258a0): the fraction cut to 32 bits, a sticky bit
    {
        Parts p = unpackD(d);
        const uint32_t high = static_cast<uint32_t>((p.fraction << 2) >> 32);
        p.fraction = (p.fraction & 0x3FFFFFFFu) ? (high | 1u) : high;
        return packF(p);
    }

    void setFloat(R5900Context *ctx, uint32_t bits) { std::memcpy(&ctx->f[0], &bits, 4); }

    void dpadd(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        SET_GPR_U64(ctx, 2, packD(addParts(unpackD(GPR_U64(ctx, 4)), unpackD(GPR_U64(ctx, 5)))));
        returnTo(ctx);
    }
    void dpsub(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        Parts b = unpackD(GPR_U64(ctx, 5));
        b.sign ^= 1u;
        SET_GPR_U64(ctx, 2, packD(addParts(unpackD(GPR_U64(ctx, 4)), b)));
        returnTo(ctx);
    }
    void dpmul(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        SET_GPR_U64(ctx, 2, packD(mulParts(unpackD(GPR_U64(ctx, 4)), unpackD(GPR_U64(ctx, 5)))));
        returnTo(ctx);
    }
    void dpdiv(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        SET_GPR_U64(ctx, 2, packD(divParts(unpackD(GPR_U64(ctx, 4)), unpackD(GPR_U64(ctx, 5)))));
        returnTo(ctx);
    }
    void dpcmp(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        SET_GPR_S64(ctx, 2, static_cast<int64_t>(cmpParts(unpackD(GPR_U64(ctx, 4)), unpackD(GPR_U64(ctx, 5)))));
        returnTo(ctx);
    }
    void litodp(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        SET_GPR_U64(ctx, 2, litodpBits(static_cast<int32_t>(GPR_U32(ctx, 4))));
        returnTo(ctx);
    }
    void dptoli(uint8_t *, R5900Context *ctx, PS2Runtime *) // 0x131b10
    {
        const Parts p = unpackD(GPR_U64(ctx, 4));
        int32_t r;
        if (p.cls == kZero || p.cls < 2u || (p.cls != kInfinity && p.exp < 0))
            r = 0;
        else if (p.cls != kInfinity && p.exp < 31)
        {
            r = static_cast<int32_t>(p.fraction >> (60 - p.exp));
            if (p.sign)
                r = -r;
        }
        else
            r = p.sign ? INT32_MIN : INT32_MAX;
        SET_GPR_S64(ctx, 2, static_cast<int64_t>(r));
        returnTo(ctx);
    }
    void fptodp(uint8_t *, R5900Context *ctx, PS2Runtime *) // 0x125040
    {
        uint32_t f;
        std::memcpy(&f, &ctx->f[12], 4);
        Parts p = unpackF(f);
        p.fraction <<= 30;
        SET_GPR_U64(ctx, 2, packD(p));
        returnTo(ctx);
    }
    void dptofp(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setFloat(ctx, dptofpBits(GPR_U64(ctx, 4)));
        returnTo(ctx);
    }
    void floatdisf(uint8_t *, R5900Context *ctx, PS2Runtime *) // __floatdisf (0x124b90): int64 -> float, through doubles
    {
        uint64_t x = GPR_U64(ctx, 4);
        if (x + 0x1FFFFFFFFFFFFFull > 0x3FFFFFFFFFFFFEull && (x & 0x7FFu) != 0u)
            x |= 0x800u; // a sticky bit where the double will cut
        uint64_t d = litodpBits(static_cast<int32_t>(static_cast<int64_t>(x) >> 32));
        d = packD(mulParts(unpackD(d), unpackD(0x40F0000000000000ull)));
        d = packD(mulParts(unpackD(d), unpackD(0x40F0000000000000ull)));
        uint64_t low = litodpBits(static_cast<int32_t>(x));
        if (static_cast<int32_t>(x) < 0)
            low = packD(addParts(unpackD(low), unpackD(0x41F0000000000000ull)));
        d = packD(addParts(unpackD(d), unpackD(low)));
        setFloat(ctx, dptofpBits(d));
        returnTo(ctx);
    }
}

SOTC_DECOMP_FUNCTION(0x001252E0u, dpadd, dpadd);
SOTC_DECOMP_FUNCTION(0x00125338u, dpsub, dpsub);
SOTC_DECOMP_FUNCTION(0x001253A0u, dpmul, dpmul);
SOTC_DECOMP_FUNCTION(0x00125628u, dpdiv, dpdiv);
SOTC_DECOMP_FUNCTION(0x00125768u, dpcmp, dpcmp);
SOTC_DECOMP_FUNCTION(0x001257B8u, litodp, litodp);
SOTC_DECOMP_FUNCTION(0x00131B10u, dptoli, dptoli);
SOTC_DECOMP_FUNCTION(0x00125040u, fptodp, fptodp);
SOTC_DECOMP_FLOAT_FUNCTION(0x001258A0u, dptofp, dptofp);
SOTC_DECOMP_FLOAT_FUNCTION(0x00124B90u, floatdisf, floatdisf);
