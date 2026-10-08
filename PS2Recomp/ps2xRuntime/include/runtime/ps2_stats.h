#pragma once

#include <atomic>
#include <cstdint>

// Lightweight counters for diagnosing rendering (printed with PS2X_STATS).
namespace ps2_stats
{
    inline std::atomic<uint64_t> vu1Mscal{0};
    inline std::atomic<uint64_t> vu1Xgkick{0};
    inline std::atomic<uint64_t> vu1Mscnt{0};
    inline std::atomic<uint64_t> vifMpg{0};
    inline std::atomic<uint64_t> vu1Instr{0};
    inline std::atomic<uint64_t> vu1StallCycles{0};
    inline std::atomic<uint64_t> vifOpcodes[128]{};
    inline std::atomic<uint64_t> vifUnpack{0};
    inline std::atomic<uint64_t> vif1Bytes{0};
    inline std::atomic<uint64_t> gsPrims[8]{};
    inline std::atomic<uint64_t> gsPixels{0};
    inline std::atomic<uint64_t> vif1Chains{0};
    inline std::atomic<uint64_t> presentedFrames{0};
    inline std::atomic<uint64_t> frameSequence{0}; // captured game frames, ever
    // Host refreshes each new game frame stayed in the window: [1], [2], [3], [4+].
    inline std::atomic<uint64_t> frameHolds[4]{};
    inline std::atomic<uint64_t> vu1RecRuns{0};
    inline std::atomic<uint64_t> vu1RecBails{0};
    inline std::atomic<uint64_t> vu1InterpRuns{0}; // one display list per game frame
}
