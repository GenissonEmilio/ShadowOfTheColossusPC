#include "native_backend.h"
#include "../../PS2Recomp/ps2xRuntime/src/lib/desktop_frame_pacing.h"

#include <cassert>
#include <cstdio>

using native_renderer::FrameRead;
using native_renderer::detail::FrameOutput;

int main()
{
    FrameOutput output;
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    uint64_t seen = 0;
    assert(output.read(pixels, width, height, &seen) == FrameRead::Missing);

    int uploads = 0;
    for (int refresh = 0; refresh < 100; ++refresh)
    {
        if (refresh % 5 == 0)
        {
            const std::lock_guard lock(output.mutex);
            output.width = refresh < 50 ? 512 : 1024;
            output.height = 448;
            output.pixels.assign(output.width * output.height * 4u, static_cast<uint8_t>(refresh));
            ++output.revision;
        }
        const auto result = output.read(pixels, width, height, &seen);
        assert(width == (refresh < 50 ? 512u : 1024u) && height == 448);
        assert(pixels.size() == width * height * 4u);
        if (result == FrameRead::Updated)
        {
            assert(refresh % 5 == 0 && pixels[0] == refresh);
            pixels[0] = 255;
            ++uploads;
        }
        else
            assert(result == FrameRead::Unchanged && pixels[0] == 255);
    }
    // Diagnostics and screenshots may still request a fresh copy on demand.
    assert(output.read(pixels, width, height, nullptr) == FrameRead::Updated && pixels[0] == 95);
    assert(uploads == 20);
    std::puts("PASS: 20 frame copies for 100 host refreshes, including a resolution change");

    using desktop_video::presentationLimit;
    static_assert(presentationLimit(120, false, false, true) == 120);
    static_assert(presentationLimit(100, false, false, true) == 100);
    static_assert(presentationLimit(120, false, false, false) == 60);
    static_assert(presentationLimit(120, true, false, true) == 132);
    static_assert(presentationLimit(120, true, true, true) == 30);
    static_assert(presentationLimit(60, false, false, true) == 60);
    using desktop_video::midpointHold;
    static_assert(midpointHold(2, 1.0 / 60.0, true) == 1); // 60 on 120 Hz
    static_assert(midpointHold(2, 1.0 / 50.0, true) == 1); // 50 on 100 Hz
    static_assert(midpointHold(5, 1.0 / 20.0, true) == 2); // 20 on 100 Hz
    static_assert(midpointHold(1, 1.0 / 60.0, true) == 0);
    static_assert(midpointHold(8, 0.100, true) == 0); // stall
    static_assert(midpointHold(2, 1.0 / 50.0, false) == 0); // missing real frame

    using desktop_video::FrameSource;
    desktop_video::FrameCounts presentation;
    presentation.submit({});
    presentation.submit({FrameSource::Gs, 0});
    presentation.submit({FrameSource::Gs, 0});
    presentation.submit({FrameSource::Native, 1});
    presentation.submit({FrameSource::Generated, 2});
    presentation.submit({FrameSource::Generated, 2});
    presentation.submit({FrameSource::Native, 2});
    presentation.submit({FrameSource::Native, 2});
    assert(presentation.real == 3 && presentation.generated == 1 && presentation.repeated == 4);
    assert(presentation.shown() == 4);
    std::puts("PASS: midpoint holds and counts for real, generated, and repeated swaps");
}
