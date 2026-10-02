#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Minimal persistent worker pool for splitting rasterization into row bands.
// The calling thread participates, and run() returns once every band is done.
class GSParallelFor
{
public:
    static GSParallelFor &instance()
    {
        static GSParallelFor pool;
        return pool;
    }

    unsigned workerCount() const { return static_cast<unsigned>(m_workers.size()); }

    void run(int bandCount, const std::function<void(int)> &job)
    {
        if (bandCount <= 1 || m_workers.empty())
        {
            for (int i = 0; i < bandCount; ++i)
                job(i);
            return;
        }
        // Each run has its own counters, so a worker that wakes late for an
        // earlier run can never take or complete a band of this one.
        Job current;
        current.fn = &job;
        current.count = bandCount;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_current = &current;
            ++m_generation;
        }
        m_wake.notify_all();
        participate(current);
        std::unique_lock<std::mutex> lock(m_mutex);
        m_finished.wait(lock, [&current]
                        { return current.done.load(std::memory_order_acquire) == current.count && current.active == 0; });
        m_current = nullptr;
    }

private:
    GSParallelFor()
    {
        unsigned n = std::thread::hardware_concurrency();
        if (const char *env = std::getenv("PS2X_GS_THREADS"))
            n = static_cast<unsigned>(std::max(1, std::atoi(env)));
        else
            n = std::clamp(n > 2u ? n - 2u : 1u, 1u, 8u);
        for (unsigned i = 1; i < n; ++i)
            m_workers.emplace_back([this] { workerLoop(); });
    }

    ~GSParallelFor()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
            ++m_generation;
        }
        m_wake.notify_all();
        for (auto &t : m_workers)
            t.join();
    }

    struct Job
    {
        const std::function<void(int)> *fn = nullptr;
        int count = 0;
        std::atomic<int> next{0};
        std::atomic<int> done{0};
        int active = 0; // workers inside participate(), guarded by m_mutex
    };

    void participate(Job &job)
    {
        for (;;)
        {
            const int band = job.next.fetch_add(1, std::memory_order_relaxed);
            if (band >= job.count)
                break;
            (*job.fn)(band);
            if (job.done.fetch_add(1, std::memory_order_acq_rel) + 1 == job.count)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_finished.notify_all();
            }
        }
    }

    void workerLoop()
    {
        uint64_t seen = 0;
        for (;;)
        {
            Job *job = nullptr;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [&] { return m_stop || m_generation != seen; });
                if (m_stop)
                    return;
                seen = m_generation;
                job = m_current;
                if (!job)
                    continue;
                ++job->active;
            }
            participate(*job);
            std::lock_guard<std::mutex> lock(m_mutex);
            if (--job->active == 0)
                m_finished.notify_all();
        }
    }

    std::vector<std::thread> m_workers;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_finished;
    Job *m_current = nullptr;
    uint64_t m_generation = 0;
    bool m_stop = false;
};
