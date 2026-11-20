#include "runtime/desktop_video.h"
#if defined(__linux__) && !defined(__ANDROID__)
#include "rlgl.h"
#include "GLFW/glfw3.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

namespace desktop_video
{
namespace
{
    Shader shader{};
    int sourceLocation = -1, outputLocation = -1, timeLocation = -1;
    int windowWidth = 1024, windowHeight = 720;
    Vector2 windowPosition{};
    int restoreFrames = 0;
    bool fullscreen = false;
}

Rectangle destination(int width, int height, int sourceWidth, int sourceHeight, int mode)
{
    const float w = static_cast<float>(std::max(1, width));
    const float h = static_cast<float>(std::max(1, height));
    float dw = w, dh = h;
    if (mode == 0)
    {
        dw = std::min(w, h * (4.0f / 3.0f));
        dh = dw * 0.75f;
    }
    else if (mode == 2)
    {
        const float sw = static_cast<float>(std::max(1, sourceWidth));
        const float sh = static_cast<float>(std::max(1, sourceHeight));
        const float scale = std::min(w / sw, h / sh);
        // Small windows must fit too, rather than cropping the source.
        const float whole = scale >= 1.0f ? std::floor(scale) : scale;
        dw = sw * whole; dh = sh * whole;
    }
    return {std::max(0.0f, (w - dw) * 0.5f), std::max(0.0f, (h - dh) * 0.5f), dw, dh};
}

void setFullscreen(bool enabled)
{
    if (enabled == fullscreen) return;
    GLFWwindow *window = static_cast<GLFWwindow *>(GetWindowHandle());
    if (enabled)
    {
        restoreFrames = 0;
        windowWidth = GetScreenWidth(); windowHeight = GetScreenHeight();
        windowPosition = GetWindowPosition();
        int count = 0;
        GLFWmonitor **monitors = glfwGetMonitors(&count);
        const int monitor = GetCurrentMonitor();
        const GLFWvidmode *mode = monitor >= 0 && monitor < count ? glfwGetVideoMode(monitors[monitor]) : nullptr;
        if (!mode) return;
        // Fullscreen at the monitor's current video mode: no mode switch (raylib's
        // ToggleFullscreen asked for the window's size and the monitor changed
        // to its 59.97 Hz mode), and the window manager sees a fullscreen
        // window (panels and docks hide).
        glfwSetWindowMonitor(window, monitors[monitor], 0, 0, mode->width, mode->height, mode->refreshRate);
    }
    else
    {
        glfwSetWindowMonitor(window, nullptr, static_cast<int>(windowPosition.x), static_cast<int>(windowPosition.y),
                             windowWidth, windowHeight, GLFW_DONT_CARE);
        // X11 window managers may apply their decorations after this resize.
        // Reapply the content size once those asynchronous events have arrived.
        restoreFrames = 3;
    }
    fullscreen = enabled;
}

bool isFullscreen() { return fullscreen; }

bool shaderActive() { return shader.id != 0; }

bool loadShader(const char *path, std::string &error)
{
    error.clear();
    if (!path || !*path)
    {
        if (shader.id) UnloadShader(shader);
        shader = {};
        return true;
    }
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() <= 0 || input.tellg() > 262144)
    {
        error = "Arquivo de shader ausente, vazio ou maior que 256 KiB.";
        return false;
    }
    input.seekg(0);
    const std::string code{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (code.find('\0') != std::string::npos)
    {
        error = "O shader precisa ser um arquivo de texto GLSL.";
        return false;
    }
    Shader candidate = LoadShaderFromMemory(nullptr, code.c_str());
    // Raylib returns its *valid default shader* on a GLSL compile/link error.
    if (!IsShaderValid(candidate) || candidate.id == rlGetShaderIdDefault())
    {
        error = "O driver rejeitou o shader GLSL 330. Confira a sintaxe e os detalhes no log.";
        if (candidate.id && candidate.id != rlGetShaderIdDefault()) UnloadShader(candidate);
        return false;
    }
    if (shader.id) UnloadShader(shader);
    shader = candidate;
    sourceLocation = GetShaderLocation(shader, "sourceSize");
    outputLocation = GetShaderLocation(shader, "outputSize");
    timeLocation = GetShaderLocation(shader, "time");
    return true;
}

void draw(Texture2D texture, int width, int height, bool flipY, int mode)
{
    if (restoreFrames > 0 && --restoreFrames == 0)
    {
        SetWindowSize(windowWidth, windowHeight);
        SetWindowPosition(static_cast<int>(windowPosition.x), static_cast<int>(windowPosition.y));
    }
    const Rectangle target = destination(GetScreenWidth(), GetScreenHeight(), width, height, mode);
    const Rectangle source{0, 0, static_cast<float>(width), static_cast<float>(flipY ? -height : height)};
    if (shader.id)
    {
        const float sourceSize[]{static_cast<float>(width), static_cast<float>(height)};
        const float outputSize[]{target.width * GetRenderWidth() / std::max(1, GetScreenWidth()),
                                 target.height * GetRenderHeight() / std::max(1, GetScreenHeight())};
        const float seconds = static_cast<float>(GetTime());
        if (sourceLocation >= 0) SetShaderValue(shader, sourceLocation, sourceSize, SHADER_UNIFORM_VEC2);
        if (outputLocation >= 0) SetShaderValue(shader, outputLocation, outputSize, SHADER_UNIFORM_VEC2);
        if (timeLocation >= 0) SetShaderValue(shader, timeLocation, &seconds, SHADER_UNIFORM_FLOAT);
        BeginShaderMode(shader);
    }
    DrawTexturePro(texture, source, target, {0, 0}, 0, WHITE);
    if (shader.id) EndShaderMode();
}

void shutdown()
{
    if (shader.id) UnloadShader(shader);
    shader = {};
}
}
#endif
