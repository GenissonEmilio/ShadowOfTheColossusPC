#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "runtime/ps2_thread_load.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace ps2_load
{
    namespace
    {
        struct ThreadEntry
        {
            HANDLE handle = nullptr;
            uint64_t id = 0;
        };

        std::mutex threadMutex;
        std::array<ThreadEntry, kSlots> threads{};
        uint64_t nextId = 1;

        struct Registration
        {
            Slot slot = kNone;
            HANDLE handle = nullptr;

            ~Registration() { bind(kNone); }

            void bind(Slot next)
            {
                if (next == slot)
                    return;

                std::lock_guard lock(threadMutex);
                if (handle)
                {
                    if (threads[slot].handle == handle)
                        threads[slot] = {};
                    CloseHandle(handle);
                }

                slot = next;
                handle = nullptr;
                if (slot != kNone)
                {
                    // Own a query-only handle for this thread instance. A
                    // sampler can never race thread ID reuse or handle closure.
                    handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
                    if (handle)
                        threads[slot] = {handle, nextId++};
                }
            }
        };

        uint64_t ticks(FILETIME value)
        {
            return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
        }
    }

    void registerWindowsThread(Slot slot)
    {
        thread_local Registration registration;
        registration.bind(slot);
    }

    ThreadCpuTimes readWindowsThreadCpu()
    {
        std::lock_guard lock(threadMutex);
        ThreadCpuTimes result{};
        for (size_t i = 0; i < threads.size(); ++i)
        {
            FILETIME created, exited, kernel, user;
            const auto &entry = threads[i];
            if (entry.handle && GetThreadTimes(entry.handle, &created, &exited, &kernel, &user))
                result[i] = {entry.id, ticks(kernel) + ticks(user)};
        }
        return result;
    }

    std::optional<int> threadCpuPercent(ThreadCpuTime before, ThreadCpuTime after, double wallUs)
    {
        if (!before.registrationId || before.registrationId != after.registrationId ||
            before.cpuTicks > after.cpuTicks || !std::isfinite(wallUs) || wallUs <= 0.0)
            return std::nullopt;

        const double percent = (after.cpuTicks - before.cpuTicks) * 10.0 / wallUs;
        return static_cast<int>(std::min(percent, 100.0) + 0.5);
    }
}
