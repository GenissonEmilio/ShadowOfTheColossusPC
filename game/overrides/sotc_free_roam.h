#pragma once

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

namespace sotc_free_roam {

constexpr uint32_t kSetActiveBoss = 0x013494D8u;
constexpr uint32_t kInitialSelectionReturn = 0x01350F10u;
constexpr uint32_t kBossCount = 16;
constexpr uint32_t kRecordSize = 0xECu;
constexpr uint32_t kAttractMode = 0x01465E38u;

inline PS2Runtime::RecompiledFunction guestSetActiveBoss = nullptr;
inline uint32_t chosenBoss = 0;

inline uint32_t word(const uint8_t *ram, uint32_t address)
{
    uint32_t value;
    std::memcpy(&value, ram + address, sizeof(value));
    return value;
}

inline bool inRam(uint32_t address, uint32_t size)
{
    return address >= 0x00100000u && (address & 3u) == 0 && address <= PS2_RAM_SIZE - size;
}

inline uint32_t bossRecords(const uint8_t *ram)
{
    // Decode the live, relocated boss status sheet address from
    // bossStatGetDeadBossNum. GAMECORE relocates both immediates.
    const uint32_t upper = word(ram, 0x0134A38Cu);
    const uint32_t lower = word(ram, 0x0134A398u);
    if ((upper >> 16) != 0x3C02u || (lower >> 16) != 0x2442u)
        return 0;
    const uint32_t sheet = (upper << 16) + static_cast<int16_t>(lower & 0xFFFFu);
    if (!inRam(sheet, 0x14u) || word(ram, sheet + 0x0Cu) != kBossCount)
        return 0;
    const uint32_t records = word(ram, sheet + 0x10u);
    return inRam(records, kBossCount * kRecordSize) ? records : 0;
}

inline void selectBoss(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (GPR_U32(ctx, 31) == kInitialSelectionReturn && ram && !word(ram, kAttractMode))
    {
        const uint32_t first = bossRecords(ram);
        // The original campaign is about to set Valus for the first time.
        if (first && GPR_U32(ctx, 4) == first)
        {
            SET_GPR_U32(ctx, 4, first + (chosenBoss - 1u) * kRecordSize);
            std::fprintf(stderr, "[free roam] selected colossus %u\n", chosenBoss);
        }
        else
        {
            std::fputs("[free roam] could not change the initial selection\n", stderr);
        }
    }
    guestSetActiveBoss(ram, ctx, runtime);
}

inline void install(PS2Runtime &runtime)
{
    const char *selection = std::getenv("SOTC_FREE_ROAM_BOSS");
    if (!selection || std::getenv("PS2X_MODULE_CAPTURE") || guestSetActiveBoss ||
        !runtime.hasFunction(kSetActiveBoss))
        return;
    const char *cards = std::getenv("PS2X_MEMCARD_DIR");
    const char *states = std::getenv("PS2X_STATE_DIR");
    if (!cards || !*cards || !states || !*states)
        return;
    uint32_t boss = 0;
    const char *end = selection + std::strlen(selection);
    const auto result = std::from_chars(selection, end, boss);
    if (result.ec != std::errc{} || result.ptr != end || boss == 0 || boss > kBossCount)
        return;
    chosenBoss = boss;
    guestSetActiveBoss = runtime.lookupFunction(kSetActiveBoss);
    runtime.registerFunction(kSetActiveBoss, selectBoss);
    // The older directory-backed card API also needs a separate root.
    auto paths = PS2Runtime::getIoPaths();
    paths.mcRoot = std::filesystem::path(cards).parent_path() / "mc0";
    PS2Runtime::setIoPaths(paths);
}

} // namespace sotc_free_roam
