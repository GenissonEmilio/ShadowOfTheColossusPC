// Shadow of the Colossus (SCUS-97472) runtime overrides.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "game_overrides.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_stats.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

void installSotcNative(PS2Runtime &runtime); // sotc_native.cpp

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

    // SOTC_CENSUS=1: calls per frame of the game's drawing functions
    // (docs/native_renderer_map.md), printed every 120 game frames; for
    // modelDisp also the distinct models and their draw packets.
    struct CensusEntry
    {
        uint32_t addr;
        const char *name;
    };
    constexpr CensusEntry kCensus[] = {
        {0x01097118u, "modelDisp"}, {0x01098450u, "modelClipSkin"}, {0x01098840u, "modelClipSolid"}, {0x01098cc0u, "skinDisp?"},
        {0x0109ab80u, "meshDisp"}, {0x0109a4d8u, "meshDispInstance"}, {0x0109e0e8u, "shadowDisp"}, {0x0109d320u, "shadowDraw"},
        {0x0109a228u, "ptclmanEnd"}, {0x0109a170u, "ptclDisplayListPut"}, {0x0109c108u, "waterRefraction"}, {0x01085258u, "bgDraw"},
        {0x01090a88u, "kPrintf"}, {0x001c5548u, "spriteTexFast"}, {0x010a32b8u, "spriteDrawSprite"}, {0x01114b50u, "transMicroCode"},
        {0x010aad48u, "texTransRequest"}, {0x010aae70u, "texTransRequestMulti"}, {0x01092f50u, "lineObjectDisp"},
        {0x01348468u, "dispVuColiDispWork"}, {0x01385128u, "dischargeMain"},
    };
    constexpr int kCensusCount = static_cast<int>(sizeof(kCensus) / sizeof(kCensus[0]));
    std::array<PS2Runtime::RecompiledFunction, kCensusCount> g_censusOriginal{};
    std::array<uint64_t, kCensusCount> g_censusCalls{};
    uint64_t g_censusModelPackets = 0;
    std::array<uint32_t, 4096> g_censusModels{}; // distinct model headers this window (open addressing)
    uint64_t g_censusDistinct = 0, g_censusFrame0 = 0;

    void censusReport()
    {
        const uint64_t frame = ps2_stats::vif1Chains.load(std::memory_order_relaxed);
        if (g_censusFrame0 == 0)
            g_censusFrame0 = frame;
        if (frame < g_censusFrame0 + 120u)
            return;
        const double frames = static_cast<double>(frame - g_censusFrame0);
        char line[128];
        std::string out = "[census] frame=" + std::to_string(frame);
        for (int i = 0; i < kCensusCount; ++i)
            if (g_censusCalls[i])
            {
                std::snprintf(line, sizeof(line), " %s=%.1f", kCensus[i].name, g_censusCalls[i] / frames);
                out += line;
                g_censusCalls[i] = 0;
            }
        std::snprintf(line, sizeof(line), " | distinctModels=%llu modelPackets/frame=%.1f", (unsigned long long)g_censusDistinct,
                      g_censusModelPackets / frames);
        out += line;
        std::cerr << out << std::endl;
        g_censusModelPackets = 0;
        g_censusDistinct = 0;
        g_censusModels.fill(0);
        g_censusFrame0 = frame;
    }

    template <int Slot>
    void censusCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_censusCalls[Slot];
        if (Slot == 0) // modelDisp(instance, matrix): header = *(*instance + 0x20)
        {
            auto word = [rdram](uint32_t addr) { uint32_t v = 0; std::memcpy(&v, rdram + (addr & 0x1FFFFFCu), 4); return v; };
            const uint32_t header = word(word(GPR_U32(ctx, 4)) + 0x20u);
            g_censusModelPackets += word(header + 0x64u);
            for (uint32_t probe = (header >> 4) & 4095u;; probe = (probe + 1u) & 4095u)
            {
                if (g_censusModels[probe] == header)
                    break;
                if (g_censusModels[probe] == 0u)
                {
                    g_censusModels[probe] = header;
                    ++g_censusDistinct;
                    break;
                }
            }
            censusReport();
        }
        g_censusOriginal[Slot](rdram, ctx, runtime);
    }

    template <int... Slots>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(Slots)> makeCensus(std::integer_sequence<int, Slots...>)
    {
        return {&censusCall<Slots>...};
    }

    void installCensus(PS2Runtime &runtime)
    {
        if (!std::getenv("SOTC_CENSUS"))
            return;
        static const auto hooks = makeCensus(std::make_integer_sequence<int, kCensusCount>{});
        for (int i = 0; i < kCensusCount; ++i)
            if (runtime.hasFunction(kCensus[i].addr))
            {
                g_censusOriginal[i] = runtime.lookupFunction(kCensus[i].addr);
                runtime.registerFunction(kCensus[i].addr, hooks[i]);
            }
            else
                std::cerr << "[census] no function at 0x" << std::hex << kCensus[i].addr << std::dec << " (" << kCensus[i].name << ")" << std::endl;
    }

    // SOTC_MODEL_DUMP=<game frame>: in that frame, each modelDisp call
    // (instance, model header, model matrix, camera context and its matrix at
    // +0x5a0) goes to logs/model_dump/calls.txt, then guest RAM to
    // logs/model_dump/ram.bin: input for tools/model_view.py.
    constexpr uint32_t kModelDisp = 0x01097118u, kGetCameraContext = 0x0108F518u;
    PS2Runtime::RecompiledFunction g_modelDispOriginal = nullptr, g_getCameraOriginal = nullptr;
    uint32_t g_lastCameraContext = 0;
    std::string g_modelDumpCalls;
    int g_modelDumpState = 0; // 0 waiting, 1 recording, 2 done

    void getCameraContextHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_getCameraOriginal(rdram, ctx, runtime);
        g_lastCameraContext = GPR_U32(ctx, 2);
    }

    void modelDispDumpHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const uint64_t at = std::strtoull(std::getenv("SOTC_MODEL_DUMP"), nullptr, 10);
        const uint64_t frame = ps2_stats::vif1Chains.load(std::memory_order_relaxed);
        auto word = [rdram](uint32_t addr) { uint32_t v = 0; std::memcpy(&v, rdram + (addr & 0x1FFFFFCu), 4); return v; };
        auto floats = [rdram](uint32_t addr, int n)
        {
            std::string out;
            for (int i = 0; i < n; ++i)
            {
                float f = 0.0f;
                std::memcpy(&f, rdram + ((addr + i * 4u) & 0x1FFFFFCu), 4);
                char text[32];
                std::snprintf(text, sizeof(text), " %.9g", f);
                out += text;
            }
            return out;
        };
        if (g_modelDumpState == 0 && frame >= at)
            g_modelDumpState = 1;
        if (g_modelDumpState == 1 && frame > at + 1u)
        {
            std::system("mkdir -p logs/model_dump");
            if (FILE *f = std::fopen("logs/model_dump/calls.txt", "w"))
            {
                std::fputs(g_modelDumpCalls.c_str(), f);
                std::fclose(f);
            }
            if (FILE *f = std::fopen("logs/model_dump/ram.bin", "wb"))
            {
                std::fwrite(rdram, 1, 32u << 20, f);
                std::fclose(f);
            }
            std::cerr << "[sotc] model dump written (frame " << at << ")" << std::endl;
            g_modelDumpState = 2;
        }
        const uint32_t instance = GPR_U32(ctx, 4), matrix = GPR_U32(ctx, 5);
        g_modelDispOriginal(rdram, ctx, runtime);
        if (g_modelDumpState == 1 && frame == at + 1u)
        {
            const uint32_t header = word(word(instance) + 0x20u);
            char head[96];
            std::snprintf(head, sizeof(head), "call instance=%08x header=%08x camera=%08x", instance, header, g_lastCameraContext);
            g_modelDumpCalls += std::string(head) + " model" + floats(matrix, 16) + " camera" + floats(g_lastCameraContext + 0x5a0u, 16) + "\n";
        }
    }

    // Cloth and other grid meshes (meshCreate): meshDispInstance(instance)
    // and meshDisp(mesh) calls of the dumped frame, as "mesh" lines.
    constexpr uint32_t kMeshDispInstance = 0x0109A4D8u, kMeshDisp = 0x0109AB80u;
    PS2Runtime::RecompiledFunction g_meshDispInstanceOriginal = nullptr, g_meshDispOriginal = nullptr;

    template <PS2Runtime::RecompiledFunction *Original, char Kind>
    void meshDumpHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t arg = GPR_U32(ctx, 4);
        (*Original)(rdram, ctx, runtime);
        static const uint64_t at = std::strtoull(std::getenv("SOTC_MODEL_DUMP"), nullptr, 10);
        if (g_modelDumpState == 1 && ps2_stats::vif1Chains.load(std::memory_order_relaxed) == at + 1u)
        {
            char line[96];
            std::snprintf(line, sizeof(line), "mesh %s=%08x camera=%08x\n", Kind == 'i' ? "instance" : "mesh", arg, g_lastCameraContext);
            g_modelDumpCalls += line;
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
        installCensus(runtime);
        installSotcNative(runtime);
        if (std::getenv("SOTC_MODEL_DUMP") && runtime.hasFunction(kModelDisp) && runtime.hasFunction(kGetCameraContext))
        {
            g_modelDispOriginal = runtime.lookupFunction(kModelDisp);
            runtime.registerFunction(kModelDisp, modelDispDumpHook);
            g_getCameraOriginal = runtime.lookupFunction(kGetCameraContext);
            runtime.registerFunction(kGetCameraContext, getCameraContextHook);
            if (runtime.hasFunction(kMeshDispInstance) && runtime.hasFunction(kMeshDisp))
            {
                g_meshDispInstanceOriginal = runtime.lookupFunction(kMeshDispInstance);
                runtime.registerFunction(kMeshDispInstance, meshDumpHook<&g_meshDispInstanceOriginal, 'i'>);
                g_meshDispOriginal = runtime.lookupFunction(kMeshDisp);
                runtime.registerFunction(kMeshDisp, meshDumpHook<&g_meshDispOriginal, 'm'>);
            }
        }
        if (runtime.hasFunction(kGamecoreEntry))
        {
            g_gamecoreEntry = runtime.lookupFunction(kGamecoreEntry);
            runtime.registerFunction(kGamecoreEntry, gamecoreEntryHook);
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("Shadow of the Colossus (USA)", "SCUS_974.72", 0u, 0u, applySotcOverrides)
