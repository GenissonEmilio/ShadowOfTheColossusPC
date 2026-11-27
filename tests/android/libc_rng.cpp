#include "android_guest_random.h"
#include "runtime/android_state_identity.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string_view>

#if !defined(__BIONIC__)
#error "Run this fixture against the Android Bionic C library."
#endif

namespace
{
    using ps2_stubs::detail::AndroidGuestRandom;

    bool match(std::string_view phase, uint32_t seed, size_t index, uint32_t actual, uint32_t expected)
    {
        if (actual == expected)
            return true;
        std::cerr << phase << " mismatch for seed 0x" << std::hex << seed << std::dec
                  << " at index " << index << ": expected " << expected << ", got " << actual << '\n';
        return false;
    }

    bool verifySeed(uint32_t seed)
    {
        constexpr size_t warmup = 1027;
        constexpr size_t replay = 1024;
        std::srand(seed);
        AndroidGuestRandom guest(seed);
        for (size_t i = 0; i < warmup; ++i)
            if (!match("Bionic", seed, i, guest.next(), static_cast<uint32_t>(std::rand())))
                return false;

        ps2state::Writer snapshot;
        if (!guest.archive(snapshot) || !snapshot.finish() || snapshot.data.size() != 128)
        {
            std::cerr << "Unable to capture RNG state: " << snapshot.error << '\n';
            return false;
        }

        std::array<uint32_t, replay> expected{};
        for (size_t i = 0; i < replay; ++i)
        {
            expected[i] = static_cast<uint32_t>(std::rand());
            if (!match("Bionic", seed, warmup + i, guest.next(), expected[i]))
                return false;
        }

        AndroidGuestRandom restored(seed ^ 0xfedcba98u);
        ps2state::Reader reader(snapshot.data);
        if (!restored.archive(reader) || !reader.finish())
        {
            std::cerr << "Unable to restore RNG state: " << reader.error << '\n';
            return false;
        }
        for (size_t i = 0; i < replay; ++i)
            if (!match("Restored RNG", seed, i, restored.next(), expected[i]))
                return false;

        auto malformed = snapshot.data;
        std::fill_n(malformed.begin(), sizeof(uint32_t), 0xff);
        ps2state::Reader badIndex(malformed);
        if (restored.archive(badIndex))
        {
            std::cerr << "RNG state accepted an invalid rear index\n";
            return false;
        }
        malformed = snapshot.data;
        malformed.pop_back();
        ps2state::Reader truncated(malformed);
        if (restored.archive(truncated))
        {
            std::cerr << "RNG state accepted a truncated payload\n";
            return false;
        }
        return true;
    }

    bool verifyBuildIdentity()
    {
        // These equally sized IDs collide under IEEE CRC32 at 0xe533515e.
        const std::vector<uint8_t> expected{198, 123, 14, 197, 54, 16, 117, 149, 157, 169,
                                            30, 158, 14, 167, 111, 170, 188, 94, 104, 201};
        const std::vector<uint8_t> other{24, 54, 125, 3, 213, 74, 228, 212, 4, 83,
                                         28, 126, 128, 155, 127, 53, 195, 52, 117, 189};
        ps2state::Writer state;
        if (!ps2state::android::writeBuildId(state, expected) || !state.finish())
            return false;
        ps2state::Reader same(state.data);
        ps2state::Reader collision(state.data);
        ps2state::Reader unidentified(state.data);
        return ps2state::android::verifyBuildId(same, expected) && same.finish() &&
               !ps2state::android::verifyBuildId(collision, other) &&
               !ps2state::android::verifyBuildId(unidentified, {});
    }
}

int main()
{
    constexpr std::array<uint32_t, 6> seeds{0, 1, 42, 0x7fffffffu, 0x80000000u, 0xffffffffu};
    for (uint32_t seed : seeds)
        if (!verifySeed(seed))
            return 1;

    std::cout << "PASS: six seeds matched Bionic across 2051 draws each; "
                 "1024 draws replayed exactly per seed and malformed states were rejected\n";
    if (!verifyBuildIdentity())
    {
        std::cerr << "Android save-state build ID validation failed\n";
        return 1;
    }
    std::cout << "PASS: Android state codec rejected a same-length CRC32 collision\n";
    return 0;
}
