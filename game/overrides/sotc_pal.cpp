#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <iostream>

namespace
{
    constexpr uint32_t kPalSyncPath = 0x001BBD78u;
    constexpr uint32_t kPalGamecoreEntry = 0x01355490u;
    PS2Runtime::RecompiledFunction g_syncPath = nullptr;
    PS2Runtime::RecompiledFunction g_gamecoreEntry = nullptr;
    bool g_gamecoreEntered = false;

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
        if (runtime.hasFunction(kPalSyncPath))
        {
            g_syncPath = runtime.lookupFunction(kPalSyncPath);
            runtime.registerFunction(kPalSyncPath, syncPath);
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("Shadow of the Colossus (PAL)", "SCES_533.26", 0x00100010u, 0x5DC13BBCu, applyPalOverrides)
