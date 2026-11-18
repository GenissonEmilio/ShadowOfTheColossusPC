#include "../../game/overrides/sotc_assists.h"

#include <cassert>
#include <limits>
#include <memory>
#include <vector>

namespace {
using namespace sotc_assists;

constexpr uint32_t kWander = 0x00180000u;
constexpr uint32_t kWanderWork = 0x00190000u;
constexpr uint32_t kMaxima = 0x001A0000u;
std::vector<uint8_t> ram(PS2_RAM_SIZE);
float guestArgument = 0.f;
unsigned calls = 0;
bool alternateOriginalCalled = false;

template <typename T>
void put(uint32_t address, T value)
{
    std::memcpy(ram.data() + address, &value, sizeof(value));
}

void mockOriginal(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    ++calls;
    guestArgument = ctx->f[12];
    ctx->pc = GPR_U32(ctx, 31);
}

void alternateOriginal(uint8_t *memory, R5900Context *ctx, PS2Runtime *runtime)
{
    alternateOriginalCalled = true;
    mockOriginal(memory, ctx, runtime);
}

template <Change change>
constexpr uint32_t target = change == Change::AddHealth ? kAddPlayerHp :
    (change == Change::SetHealth ? kSetPlayerHp : kAddPlayerGrip);

template <Change change>
float argument(PS2Runtime &runtime, uint32_t returnAddress, float value, uint32_t player = kWander)
{
    R5900Context ctx{};
    SET_GPR_U32(&ctx, 18, player);
    SET_GPR_U32(&ctx, 19, kWanderWork);
    SET_GPR_U32(&ctx, 31, returnAddress);
    ctx.f[12] = value;
    const unsigned before = calls;
    const bool returned = runtime.callGuest(ram.data(), &ctx, target<change>, returnAddress,
                                            returnAddress - 8u, PS2Runtime::GuestBranchKind::DirectCall, "JAL");
    assert(returned);
    assert(calls == before + 1);
    assert(ctx.pc == returnAddress);
    return guestArgument;
}
} // namespace

