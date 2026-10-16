#include "iop_spu2.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr int32_t kFilter0[5] = {0, 60, 115, 98, 122};
        constexpr int32_t kFilter1[5] = {0, 0, -52, -55, -60};

        // Register offsets inside a core (core 1 is +0x400).
        constexpr uint32_t kPmon = 0x180, kNon = 0x184, kVmixl = 0x188, kVmixel = 0x18C,
                           kVmixr = 0x190, kVmixer = 0x194, kMmix = 0x198, kAttr = 0x19A,
                           kIrqaH = 0x19C, kIrqaL = 0x19E, kKon = 0x1A0, kKoff = 0x1A4,
                           kTsaH = 0x1A8, kTsaL = 0x1AA, kData = 0x1AC, kAdmas = 0x1B0,
                           kVoiceAddr = 0x1C0, kEndx = 0x340, kStatx = 0x344;
        // Shared area: core 0 volumes at 0x760, core 1 at 0x788.
        constexpr uint32_t kVolBase = 0x760, kVolStride = 0x28, kIrqInfo = 0x7C2;

        inline int16_t clamp16(int32_t value)
        {
            return static_cast<int16_t>(std::clamp<int32_t>(value, -32768, 32767));
        }

        inline uint32_t setHigh(uint32_t address, uint16_t value) { return ((value & 0xFu) << 16u) | (address & 0xFFFFu); }
        inline uint32_t setLow(uint32_t address, uint16_t value) { return (address & 0xF0000u) | value; }
    }

    Spu2::Spu2()
        : m_ram(RamWords), m_ring(kRingFrames * 2u)
    {
        reset();
    }

    void Spu2::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
        m_regs.fill(0);
        for (Core &core : m_cores)
            core = Core{};
        m_irqInfo = 0;
        m_irqPending = false;
        m_cycleValid = false;
        m_ringWrite.store(0);
        m_ringRead.store(0);
    }

    int32_t Spu2::fixedVolume(uint16_t value) noexcept
    {
        // Bit 15 clear: fixed volume, 15-bit signed (bit 14 is the sign).
        // Sweep mode is approximated by its maximum magnitude.
        if (value & 0x8000u)
            return 0x7FFF;
        return static_cast<int16_t>(static_cast<uint16_t>(value << 1u));
    }

    void Spu2::keyOn(int coreIndex, uint32_t mask)
    {
        Core &core = m_cores[coreIndex];
        static const bool trace = std::getenv("PS2X_SPU2_TRACE") != nullptr;
        if (trace)
        {
            static int logged = 0;
            for (int v = 0; v < 24 && logged < 400; ++v)
                if (mask & (1u << v))
                {
                    ++logged;
                    const Voice &voice = core.voices[v];
                    std::fprintf(stderr, "[spu2] KON core=%d v=%d ssa=0x%05x pitch=0x%04x adsr=%04x/%04x vol=%04x/%04x mmix=%03x mvol=%04x/%04x vmixl=%06x attr=%04x\n",
                                 coreIndex, v, voice.ssa, voice.pitch, voice.adsr1, voice.adsr2, voice.volL, voice.volR,
                                 core.mmix, core.mvolL, core.mvolR, core.vmixl, core.attr);
                }
        }
        for (int v = 0; v < 24; ++v)
        {
            if (!(mask & (1u << v)))
                continue;
            Voice &voice = core.voices[v];
            voice.phase = Phase::Attack;
            voice.level = 0;
            voice.envCounter = 0;
            voice.counter = 0;
            voice.prev1 = voice.prev2 = 0;
            voice.history.fill(0);
            voice.nax = voice.ssa;
            voice.blockIndex = 28;
            voice.blockFlags = 0;
            core.endx &= ~(1u << v);
        }
    }

    void Spu2::keyOff(int coreIndex, uint32_t mask)
    {
        Core &core = m_cores[coreIndex];
        for (int v = 0; v < 24; ++v)
        {
            if ((mask & (1u << v)) && core.voices[v].phase != Phase::Off)
            {
                core.voices[v].phase = Phase::Release;
                core.voices[v].envCounter = 0;
            }
        }
    }

    void Spu2::checkIrq(uint32_t start, uint32_t words)
    {
        for (int c = 0; c < 2; ++c)
        {
            const Core &core = m_cores[c];
            if (!(core.attr & 0x40u))
                continue;
            const uint32_t offset = (core.irqa - start) & (RamWords - 1u);
            if (offset < words)
            {
                m_irqInfo |= static_cast<uint16_t>(4u << c);
                m_irqPending = true;
            }
        }
    }

    bool Spu2::takeIrq() noexcept
    {
        const bool pending = m_irqPending;
        m_irqPending = false;
        return pending;
    }

    uint16_t Spu2::read16(uint32_t phys)
    {
        const uint32_t offset = (phys - Base) & (RegisterBytes - 2u);
        if (offset >= kVolBase)
        {
            if (offset == kIrqInfo)
            {
                const uint16_t value = m_irqInfo;
                m_irqInfo = 0;
                return value;
            }
            return m_regs[offset / 2u];
        }
        const int c = offset >= 0x400u ? 1 : 0;
        const uint32_t rel = offset - static_cast<uint32_t>(c) * 0x400u;
        Core &core = m_cores[c];
        if (rel < 0x180u)
        {
            Voice &voice = core.voices[rel >> 4u];
            switch (rel & 0xFu)
            {
            case 0xA: return static_cast<uint16_t>(voice.level);
            case 0xC: return static_cast<uint16_t>(voice.volXL);
            case 0xE: return static_cast<uint16_t>(voice.volXR);
            default: return m_regs[offset / 2u];
            }
        }
        if (rel >= kVoiceAddr && rel < kVoiceAddr + 24u * 0xCu)
        {
            const Voice &voice = core.voices[(rel - kVoiceAddr) / 0xCu];
            switch ((rel - kVoiceAddr) % 0xCu)
            {
            case 0x0: return static_cast<uint16_t>(voice.ssa >> 16u);
            case 0x2: return static_cast<uint16_t>(voice.ssa);
            case 0x4: return static_cast<uint16_t>(voice.lsax >> 16u);
            case 0x6: return static_cast<uint16_t>(voice.lsax);
            case 0x8: return static_cast<uint16_t>(voice.nax >> 16u);
            default: return static_cast<uint16_t>(voice.nax);
            }
        }
        switch (rel)
        {
        case kAttr: return core.attr;
        case kIrqaH: return static_cast<uint16_t>(core.irqa >> 16u);
        case kIrqaL: return static_cast<uint16_t>(core.irqa);
        case kTsaH: return static_cast<uint16_t>(core.tsa >> 16u);
        case kTsaL: return static_cast<uint16_t>(core.tsa);
        case kAdmas: return core.admas;
        case kEndx: return static_cast<uint16_t>(core.endx);
        case kEndx + 2: return static_cast<uint16_t>(core.endx >> 16u);
        case kStatx: return core.statx;
        case kData:
        {
            const uint16_t value = m_ram[core.tsa];
            core.tsa = (core.tsa + 1u) & (RamWords - 1u);
            return value;
        }
        default:
            return m_regs[offset / 2u];
        }
    }

    void Spu2::write16(uint32_t phys, uint16_t value)
    {
        const uint32_t offset = (phys - Base) & (RegisterBytes - 2u);
        m_regs[offset / 2u] = value;
        if (offset >= kVolBase)
        {
            for (int c = 0; c < 2; ++c)
            {
                const uint32_t base = kVolBase + static_cast<uint32_t>(c) * kVolStride;
                Core &core = m_cores[c];
                switch (offset - base)
                {
                case 0x0: core.mvolL = value; break;
                case 0x2: core.mvolR = value; break;
                case 0x4: core.evolL = value; break;
                case 0x6: core.evolR = value; break;
                case 0x8: core.avolL = value; break;
                case 0xA: core.avolR = value; break;
                case 0xC: core.bvolL = value; break;
                case 0xE: core.bvolR = value; break;
                default:
                    if (offset - base >= 0x14u && offset - base < 0x28u)
                        core.revVol[(offset - base - 0x14u) / 2u] = static_cast<int16_t>(value);
                    break;
                }
            }
            return;
        }
        const int c = offset >= 0x400u ? 1 : 0;
        const uint32_t rel = offset - static_cast<uint32_t>(c) * 0x400u;
        Core &core = m_cores[c];
        if (rel < 0x180u)
        {
            Voice &voice = core.voices[rel >> 4u];
            switch (rel & 0xFu)
            {
            case 0x0: voice.volL = value; break;
            case 0x2: voice.volR = value; break;
            case 0x4: voice.pitch = value; break;
            case 0x6: voice.adsr1 = value; break;
            case 0x8: voice.adsr2 = value; break;
            case 0xA: voice.level = value & 0x7FFF; break;
            case 0xC: voice.volXL = static_cast<int16_t>(value); break;
            case 0xE: voice.volXR = static_cast<int16_t>(value); break;
            default: break;
            }
            return;
        }
        if (rel >= kVoiceAddr && rel < kVoiceAddr + 24u * 0xCu)
        {
            Voice &voice = core.voices[(rel - kVoiceAddr) / 0xCu];
            switch ((rel - kVoiceAddr) % 0xCu)
            {
            case 0x0: voice.ssa = setHigh(voice.ssa, value); break;
            case 0x2: voice.ssa = setLow(voice.ssa, value); break;
            case 0x4: voice.lsax = setHigh(voice.lsax, value); break;
            case 0x6: voice.lsax = setLow(voice.lsax, value); break;
            case 0x8: voice.nax = setHigh(voice.nax, value); break;
            default: voice.nax = setLow(voice.nax, value); break;
            }
            return;
        }
        switch (rel)
        {
        case kVmixl: core.vmixl = (core.vmixl & 0xFF0000u) | value; break;
        case kVmixl + 2: core.vmixl = (core.vmixl & 0xFFFFu) | ((value & 0xFFu) << 16u); break;
        case kVmixr: core.vmixr = (core.vmixr & 0xFF0000u) | value; break;
        case kVmixr + 2: core.vmixr = (core.vmixr & 0xFFFFu) | ((value & 0xFFu) << 16u); break;
        case kVmixel: core.vmixel = (core.vmixel & 0xFF0000u) | value; break;
        case kVmixel + 2: core.vmixel = (core.vmixel & 0xFFFFu) | ((value & 0xFFu) << 16u); break;
        case kVmixer: core.vmixer = (core.vmixer & 0xFF0000u) | value; break;
        case kVmixer + 2: core.vmixer = (core.vmixer & 0xFFFFu) | ((value & 0xFFu) << 16u); break;
        case 0x2E0: core.esa = setHigh(core.esa, value); break;
        case 0x2E2: core.esa = setLow(core.esa, value); break;
        case 0x33C: core.eea = ((value & 0xFu) << 16u) | 0xFFFFu; break;
        case kMmix: core.mmix = value; break;
        case kAttr: core.attr = value; break;
        case kIrqaH: core.irqa = setHigh(core.irqa, value); break;
        case kIrqaL: core.irqa = setLow(core.irqa, value); break;
        case kKon: keyOn(c, value); break;
        case kKon + 2: keyOn(c, static_cast<uint32_t>(value & 0xFFu) << 16u); break;
        case kKoff: keyOff(c, value); break;
        case kKoff + 2: keyOff(c, static_cast<uint32_t>(value & 0xFFu) << 16u); break;
        case kTsaH: core.tsa = setHigh(core.tsa, value); break;
        case kTsaL: core.tsa = setLow(core.tsa, value); break;
        case kData:
            checkIrq(core.tsa, 1);
            m_ram[core.tsa] = value;
            core.tsa = (core.tsa + 1u) & (RamWords - 1u);
            break;
        case kAdmas:
            core.admas = value;
            if (!(value & (1u << c)))
                core.input.clear();
            break;
        case kStatx: core.statx = value; break;
        default:
            if (rel >= 0x2E4u && rel < 0x33Cu)
            {
                uint32_t &address = core.revAddr[(rel - 0x2E4u) / 4u];
                address = (rel & 2u) ? setLow(address, value) : setHigh(address, value);
            }
            break;
        case kEndx:
        case kEndx + 2:
            break;
        }
    }

    uint64_t Spu2::dmaWrite(int c, const uint8_t *source, uint32_t bytes)
    {
        Core &core = m_cores[c];
        static const bool trace = std::getenv("PS2X_SPU2_TRACE") != nullptr;
        if (trace)
        {
            static int logged = 0;
            if (logged++ < 300)
                std::fprintf(stderr, "[spu2] DMA core=%d bytes=%u tsa=0x%05x admas=%x\n", c, bytes, core.tsa, core.admas);
        }
        core.statx |= 0x80u;
        if (core.admas & (1u << c))
        {
            // Auto-DMA: blocks of 256 left samples followed by 256 right samples.
            for (uint32_t block = 0; block + 1024u <= bytes; block += 1024u)
            {
                for (uint32_t i = 0; i < 256u; ++i)
                {
                    int16_t left, right;
                    std::memcpy(&left, source + block + i * 2u, 2);
                    std::memcpy(&right, source + block + 512u + i * 2u, 2);
                    core.input.push_back({left, right});
                }
            }
            // Ask for more data while about two blocks are still queued.
            const size_t queued = core.input.size();
            return queued > 512u ? (queued - 512u) * CyclesPerSample : 64u;
        }
        const uint32_t words = bytes / 2u;
        checkIrq(core.tsa, words);
        for (uint32_t i = 0; i < words; ++i)
        {
            uint16_t value;
            std::memcpy(&value, source + i * 2u, 2);
            m_ram[core.tsa] = value;
            core.tsa = (core.tsa + 1u) & (RamWords - 1u);
        }
        return std::max<uint64_t>(words, 64u);
    }

    uint64_t Spu2::dmaRead(int c, uint8_t *destination, uint32_t bytes)
    {
        Core &core = m_cores[c];
        core.statx |= 0x80u;
        const uint32_t words = bytes / 2u;
        for (uint32_t i = 0; i < words; ++i)
        {
            std::memcpy(destination + i * 2u, &m_ram[core.tsa], 2);
            core.tsa = (core.tsa + 1u) & (RamWords - 1u);
        }
        return std::max<uint64_t>(words, 64u);
    }

    void Spu2::decodeBlock(Core &core, int coreIndex, Voice &voice)
    {
        (void)core;
        (void)coreIndex;
        const uint32_t address = voice.nax & (RamWords - 8u);
        checkIrq(address, 8);
        const uint16_t header = m_ram[address];
        uint32_t shift = header & 0xFu;
        const uint32_t filter = std::min<uint32_t>((header >> 4u) & 7u, 4u);
        if (shift > 12u)
            shift = 9u;
        voice.blockFlags = static_cast<uint8_t>(header >> 8u);
        voice.blockAddress = address;
        if (voice.blockFlags & 4u)
            voice.lsax = address;
        for (uint32_t i = 0; i < 28u; ++i)
        {
            const uint16_t word = m_ram[address + 1u + i / 4u];
            const int32_t nibble = (word >> ((i & 3u) * 4u)) & 0xFu;
            int32_t sample = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12u)) >> shift;
            sample += (voice.prev1 * kFilter0[filter] + voice.prev2 * kFilter1[filter] + 32) >> 6;
            sample = clamp16(sample);
            voice.prev2 = voice.prev1;
            voice.prev1 = sample;
            voice.block[i] = static_cast<int16_t>(sample);
        }
        voice.blockIndex = 0;
    }

    int32_t Spu2::nextVoiceSample(Core &core, int coreIndex, Voice &voice, int voiceIndex)
    {
        if (voice.blockIndex >= 28u)
            decodeBlock(core, coreIndex, voice);
        const int32_t sample = voice.block[voice.blockIndex++];
        if (voice.blockIndex >= 28u)
        {
            // End of block: follow the loop flags.
            if (voice.blockFlags & 1u)
            {
                core.endx |= 1u << voiceIndex;
                voice.nax = voice.lsax;
                if (!(voice.blockFlags & 2u))
                {
                    voice.phase = Phase::Off;
                    voice.level = 0;
                }
            }
            else
            {
                voice.nax = (voice.blockAddress + 8u) & (RamWords - 1u);
            }
        }
        return sample;
    }

    void Spu2::tickEnvelope(Voice &voice)
    {
        auto step = [&voice](uint32_t rate, bool increase, bool exponential)
        {
            const int32_t shift = static_cast<int32_t>(rate >> 2u);
            const int32_t stepValue = static_cast<int32_t>(rate & 3u);
            const int32_t base = increase ? (7 - stepValue) : (-8 + stepValue);
            uint32_t cycles = 1u << std::max(0, shift - 11);
            int32_t delta = base * (1 << std::max(0, 11 - shift));
            if (exponential && increase && voice.level > 0x6000)
                cycles *= 4u;
            if (exponential && !increase)
                delta = (delta * voice.level) >> 15;
            if (++voice.envCounter >= cycles)
            {
                voice.envCounter = 0;
                voice.level = std::clamp(voice.level + delta, 0, 0x7FFF);
            }
        };
        switch (voice.phase)
        {
        case Phase::Attack:
            step((voice.adsr1 >> 8u) & 0x7Fu, true, (voice.adsr1 & 0x8000u) != 0u);
            if (voice.level >= 0x7FFF)
            {
                voice.level = 0x7FFF;
                voice.phase = Phase::Decay;
                voice.envCounter = 0;
            }
            break;
        case Phase::Decay:
        {
            step(((voice.adsr1 >> 4u) & 0xFu) << 2u, false, true);
            const int32_t sustain = ((voice.adsr1 & 0xFu) + 1) << 11;
            if (voice.level <= sustain)
            {
                voice.level = std::min(sustain, 0x7FFF);
                voice.phase = Phase::Sustain;
                voice.envCounter = 0;
            }
            break;
        }
        case Phase::Sustain:
            step((voice.adsr2 >> 6u) & 0x7Fu, (voice.adsr2 & 0x4000u) == 0u, (voice.adsr2 & 0x8000u) != 0u);
            break;
        case Phase::Release:
            step((voice.adsr2 & 0x1Fu) << 2u, false, (voice.adsr2 & 0x20u) != 0u);
            if (voice.level <= 0)
            {
                voice.level = 0;
                voice.phase = Phase::Off;
            }
            break;
        case Phase::Off:
            break;
        }
    }

    void Spu2::processReverb(Core &core, int32_t inL, int32_t inR, int32_t &outL, int32_t &outR)
    {
        // PS1/PS2 reverb, run at 24 kHz: every other sample consumes the
        // averaged input of two samples; output is interpolated in between.
        core.revInL += inL;
        core.revInR += inR;
        core.reverbOdd = !core.reverbOdd;
        if (core.reverbOdd)
        {
            outL = (core.revPrevL + core.revOutL) / 2;
            outR = (core.revPrevR + core.revOutR) / 2;
            return;
        }
        const int32_t Lin0 = core.revInL / 2, Rin0 = core.revInR / 2;
        core.revInL = core.revInR = 0;
        core.revPrevL = core.revOutL;
        core.revPrevR = core.revOutR;

        const uint32_t esa = core.esa & (RamWords - 1u);
        const uint32_t eea = std::max(core.eea & (RamWords - 1u), esa);
        const uint32_t size = eea - esa + 1u;
        if (!(core.attr & 0x80u) || size < 0x100u)
        {
            core.revOutL = core.revOutR = 0;
            outL = core.revPrevL;
            outR = core.revPrevR;
            return;
        }
        const auto &A = core.revAddr;
        const auto &V = core.revVol;
        // Wrap into the work area. Offsets land within one wrap of it in
        // practice: a compare instead of a division (the Cortex-A9 has no
        // integer divide; this runs ~40 times per sample).
        const int32_t isize = static_cast<int32_t>(size);
        auto index = [&](int64_t offset) -> uint32_t
        {
            int32_t position = static_cast<int32_t>(core.reverbX) + static_cast<int32_t>(offset);
            if (position >= isize)
            {
                position -= isize;
                if (position >= isize)
                    position %= isize;
            }
            else if (position < 0)
            {
                position += isize;
                if (position < 0)
                {
                    position %= isize;
                    if (position < 0)
                        position += isize;
                }
            }
            return esa + static_cast<uint32_t>(position);
        };
        auto rd = [&](int64_t offset) -> int32_t { return static_cast<int16_t>(m_ram[index(offset)]); };
        auto wr = [&](int64_t offset, int32_t value) { m_ram[index(offset)] = static_cast<uint16_t>(clamp16(value)); };
        auto mul = [](int32_t a, int32_t b) -> int32_t { return (a * b) >> 15; };

        const int32_t Lin = mul(Lin0, V[8]);
        const int32_t Rin = mul(Rin0, V[9]);
        const int64_t dAPF1 = A[0], dAPF2 = A[1], mLSAME = A[2], mRSAME = A[3], mLCOMB1 = A[4], mRCOMB1 = A[5],
                      mLCOMB2 = A[6], mRCOMB2 = A[7], dLSAME = A[8], dRSAME = A[9], mLDIFF = A[10], mRDIFF = A[11],
                      mLCOMB3 = A[12], mRCOMB3 = A[13], mLCOMB4 = A[14], mRCOMB4 = A[15], dLDIFF = A[16], dRDIFF = A[17],
                      mLAPF1 = A[18], mRAPF1 = A[19], mLAPF2 = A[20], mRAPF2 = A[21];
        const int32_t vIIR = V[0], vWALL = V[5], vAPF1 = V[6], vAPF2 = V[7];

        wr(mLSAME, mul(clamp16(Lin + mul(rd(dLSAME), vWALL) - rd(mLSAME - 1)), vIIR) + rd(mLSAME - 1));
        wr(mRSAME, mul(clamp16(Rin + mul(rd(dRSAME), vWALL) - rd(mRSAME - 1)), vIIR) + rd(mRSAME - 1));
        wr(mLDIFF, mul(clamp16(Lin + mul(rd(dRDIFF), vWALL) - rd(mLDIFF - 1)), vIIR) + rd(mLDIFF - 1));
        wr(mRDIFF, mul(clamp16(Rin + mul(rd(dLDIFF), vWALL) - rd(mRDIFF - 1)), vIIR) + rd(mRDIFF - 1));

        int32_t Lout = clamp16(mul(V[1], rd(mLCOMB1)) + mul(V[2], rd(mLCOMB2)) + mul(V[3], rd(mLCOMB3)) + mul(V[4], rd(mLCOMB4)));
        int32_t Rout = clamp16(mul(V[1], rd(mRCOMB1)) + mul(V[2], rd(mRCOMB2)) + mul(V[3], rd(mRCOMB3)) + mul(V[4], rd(mRCOMB4)));

        Lout = clamp16(Lout - mul(vAPF1, rd(mLAPF1 - dAPF1)));
        wr(mLAPF1, Lout);
        Lout = clamp16(mul(Lout, vAPF1) + rd(mLAPF1 - dAPF1));
        Rout = clamp16(Rout - mul(vAPF1, rd(mRAPF1 - dAPF1)));
        wr(mRAPF1, Rout);
        Rout = clamp16(mul(Rout, vAPF1) + rd(mRAPF1 - dAPF1));

        Lout = clamp16(Lout - mul(vAPF2, rd(mLAPF2 - dAPF2)));
        wr(mLAPF2, Lout);
        Lout = clamp16(mul(Lout, vAPF2) + rd(mLAPF2 - dAPF2));
        Rout = clamp16(Rout - mul(vAPF2, rd(mRAPF2 - dAPF2)));
        wr(mRAPF2, Rout);
        Rout = clamp16(mul(Rout, vAPF2) + rd(mRAPF2 - dAPF2));

        core.reverbX = core.reverbX + 1u >= size ? 0u : core.reverbX + 1u;
        core.revOutL = Lout;
        core.revOutR = Rout;
        outL = core.revPrevL;
        outR = core.revPrevR;
    }

    void Spu2::mixSample(int16_t &outL, int16_t &outR)
    {
        int32_t externalL = 0, externalR = 0; // core 0 output feeds core 1
        for (int c = 0; c < 2; ++c)
        {
            Core &core = m_cores[c];
            int32_t dryL = 0, dryR = 0, wetL = 0, wetR = 0;
            for (int v = 0; v < 24; ++v)
            {
                Voice &voice = core.voices[v];
                if (voice.phase == Phase::Off)
                {
                    voice.volXL = voice.volXR = 0;
                    continue;
                }
                voice.counter += std::min<uint32_t>(voice.pitch, 0x3FFFu);
                while (voice.counter >= 0x1000u && voice.phase != Phase::Off)
                {
                    voice.counter -= 0x1000u;
                    voice.history[0] = voice.history[1];
                    voice.history[1] = voice.history[2];
                    voice.history[2] = voice.history[3];
                    voice.history[3] = static_cast<int16_t>(nextVoiceSample(core, c, voice, v));
                }
                // 4-point cubic interpolation between history[1] and history[2].
                const float t = static_cast<float>(voice.counter) / 4096.0f;
                const float p0 = voice.history[0], p1 = voice.history[1], p2 = voice.history[2], p3 = voice.history[3];
                const float interpolated = p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
                tickEnvelope(voice);
                const int32_t sample = (clamp16(static_cast<int32_t>(interpolated)) * voice.level) >> 15;
                const int32_t left = (sample * fixedVolume(voice.volL)) >> 15;
                const int32_t right = (sample * fixedVolume(voice.volR)) >> 15;
                voice.volXL = clamp16(left);
                voice.volXR = clamp16(right);
                if (core.vmixl & (1u << v))
                    dryL += left;
                if (core.vmixr & (1u << v))
                    dryR += right;
                if (core.vmixel & (1u << v))
                    wetL += left;
                if (core.vmixer & (1u << v))
                    wetR += right;
            }

            int32_t inputL = 0, inputR = 0;
            if (!core.input.empty())
            {
                const auto frame = core.input.front();
                core.input.pop_front();
                inputL = (frame[0] * static_cast<int16_t>(core.bvolL)) >> 15;
                inputR = (frame[1] * static_cast<int16_t>(core.bvolR)) >> 15;
            }
            // AVOL scales the external input (core 0's output into core 1),
            // BVOL the sound data input (auto-DMA stream); both are plain
            // signed 16-bit volumes, unlike the sweep-format MVOL/voice VOL.
            const int32_t extL = c == 1 ? (externalL * static_cast<int16_t>(core.avolL)) >> 15 : 0;
            const int32_t extR = c == 1 ? (externalR * static_cast<int16_t>(core.avolR)) >> 15 : 0;

            // MMIX gates (dry): 0x800/0x400 voices, 0x80/0x40 sound data input, 0x8/0x4 external input.
            int32_t mixL = 0, mixR = 0;
            if (core.mmix & 0x800u) mixL += dryL;
            if (core.mmix & 0x400u) mixR += dryR;
            if (core.mmix & 0x080u) mixL += inputL;
            if (core.mmix & 0x040u) mixR += inputR;
            if (core.mmix & 0x008u) mixL += extL;
            if (core.mmix & 0x004u) mixR += extR;

            // Wet gates feed the reverb: 0x200/0x100 voices, 0x20/0x10 input, 0x2/0x1 external.
            int32_t revL = 0, revR = 0;
            if (core.mmix & 0x200u) revL += wetL;
            if (core.mmix & 0x100u) revR += wetR;
            if (core.mmix & 0x020u) revL += inputL;
            if (core.mmix & 0x010u) revR += inputR;
            if (core.mmix & 0x002u) revL += extL;
            if (core.mmix & 0x001u) revR += extR;
            int32_t reverbL = 0, reverbR = 0;
            processReverb(core, clamp16(revL), clamp16(revR), reverbL, reverbR);
            mixL += (reverbL * static_cast<int16_t>(core.evolL)) >> 15;
            mixR += (reverbR * static_cast<int16_t>(core.evolR)) >> 15;

            externalL = clamp16((mixL * fixedVolume(core.mvolL)) >> 15);
            externalR = clamp16((mixR * fixedVolume(core.mvolR)) >> 15);
        }
        outL = static_cast<int16_t>(externalL);
        outR = static_cast<int16_t>(externalR);
    }

    void Spu2::pushOutput(int16_t left, int16_t right) noexcept
    {
        // PS2X_SPU2_WAV=<file>: raw 48 kHz stereo s16le capture of the mix.
        static FILE *wav = []() -> FILE *
        {
            const char *path = std::getenv("PS2X_SPU2_WAV");
            return path ? std::fopen(path, "wb") : nullptr;
        }();
        if (wav)
        {
            const int16_t frame[2] = {left, right};
            std::fwrite(frame, sizeof(frame), 1, wav);
        }
        const size_t write = m_ringWrite.load(std::memory_order_relaxed);
        const size_t read = m_ringRead.load(std::memory_order_acquire);
        if (write - read >= kRingFrames)
            return; // host is not draining; drop
        const size_t slot = (write % kRingFrames) * 2u;
        m_ring[slot] = left;
        m_ring[slot + 1u] = right;
        m_ringWrite.store(write + 1u, std::memory_order_release);
    }

    size_t Spu2::readOutput(int16_t *destination, size_t frames) noexcept
    {
        const size_t read = m_ringRead.load(std::memory_order_relaxed);
        const size_t write = m_ringWrite.load(std::memory_order_acquire);
        // Keep ~40 ms queued before (re)starting playback so producer jitter
        // does not turn into crackles.
        constexpr size_t kPrebufferFrames = 2048u;
        if (!m_outputStarted)
        {
            if (write - read < kPrebufferFrames)
                return 0;
            m_outputStarted = true;
        }
        if (write - read < frames)
            m_outputStarted = false;
        const size_t available = std::min(frames, write - read);
        for (size_t i = 0; i < available; ++i)
        {
            const size_t slot = ((read + i) % kRingFrames) * 2u;
            destination[i * 2u] = m_ring[slot];
            destination[i * 2u + 1u] = m_ring[slot + 1u];
        }
        m_ringRead.store(read + available, std::memory_order_release);
        return available;
    }

    void Spu2::advanceTo(uint64_t iopCycle)
    {
        if (!m_cycleValid)
        {
            m_cycle = iopCycle;
            m_cycleValid = true;
            return;
        }
        // Never generate more than ~1/4 s at once (e.g. after a long stall).
        if (iopCycle > m_cycle + CyclesPerSample * 12000u)
            m_cycle = iopCycle - CyclesPerSample * 12000u;
        static const bool trace = std::getenv("PS2X_SPU2_TRACE") != nullptr;
        {
            // PS2X_SPU2_DUMPREGS=1: the core set-up now and then (what libsd leaves).
            static const bool dump = std::getenv("PS2X_SPU2_DUMPREGS") != nullptr;
            static uint64_t lastDump = 0;
            if (dump && iopCycle > lastDump + 36864000ull * 10u)
            {
                lastDump = iopCycle;
                for (int c = 0; c < 2; ++c)
                {
                    const Core &k = m_cores[c];
                    std::fprintf(stderr, "[spu2regs] core %d attr %04x mmix %04x vmixl %06x vmixr %06x vmixel %06x vmixer %06x mvol %04x %04x evol %04x %04x avol %04x %04x bvol %04x %04x esa %05x eea %05x admas %x\n",
                                 c, k.attr, k.mmix, k.vmixl, k.vmixr, k.vmixel, k.vmixer, k.mvolL, k.mvolR, k.evolL, k.evolR, k.avolL, k.avolR, k.bvolL, k.bvolR, k.esa, k.eea, k.admas);
                    std::fprintf(stderr, "[spu2regs] core %d rev:", c);
                    for (uint32_t r = 0x2E4u; r < 0x33Cu; r += 2u)
                        std::fprintf(stderr, " %04x", m_regs[(c * 0x400u + r) / 2u]);
                    std::fprintf(stderr, " | 774:");
                    for (uint32_t r = 0x774u; r < 0x7B0u; r += 2u)
                        std::fprintf(stderr, " %04x", m_regs[(c * 0x28u + r) / 2u]);
                    std::fprintf(stderr, "\n");
                }
            }
        }
        while (m_cycle + CyclesPerSample <= iopCycle)
        {
            m_cycle += CyclesPerSample;
            static uint64_t samples = 0;
            if (trace && (++samples % (48000u * 5u)) == 0u)
            {
                for (int c = 0; c < 2; ++c)
                {
                    const Core &core = m_cores[c];
                    int active = 0;
                    int32_t maxLevel = 0;
                    for (const Voice &voice : core.voices)
                        if (voice.phase != Phase::Off)
                        {
                            ++active;
                            maxLevel = std::max(maxLevel, voice.level);
                        }
                    std::fprintf(stderr, "[spu2] t=%llus core=%d active=%d maxEnv=%d mmix=%03x attr=%04x mvol=%04x/%04x avol=%04x/%04x bvol=%04x/%04x vmix=%06x/%06x input=%zu\n",
                                 static_cast<unsigned long long>(samples / 48000u), c, active, maxLevel, core.mmix, core.attr, core.mvolL, core.mvolR,
                                 core.avolL, core.avolR, core.bvolL, core.bvolR, core.vmixl, core.vmixr, core.input.size());
                    std::fprintf(stderr, "    reverb esa=0x%05x eea=0x%05x evol=%04x/%04x vmixe=%06x/%06x vol=", core.esa, core.eea, core.evolL, core.evolR, core.vmixel, core.vmixer);
                    for (int16_t value : core.revVol)
                        std::fprintf(stderr, "%d ", value);
                    std::fprintf(stderr, " addr=");
                    for (uint32_t value : core.revAddr)
                        std::fprintf(stderr, "%x ", value);
                    std::fprintf(stderr, "\n");
                    for (int v = 0; v < 24; ++v)
                    {
                        const Voice &voice = core.voices[v];
                        if (voice.phase == Phase::Off)
                            continue;
                        int peak = 0;
                        for (int16_t x : voice.block)
                            peak = std::max(peak, std::abs(static_cast<int>(x)));
                        uint32_t nonzero = 0;
                        for (uint32_t w = 0; w < 0x800u; ++w)
                            nonzero += m_ram[(voice.ssa + w) & (RamWords - 1u)] != 0u;
                        std::fprintf(stderr, "    v%d phase=%d level=%d nax=0x%05x ssa=0x%05x lsax=0x%05x flags=%x peak=%d volx=%d/%d ramNonZero=%u\n", v,
                                     static_cast<int>(voice.phase), voice.level, voice.nax, voice.ssa, voice.lsax, voice.blockFlags, peak,
                                     voice.volXL, voice.volXR, nonzero);
                    }
                }
            }
            int16_t left, right;
            mixSample(left, right);
            pushOutput(left, right);
        }
    }
}
