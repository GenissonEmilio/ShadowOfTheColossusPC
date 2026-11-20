#include "native_backend.h"

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
}
