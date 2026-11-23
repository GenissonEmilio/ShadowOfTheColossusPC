#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace ps2x::android
{
    struct LaunchPaths
    {
        std::string discDirectory;
        std::string bootElf;
        std::string saveDirectory;
        std::string logFile;
        int resolutionPercent = 100;
    };

    struct PadInput
    {
        uint16_t buttons = 0; // Active-high DualShock 2 mask.
        float lx = 0, ly = 0, rx = 0, ry = 0;
    };

    LaunchPaths launchPaths();
    PadInput padInput();
    bool paused();
    std::chrono::steady_clock::duration waitWhilePaused(const std::atomic<bool> &stopRequested);
    void wakeGuest();
    bool initializeRenderer();
    void shutdownRenderer();
}
