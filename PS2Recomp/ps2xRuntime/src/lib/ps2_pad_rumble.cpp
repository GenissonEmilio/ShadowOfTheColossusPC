// DualShock 2 rumble on the host's controller (Linux evdev force feedback).
// Kept apart from ps2_pad.cpp: <linux/input.h> defines KEY_W, KEY_UP, ... as
// macros with evdev codes, which would replace raylib's keys of the same name.
#include "runtime/ps2_pad.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#if defined(__linux__) && !defined(__ANDROID__)
namespace
{
    // The evdev node of the controller in use that has rumble (-1: none found).
    int openRumbleDevice(const char *name)
    {
        int fallback = -1;
        for (int i = 0; i < 64; ++i)
        {
            char path[32];
            std::snprintf(path, sizeof(path), "/dev/input/event%d", i);
            const int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd < 0)
                continue;
            unsigned long ff[(FF_CNT + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long))] = {};
            char devName[256] = {};
            const bool rumble = ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff)), ff) >= 0 &&
                                (ff[FF_RUMBLE / (8 * sizeof(unsigned long))] >> (FF_RUMBLE % (8 * sizeof(unsigned long)))) & 1ul;
            ioctl(fd, EVIOCGNAME(sizeof(devName) - 1), devName);
            if (rumble && name && std::strcmp(devName, name) == 0)
            {
                if (fallback >= 0)
                    close(fallback);
                std::fprintf(stderr, "[pad] rumble on %s (%s)\n", path, devName);
                return fd;
            }
            if (rumble && fallback < 0)
            {
                std::fprintf(stderr, "[pad] rumble candidate %s (%s)\n", path, devName);
                fallback = fd;
            }
            else
                close(fd);
        }
        return fallback;
    }
}
#endif

namespace
{
#if defined(__linux__) && !defined(__ANDROID__)
    // The motors as last asked for, and the thread that plays them: the game
    // thread never waits on the controller (each change is a USB transfer).
    std::mutex g_rumbleMutex;
    std::condition_variable g_rumbleWake;
    bool g_rumbleSmall = false, g_rumbleChanged = false;
    float g_rumbleLarge = 0.0f;

    void rumbleThread()
    {
        int fd = -1, forPad = -2;
        ff_effect effect{};
        bool playing = false;
        uint16_t lastStrong = 0, lastWeak = 0;
        static const float strength = std::getenv("PS2X_RUMBLE") ? static_cast<float>(std::atof(std::getenv("PS2X_RUMBLE"))) : 1.0f;
        for (;;)
        {
            bool small = false;
            float large = 0.0f;
            {
                std::unique_lock<std::mutex> lock(g_rumbleMutex);
                g_rumbleWake.wait(lock, [] { return g_rumbleChanged; });
                g_rumbleChanged = false;
                small = g_rumbleSmall;
                large = g_rumbleLarge;
            }
            const int pad = PSPadBackend::gamepadIndex();
            if (pad != forPad)
            {
                if (fd >= 0)
                    close(fd);
                fd = pad >= 0 ? openRumbleDevice(PSPadBackend::gamepadName()) : -1;
                forPad = pad;
                effect = {};
                effect.type = FF_RUMBLE;
                effect.id = -1;
                playing = false;
                lastStrong = lastWeak = 0;
            }
            if (fd < 0)
                continue;
            const uint16_t strong = static_cast<uint16_t>(std::clamp(large * strength, 0.0f, 1.0f) * 0xFFFF);
            const uint16_t weak = small ? static_cast<uint16_t>(std::clamp(0.6f * strength, 0.0f, 1.0f) * 0xFFFF) : 0;
            if (strong == lastStrong && weak == lastWeak && (playing || (!strong && !weak)))
                continue;
            lastStrong = strong;
            lastWeak = weak;
            input_event play{};
            play.type = EV_FF;
            if (!strong && !weak)
            {
                if (playing && effect.id >= 0)
                {
                    play.code = static_cast<uint16_t>(effect.id);
                    play.value = 0;
                    (void)!write(fd, &play, sizeof(play));
                }
                playing = false;
            }
            else
            {
                effect.u.rumble.strong_magnitude = strong;
                effect.u.rumble.weak_magnitude = weak;
                effect.replay.length = 2000; // the game sets the motors again or turns them off well before
                if (ioctl(fd, EVIOCSFF, &effect) >= 0)
                {
                    play.code = static_cast<uint16_t>(effect.id);
                    play.value = 1;
                    playing = write(fd, &play, sizeof(play)) == static_cast<ssize_t>(sizeof(play));
                }
            }
            // At most ~60 changes a second: the game may set the motors many times a frame.
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
#endif
}

void PSPadBackend::setRumble(bool small, float large)
{
#if defined(__linux__) && !defined(__ANDROID__)
    static std::once_flag started;
    std::call_once(started, [] { std::thread(rumbleThread).detach(); });
    {
        std::lock_guard<std::mutex> lock(g_rumbleMutex);
        if (g_rumbleSmall == small && g_rumbleLarge == large)
            return;
        g_rumbleSmall = small;
        g_rumbleLarge = large;
        g_rumbleChanged = true;
    }
    g_rumbleWake.notify_one();
#else
    (void)small;
    (void)large;
#endif
}
