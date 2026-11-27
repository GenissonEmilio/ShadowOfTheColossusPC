#pragma once

#if defined(_WIN32) || defined(__ANDROID__) || (defined(__linux__) && !defined(__ANDROID__))
#define PS2X_DESKTOP_VIDEO 1
#include "raylib.h"
#include <array>
#include <string>

// Presentation only: the game picture is filtered before the menu/HUD is drawn.
namespace desktop_video
{
    struct WindowResolution { int width; int height; };
    inline constexpr std::array<WindowResolution, 9> kWindowResolutions{{
        {0, 0}, {640, 480}, {800, 600}, {1024, 720}, {1280, 720},
        {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160},
    }};
    inline constexpr int kWindowResolutionCount = static_cast<int>(kWindowResolutions.size());

    Rectangle destination(int width, int height, int sourceWidth, int sourceHeight, int mode);
    void setWindowResolution(int resolution);
    void setFullscreen(bool enabled);
    void updatePresentation(bool vsync, bool cap30, bool frameGeneration = false);
    Texture2D interpolate(Texture2D previous, Texture2D current);
    bool loadShader(const char *path, std::string &error);
    bool shaderActive();
    bool fxaaAvailable();
    bool frameGenerationAvailable();
    bool fsrAvailable();
    void draw(Texture2D texture, int width, int height, bool flipY, int mode,
              bool upscaleFsr = false, bool fxaa = false);
    void shutdown();
}
#endif
