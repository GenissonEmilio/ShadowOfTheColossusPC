#pragma once

#include <array>
#include <cstdint>

namespace ps2_stubs::detail
{
    // Bionic rand() uses the NetBSD 31-word additive generator. Keep the
    // same sequence, including seed 0, in guest-owned, serializable storage.
    // https://android.googlesource.com/platform/bionic/+/main/libc/upstream-netbsd/common/lib/libc/stdlib/random.c
    class AndroidGuestRandom
    {
    public:
        constexpr explicit AndroidGuestRandom(uint32_t seed = 1)
        {
            constexpr int64_t modulus = 2147483647;
            words[0] = seed;
            for (uint32_t i = 1; i < Degree; ++i)
            {
                const int64_t previous = static_cast<int32_t>(words[i - 1]);
                const auto word = (previous * 16807) % modulus;
                words[i] = static_cast<uint32_t>(word <= 0 ? word + modulus : word);
            }
            for (uint32_t i = 0; i < Degree * 10; ++i)
                next();
        }

        constexpr uint32_t next()
        {
            const uint32_t front = (rear + 3) % Degree;
            words[front] += words[rear];
            rear = (rear + 1) % Degree;
            return words[front] >> 1;
        }

        template <class Archive>
        bool archive(Archive &ar)
        {
            if (!ar.pod(rear) || rear >= Degree)
                return ar.fail("Invalid guest Android random index.");
            for (auto &word : words)
                if (!ar.pod(word))
                    return false;
            return true;
        }

    private:
        static constexpr uint32_t Degree = 31;
        uint32_t rear = 0;
        std::array<uint32_t, Degree> words{};
    };

    static_assert(AndroidGuestRandom().next() == 1804289383u);
    static_assert(AndroidGuestRandom(0).next() == 2059215011u);
}
