// Shadow of the Colossus (SCUS-97472) runtime overrides.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "game_overrides.h"
#include "runtime/ee_scheduler.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
    // Entry point of GAMECORE.XFF as laid out by the game's own loader.
    constexpr uint32_t kGamecoreEntry = 0x01344788u;

    PS2Runtime::RecompiledFunction g_gamecoreEntry = nullptr;

    // Module placement depends on the guest heap, which is only reproducible
    // while emulated time is driven purely by executed cycles. Once all code
    // modules are loaded, switch to real-time pacing.
    void gamecoreEntryHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static bool enabled = false;
        if (!enabled)
        {
            enabled = true;
            runtime->eeScheduler().enableHostClockSync();
            std::cerr << "[sotc] GAMECORE entered; host clock sync enabled" << std::endl;
        }
        g_gamecoreEntry(rdram, ctx, runtime);
    }

    // SOTC_TRACE_CALLS="0xaddr,0xaddr": count calls that go through the function
    // table (indirect calls, callbacks) and report them periodically.
    constexpr int kMaxTraced = 48;
    std::array<uint32_t, kMaxTraced> g_tracedAddr{};
    std::array<PS2Runtime::RecompiledFunction, kMaxTraced> g_tracedOriginal{};
    std::array<std::atomic<uint64_t>, kMaxTraced> g_tracedCount{};

    template <int Slot>
    void tracedCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint64_t n = g_tracedCount[Slot].fetch_add(1, std::memory_order_relaxed) + 1;
        const uint32_t a0 = GPR_U32(ctx, 4), a1 = GPR_U32(ctx, 5), ra = GPR_U32(ctx, 31);
        g_tracedOriginal[Slot](rdram, ctx, runtime);
        static const uint64_t every = std::getenv("SOTC_TRACE_EVERY") ? std::strtoull(std::getenv("SOTC_TRACE_EVERY"), nullptr, 0) : 1000;
        if (n <= 3 || n % every == 0)
        {
            std::cerr << "[sotc:call] 0x" << std::hex << g_tracedAddr[Slot] << std::dec << " count=" << n
                      << " a0=0x" << std::hex << a0 << " a1=0x" << a1 << " ra=0x" << ra << " -> v0=0x" << GPR_U32(ctx, 2) << std::dec << std::endl;
        }
    }

    template <int... Slots>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(Slots)> makeTracers(std::integer_sequence<int, Slots...>)
    {
        return {&tracedCall<Slots>...};
    }

    void installCallTracing(PS2Runtime &runtime)
    {
        const char *env = std::getenv("SOTC_TRACE_CALLS");
        if (!env)
            return;
        static const auto tracers = makeTracers(std::make_integer_sequence<int, kMaxTraced>{});
        std::stringstream list(env);
        std::string item;
        int slot = 0;
        while (std::getline(list, item, ',') && slot < kMaxTraced)
        {
            const uint32_t addr = static_cast<uint32_t>(std::stoul(item, nullptr, 0));
            if (!runtime.hasFunction(addr))
                continue;
            g_tracedAddr[slot] = addr;
            g_tracedOriginal[slot] = runtime.lookupFunction(addr);
            runtime.registerFunction(addr, tracers[slot]);
            ++slot;
        }
    }

    // SOTC_DUMP_SPRITE_PACKETS=1: dump the display-list bytes spriteDrawSprite emits.
    constexpr uint32_t kSpriteDrawSprite = 0x010A32B8u;
    constexpr uint32_t kDlAllocPtr = 0x002132E8u; // iosAllocPacketPtrPath1UnCachedA cursor
    PS2Runtime::RecompiledFunction g_spriteDrawSprite = nullptr;

    void spriteDrawSpriteHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t before = 0;
        std::memcpy(&before, rdram + kDlAllocPtr, 4);
        const uint32_t obj = GPR_U32(ctx, 4);
        static int logged = 0;
        const bool logWrites = logged++ < 2;
        if (logWrites)
        {
            g_ps2WatchWriteEnabled = true;
            g_ps2LogAllWrites = true;
            std::fprintf(stderr, "[spritepkt] begin obj=0x%08x\n", obj);
        }
        g_spriteDrawSprite(rdram, ctx, runtime);
        if (logWrites)
        {
            g_ps2LogAllWrites = false;
            g_ps2WatchWriteEnabled = std::getenv("PS2X_WATCH_WRITE") != nullptr;
            std::fprintf(stderr, "[spritepkt] end\n");
        }
        uint32_t after = 0;
        std::memcpy(&after, rdram + kDlAllocPtr, 4);
        static int dumped = 0;
        if (dumped < 12)
        {
            ++dumped;
            std::fprintf(stderr, "[spritepkt] obj=0x%08x alloc 0x%08x -> 0x%08x (%d bytes)\n", obj, before, after, static_cast<int>(after - before));
            for (uint32_t a = before; a < after && a < before + 0x200u; a += 16)
            {
                uint32_t w[4];
                std::memcpy(w, rdram + (a & 0x1FFFFFFu), 16);
                std::fprintf(stderr, "   %08x: %08x %08x %08x %08x\n", a, w[0], w[1], w[2], w[3]);
            }
        }
    }

    // KERNEL 0x1ad270 is SotC's idle thread: it loops iosRecvMsg(0x1fc870,
    // 0, 0) (a poll whose result it drops; iosWaitIdleThread's message) and
    // reads the CD clock every 1e6 turns (the queue has no buffer: the poll
    // returns 0 either way). After each poll, when no other thread can run
    // (a sender it woke is ready), wait on the host for an event instead of
    // spinning: on the Vita the spin took a core from VU1 and GS.
    constexpr uint32_t kIosRecvMsg = 0x001B1138u;
    constexpr uint32_t kIdleQueue = 0x001FC870u;
    PS2Runtime::RecompiledFunction g_iosRecvMsg = nullptr;

    void iosRecvMsgHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool idlePoll = GPR_U32(ctx, 4) == kIdleQueue && GPR_U32(ctx, 6) == 0u;
        g_iosRecvMsg(rdram, ctx, runtime);
        if (idlePoll)
            runtime->eeScheduler().idleWait();
    }

    // SotC's frame loop: the main thread blocks sending to queue 0x26d9a8;
    // its GP thread, at each VBlank once the GS has finished the previous
    // frame, calls 0x1c8078, which receives that message and lets the next
    // frame start. Nothing caps the rate (60, 30, 20... as drawing allows).
    // SOTC_FRAME_INTERVAL=<n>: release a frame only n VBlanks after the
    // previous one at the earliest (2 = steady 30 fps).
    constexpr uint32_t kGpReleaseFrame = 0x001C8078u;
    PS2Runtime::RecompiledFunction g_gpReleaseFrame = nullptr;

    void gpReleaseFrameHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const uint64_t interval = std::strtoull(std::getenv("SOTC_FRAME_INTERVAL"), nullptr, 10);
        static uint64_t lastRelease = 0;
        const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
        if (tick < lastRelease + interval)
            return; // the GP thread calls again at the next VBlank
        lastRelease = tick;
        g_gpReleaseFrame(rdram, ctx, runtime);
    }

    void applySotcOverrides(PS2Runtime &runtime)
    {
        if (std::getenv("SOTC_FRAME_INTERVAL") && runtime.hasFunction(kGpReleaseFrame))
        {
            g_gpReleaseFrame = runtime.lookupFunction(kGpReleaseFrame);
            runtime.registerFunction(kGpReleaseFrame, gpReleaseFrameHook);
        }
        if (!std::getenv("SOTC_IDLE_SPIN") && runtime.hasFunction(kIosRecvMsg))
        {
            g_iosRecvMsg = runtime.lookupFunction(kIosRecvMsg);
            runtime.registerFunction(kIosRecvMsg, iosRecvMsgHook);
        }
        if (std::getenv("SOTC_DUMP_SPRITE_PACKETS") && runtime.hasFunction(kSpriteDrawSprite))
        {
            g_spriteDrawSprite = runtime.lookupFunction(kSpriteDrawSprite);
            runtime.registerFunction(kSpriteDrawSprite, spriteDrawSpriteHook);
        }
        installCallTracing(runtime);
        if (runtime.hasFunction(kGamecoreEntry))
        {
            g_gamecoreEntry = runtime.lookupFunction(kGamecoreEntry);
            runtime.registerFunction(kGamecoreEntry, gamecoreEntryHook);
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("Shadow of the Colossus (USA)", "SCUS_974.72", 0u, 0u, applySotcOverrides)
