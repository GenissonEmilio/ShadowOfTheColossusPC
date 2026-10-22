#pragma once

#if defined(__linux__) && !defined(__ANDROID__)
#include "raylib.h"
#include <string>

// Presentation only: the game picture is filtered before the menu/HUD is drawn.
namespace desktop_video
{
    Rectangle destination(int width, int height, int sourceWidth, int sourceHeight, int mode);
    void setFullscreen(bool enabled);
    bool loadShader(const char *path, std::string &error);
    bool shaderActive();
    void draw(Texture2D texture, int width, int height, bool flipY, int mode);
    void shutdown();
}
#endif