int main()
{
    using namespace sotc_assists;
    assert(enabled("1"));
    assert(!enabled(nullptr) && !enabled("") && !enabled("0") && !enabled("10"));

    put(kPlayer, kWander);
    put(kWander + 8, uint8_t{9});
    put(kWander + 0x1220, kWanderWork);
    put(kPlayerCanDie, 1u);
    put(kGameFWork, kMaxima);
    put(kMaxima + 11 * sizeof(float), 100.f);
    put(kMaxima + 12 * sizeof(float), 340.f);
    put(kHealth, 30.f);
    put(kGrip, 0.25f);

    // Populate the same function table used by the generated game and exercise
    // startup registration and direct guest dispatch against synthetic callees.
    auto machine = std::make_unique<PS2Runtime>();
    auto &runtime = *machine;
    assert(runtime.registerFunction(kAddPlayerHp, mockOriginal));
    assert(runtime.registerFunction(kSetPlayerHp, mockOriginal));
    assert(runtime.registerFunction(kAddPlayerGrip, mockOriginal));
    unsetenv("SOTC_INFINITE_HEALTH");
    unsetenv("SOTC_INFINITE_GRIP");
    install(runtime);
    assert(argument<Change::AddHealth>(runtime, 0x013B10C4u, -300.f) == -300.f);
    assert(argument<Change::AddGrip>(runtime, 0x013B1BC0u, -8.f) == -8.f);
    setenv("SOTC_INFINITE_HEALTH", "1", 1);
    install(runtime);
    assert(argument<Change::AddHealth>(runtime, 0x013B10C4u, -300.f) == 0.f);
    assert(argument<Change::AddGrip>(runtime, 0x013B1BC0u, -8.f) == -8.f);
    setenv("SOTC_INFINITE_GRIP", "1", 1);
    install(runtime);
    install(runtime);

    const auto untouched = ram;
    // Link values emitted for the original 013B0EB8 damage handler.
    for (uint32_t address : {0x013B0FB0u, 0x013B0FCCu, 0x013B10C4u, 0x013B1644u, 0x013B17DCu, 0x013B1858u})
        assert(argument<Change::AddHealth>(runtime, address, -300.f) == 0.f);
    for (uint32_t address : {0x013B18D0u, 0x013B1A0Cu, 0x013B1AB0u, 0x013B1B54u, 0x013B1BC0u, 0x013B1C68u})
        assert(argument<Change::AddGrip>(runtime, address, -8.f) == 0.f);
    assert(argument<Change::SetHealth>(runtime, 0x013B0F6Cu, 0.f) == 30.f);
    assert(argument<Change::SetHealth>(runtime, 0x013B1084u, 0.f) == 30.f);
    assert(argument<Change::AddGrip>(runtime, 0x013B1BBCu, -8.f) == -8.f); // delay slot
    assert(argument<Change::AddHealth>(runtime, 0x013B10C4u, 3.f) == 3.f);
    assert(argument<Change::AddGrip>(runtime, 0x013B1BC0u, 8.f) == 8.f);
    assert(argument<Change::SetHealth>(runtime, 0x013B16ACu, 0.01f) == 0.01f);
    assert(argument<Change::AddHealth>(runtime, 0x0138576Cu, -10.f) == -10.f); // Dormin
    assert(argument<Change::SetHealth>(runtime, 0x013571E8u, 0.f) == 0.f); // st_opflow
    assert(argument<Change::AddGrip>(runtime, 0x013B2CC0u, -8.f) == -8.f); // pickups
    assert(argument<Change::AddHealth>(runtime, 0x013B10C4u, -10.f, kWander + 4) == -10.f);
    assert(argument<Change::AddHealth>(runtime, 0x013B10C4u, -10.f, 0xFFFFFFFCu) == -10.f);
    assert(std::isnan(argument<Change::AddHealth>(runtime, 0x013B10C4u, std::numeric_limits<float>::quiet_NaN())));
    assert(ram == untouched);

    for (uint32_t address : {kAttractMode, kGameFlowEvent, kWanderWork + 0x134})
    {
        put(address, 1u);
        assert(argument<Change::AddHealth>(runtime, 0x013B10C4u, -10.f) == -10.f);
        assert(argument<Change::AddGrip>(runtime, 0x013B1AB0u, -8.f) == -8.f);
        put(address, 0u);
    }
    put(kPlayerCanDie, 0u);
    assert(argument<Change::AddHealth>(runtime, 0x013B10C4u, -10.f) == -10.f);
    put(kPlayerCanDie, 1u);

    put(kWander + 0x1220, 0u);
    assert(argument<Change::AddHealth>(runtime, 0x013B10C4u, -10.f) == -10.f);
    put(kWander + 0x1220, kWanderWork);
    put(kWander + 8, uint8_t{10});
    assert(argument<Change::AddHealth>(runtime, 0x013B10C4u, -10.f) == -10.f);
    put(kWander + 8, uint8_t{9});
    put(kGameFWork, PS2_RAM_SIZE - 16);
    assert(argument<Change::AddHealth>(runtime, 0x013B10C4u, -10.f) == -10.f);
    put(kGameFWork, kMaxima);
    put(kHealth, 0.f);
    assert(argument<Change::AddHealth>(runtime, 0x013B10C4u, -10.f) == -10.f);
    assert(argument<Change::SetHealth>(runtime, 0x013B0F6Cu, 0.f) == 0.f);
    put(kHealth, 30.f);
    put(kMaxima + 11 * sizeof(float), 571.f);
    assert(argument<Change::AddHealth>(runtime, 0x013B10C4u, -10.f) == -10.f);
    put(kMaxima + 11 * sizeof(float), 100.f);
    put(kGrip, 0.f);
    assert(argument<Change::AddGrip>(runtime, 0x013B1AB0u, -8.f) == -8.f);
    put(kGrip, 1.f);
    put(kMaxima + 12 * sizeof(float), 701.f);
    assert(argument<Change::AddGrip>(runtime, 0x013B1AB0u, -8.f) == -8.f);

    // The global table can retain some hooks when a second ELF replaces others.
    // Restore one entry and ensure installation captures its new callee while
    // keeping the first runtime's untouched setter and grip originals.
    put(kMaxima + 12 * sizeof(float), 340.f);
    machine.reset();
    auto second = std::make_unique<PS2Runtime>();
    assert(second->registerFunction(kAddPlayerHp, alternateOriginal));
    install(*second);
    install(*second);
    assert(argument<Change::AddHealth>(*second, 0x013B10C4u, -300.f) == 0.f);
    assert(alternateOriginalCalled);
    alternateOriginalCalled = false;
    assert(argument<Change::SetHealth>(*second, 0x013B0F6Cu, 0.f) == 30.f);
    assert(argument<Change::AddGrip>(*second, 0x013B1BC0u, -8.f) == 0.f);
    assert(!alternateOriginalCalled);
}
