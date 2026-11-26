#pragma once

// Known waits are counted for the frame timeline and the portable load estimate.
// On Windows the [load] report measures OS-accounted CPU time instead.

#include <atomic>
#include <chrono>
#include <cstdint>
#if defined(_WIN32)
#include <array>
#include <optional>
#endif

namespace ps2_load
{
    enum Slot : int
    {
        kNone = -1,
        kGame = 0,
        kVu1,
        kGif,
        kGs,
#if defined(_WIN32)
        kRender,
        kHost,
#endif
        kSlots
    };
    inline const char *const kNames[kSlots] = {
        "game", "vu1", "gif", "gs",
#if defined(_WIN32)
        "render", "host",
#endif
    };

    inline std::atomic<uint64_t> waitUs[kSlots]{};
    inline std::atomic<uint64_t> gsGlLockWaitUs{0}; // the GS thread waiting for the host's GL turn (Vita)

#if defined(_WIN32)
    struct ThreadCpuTime
    {
        uint64_t registrationId = 0;
        uint64_t cpuTicks = 0; // Windows FILETIME units (100 ns)
    };

    using ThreadCpuTimes = std::array<ThreadCpuTime, kSlots>;

    void registerWindowsThread(Slot slot);
    ThreadCpuTimes readWindowsThreadCpu();
    std::optional<int> threadCpuPercent(ThreadCpuTime before, ThreadCpuTime after, double wallUs);
#endif

    inline thread_local int t_slot = kNone;
    inline void setThreadSlot(Slot slot)
    {
        t_slot = slot;
#if defined(_WIN32)
        registerWindowsThread(slot);
#endif
    }

    // Counts its lifetime as waiting for the calling thread's slot.
    struct Wait
    {
        int slot = t_slot;
        std::chrono::steady_clock::time_point start = slot != kNone ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        ~Wait()
        {
            if (slot != kNone)
                waitUs[slot].fetch_add(static_cast<uint64_t>(
                                           std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()),
                                       std::memory_order_relaxed);
        }
    };
}
