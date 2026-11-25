#pragma once

#include <algorithm>
#include <cstdint>

namespace desktop_video
{
    constexpr int presentationLimit(int refreshRate, bool vsync, bool cap30, bool frameGeneration)
    {
        if (cap30)
            return 30;
        if (vsync)
            return refreshRate + std::max(5, refreshRate / 10);
        if (frameGeneration)
            return std::clamp(refreshRate, 60, 240);
        return 60;
    }

    // A midpoint needs a separate swap from the endpoint. Count observed host
    // swaps so a 60 Hz source can interpolate on a 120 Hz display.
    constexpr uint64_t midpointHold(uint64_t swaps, double period, bool consecutive)
    {
        return consecutive && swaps >= 2 && period > 0.0 && period <= 0.080 ? swaps / 2 : 0;
    }

    enum class FrameSource { Blank, Gs, Native, Generated };

    struct PresentedFrame
    {
        FrameSource source{};
        uint64_t revision = 0;

        bool operator==(const PresentedFrame &) const = default;
    };

    struct FrameCounts
    {
        uint64_t real = 0;
        uint64_t generated = 0;
        uint64_t repeated = 0;

        uint64_t shown() const { return real + generated; }

        void submit(PresentedFrame frame)
        {
            if (frame == last || frame.source == FrameSource::Blank)
                ++repeated;
            else if (frame.source == FrameSource::Generated)
                ++generated;
            else
                ++real;
            last = frame;
        }

    private:
        PresentedFrame last{};
    };
}
