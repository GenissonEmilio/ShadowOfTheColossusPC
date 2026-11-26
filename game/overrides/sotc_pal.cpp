#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>

void installSotcPalNativeComparison(PS2Runtime &runtime);

namespace
{
    constexpr uint32_t kPalSyncPath = 0x001BBD78u;
    constexpr uint32_t kPalGamecoreEntry = 0x01355490u;
    constexpr uint32_t kPalBootChooseLanguage = 0x0138C2A8u;
    constexpr uint32_t kPalLanguageChosen = 0x01477F3Cu;
    constexpr uint32_t kPalChosenLanguage = 0x01477F40u;
    PS2Runtime::RecompiledFunction g_syncPath = nullptr;
    PS2Runtime::RecompiledFunction g_gamecoreEntry = nullptr;
    PS2Runtime::RecompiledFunction g_bootChooseLanguage = nullptr;
    bool g_gamecoreEntered = false;

    uint32_t guestLanguageCode(std::string_view language)
    {
        if (language == "en") return 1;
        if (language == "fr") return 2;
        if (language == "es") return 3;
        if (language == "de") return 4;
        if (language == "it") return 5;
        return 0;
    }

    void bootChooseLanguage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Use native startup during module capture so the launcher preference
        // cannot change the guest state used to compile any language.
        const char *preference = std::getenv("SOTC_CAMPAIGN_PAL_LANGUAGE");
        const uint32_t language = preference && !std::getenv("PS2X_MODULE_CAPTURE")
            ? guestLanguageCode(preference) : 0;
        if (!language)
        {
            g_bootChooseLanguage(rdram, ctx, runtime);
            return;
        }

        // Match bootChooseLangScript's successful memory-card branch in RAM.
        // The guest applies this code before loading localized assets and
        // separately reads its usual display mode from the card.
        constexpr uint32_t chosen = 1;
        std::memcpy(getMemPtr(rdram, kPalLanguageChosen), &chosen, sizeof(chosen));
        std::memcpy(getMemPtr(rdram, kPalChosenLanguage), &language, sizeof(language));
        SET_GPR_U64(ctx, 2, chosen);
        ctx->pc = GPR_U32(ctx, 31);
        std::cerr << "[campaign-language] " << preference << '\n';
    }

    void syncPath(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_gamecoreEntered && GPR_U32(ctx, 4) == 0u)
        {
            auto &memory = runtime->memory();
            memory.processPendingTransfers();
            // This also waits for GIF packet work dispatched by the VU1
            // worker. The caller's guest timeout measures PS2 work and would
            // otherwise expire while the host workers are still processing it.
            memory.syncVu1();
        }
        // Keep the game's VIF/DMA status, timeout and error handling. Polls
        // (a0 != 0) must stay nonblocking.
        g_syncPath(rdram, ctx, runtime);
    }

    void enterGamecore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_gamecoreEntered)
        {
            g_gamecoreEntered = true;
            runtime->eeScheduler().enableHostClockSync();
            std::cerr << "[sotc] PAL GAMECORE entered\n";
        }
        g_gamecoreEntry(rdram, ctx, runtime);
    }

    void applyPalOverrides(PS2Runtime &runtime)
    {
        if (!runtime.hasFunction(kPalGamecoreEntry))
            return;

        g_gamecoreEntered = false;
        g_gamecoreEntry = runtime.lookupFunction(kPalGamecoreEntry);
        runtime.registerFunction(kPalGamecoreEntry, enterGamecore);
        installSotcPalNativeComparison(runtime);
        if (runtime.hasFunction(kPalBootChooseLanguage))
        {
            if (const auto original = runtime.lookupFunction(kPalBootChooseLanguage); original != bootChooseLanguage)
            {
                g_bootChooseLanguage = original;
                runtime.registerFunction(kPalBootChooseLanguage, bootChooseLanguage);
            }
        }
        if (runtime.hasFunction(kPalSyncPath))
        {
            g_syncPath = runtime.lookupFunction(kPalSyncPath);
            runtime.registerFunction(kPalSyncPath, syncPath);
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("Shadow of the Colossus (PAL)", "SCES_533.26", 0x00100010u, 0x5DC13BBCu, applyPalOverrides)
