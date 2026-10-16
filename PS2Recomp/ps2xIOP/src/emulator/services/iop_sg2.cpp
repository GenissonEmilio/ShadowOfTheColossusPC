#include "iop_sg2.h"

#include "emulator/core/iop_memory.h"
#include "emulator/core/iop_spu2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        // SPU2 register offsets (core 1 is +0x400; volumes at 0x760 + core * 0x28).
        constexpr uint32_t kVolumes = 0x760u, kVolumeStride = 0x28u, kVoiceAddresses = 0x1C0u;

        // Note pitches, 192 steps per octave, 0x1000 at index 0xd0: the
        // driver's table (0x46a0) is floor(0x1000 * 2^((i - 0xd0) / 192))
        // with these few entries off by one (and two typos).
        constexpr int kPitchEntries = 608;
        constexpr std::pair<int, int> kPitchFixes[] = {
            {1, -1}, {5, 1}, {13, 1}, {62, 1}, {64, -75}, {121, 1}, {131, 1}, {141, 1}, {144, 1},
            {156, 1}, {159, 1}, {170, 1}, {171, 1}, {172, -256}, {173, 1}, {174, 1}, {175, 1},
            {179, 1}, {187, 1}, {198, 1}, {201, 1}, {205, 1}, {444, 1}, {448, 1}, {490, 1},
            {491, 1}, {528, 1}, {550, 1}, {552, 1}, {586, 1}, {587, 1}, {591, 1}};

        const std::array<uint16_t, kPitchEntries> &pitchTable()
        {
            static const std::array<uint16_t, kPitchEntries> table = []
            {
                std::array<uint16_t, kPitchEntries> t{};
                for (int i = 0; i < kPitchEntries; ++i)
                    t[i] = static_cast<uint16_t>(std::floor(4096.0 * std::pow(2.0, (i - 208) / 192.0)));
                for (const auto &[index, delta] : kPitchFixes)
                    t[index] = static_cast<uint16_t>(t[index] + delta);
                return t;
            }();
            return table;
        }

        // Reverb "studio C" (libsd mode 4, the one SotC uses): the 22 work
        // area offsets (words, register order from 0x2E4) and the 10
        // coefficients (0x774..), as libsd leaves them. Work area 0x37f0 words.
        constexpr uint32_t kStudioCWords = 0x37F0u;
        constexpr uint16_t kStudioCAddresses[22] = {
            0x038c, 0x02a4, 0x37ec, 0x2d60, 0x3424, 0x28f0, 0x2f64, 0x25cc, 0x2d64, 0x2368, 0x2364,
            0x17a4, 0x1fb0, 0x12c0, 0x1bbc, 0x0f48, 0x17a8, 0x0c74, 0x0c70, 0x08e0, 0x0550, 0x02a8};
        constexpr uint16_t kStudioCCoefficients[10] = {
            0x6f60, 0x4fa8, 0xbce0, 0x4510, 0xbef0, 0xa680, 0x5680, 0x52c0, 0x8000, 0x8000};

        uint32_t voiceParam(int32_t voice)
        {
            return static_cast<uint32_t>((voice / 24) | ((voice % 24) << 1)) & 0xFFFFu;
        }
    }

    Sg2Driver::Sg2Driver(IopHost &host, IopMemory &memory)
        : m_host(host), m_memory(memory), m_spu(memory.spu2())
    {
    }

    bool Sg2Driver::enabled()
    {
        static const bool on = []
        {
            // PS2X_NATIVE_SG2=1/0; by default on with SOTC_NATIVE=1.
            if (const char *value = std::getenv("PS2X_NATIVE_SG2"))
                return std::atoi(value) != 0;
            const char *native = std::getenv("SOTC_NATIVE");
            return native && std::atoi(native) == 1;
        }();
        return on;
    }

    bool Sg2Driver::isDriverImage(std::span<const uint8_t> image)
    {
        // Only inside the ELF (its section headers end it): the buffer read
        // runs to the end of IOP RAM.
        constexpr std::string_view name = "sg2iop_driver";
        if (image.size() < 0x34u)
            return false;
        uint32_t sectionOffset;
        uint16_t sectionSize, sectionCount;
        std::memcpy(&sectionOffset, image.data() + 0x20, 4);
        std::memcpy(&sectionSize, image.data() + 0x2E, 2);
        std::memcpy(&sectionCount, image.data() + 0x30, 2);
        const size_t end = std::min<size_t>(image.size(), size_t{sectionOffset} + size_t{sectionSize} * sectionCount);
        return std::search(image.begin(), image.begin() + end, name.begin(), name.end()) != image.begin() + end;
    }

    void Sg2Driver::reset()
    {
        m_reply.fill(0);
        m_streams.fill({});
        m_streamOps.fill({});
        m_opWrite = m_opRead = 0;
        m_pendingAdvance = 0;
        m_effects.fill({});
        m_transfer = 0;
        m_pcmOn = false;
        m_spu.setInputRefill(nullptr);
    }

    RpcResult Sg2Driver::handleRpc(const RpcRequest &request)
    {
        RpcResult result;
        if (request.function == 0u)
        {
            std::vector<uint32_t> commands(request.send.size / 4u);
            if (!commands.empty() &&
                m_host.readGuest(request.send.address, commands.data(), commands.size() * 4u))
            {
                for (size_t i = 0; i + 4u <= commands.size(); i += 4u)
                    runCommand(&commands[i]);
            }
            fillReply();
            if (request.receive.address != 0u && request.receive.size != 0u)
                (void)m_host.writeGuest(request.receive.address, m_reply.data(),
                                        std::min<size_t>(request.receive.size, m_reply.size()));
        }
        result.handled = true;
        result.resultAddress = request.receive.address;
        result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;
        result.signalNowaitCompletion = true;
        result.signalCompletion = true;
        return result;
    }

    // The driver's command table (0x4ba0); comments give the handler address.
    void Sg2Driver::runCommand(const uint32_t *command)
    {
        const uint32_t cmd = command[0], a = command[1], b = command[2], c = command[3];
        const int32_t voice = static_cast<int32_t>(a);
        static const bool trace = std::getenv("PS2X_SG2_TRACE") != nullptr;
        if (trace && cmd != 0x1a && cmd != 1)
            std::fprintf(stderr, "[sg2] cmd %02x %08x %08x %08x\n", cmd, a, b, c);
        switch (cmd)
        {
        case 0x01: // 0x6fc: voice volume (negative: + 0x7fff)
        {
            const int32_t left = static_cast<int32_t>(b) < 0 ? static_cast<int32_t>(b) + 0x7FFF : static_cast<int32_t>(b);
            const int32_t right = static_cast<int32_t>(c) < 0 ? static_cast<int32_t>(c) + 0x7FFF : static_cast<int32_t>(c);
            setParam(voiceParam(voice), static_cast<uint16_t>(left));
            setParam(voiceParam(voice) | 0x100u, static_cast<uint16_t>(right));
            break;
        }
        case 0x02: // 0x8e8: ADSR
            setParam(voiceParam(voice) | 0x300u, static_cast<uint16_t>(b));
            setParam(voiceParam(voice) | 0x400u, static_cast<uint16_t>(c));
            break;
        case 0x03: // 0xa44: start address
            setAddr(voiceParam(voice) | 0x2040u, b);
            break;
        case 0x04: // 0xb04: pitch from note, centre, fine tune, bend and bend range
        {
            const int32_t note = static_cast<int32_t>(b >> 24u);
            const int32_t centre = static_cast<int32_t>((b >> 16u) & 0xFFu);
            const int32_t fine = static_cast<int8_t>(b >> 8u);
            const int32_t bend = static_cast<int32_t>(static_cast<uint32_t>((b & 0xFFu) - 0x40u) * (c >> 24u)) >> 2;
            const auto &table = pitchTable();
            uint32_t step;
            if (centre < note)
            {
                const int32_t d = note - centre;
                const int index = std::clamp((12 - d % 12) * 16 + bend + 0xD0 + fine, 0, kPitchEntries - 1);
                step = static_cast<uint32_t>(table[index]) >> ((d / 12 + 1) & 31);
            }
            else
            {
                const int32_t d = centre - note;
                const int index = std::clamp((d % 12) * 16 + bend + 0xD0 + fine, 0, kPitchEntries - 1);
                step = static_cast<uint32_t>(table[index]) << ((d / 12) & 31);
            }
            setParam(voiceParam(voice) | 0x200u,
                     static_cast<uint16_t>(((c & 0xFFFFu) * ((step * 441u) / 480u)) >> 12u));
            break;
        }
        case 0x05: // 0xf04: core attributes
            if (a == 0u)
                setCoreAttr((static_cast<int32_t>(b) / 24) | 8u, c);
            else if (a == 1u)
                setCoreAttr(10u, b);
            break;
        case 0x06: // 0xfdc: master volume
            setParam((a & 0xFFFFu) | 0x980u, static_cast<uint16_t>(b));
            setParam((a & 0xFFFFu) | 0xA80u, static_cast<uint16_t>(c));
            break;
        case 0x07: // 0x1068: key on
            setSwitch(0x1500u, b);
            setSwitch(0x1501u, c);
            break;
        case 0x08: // 0x10bc: key off
            setSwitch(0x1600u, b);
            setSwitch(0x1601u, c);
            break;
        case 0x09: // 0x1110: reverb send
            setSwitch(0x1900u, b);
            setSwitch(0x1B00u, b);
            setSwitch(0x1901u, c);
            setSwitch(0x1B01u, c);
            break;
        case 0x0A: // 0x1194: noise
            setSwitch(0x1400u, b);
            setSwitch(0x1401u, c);
            break;
        case 0x0B: // 0x11e8: effect end address
            setAddr((a & 0xFFFFu) | 0x1D00u, b);
            break;
        case 0x0C: // 0x1240: effect mode, applied now
            if (a < 2u)
            {
                m_effects[a].mode = b;
                setEffect(static_cast<int>(a));
            }
            break;
        case 0x0D: // 0x12bc: effect mode (a new one clears the work area), applied by the driver thread
            if (a < 2u)
            {
                if (m_effects[a].mode != b)
                    m_effects[a].mode = b | 0x100u;
                setEffect(static_cast<int>(a));
            }
            break;
        case 0x0E: // 0x1384: effect depth
            if (a < 2u)
            {
                m_effects[a].depthL = static_cast<int16_t>(b);
                m_effects[a].depthR = static_cast<int16_t>(c);
            }
            break;
        case 0x0F: // 0x1420
            if (a < 2u)
                m_effects[a].delay = static_cast<int32_t>(b);
            break;
        case 0x10: // 0x1488
            if (a < 2u)
                m_effects[a].feedback = static_cast<int32_t>(b);
            break;
        case 0x11: // 0x14f0: init
            if (a == 0u)
            {
                sdInit();
                for (uint32_t core = 0; core < 2u; ++core)
                    for (uint32_t param : {0x980u, 0xA80u, 0xB80u, 0xC80u, 0xF80u, 0x1080u})
                        setParam(core | param, 0);
            }
            setCoreAttr(2u, 1u);
            setCoreAttr(3u, 1u);
            break;
        case 0x13: // 0x1690: sound data to sound RAM (driver thread op 1, sceSdVoiceTrans write)
        {
            iopToSpu(a, b, c);
            m_transfer = 2;
            break;
        }
        case 0x14: // 0x1720: op 2, the same as a read back
        {
            std::vector<uint8_t> data(c);
            m_spu.readRam(b, data.data(), c);
            (void)m_memory.writeRam(a, data.data(), data.size());
            m_transfer = 2;
            break;
        }
        case 0x15: // 0x17b0: effect return volume
            setParam((a & 0xFFFFu) | 0xB80u, static_cast<uint16_t>(b));
            setParam((a & 0xFFFFu) | 0xC80u, static_cast<uint16_t>(c));
            break;
        case 0x16: // 0x2bb0: streams reset
            m_streams.fill({});
            m_streamOps.fill({});
            m_opWrite = m_opRead = 0;
            m_pendingAdvance = 0;
            break;
        case 0x18: // 0x2c20: streamed voice set-up
        {
            const uint32_t index = a >> 24u;
            Stream &s = m_streams[(index / 24u) * 24u + index % 24u];
            s.channels = static_cast<int32_t>((a >> 16u) & 0xFFu);
            s.source = static_cast<int32_t>(c & 0xFFFFFFu);
            s.length = static_cast<int32_t>(((b & 0xFFFFu) << 8u) | (c >> 24u));
            s.spuAddress = static_cast<int32_t>(((a & 0xFFu) << 16u) | (b >> 16u));
            s.size = static_cast<int32_t>(a & 0xFF00u);
            const uint32_t param = (index / 24u) | ((index % 24u) << 1u);
            setAddr(param | 0x2040u, static_cast<uint32_t>(s.spuAddress));
            setParam(param | 0x300u, 0x8080u);
            setParam(param | 0x400u, 0x808Au);
            break;
        }
        case 0x19: // 0x2fac: streamed voice release (the driver drops queued ops of voice % 24)
            if (voice >= 0 && voice < 48)
            {
                dropStreamOps(static_cast<uint32_t>(voice % 24));
                m_streams[voice] = {};
            }
            break;
        case 0x1A: // 0x30a8: volume for every voice in the masks
            for (uint32_t core = 0; core < 2u; ++core)
                for (uint32_t v = 0; v < 24u; ++v)
                    if (((core ? b : a) >> v) & 1u)
                    {
                        setParam(core | (v << 1u), static_cast<uint16_t>(c >> 16u));
                        setParam(core | (v << 1u) | 0x100u, static_cast<uint16_t>(c));
                    }
            break;
        case 0x1B: // 0x3234: pitch from a rate in Hz for every voice in the masks
        {
            const uint16_t pitch = static_cast<uint16_t>((static_cast<int32_t>(c) << 12) / 48000);
            for (uint32_t core = 0; core < 2u; ++core)
                for (uint32_t v = 0; v < 24u; ++v)
                    if (((core ? b : a) >> v) & 1u)
                        setParam(core | (v << 1u) | 0x200u, pitch);
            break;
        }
        case 0x1C: // 0x3394: streamed voices start: fill the first half, then key on
            for (uint32_t core = 0; core < 2u; ++core)
                for (uint32_t v = 0; v < 24u; ++v)
                    if (((core ? b : a) >> v) & 1u)
                    {
                        const Stream &s = m_streams[core * 24u + v];
                        queueStream(1, core * 24u + v, 2, static_cast<uint32_t>(s.channels),
                                    static_cast<uint32_t>(s.source), static_cast<uint32_t>(s.spuAddress),
                                    static_cast<uint32_t>(s.size / 2));
                    }
            queueStream(2, a, b);
            break;
        case 0x1D: // 0x3634: streamed voices stop
            if (a != 0u || b != 0u)
                queueStream(3, a, b);
            break;
        case 0x1E: // 0x3914: PCM input silent
        case 0x1F: // 0x39c4: PCM input stop
        case 0x21: // 0x3b90
            setParam(0xF80u, 0);
            setParam(0x1080u, 0);
            pcmStop();
            break;
        case 0x20: // 0x3a5c: PCM input start
            pcmStart(a, b);
            break;
        case 0x22: // 0x3c28: PCM input volume
            m_pcmVolL = static_cast<uint16_t>(a);
            m_pcmVolR = static_cast<uint16_t>(b);
            setParam(0xF80u, m_pcmVolL);
            setParam(0x1080u, m_pcmVolR);
            break;
        case 0x23: // 0x3cd4: dry/wet routing
            if (a == 0u)
            {
                setParam(0x800u, 0xFFC0u);
                setParam(0x801u, 0xFFCCu);
            }
            else if (a == 1u)
            {
                setParam(0x800u, 0xFFF0u);
                setParam(0x801u, 0xFFFCu);
            }
            break;
        default: // 0, 0x12 (transfer interrupt handler off), 0x17
            break;
        }
    }

    // 0x400c: status for the EE, then the stream bookkeeping.
    void Sg2Driver::fillReply()
    {
        auto put = [this](uint32_t offset, uint32_t value)
        { std::memcpy(m_reply.data() + offset, &value, 4); };
        put(0x180u, m_pcmPosition);
        if (m_transfer == 1)
            put(0x184u, 1u);
        else
        {
            put(0x184u, 0u);
            if (m_transfer == 2)
                m_transfer = 0;
        }
        for (uint32_t core = 0; core < 2u; ++core)
            for (uint32_t v = 0; v < 24u; ++v)
                put((core * 24u + v) * 4u, getParam(core | (v << 1u) | 0x500u));
        for (uint32_t i = 0; i < 48u; ++i) // 0x2518
            put(0xC0u + i * 4u, static_cast<uint32_t>(m_streams[i].position));
        streamTick();
        runStreamQueue();
    }

    // 0x2204: a streamed voice playing one half gets the other one refilled.
    void Sg2Driver::streamTick()
    {
        for (uint32_t core = 0; core < 2u; ++core)
            for (uint32_t v = 0; v < 24u; ++v)
            {
                Stream &s = m_streams[core * 24u + v];
                if (s.state == 0)
                    continue;
                const int32_t nax = static_cast<int32_t>(getAddr(core | (v << 1u) | 0x2240u));
                const int32_t half = s.size / 2;
                int32_t destination = s.spuAddress;
                if (s.spuAddress < nax && nax < half + s.spuAddress)
                {
                    s.half = 1;
                    destination += half;
                }
                if (half + s.spuAddress < nax && nax < s.spuAddress + s.size)
                    s.half = 2;
                if (s.half != s.queuedHalf)
                {
                    queueStream(1, core * 24u + v, static_cast<uint32_t>(s.half), static_cast<uint32_t>(s.channels),
                                static_cast<uint32_t>(s.source + s.position), static_cast<uint32_t>(destination),
                                static_cast<uint32_t>(half));
                    s.queuedHalf = s.half;
                }
            }
    }

    // 0x2620: the position moves on after a refill; one queued op per status.
    void Sg2Driver::runStreamQueue()
    {
        for (;;)
        {
            if (m_pendingAdvance != 0u)
            {
                const uint32_t index = m_pendingAdvance & 0xFFu;
                if (index < 48u)
                {
                    Stream &s = m_streams[index];
                    s.position += (s.size / 2) * s.channels;
                    if (s.position != 0 && s.length != 0)
                        s.position %= s.length;
                }
                m_pendingAdvance = 0;
            }
            if (m_opWrite == m_opRead)
                return;
            StreamOp &op = m_streamOps[m_opRead];
            const bool skip = op.op < 1 || op.op > 3;
            if (op.op == 1)
                refill(op);
            else if (op.op == 2)
                streamKey(op, true);
            else if (op.op == 3)
                streamKey(op, false);
            op.op = 0;
            m_opRead = (m_opRead + 1u) % m_streamOps.size();
            if (!skip)
                return;
        }
    }

    void Sg2Driver::queueStream(int32_t op, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
    {
        StreamOp &entry = m_streamOps[m_opWrite];
        entry.op = op;
        entry.arg[0] = a0;
        entry.arg[1] = a1;
        entry.arg[2] = a2;
        entry.arg[3] = a3;
        entry.arg[4] = a4;
        entry.arg[5] = a5;
        m_opWrite = (m_opWrite + 1u) % m_streamOps.size();
    }

    // 0x1988
    void Sg2Driver::dropStreamOps(uint32_t voice)
    {
        if ((m_pendingAdvance & 0xFFu) == voice)
            m_pendingAdvance = 0;
        for (StreamOp &op : m_streamOps)
        {
            if (op.op == 1 && (op.arg[0] & 0xFFu) == voice)
                op.op = 0;
            else if (op.op == 2)
                op.arg[0] &= ~(1u << (voice & 31u));
        }
    }

    // 0x1adc: the voice's channel out of each 0x800 bytes of interleaved
    // source, with the ADPCM loop flags that make the two halves a ring.
    void Sg2Driver::refill(const StreamOp &op)
    {
        const uint32_t voice = op.arg[0], half = op.arg[1], channels = op.arg[2];
        const uint32_t bytes = op.arg[5];
        if (channels == 0u || bytes < 0x10u || (half != 1u && half != 2u))
            return;
        const uint32_t chunk = 0x800u / channels;
        if (chunk == 0u)
            return;
        std::vector<uint8_t> staging(bytes, 0);
        uint32_t source = op.arg[3];
        for (uint32_t i = 0; i < bytes / chunk; ++i, source += 0x800u)
            (void)m_memory.readRam(source, staging.data() + i * chunk, chunk);
        staging[1] = half == 1u ? 2u : 6u;
        staging[bytes - 0x10u + 1u] = half == 1u ? 3u : 2u;
        m_spu.writeRam(op.arg[4], staging.data(), bytes);
        m_pendingAdvance = voice | 0x10000u;
    }

    // 0x1d38 / 0x1f64
    void Sg2Driver::streamKey(const StreamOp &op, bool on)
    {
        const uint32_t masks[2] = {op.arg[0], op.arg[1]};
        for (uint32_t core = 0; core < 2u; ++core)
            if (masks[core] != 0u)
                setSwitch((on ? 0x1500u : 0x1600u) | core, masks[core]);
        for (uint32_t core = 0; core < 2u; ++core)
            for (uint32_t v = 0; v < 24u; ++v)
                if ((masks[core] >> v) & 1u)
                {
                    Stream &s = m_streams[core * 24u + v];
                    if (!on)
                    {
                        dropStreamOps(core * 24u + v);
                        s.position = 0;
                    }
                    s.state = on ? 2 : 0;
                    s.half = s.queuedHalf = 0;
                }
    }

    // 0x120 + sceSdSetEffectAttr: reverb preset, work area below EEA, depth
    // into EVOL; a mode with 0x100 clears the work area with the master
    // volume down.
    void Sg2Driver::setEffect(int core)
    {
        const Effect &effect = m_effects[core];
        const uint32_t base = static_cast<uint32_t>(core) * 0x400u;
        const uint32_t volumes = kVolumes + static_cast<uint32_t>(core) * kVolumeStride;
        const bool clear = (effect.mode & 0x100u) != 0u;
        const uint16_t mvolL = read(volumes), mvolR = read(volumes + 2u);
        if (clear)
            for (uint32_t offset = 0; offset < 8u; offset += 2u)
                write(volumes + offset, 0);

        const uint32_t type = effect.mode & 0xFFu;
        static bool warned = false;
        if (type != 4u && type != 0u && !warned)
        {
            warned = true;
            std::fprintf(stderr, "[sg2] reverb mode %u not known, using studio C\n", type);
        }
        const uint32_t eea = ((static_cast<uint32_t>(read(base + 0x33Cu)) & 0xFu) << 16u) | 0xFFFFu;
        const uint32_t esa = eea - (kStudioCWords - 1u);
        write(base + 0x2E0u, static_cast<uint16_t>(esa >> 16u));
        write(base + 0x2E2u, static_cast<uint16_t>(esa));
        for (uint32_t i = 0; i < 22u; ++i)
        {
            write(base + 0x2E4u + i * 4u, 0);
            write(base + 0x2E6u + i * 4u, kStudioCAddresses[i]);
        }
        for (uint32_t i = 0; i < 10u; ++i)
            write(volumes + 0x14u + i * 2u, type == 0u ? 0 : kStudioCCoefficients[i]);
        write(volumes + 4u, static_cast<uint16_t>(effect.depthL));
        write(volumes + 6u, static_cast<uint16_t>(effect.depthR));
        if (clear)
        {
            const std::vector<uint8_t> zeros((eea - esa + 1u) * 2u, 0);
            m_spu.writeRam(esa * 2u, zeros.data(), static_cast<uint32_t>(zeros.size()));
            write(volumes, mvolL);
            write(volumes + 2u, mvolR);
        }
    }

    // 0x3a5c and the transfer handler 0x36b0: the buffer plays as blocks
    // of 0x400 bytes (256 left, 256 right samples) into core 0's sound data
    // input; the position the EE sees is the block last queued.
    void Sg2Driver::pcmStart(uint32_t buffer, uint32_t size)
    {
        m_pcmBuffer = buffer;
        m_pcmSize = size;
        m_pcmPosition = 0;
        m_pcmFirst = true;
        setParam(0xF80u, m_pcmVolL);
        setParam(0x1080u, m_pcmVolR);
        m_spu.clearInput(0);
        uint8_t block[0x400];
        for (uint32_t offset = 0; offset < 0x800u; offset += 0x400u)
        {
            (void)m_memory.readRam(buffer + offset, block, sizeof(block));
            m_spu.pushInputBlock(0, block);
        }
        m_pcmOn = true;
        m_spu.setInputRefill([this](int core)
                             {
                                 if (core == 0)
                                     pcmRefill();
                             });
    }

    void Sg2Driver::pcmStop()
    {
        m_pcmOn = false;
        m_spu.setInputRefill(nullptr);
        m_spu.clearInput(0);
    }

    void Sg2Driver::pcmRefill()
    {
        if (!m_pcmOn)
            return;
        m_pcmPosition += m_pcmFirst ? 0x800u : 0x400u;
        m_pcmFirst = false;
        if (m_pcmSize <= m_pcmPosition)
            m_pcmPosition = 0;
        uint8_t block[0x400];
        (void)m_memory.readRam(m_pcmBuffer + m_pcmPosition, block, sizeof(block));
        m_spu.pushInputBlock(0, block);
    }

    void Sg2Driver::setParam(uint32_t param, uint16_t value)
    {
        const uint32_t core = param & 1u, reg = (param >> 8u) & 0xFFu;
        if ((param & 0x80u) || reg == 8u)
        {
            if (reg == 8u) // MMIX
                write(core * 0x400u + 0x198u, value);
            else if (reg >= 9u && reg <= 0x10u) // MVOL, EVOL, AVOL, BVOL
                write(kVolumes + core * kVolumeStride + (reg - 9u) * 2u, value);
            return;
        }
        const uint32_t v = (param >> 1u) & 0x1Fu;
        if (v < 24u && reg < 8u)
            write(core * 0x400u + v * 0x10u + reg * 2u, value);
    }

    uint16_t Sg2Driver::getParam(uint32_t param)
    {
        const uint32_t core = param & 1u, reg = (param >> 8u) & 0xFu, v = (param >> 1u) & 0x1Fu;
        return v < 24u ? read(core * 0x400u + v * 0x10u + reg * 2u) : 0;
    }

    void Sg2Driver::setSwitch(uint32_t entry, uint32_t mask)
    {
        static constexpr uint32_t kOffsets[] = {0x180u, 0x184u, 0x1A0u, 0x1A4u, 0u, 0x188u, 0x18Cu, 0x190u, 0x194u};
        const uint32_t core = entry & 1u, reg = entry >> 8u;
        if (reg < 0x13u || reg > 0x1Bu || reg == 0x17u)
            return;
        const uint32_t offset = core * 0x400u + kOffsets[reg - 0x13u];
        write(offset, static_cast<uint16_t>(mask));
        write(offset + 2u, static_cast<uint16_t>((mask >> 16u) & 0xFFu));
    }

    void Sg2Driver::setAddr(uint32_t entry, uint32_t byteAddress)
    {
        const uint32_t core = entry & 1u, reg = (entry >> 8u) & 0xFFu, v = (entry >> 1u) & 0x1Fu;
        const uint32_t words = (byteAddress >> 1u) & 0xFFFFFu;
        uint32_t offset;
        if (reg == 0x1Cu) // ESA
            offset = core * 0x400u + 0x2E0u;
        else if (reg == 0x1Du) // EEA: the top bits only
        {
            write(core * 0x400u + 0x33Cu, static_cast<uint16_t>(byteAddress >> 17u));
            return;
        }
        else if (reg >= 0x20u && reg <= 0x22u && v < 24u) // SSA, LSAX, NAX
            offset = core * 0x400u + kVoiceAddresses + v * 0xCu + (reg - 0x20u) * 4u;
        else
            return;
        write(offset, static_cast<uint16_t>(words >> 16u));
        write(offset + 2u, static_cast<uint16_t>(words));
    }

    uint32_t Sg2Driver::getAddr(uint32_t entry)
    {
        const uint32_t core = entry & 1u, reg = (entry >> 8u) & 0xFFu, v = (entry >> 1u) & 0x1Fu;
        if (reg < 0x20u || reg > 0x22u || v >= 24u)
            return 0;
        const uint32_t offset = core * 0x400u + kVoiceAddresses + v * 0xCu + (reg - 0x20u) * 4u;
        return ((static_cast<uint32_t>(read(offset) & 0xFu) << 16u) | read(offset + 2u)) << 1u;
    }

    void Sg2Driver::setCoreAttr(uint32_t entry, uint32_t value)
    {
        const uint32_t core = entry & 1u, type = entry >> 1u;
        const uint32_t offset = core * 0x400u + 0x19Au;
        uint16_t attr = read(offset);
        switch (type)
        {
        case 1: attr = static_cast<uint16_t>((attr & ~0x80u) | (value ? 0x80u : 0u)); break;   // effect
        case 2: attr = static_cast<uint16_t>((attr & ~0x40u) | (value ? 0x40u : 0u)); break;   // IRQ
        case 3: attr = static_cast<uint16_t>((attr & ~0x4000u) | (value ? 0u : 0x4000u)); break; // mute
        case 4: attr = static_cast<uint16_t>((attr & ~0x3F00u) | ((value & 0x3Fu) << 8u)); break; // noise clock
        default: return; // SPDIF and the rest: not part of the mix
        }
        write(offset, attr);
    }

    // sceSdInit(0) as far as the mix sees it (values as libsd leaves them).
    void Sg2Driver::sdInit()
    {
        pcmStop();
        for (uint32_t core = 0; core < 2u; ++core)
        {
            const uint32_t base = core * 0x400u;
            const uint32_t volumes = kVolumes + core * kVolumeStride;
            setSwitch(0x1600u | core, 0xFFFFFFu);
            write(base + 0x19Au, 0xC000u);
            setSwitch(0x1300u | core, 0);
            setSwitch(0x1400u | core, 0);
            setSwitch(0x1800u | core, 0xFFFFFFu);
            setSwitch(0x1A00u | core, 0xFFFFFFu);
            setSwitch(0x1900u | core, 0);
            setSwitch(0x1B00u | core, 0);
            write(base + 0x198u, core ? 0x0FFCu : 0x0FF0u);
            write(base + 0x1B0u, 0);
            write(base + 0x33Cu, core ? 0xEu : 0xFu);
            write(volumes + 0x0u, 0x3FFFu);
            write(volumes + 0x2u, 0x3FFFu);
            write(volumes + 0x4u, 0);
            write(volumes + 0x6u, 0);
            write(volumes + 0x8u, core ? 0x7FFFu : 0u);
            write(volumes + 0xAu, core ? 0x7FFFu : 0u);
            write(volumes + 0xCu, 0);
            write(volumes + 0xEu, 0);
            for (uint32_t i = 0; i < 10u; ++i)
                write(volumes + 0x14u + i * 2u, 0);
            for (uint32_t v = 0; v < 24u; ++v)
                for (uint32_t reg = 0; reg < 5u; ++reg)
                    write(base + v * 0x10u + reg * 2u, 0);
        }
    }

    void Sg2Driver::write(uint32_t offset, uint16_t value)
    {
        m_spu.write16(Spu2::Base + offset, value);
    }

    uint16_t Sg2Driver::read(uint32_t offset)
    {
        return m_spu.read16(Spu2::Base + offset);
    }

    void Sg2Driver::iopToSpu(uint32_t iopAddress, uint32_t spuAddress, uint32_t bytes)
    {
        std::vector<uint8_t> data(bytes);
        if (bytes && m_memory.readRam(iopAddress, data.data(), bytes))
            m_spu.writeRam(spuAddress, data.data(), bytes);
    }
}
