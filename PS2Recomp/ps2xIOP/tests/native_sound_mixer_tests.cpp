#include "native/sound_mixer.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using ps2x::iop::detail::SoundMixer;
    constexpr uint64_t kCycles = SoundMixer::CyclesPerSample;
    constexpr uint32_t kLead = 1024u;
    constexpr uint64_t kStart = 10000u * kCycles;
    constexpr int kMusic = 18, kEffect = 6;

    void expect(bool condition, const char *message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    class Fixture
    {
    public:
        Fixture() { start(); }

        void start()
        {
            auto lock = mixer.hold();
            for (const int voice : {kMusic, kEffect})
            {
                mixer.setStart(voice, voice * 0x1000u);
                mixer.setPitch(voice, 0x1000u);
                mixer.setAdsr1(voice, 0x80ffu);
                mixer.setAdsr2(voice, 0x5fcfu);
                mixer.keyOn(0, 1u << voice);
            }
            mixer.setStreamVoices(0, 1u << kMusic, true);
        }

        void pull(size_t frames)
        {
            std::vector<int16_t> stereo(frames * 2u);
            expect(mixer.readOutput(stereo.data(), frames) == frames, "The callback must keep supplying frames");
        }

        SoundMixer mixer;
    };

    void testLeadAndEffects()
    {
        Fixture fixture;
        auto &mixer = fixture.mixer;
        fixture.pull(kLead);
        expect(mixer.nextAddress(kMusic) == kMusic * 0x1000u && mixer.envelope(kMusic) == 0u,
               "The stream must hold until the IOP clock is known");
        expect(mixer.nextAddress(kEffect) > kEffect * 0x1000u && mixer.envelope(kEffect) > 0u,
               "Effects must advance on the audio clock");

        mixer.advanceTo(kStart);
        fixture.pull(kLead);
        const auto musicAtLimit = mixer.nextAddress(kMusic);
        const auto envelopeAtLimit = mixer.envelope(kMusic);
        const auto effectAtLimit = mixer.nextAddress(kEffect);
        expect(musicAtLimit > kMusic * 0x1000u, "The stream must use the bounded lookahead");
        fixture.pull(4u * kLead);
        expect(mixer.nextAddress(kMusic) == musicAtLimit && mixer.envelope(kMusic) == envelopeAtLimit,
               "The stream cursor and envelope must hold when the guest stops");
        expect(mixer.nextAddress(kEffect) > effectAtLimit, "A stalled stream must leave effects running");

        mixer.advanceTo(kStart + 256u * kCycles);
        fixture.pull(256u);
        const auto musicAfterRecovery = mixer.nextAddress(kMusic);
        expect(musicAfterRecovery > musicAtLimit, "Guest progress must let the stream recover");
        fixture.pull(kLead);
        expect(mixer.nextAddress(kMusic) == musicAfterRecovery, "Recovery must not renew the lookahead budget");

        mixer.setStreamVoices(0, 1u << kMusic, false);
        fixture.pull(kLead);
        expect(mixer.nextAddress(kMusic) > musicAfterRecovery, "A released stream slot must return to pull timing");
    }

    void testBacklogAndClockRewind()
    {
        Fixture paused, reference;
        paused.mixer.advanceTo(kStart);
        reference.mixer.advanceTo(kStart);
        reference.pull(kLead);
        reference.mixer.advanceTo(kStart + kLead * kCycles);
        reference.pull(kLead);

        paused.pull(kLead);
        const auto beforeGap = paused.mixer.nextAddress(kMusic);
        paused.mixer.advanceTo(kStart + 48000u * kCycles);
        paused.pull(4u * kLead);
        expect(paused.mixer.nextAddress(kMusic) > beforeGap, "Guest progress must reopen the stream budget");
        reference.mixer.advanceTo(kStart + 2u * kLead * kCycles);
        reference.pull(kLead);
        expect(paused.mixer.nextAddress(kMusic) == reference.mixer.nextAddress(kMusic),
               "After a device pause, stale guest credit must be capped");

        paused.mixer.advanceTo(kStart);
        const auto beforeRewind = paused.mixer.nextAddress(kMusic);
        paused.pull(kLead);
        const auto afterRewind = paused.mixer.nextAddress(kMusic);
        paused.pull(kLead);
        expect(afterRewind > beforeRewind && paused.mixer.nextAddress(kMusic) == afterRewind,
               "A backwards clock must rebase without unlimited credit");
    }

    void testResetAndRestore()
    {
        Fixture fixture;
        auto &mixer = fixture.mixer;
        mixer.advanceTo(kStart);
        fixture.pull(kLead);
        const auto savedCursor = mixer.nextAddress(kMusic);
        std::vector<uint8_t> data;
        std::string error;
        expect(mixer.captureState(data, error), "Could not save the mixer state");
        mixer.advanceTo(kStart + kLead * kCycles);
        fixture.pull(kLead);
        expect(mixer.restoreState(data, error), "Could not restore the mixer state");
        fixture.pull(kLead);
        expect(mixer.nextAddress(kMusic) == savedCursor, "Load must discard clock credit from the abandoned timeline");
        mixer.advanceTo(kStart);
        fixture.pull(kLead);
        expect(mixer.nextAddress(kMusic) > savedCursor, "A restored stream must recover with the new clock");

        mixer.reset();
        fixture.start();
        fixture.pull(kLead);
        expect(mixer.nextAddress(kMusic) == kMusic * 0x1000u, "Reset must discard clock credit");
        mixer.advanceTo(kStart);
        fixture.pull(kLead);
        expect(mixer.nextAddress(kMusic) > kMusic * 0x1000u, "A new stream must recover after reset");
    }

    void testRestoreWhileDevicePulls()
    {
        Fixture fixture;
        auto &mixer = fixture.mixer;
        mixer.advanceTo(kStart);
        fixture.pull(kLead / 2u);
        const auto savedCursor = mixer.nextAddress(kMusic);
        std::vector<uint8_t> data;
        std::string error;
        expect(mixer.captureState(data, error), "Could not save the mixer state");

        std::atomic<unsigned> callbacks{0};
        std::jthread device([&](std::stop_token stop) {
            std::array<int16_t, 128> stereo{};
            while (!stop.stop_requested())
            {
                mixer.readOutput(stereo.data(), stereo.size() / 2u);
                callbacks.fetch_add(1, std::memory_order_release);
            }
        });
        while (callbacks.load(std::memory_order_acquire) == 0u)
            std::this_thread::yield();
        const unsigned beforeLoad = callbacks.load(std::memory_order_acquire);
        expect(mixer.restoreState(data, error), "Could not restore while the audio device was pulling");
        while (callbacks.load(std::memory_order_acquire) <= beforeLoad + kLead / 64u)
            std::this_thread::yield();
        device.request_stop();
        device.join();
        expect(mixer.nextAddress(kMusic) == savedCursor, "Device pulls must preserve the restored cursor until new guest time");
    }
}

int main()
{
    try
    {
        testLeadAndEffects();
        testBacklogAndClockRewind();
        testResetAndRestore();
        testRestoreWhileDevicePulls();
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
