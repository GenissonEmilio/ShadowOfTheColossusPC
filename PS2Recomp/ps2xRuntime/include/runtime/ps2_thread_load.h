#pragma once

// How busy each emulation thread is: the time it spends blocked is counted
// at its waits, the rest of the wall time is work. Printed with the [stats]
// line (always on the Vita, where there is no per-thread CPU time to ask for).

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ps2_load
{
    enum Slot : int
    {
        kNone = -1,
        kGame = 0,
        kVu1,
        kGif,
        kGs,
        kSlots
    };
    inline const char *const kNames[kSlots] = {"game", "vu1", "gif", "gs"};

    inline std::atomic<uint64_t> waitUs[kSlots]{};
    inline std::atomic<uint64_t> gsGlLockWaitUs{0}; // the GS thread waiting for the host's GL turn (Vita)

    inline thread_local int t_slot = kNone;
    inline void setThreadSlot(Slot slot) { t_slot = slot; }

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
