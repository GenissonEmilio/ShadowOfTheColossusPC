#pragma once

// A presentation frame that is a GL texture instead of pixels: the GPU GS
// renderer copies the displayed area into a texture shared with the host's
// GL context, and the frame's pixel bytes carry only this record (so the
// frame types used everywhere stay as they are).

#include <cstdint>
#include <cstring>
#include <vector>

struct GSDirectFrame
{
    static constexpr uint32_t kMagic = 0x58544C47u; // "GLTX"
    uint32_t magic = kMagic;
    uint32_t texture = 0; // GL texture name, RGBA, the frame's width x height
    uint64_t fence = 0;   // GLsync to wait on before sampling it (0: none)

    void store(std::vector<uint8_t> &pixels) const
    {
        pixels.resize(sizeof(GSDirectFrame));
        std::memcpy(pixels.data(), this, sizeof(GSDirectFrame));
    }

    static bool load(const std::vector<uint8_t> &pixels, GSDirectFrame &out)
    {
        if (pixels.size() != sizeof(GSDirectFrame))
            return false;
        std::memcpy(&out, pixels.data(), sizeof(GSDirectFrame));
        return out.magic == kMagic && out.texture != 0u;
    }
};
