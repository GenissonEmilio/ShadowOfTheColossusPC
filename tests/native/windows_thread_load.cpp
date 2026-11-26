#include "runtime/ps2_thread_load.h"

#include <atomic>
#include <future>
#include <iostream>
#include <limits>
#include <thread>

int main()
{
    using namespace std::chrono_literals;
    using ps2_load::ThreadCpuTime;

    const ThreadCpuTime before{1, 25};
    if (ps2_load::threadCpuPercent(before, {1, 525}, 100.0) != 50 ||
        ps2_load::threadCpuPercent(before, {1, 25}, 100.0) != 0 ||
        ps2_load::threadCpuPercent(before, {1, 24}, 100.0) ||
        ps2_load::threadCpuPercent(before, {2, 525}, 100.0) ||
        ps2_load::threadCpuPercent(before, {}, 100.0) ||
        ps2_load::threadCpuPercent({}, before, 100.0) ||
        ps2_load::threadCpuPercent(before, before, 0.0) ||
        ps2_load::threadCpuPercent(before, before, std::numeric_limits<double>::quiet_NaN()) ||
        ps2_load::threadCpuPercent(before, before, std::numeric_limits<double>::infinity()))
    {
        std::cerr << "Invalid CPU interval accounting\n";
        return 1;
    }

    std::promise<void> registered, stop;
    auto stopRequested = stop.get_future();
    std::thread worker([&] {
        ps2_load::setThreadSlot(ps2_load::kRender);
        registered.set_value();
        stopRequested.wait();
    });
    registered.get_future().wait();

    const auto started = std::chrono::steady_clock::now();
    const auto first = ps2_load::readWindowsThreadCpu()[ps2_load::kRender];
    std::this_thread::sleep_for(200ms);
    const auto last = ps2_load::readWindowsThreadCpu()[ps2_load::kRender];
    const double elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();

    stop.set_value();
    worker.join();

    const auto percent = ps2_load::threadCpuPercent(first, last, elapsed);
    if (!percent || *percent > 15)
    {
        std::cerr << "A blocked render worker must register near-zero CPU time\n";
        return 1;
    }
    if (ps2_load::readWindowsThreadCpu()[ps2_load::kRender].registrationId != 0)
    {
        std::cerr << "The exited render worker must be unavailable\n";
        return 1;
    }
    std::promise<void> busyRegistered;
    std::atomic<bool> stopBusy{false};
    std::thread busy([&] {
        ps2_load::setThreadSlot(ps2_load::kRender);
        busyRegistered.set_value();
        volatile uint64_t work = 1;
        while (!stopBusy.load(std::memory_order_relaxed))
            work = work * 1664525 + 1013904223;
    });
    busyRegistered.get_future().wait();
    const auto busyFirst = ps2_load::readWindowsThreadCpu()[ps2_load::kRender];
    bool cpuAdvanced = false;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(20ms);
        const auto sample = ps2_load::readWindowsThreadCpu()[ps2_load::kRender];
        if (sample.registrationId == busyFirst.registrationId && sample.cpuTicks > busyFirst.cpuTicks)
        {
            cpuAdvanced = true;
            break;
        }
    }
    stopBusy.store(true, std::memory_order_relaxed);
    busy.join();
    if (!cpuAdvanced || busyFirst.registrationId == first.registrationId ||
        ps2_load::readWindowsThreadCpu()[ps2_load::kRender].registrationId != 0)
    {
        std::cerr << "An active worker must accumulate CPU time under its own registration\n";
        return 1;
    }
    std::cout << "Blocked render worker CPU: " << *percent << "%\n";
    std::cout << "Active render worker CPU time advanced\n";
}
