#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2x::android
{
    inline constexpr float kOriginalAspect = 4.0f / 3.0f;

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
    // Zero asks for the original 4:3 presentation.
    float requestedWidescreenAspect();
    // Published by the guest thread once it updates the camera projection.
    void setActiveWidescreenAspect(float aspect);
    float activeWidescreenAspect();
    bool paused();
    std::chrono::steady_clock::duration waitWhilePaused(const std::atomic<bool> &stopRequested);
    void wakeGuest();
    void initializeOptions();
    void setPresentationOptions(bool fxaa, bool frameGeneration, bool fsr);
    void setMaxAnisotropy(float amount);
    float displayRefreshRate();
    const std::vector<uint8_t> &runnerBuildId();
    bool initializeRenderer();
    void shutdownRenderer();
}
