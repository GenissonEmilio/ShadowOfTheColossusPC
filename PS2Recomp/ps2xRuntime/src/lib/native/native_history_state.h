#pragma once

#include "runtime/save_state_archive.h"
#include <algorithm>

namespace native_renderer::detail
{
    constexpr size_t HistoryStateLimit = 32u * 1024u * 1024u;
    struct HistoryState
    {
        uint32_t width = 0, height = 0;
        bool bloom = false, gsBloom = false;
        // RGBA8 in the GL targets' original bottom-up order, without handles.
        std::vector<uint8_t> previous, bloomPixels, gsBloomPixels;
    };

    inline uint64_t historyStateBytes(uint32_t width, uint32_t height, bool bloom, bool gsBloom)
    {
        return uint64_t(width) * height * 4u +
               (bloom ? uint64_t(std::max(width / 4u, 1u)) * std::max(height / 4u, 1u) * 4u : 0u) +
               (gsBloom ? 64u * 64u * 4u : 0u);
    }

    template<class A> bool historyStateCodec(A &a, HistoryState &s)
    {
        uint32_t magic = 0x31485047u, version = 1, endian = 0x01020304u;
        if (!a.pod(magic) || !a.pod(version) || !a.pod(endian) || magic != 0x31485047u ||
            version != 1 || endian != 0x01020304u) return a.fail("Unsupported OpenGL temporal history state.");
        if (!a.pod(s.width) || !a.pod(s.height) || !a.pod(s.bloom) || !a.pod(s.gsBloom)) return false;
        if (!s.width || !s.height || s.width > 8192 || s.height > 8192 ||
            historyStateBytes(s.width, s.height, s.bloom, s.gsBloom) > HistoryStateLimit - 64u)
            return a.fail("OpenGL temporal histories exceed the 32 MiB state budget.");
        if (!a.vector(s.previous, HistoryStateLimit) || !a.vector(s.bloomPixels, HistoryStateLimit) ||
            !a.vector(s.gsBloomPixels, 64u * 64u * 4u)) return false;
        if (s.previous.size() != uint64_t(s.width) * s.height * 4u ||
            s.bloomPixels.size() != (s.bloom ? uint64_t(std::max(s.width / 4u, 1u)) * std::max(s.height / 4u, 1u) * 4u : 0u) ||
            s.gsBloomPixels.size() != (s.gsBloom ? 64u * 64u * 4u : 0u))
            return a.fail("Invalid OpenGL temporal history dimensions.");
        return a.finish();
    }

    inline bool decodeHistoryState(const std::vector<uint8_t> &blob, HistoryState &s, std::string &error)
    {
        ps2state::Reader r(blob, error, HistoryStateLimit);
        return historyStateCodec(r, s);
    }
}
