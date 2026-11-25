#include "../../game/overrides/sotc_decomp.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x00100000u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x01100000u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0x00400000u;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[g_ps2RecompiledFunctionTableSlotCount] = {};

namespace
{
    constexpr uint32_t kSin = 0x001BDF00, kCos = 0x001BE020;
    constexpr uint32_t kCurve = 0x010AC6C8, kMotion = 0x010B81F8, kSearch = 0x010AB3A8;
    constexpr uint32_t kAllocate = 0x001ADDC0, kInsert = 0x001ADBC0;
    constexpr uint32_t kStack = 0x00800000, kReturn = 0x00123454, kOriginal = 0xCAFEBABE;

    void original(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        SET_GPR_U32(ctx, 2, kOriginal);
        sotc_decomp::returnTo(ctx);
    }

    struct Fixture
    {
        PS2Runtime runtime;
        std::vector<uint8_t> ram = std::vector<uint8_t>(PS2_RAM_SIZE);

        Fixture()
        {
            constexpr std::pair<uint32_t, std::string_view> entries[] = {
                {kSin, "iosGetSinf"}, {kCos, "iosGetCosf"}, {kCurve, "curveEval"},
                {kMotion, "motionKeys"}, {kSearch, "texSearchMopByName"},
                {kAllocate, "kernel_sub_001ADDC0"}, {kInsert, "kernel_sub_001ADBC0"},
            };
            const auto &registry = sotc_decomp::registry();
            for (const auto &[pc, name] : entries)
            {
                assert(std::count_if(registry.begin(), registry.end(), [&](const auto &fn) { return fn.address == pc; }) == 1);
                const auto entry = std::find_if(registry.begin(), registry.end(), [&](const auto &fn) { return fn.address == pc; });
                assert(entry->name == name && !entry->collision);
                assert(runtime.registerFunction(pc, original));
            }
            sotc_decomp::install(runtime);
        }

        template <typename T = uint32_t>
        T read(uint32_t at) const
        {
            T value{};
            std::memcpy(&value, ram.data() + at, sizeof(value));
            return value;
        }

        template <typename T = uint32_t>
        void write(uint32_t at, const T &value)
        {
            std::memcpy(ram.data() + at, &value, sizeof(value));
        }

        R5900Context call(uint32_t pc, std::array<uint32_t, 4> args = {}, float f12 = 0)
        {
            R5900Context ctx{};
            ctx.pc = pc;
            ctx.f[12] = f12;
            ctx.vu0_vf[0] = _mm_set_ps(1.f, 0.f, 0.f, 0.f);
            for (int i = 0; i < 4; ++i)
                SET_GPR_U32(&ctx, i + 4, args[i]);
            SET_GPR_U32(&ctx, 16, 0xABCDu);
            SET_GPR_U32(&ctx, 29, kStack);
            SET_GPR_U32(&ctx, 31, kReturn);
            runtime.lookupFunction(pc)(ram.data(), &ctx, &runtime);
            assert(ctx.pc == kReturn && GPR_U32(&ctx, 29) == kStack);
            assert(GPR_U32(&ctx, 16) == 0xABCDu);
            return ctx;
        }
    };

    uint32_t result(const R5900Context &ctx) { return GPR_U32(&ctx, 2); }

    void trig(Fixture &f)
    {
        // Taylor coefficients in the four lanes used by the guest polynomial.
        f.write(0x001E2BC0, std::array{1.f / 362880, -1.f / 5040, 1.f / 120, -1.f / 6});
        assert(f.call(kSin).f[0] == 0.f);
        assert(f.call(kSin, {}, -0.f).f[0] == 0.f);
        for (const float angle : {-7.f, -1.57f, -.5f, 0.f, .5f, 1.57f, 7.f})
        {
            assert(std::abs(f.call(kSin, {}, angle).f[0] - std::sin(angle)) < 0.001f);
            assert(std::abs(f.call(kCos, {}, angle).f[0] - std::cos(angle)) < 0.001f);
        }
        for (const float angle : {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()})
        {
            assert(std::isfinite(f.call(kSin, {}, angle).f[0]));
            assert(std::isfinite(f.call(kCos, {}, angle).f[0]));
        }
    }

