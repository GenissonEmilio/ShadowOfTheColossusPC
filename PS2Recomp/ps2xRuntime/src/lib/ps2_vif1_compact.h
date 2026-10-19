#pragma once

#include "ps2_gif_events.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// VIF1 stream compaction for when no VU1 program runs (GS::skipRaster):
// the DMA chain is copied without the data nothing reads. UNPACK and MPG
// become NOPs (keeping the interrupt bit) and their data is left out; every
// other VIFcode and its data (DIRECT packets for the GS, STROW, STMASK,
// STCOL) is kept, so processVIF1Data sees the same registers. DIRECT packets
// (the GS's path 2) go to a GifEventScanner instead and become NOPs too.
// Fed in pieces as the chain is walked; VIFcodes never straddle pieces
// (they are whole quadwords), data may.
class Vif1Compactor
{
public:
    Vif1Compactor(uint32_t cycle, std::vector<uint8_t> &out, GifEventScanner &direct)
        : m_cycle(cycle), m_out(out), m_direct(direct) {}
    void feed(const uint8_t *data, uint32_t bytes);
    [[nodiscard]] uint64_t skippedBytes() const { return m_skipped; }

private:
    void append(const void *data, uint32_t bytes)
    {
        if (bytes == 4u)
        {
            // One VIF code: byte pushes, no library calls (resize's fill and a
            // sized memcpy were two calls per code, thousands a frame on the Vita).
            if (m_out.capacity() < m_out.size() + 4u)
                m_out.reserve(std::max<size_t>(m_out.size() * 2u, 4096u));
            const uint8_t *p = static_cast<const uint8_t *>(data);
            m_out.push_back(p[0]);
            m_out.push_back(p[1]);
            m_out.push_back(p[2]);
            m_out.push_back(p[3]);
            return;
        }
        const size_t at = m_out.size();
        if (m_out.capacity() < at + bytes)
            m_out.reserve(std::max<size_t>(at * 2u, 4096u));
        m_out.resize(at + bytes);
        std::memcpy(m_out.data() + at, data, bytes);
    }

    uint32_t m_cycle;
    std::vector<uint8_t> &m_out;
    GifEventScanner &m_direct;
    uint32_t m_copy = 0, m_skip = 0, m_gif = 0;
    uint64_t m_skipped = 0;
};
