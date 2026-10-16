#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace ps2x::iop::detail
{
    // SPU2: two cores of 24 ADPCM voices sharing 2 MiB of sound RAM, mixed at
    // 48 kHz. Implements the register file seen by LIBSD, IOP DMA (manual and
    // auto-DMA streaming), ADSR envelopes, voice/core volumes and the IRQA
    // interrupt. Reverb and noise are not emulated yet.
    class Spu2
    {
    public:
        static constexpr uint32_t Base = 0x1F900000u;
        static constexpr uint32_t RegisterBytes = 0x800u;
        static constexpr uint32_t CyclesPerSample = 768u; // 36.864 MHz / 48 kHz
        static constexpr uint32_t RamWords = 1u << 20u;   // 2 MiB

        Spu2();

        void reset();

        [[nodiscard]] static bool isRegister(uint32_t phys) noexcept
        {
            return phys >= Base && phys < Base + RegisterBytes;
        }
        [[nodiscard]] uint16_t read16(uint32_t phys);
        void write16(uint32_t phys, uint16_t value);

        // IOP DMA channel 4 (core 0) / 7 (core 1). Returns the cycles until the
        // transfer should signal completion.
        uint64_t dmaWrite(int core, const uint8_t *source, uint32_t bytes);
        uint64_t dmaRead(int core, uint8_t *destination, uint32_t bytes);

        // Produces output samples up to the given IOP cycle.
        void advanceTo(uint64_t iopCycle);
        // True once per raised SPU2 interrupt (IOP interrupt line 9).
        [[nodiscard]] bool takeIrq() noexcept;

        // Interleaved stereo frames for the host audio device (any thread).
        size_t readOutput(int16_t *destination, size_t frames) noexcept;

        // Direct access for a native sound driver (no IOP DMA): sound RAM by
        // byte address, the core's sound data input (blocks of 256 left then
        // 256 right samples) and a callback run when it is down to one block.
        void writeRam(uint32_t byteAddress, const uint8_t *source, uint32_t bytes);
        void readRam(uint32_t byteAddress, uint8_t *destination, uint32_t bytes) const;
        void pushInputBlock(int core, const uint8_t *block);
        void clearInput(int core) { m_cores[core].input.clear(); }
        void setInputRefill(std::function<void(int core)> refill) { m_inputRefill = std::move(refill); }

    private:
        enum class Phase : uint8_t
        {
            Off,
            Attack,
            Decay,
            Sustain,
            Release
        };

        struct Voice
        {
            uint16_t volL = 0, volR = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
            int16_t volXL = 0, volXR = 0;
            uint32_t ssa = 0, lsax = 0, nax = 0;
            Phase phase = Phase::Off;
            int32_t level = 0;
            uint32_t envCounter = 0;
            uint32_t counter = 0;
            int32_t prev1 = 0, prev2 = 0;
            std::array<int16_t, 28> block{};
            uint32_t blockIndex = 28;
            uint8_t blockFlags = 0;
            uint32_t blockAddress = 0;
            std::array<int16_t, 4> history{};
        };

        struct Core
        {
            std::array<Voice, 24> voices{};
            uint16_t attr = 0, mmix = 0, admas = 0, statx = 0;
            uint32_t irqa = 0, tsa = 0;
            uint32_t vmixl = 0, vmixr = 0, endx = 0;
            uint16_t mvolL = 0, mvolR = 0, avolL = 0, avolR = 0, bvolL = 0, bvolR = 0;
            // Reverb: 22 address offsets (dAPF1..mRAPF2 in PSX order), 10
            // coefficients (vIIR..vRIN), work area ESA..EEA, output EVOL.
            uint32_t vmixel = 0, vmixer = 0;
            uint16_t evolL = 0, evolR = 0;
            std::array<uint32_t, 22> revAddr{};
            std::array<int16_t, 10> revVol{};
            uint32_t esa = 0, eea = 0, reverbX = 0;
            bool reverbOdd = false;
            int32_t revInL = 0, revInR = 0;
            int32_t revOutL = 0, revOutR = 0, revPrevL = 0, revPrevR = 0;
            std::deque<std::array<int16_t, 2>> input; // auto-DMA stream
        };

        [[nodiscard]] static int32_t fixedVolume(uint16_t value) noexcept;
        void keyOn(int core, uint32_t mask);
        void keyOff(int core, uint32_t mask);
        void checkIrq(uint32_t start, uint32_t words);
        void decodeBlock(Core &core, int coreIndex, Voice &voice);
        int32_t nextVoiceSample(Core &core, int coreIndex, Voice &voice, int voiceIndex);
        void tickEnvelope(Voice &voice);
        void mixSample(int16_t &outL, int16_t &outR);
        void processReverb(Core &core, int32_t inL, int32_t inR, int32_t &outL, int32_t &outR);
        void pushOutput(int16_t left, int16_t right) noexcept;

        std::vector<uint16_t> m_ram;
        std::array<uint16_t, RegisterBytes / 2u> m_regs{};
        std::array<Core, 2> m_cores{};
        uint16_t m_irqInfo = 0;
        bool m_irqPending = false;
        uint64_t m_cycle = 0;
        bool m_cycleValid = false;

        static constexpr size_t kRingFrames = 1u << 15u;
        std::vector<int16_t> m_ring;
        std::atomic<size_t> m_ringWrite{0};
        std::atomic<size_t> m_ringRead{0};
        bool m_outputStarted = false; // consumer side only
        std::function<void(int core)> m_inputRefill;
    };
}
