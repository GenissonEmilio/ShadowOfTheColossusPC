#pragma once

#include <cstdint>
#include <cstring>
#include <functional>

// GIF data when the GS rasterizes nothing (GS::skipRaster): of a packet only
// the event registers matter (SIGNAL, FINISH, LABEL, written with A+D), the
// game waits on them; primitives, images and the rest are stepped over
// without being copied. Fed in pieces (a qword may straddle two).
class GifEventScanner
{
public:
    using WriteFn = std::function<void(uint8_t reg, uint64_t value)>;
    explicit GifEventScanner(const WriteFn &write) : m_write(write) {}

    void feed(const uint8_t *data, uint32_t bytes)
    {
        uint32_t pos = 0;
        if (m_partial != 0u)
        {
            const uint32_t n = std::min(16u - m_partial, bytes);
            std::memcpy(m_qword + m_partial, data, n);
            m_partial += n;
            pos = n;
            if (m_partial < 16u)
                return;
            m_partial = 0;
            qword(m_qword);
        }
        while (pos + 16u <= bytes)
        {
            // Data of an IMAGE or REGLIST block: skipped as a whole.
            if (m_remaining != 0u && m_format != 0u)
            {
                const uint32_t n = std::min<uint32_t>(m_remaining, (bytes - pos) / 16u);
                m_remaining -= n;
                pos += n * 16u;
                continue;
            }
            qword(data + pos);
            pos += 16u;
        }
        if (pos < bytes)
        {
            m_partial = bytes - pos;
            std::memcpy(m_qword, data + pos, m_partial);
        }
    }

private:
    void qword(const uint8_t *q)
    {
        uint64_t lo, hi;
        std::memcpy(&lo, q, 8);
        std::memcpy(&hi, q + 8, 8);
        if (m_remaining == 0u)
        {
            const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
            m_format = static_cast<uint32_t>((lo >> 58) & 3u);
            m_nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
            if (m_nreg == 0u)
                m_nreg = 16u;
            m_regs = hi;
            m_index = 0;
            m_remaining = m_format == 0u ? nloop * m_nreg : m_format == 1u ? (nloop * m_nreg + 1u) / 2u : nloop;
            return;
        }
        --m_remaining;
        if (m_format != 0u)
            return;
        const uint32_t reg = static_cast<uint32_t>(m_regs >> ((m_index % m_nreg) * 4u)) & 0xFu;
        ++m_index;
        if (reg == 0xEu) // A+D
        {
            const uint8_t address = static_cast<uint8_t>(hi & 0xFFu);
            if (address >= 0x60u && address <= 0x62u)
                m_write(address, lo);
        }
    }

    const WriteFn &m_write;
    uint8_t m_qword[16]{};
    uint32_t m_partial = 0;
    uint32_t m_remaining = 0, m_format = 0, m_nreg = 1, m_index = 0;
    uint64_t m_regs = 0;
};
