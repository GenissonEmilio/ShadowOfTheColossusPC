#pragma once

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sotc_assists {

enum class Change { AddHealth, SetHealth, AddGrip };

constexpr uint32_t kAddPlayerHp = 0x013582D8u;
constexpr uint32_t kSetPlayerHp = 0x01358258u;
constexpr uint32_t kAddPlayerGrip = 0x01358578u;

constexpr uint32_t kPlayer = 0x01465EA4u; // SetSeamlessStageControlObject
constexpr uint32_t kAttractMode = 0x01465E38u; // opflowAdvertiseDemoStat
constexpr uint32_t kGameFlowEvent = 0x011E0150u; // st_gameflow transition request
constexpr uint32_t kPlayerCanDie = 0x01465E48u; // playerEnableDeath
constexpr uint32_t kGameFWork = 0x01465B70u;
constexpr uint32_t kHealth = 0x011E3EE4u;
constexpr uint32_t kGrip = 0x011E3EE0u;

template <typename T>
T read(const uint8_t *ram, uint32_t address)
{
    T value;
    std::memcpy(&value, ram + address, sizeof(value));
    return value;
}

inline bool inRam(uint32_t address, uint32_t size)
{
    return address >= 0x00100000u && (address & 3u) == 0 && address <= PS2_RAM_SIZE - size;
}

// JAL instruction addresses in the SCUS-97472 Wander damage handler (013B0EB8).
// The guest link register points past the call's delay slot.
inline bool isGameplayCall(Change change, uint32_t returnAddress)
{
    const uint32_t call = returnAddress - 8u;
    switch (change)
    {
    case Change::AddHealth:
        switch (call)
        {
        case 0x013B0FA8u: case 0x013B0FC4u: case 0x013B10BCu:
        case 0x013B163Cu: case 0x013B17D4u: case 0x013B1850u:
            return true;
        }
        break;
    case Change::SetHealth:
        // The third setPlayerHp call at 013B16A4 sets the scripted 0.01 floor.
        return call == 0x013B0F64u || call == 0x013B107Cu;
    case Change::AddGrip:
        switch (call)
        {
        case 0x013B18C8u: case 0x013B1A04u: case 0x013B1AA8u:
        case 0x013B1B4Cu: case 0x013B1BB8u: case 0x013B1C60u:
            return true;
        }
        break;
    }
    return false;
}

inline bool skipReduction(Change change, const uint8_t *ram, const R5900Context *ctx)
{
    const float value = ctx->f[12];
    const bool reduction = change == Change::SetHealth ? value == 0.f : std::isfinite(value) && value < 0.f;
    if (!reduction || !isGameplayCall(change, GPR_U32(ctx, 31)) || !ram)
        return false;

    // $s2 holds the character thread and $s3 its AI work in the damage handler.
    const uint32_t player = GPR_U32(ctx, 18);
    const uint32_t work = GPR_U32(ctx, 19);
    if (!inRam(player, 0x1224u) || !inRam(work, 0x138u) ||
        player != read<uint32_t>(ram, kPlayer) || ram[player + 8u] != 9u ||
        work != read<uint32_t>(ram, player + 0x1220u) || read<uint32_t>(ram, work + 0x134u) ||
        read<uint32_t>(ram, kAttractMode) || read<uint32_t>(ram, kGameFlowEvent) ||
        read<uint32_t>(ram, kPlayerCanDie) != 1u)
        return false;

    // _gameFWorkChk entries 11 and 12 are the saved maxima. Read them only.
    // The guest setters cap life at 570 and grip at 700 (with a 100 grip floor).
    const uint32_t maxima = read<uint32_t>(ram, kGameFWork);
    if (!inRam(maxima, 13u * sizeof(float)))
        return false;
    const float maxHealth = read<float>(ram, maxima + 11u * sizeof(float));
    const float health = read<float>(ram, kHealth);
    if (!(maxHealth > 0.f && maxHealth <= 570.f && health > 0.f && health <= maxHealth))
        return false;

    if (change == Change::AddGrip)
    {
        const float maxGrip = read<float>(ram, maxima + 12u * sizeof(float));
        const float grip = read<float>(ram, kGrip);
        return maxGrip >= 100.f && maxGrip <= 700.f && grip > 0.f && grip <= maxGrip;
    }
    return true;
}

template <Change change>
inline PS2Runtime::RecompiledFunction original = nullptr;

template <Change change>
void guardReduction(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (skipReduction(change, ram, ctx))
        ctx->f[12] = change == Change::SetHealth ? read<float>(ram, kHealth) : 0.f;
    original<change>(ram, ctx, runtime);
}

inline bool enabled(const char *value)
{
    return value && std::strcmp(value, "1") == 0;
}

template <Change change>
bool installReduction(PS2Runtime &runtime, uint32_t address)
{
    const auto guest = runtime.lookupFunction(address);
    if (guest == guardReduction<change>)
        return false;
    original<change> = guest;
    return runtime.registerFunction(address, guardReduction<change>);
}

inline void install(PS2Runtime &runtime)
{
    if (enabled(std::getenv("SOTC_INFINITE_HEALTH")) &&
        runtime.hasFunction(kSetPlayerHp) && runtime.hasFunction(kAddPlayerHp))
    {
        const bool setInstalled = installReduction<Change::SetHealth>(runtime, kSetPlayerHp);
        const bool addInstalled = installReduction<Change::AddHealth>(runtime, kAddPlayerHp);
        if (setInstalled || addInstalled)
            std::fprintf(stderr, "[sotc] infinite health enabled\n");
    }
    if (enabled(std::getenv("SOTC_INFINITE_GRIP")) && runtime.hasFunction(kAddPlayerGrip) &&
        installReduction<Change::AddGrip>(runtime, kAddPlayerGrip))
        std::fprintf(stderr, "[sotc] infinite grip enabled\n");
}

} // namespace sotc_assists
