#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>

// Host elapsed time at existing execution boundaries. No GPU synchronization.
namespace ps2_pal_profile
{
    enum Metric
    {
        Vif, Walker, Unpack, Mpg, Direct, Vu1,
        Draw, VuBatch, Transfer, Upload, Clut, GlDraw,
        MetricCount
    };

    inline bool enabled()
    {
        static const bool on = []
        {
            const char *env = std::getenv("PS2X_PAL_PROFILE");
            return env && std::strcmp(env, "1") == 0;
        }();
        return on;
    }

    struct Counter
    {
        std::atomic<uint64_t> nanoseconds{0}, calls{0}, units{0};
    };
    inline std::array<Counter, MetricCount> counters{};

    class TimeInto
    {
    public:
        explicit TimeInto(Metric metric, uint64_t units = 0, bool collect = true)
            : m_counter(collect && enabled() ? &counters[metric] : nullptr),
              m_start(m_counter ? Clock::now() : Clock::time_point{}),
              m_units(units)
        {
        }

        ~TimeInto()
        {
            if (!m_counter)
                return;
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_start).count();
            m_counter->nanoseconds.fetch_add(elapsed, std::memory_order_relaxed);
            m_counter->calls.fetch_add(1, std::memory_order_relaxed);
            if (m_units)
                m_counter->units.fetch_add(m_units, std::memory_order_relaxed);
        }

        TimeInto(const TimeInto &) = delete;
        TimeInto &operator=(const TimeInto &) = delete;

        void setUnits(uint64_t units) { m_units = units; }

    private:
        using Clock = std::chrono::steady_clock;
        Counter *m_counter;
        Clock::time_point m_start;
        uint64_t m_units;
    };

    inline void report(int seconds, uint64_t frames, double windowSeconds)
    {
        if (!enabled())
            return;
        static constexpr std::array<const char *, MetricCount> names = {
            "vif", "vifWalk", "unpack", "mpg", "direct", "vu1",
            "gsDraw", "gsVu", "gsTransfer", "gsUpload", "gsClut", "gsGlDraw"
        };
        std::ostringstream line;
        line << std::fixed << std::setprecision(3)
             << "[palprof] t=" << seconds << " frames=" << frames << " windowSec=" << windowSeconds;
        for (size_t i = 0; i < names.size(); ++i)
        {
            auto &counter = counters[i];
            const auto ms = counter.nanoseconds.exchange(0, std::memory_order_relaxed) / 1'000'000.0;
            const auto calls = counter.calls.exchange(0, std::memory_order_relaxed);
            const auto units = counter.units.exchange(0, std::memory_order_relaxed);
            line << ' ' << names[i] << "Ms=" << ms << ' ' << names[i] << "Calls=" << calls;
            if (i == Vif || i == Walker || i == Upload)
                line << ' ' << names[i] << "Bytes=" << units;
            else if (i == Vu1)
                line << " vu1Pairs=" << units;
            else if (i == GlDraw)
                line << " gsGlDrawPasses=" << units;
        }
        std::fprintf(stderr, "%s\n", line.str().c_str());
    }
}
