#pragma once

#include "runtime/gs/gs_provenance.h"
#include <algorithm>
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
    explicit GifEventScanner(const WriteFn &write, gs_provenance::Observer *observer = nullptr)
        : m_write(write), m_observer(observer) {}

    void feed(const uint8_t *data, uint32_t bytes) { feedImpl<false>(data,bytes,{}); }
    void feedObserved(const uint8_t *data, uint32_t bytes, gs_provenance::Source source)
    {
        if (m_observer) feedImpl<true>(data,bytes,source);
        else feedImpl<false>(data,bytes,{});
    }
    bool complete() const { return m_partial == 0u && m_remaining == 0u; }
    void observeVif(uint8_t opcode, const gs_provenance::Source &source, bool deferredMask)
    {
        using gs_provenance::Gap;
        if (opcode == 0x14u || opcode == 0x15u || opcode == 0x17u) gap(source,Gap::SkippedVu);
        else if (opcode == 0x06u && deferredMask) gap(source,Gap::DeferredPathMask);
        else if ((opcode & 0x60u) != 0x60u && opcode != 0x00u && opcode != 0x01u &&
                 opcode != 0x02u && opcode != 0x03u && opcode != 0x04u && opcode != 0x05u &&
                 opcode != 0x06u && opcode != 0x07u && opcode != 0x10u && opcode != 0x11u &&
                 opcode != 0x13u && opcode != 0x20u && opcode != 0x30u && opcode != 0x31u &&
                 opcode != 0x4Au && opcode != 0x50u && opcode != 0x51u) gap(source,Gap::UnknownVif);
    }
    void gap(const gs_provenance::Source &source, gs_provenance::Gap reason)
    {
        if (m_observer) m_observer->gap(source,reason);
    }
    void compacted(const gs_provenance::Source &source, uint32_t offset, uint32_t bytes)
    {
        if (m_observer) m_observer->compacted({source,offset,bytes});
    }
private:
    template<bool Observe> void feedImpl(const uint8_t *data, uint32_t bytes, gs_provenance::Source source)
    {
        uint32_t pos = 0;
        if constexpr (Observe)
        {
            if (!source.valid()) gap(source,gs_provenance::Gap::MissingOrigin);
            if (m_partial && (source.token != m_partialSource.token || source.domain != m_partialSource.domain ||
                source.address != m_partialSource.address + m_partial || source.tagVisit != m_partialSource.tagVisit ||
                source.tagData != m_partialSource.tagData))
            {
                gap(source,gs_provenance::Gap::FragmentOrigin);
                m_partialSource.domain=gs_provenance::Domain::Unknown;
            }
        }
        if (m_partial != 0u)
        {
            const uint32_t n = std::min(16u - m_partial, bytes);
            std::memcpy(m_qword + m_partial, data, n);
            m_partial += n;
            pos = n;
            if (m_partial < 16u)
                return;
            m_partial = 0;
            qword<Observe>(m_qword,m_partialSource);
        }
        while (pos + 16u <= bytes)
        {
            // Data of an IMAGE or REGLIST block, or a PACKED one with no A+D
            // register (no event can be in it): skipped as a whole.
            if (m_remaining != 0u && (m_format != 0u || !m_hasAD))
            {
                const uint32_t n = std::min<uint32_t>(m_remaining, (bytes - pos) / 16u);
                m_remaining -= n;
                pos += n * 16u;
                continue;
            }
            qword<Observe>(data + pos,source.plus(pos));
            pos += 16u;
        }
        if (pos < bytes)
        {
            if constexpr (Observe) m_partialSource=source.plus(pos);
            m_partial = bytes - pos;
            std::memcpy(m_qword, data + pos, m_partial);
        }
    }

    template<bool Observe> void qword(const uint8_t *q, gs_provenance::Source source)
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
            m_hasAD = false;
            for (uint32_t i = 0; i < m_nreg; ++i)
                m_hasAD |= ((hi >> (i * 4u)) & 0xFu) == 0xEu;
            if constexpr (Observe)
            {
                if (m_format == 0u && ((lo >> 46) & 1u)) observe(source,0,(lo >> 47) & 0x7FFu);
                if (m_format == 1u || m_format == 3u || (m_format == 0u && !m_hasAD && nloop))
                    gap(source,gs_provenance::Gap::UnsupportedGif);
            }
            m_remaining = m_format == 0u ? nloop * m_nreg : m_format == 1u ? (nloop * m_nreg + 1u) / 2u : nloop;
            return;
        }
        --m_remaining;
        if (m_format != 0u || !m_hasAD)
            return;
        // m_index counts 0..nreg-1 (a modulo here divided in software on the Vita).
        const uint32_t reg = static_cast<uint32_t>(m_regs >> (m_index * 4u)) & 0xFu;
        if (++m_index == m_nreg)
            m_index = 0;
        if constexpr (Observe)
            if (reg != 0xEu && reg != 0xFu) gap(source,gs_provenance::Gap::UnsupportedGif);
        if (reg == 0xEu) // A+D
        {
            const uint8_t address = static_cast<uint8_t>(hi & 0xFFu);
            if constexpr (Observe) observe(source,address,lo);
            if (address >= 0x60u && address <= 0x62u)
                m_write(address, lo);
        }
    }

    void observe(const gs_provenance::Source &source, uint8_t reg, uint64_t value)
    {
        if (!source.valid()) gap(source,gs_provenance::Gap::MissingOrigin);
        else m_observer->write(source,reg,value);
    }
    const WriteFn &m_write;
    gs_provenance::Observer *m_observer=nullptr;
    gs_provenance::Source m_partialSource;
    uint8_t m_qword[16]{};
    uint32_t m_partial = 0;
    uint32_t m_remaining = 0, m_format = 0, m_nreg = 1, m_index = 0;
    uint64_t m_regs = 0;
    bool m_hasAD = false; // the PACKED register list has an A+D
};