    void curves(Fixture &f)
    {
        constexpr uint32_t table = 0x01193230, out = 0x00300000, channel = 0x00300100;
        constexpr uint32_t motion = 0x00300200, formats = 0x00300300, channels = 0x00300400, keys = 0x00300500;
        using Quaternion = std::array<float, 4>;
        constexpr Quaternion constant{2.f, 3.f, 4.f, 5.f};
        constexpr Quaternion empty{0.f, 0.f, 0.f, 1.f};
        f.write(table, 0x010AD028u);
        f.write(table + 4 * 7, 0x010AC700u);
        f.write(table + 4 * 12, 0x010AC8A8u);
        f.write(channel, constant);
        assert(result(f.call(kCurve, {out, 0, channel, 5})) == 0);
        assert(f.read<Quaternion>(out) == constant);
        assert(result(f.call(kCurve, {out, 7, channel, 0})) == 1);
        assert(f.read<Quaternion>(out) == empty);
        assert(result(f.call(kCurve, {out, 1, channel, 0})) == kOriginal);

        // Zero keys and zero offsets give the identity quaternion.
        f.write(channel + 0x18, keys);
        f.write(channel, std::array{0.f, 0.f, 0.f, 0.f, 0.f, 0.f});
        assert(result(f.call(kCurve, {out, 12, channel, 0})) == 0);
        assert(f.read<Quaternion>(out) == empty);
        f.write(channel, constant);
        f.write(motion + 8, formats);
        f.write(motion + 0x10, channels);
        f.write(motion + 0x14, 2u);
        f.write(channels, std::array{channel, channel});
        f.write(formats, std::array<uint8_t, 2>{0, 7});
        f.call(kMotion, {out, motion, 0, 2});
        assert(f.read<Quaternion>(out) == constant);
        assert(f.read<Quaternion>(out + 16) == empty);
    }

    void textureSearch(Fixture &f)
    {
        constexpr uint32_t table = 0x00200100, head = 0x00208000, node = 0x00201000;
        constexpr uint32_t item = 0x00202000, record = 0x00203000, header = 0x00204000, name = 0x00205000;
        f.write(0x010AB3AC, 0x3C020021u); // lui + signed low half
        f.write(0x010AB3B4, 0x8C448000u);
        f.write(0x010AB3FC, 0x3C020020u);
        f.write(0x010AB404, 0x24420100u);
        f.write(0x010AB428, 0x0C000000u | (0x00128E6Cu >> 2));
        f.write(head, node);
        f.write(node + 4, item);
        f.write(item + 0x28, record);
        f.write(record + 0x20, header);
        f.write(header + 0x18, 0x80u);
        f.write(header + 0x1C, 0x13u);
        f.write(table, 0x010AB418u);
        f.write(header + 0xC0, "energy");
        f.write(name, "energ");
        assert(result(f.call(kSearch, {name})) == 0);
        f.write(name, "energy");
        assert(result(f.call(kSearch, {name})) == record);
        f.write(table, 0x010AB410u);
        assert(result(f.call(kSearch, {name})) == 0);
        f.write(header + 0x480, "energy");
        assert(result(f.call(kSearch, {name})) == record);
        f.write(table, 0x010AB420u);
        assert(result(f.call(kSearch, {name})) == kOriginal);
        f.write(0x010AB428, 0x0C010000u);
        assert(result(f.call(kSearch, {name})) == kOriginal);
    }

    void heap(Fixture &f)
    {
        constexpr uint32_t heap = 0x00400000, classes = heap + 0x100;
        constexpr uint32_t block = heap + 0x200, older = heap + 0x1000, owner = 0x00403000;
        constexpr uint32_t data = heap + 0x240, rest = data + 0x20;
        f.write(heap, classes >> 2);
        f.write<uint16_t>(heap + 4, 1);
        f.write(block, 128u);
        f.call(kInsert, {block, classes, 1, heap});
        assert(f.read(classes) == block && f.read(classes + 4) == block);
        assert(result(f.call(kAllocate, {heap, 0x20, 0x40, owner})) == data);
        assert(f.read(data - 4) == block && f.read(block + 4) == 8);
        assert((f.read(block + 0x18) & 0x00FFFFFFu) == owner >> 2);
        assert(f.read(classes) == rest && f.read(classes + 4) == rest);
        assert(f.read(rest) == 104 && f.read(rest + 0x1C) == 0);
        f.write(older, 120u);
        f.call(kInsert, {older, classes, 1, heap});
        assert(f.read(classes) == rest && f.read(classes + 4) == older);
        assert(f.read(rest + 0x1C) == older && f.read(older + 0x14) == rest >> 2);
        assert(result(f.call(kAllocate, {heap, 0x1000, 4, owner})) == 0);
        assert(result(f.call(kAllocate, {heap, 0xFFFFFFFFu, 4, owner})) == kOriginal);
    }
}

int main(int argc, char **)
{
    Fixture fixture;
    if (argc == 2)
    {
        fixture.write(0x01193230, 0x010AD028u);
        fixture.write(0x00300100, 0xDEADBEEFu);
        assert(result(fixture.call(kCurve, {0x00300000, 0, 0x00300100})) == kOriginal);
        fixture.write(0x00300208, 0x00300300u);
        fixture.write(0x00300210, 0x00300400u);
        fixture.write(0x00300214, 1u);
        fixture.write(0x00300400, 0x00300100u);
        assert(result(fixture.call(kMotion, {0x00300000, 0x00300200, 0, 1})) == kOriginal);
        assert(fixture.read(0x00300000) == 0u);
        std::puts("native decomp skip checks passed");
        return 0;
    }
    trig(fixture);
    curves(fixture);
    textureSearch(fixture);
    heap(fixture);
    std::puts("native decomp checks passed");
}
