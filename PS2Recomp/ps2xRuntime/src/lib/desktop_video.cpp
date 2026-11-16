#include "runtime/desktop_video.h"
#ifdef PS2X_DESKTOP_VIDEO
#include "desktop_fsr.h"
#include "runtime/host_menu.h"
#include "rlgl.h"
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
    int windowResolution = 0;
    int restoreFrames = 0;
    int frameLimit = 0;
    int refreshRate = 60;
    double nextRefreshCheck = 0.0;

    Shader easuShader{}, rcasShader{};
    int easuSourceLocation = -1;
    bool fsrInitialized = false;
    RenderTexture2D easuTarget{}, rcasTarget{};
    int fsrWidth = 0, fsrHeight = 0;
    bool fsrWithShader = false, fsrTargetsReady = false;

    void releaseFsrTargets()
    {
        if (easuTarget.id) UnloadRenderTexture(easuTarget);
        if (rcasTarget.id) UnloadRenderTexture(rcasTarget);
        easuTarget = {};
        rcasTarget = {};
        fsrWidth = fsrHeight = 0;
        fsrTargetsReady = false;
    }

    void initializeFsr()
    {
        fsrInitialized = true;
        easuShader = LoadShaderFromMemory(nullptr, kEasuFragment);
        rcasShader = LoadShaderFromMemory(nullptr, kRcasFragment);
        const unsigned int fallback = rlGetShaderIdDefault();
        if (easuShader.id == fallback) easuShader = {};
        if (rcasShader.id == fallback) rcasShader = {};
        if (!IsShaderValid(easuShader) || !IsShaderValid(rcasShader))
        {
            TraceLog(LOG_WARNING, "VIDEO: could not compile FSR 1 shaders");
            return;
        }
        easuSourceLocation = GetShaderLocation(easuShader, "sourceSize");
    }

    bool prepareFsr(int width, int height)
    {
        if (!fsrInitialized) initializeFsr();
        if (easuSourceLocation < 0) return false;
        const bool withShader = shader.id != 0;
        if (width != fsrWidth || height != fsrHeight || withShader != fsrWithShader)
        {
            releaseFsrTargets();
            fsrWidth = width;
            fsrHeight = height;
            fsrWithShader = withShader;
            easuTarget = LoadRenderTexture(width, height);
            fsrTargetsReady = IsRenderTextureValid(easuTarget) && rlFramebufferComplete(easuTarget.id);
            if (fsrTargetsReady && withShader)
            {
                rcasTarget = LoadRenderTexture(width, height);
                fsrTargetsReady = IsRenderTextureValid(rcasTarget) && rlFramebufferComplete(rcasTarget.id);
            }
            if (!fsrTargetsReady)
                TraceLog(LOG_WARNING, "VIDEO: could not allocate FSR 1 render targets (%dx%d)", width, height);
        }
        return fsrTargetsReady;
    }

}

Rectangle destination(int width, int height, int sourceWidth, int sourceHeight, int mode)
{
    const float w = static_cast<float>(std::max(1, width));
    const float h = static_cast<float>(std::max(1, height));
    float dw = w, dh = h;
    if (mode == hostmenu::kScreenFit)
    {
        dw = std::min(w, h * (4.0f / 3.0f));
        dh = dw * 0.75f;
    }
    else if (mode == hostmenu::kScreenInteger)
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

void setWindowResolution(int resolution)
{
    if (resolution == windowResolution) return;
    windowResolution = resolution;
    if (resolution <= 0 || resolution >= kWindowResolutionCount) return;

    const auto size = kWindowResolutions[resolution];
    windowWidth = size.width;
    windowHeight = size.height;
    if (!IsWindowFullscreen())
    {
        if (IsWindowMaximized()) RestoreWindow();
        SetWindowSize(windowWidth, windowHeight);
    }
}

void setFullscreen(bool enabled)
{
    if (enabled == IsWindowFullscreen()) return;
    nextRefreshCheck = 0.0;
    if (enabled)
    {
        restoreFrames = 0;
        windowWidth = GetScreenWidth(); windowHeight = GetScreenHeight();
        windowPosition = GetWindowPosition();
        const int monitor = GetCurrentMonitor();
        const int nativeWidth = GetMonitorWidth(monitor), nativeHeight = GetMonitorHeight(monitor);
        // ToggleFullscreen alone retains the old 1024x720 video mode.
        // Attach before resizing, so a window near a monitor boundary cannot
        // accidentally select its neighbour after the size changes.
        ToggleFullscreen();
        SetWindowSize(nativeWidth, nativeHeight);
    }
    else
    {
        ToggleFullscreen();
        SetWindowSize(windowWidth, windowHeight);
        SetWindowPosition(static_cast<int>(windowPosition.x), static_cast<int>(windowPosition.y));
        // X11 window managers may apply their decorations after this resize.
        // Reapply the content size once those asynchronous events have arrived.
        restoreFrames = 3;
    }
}

void updatePresentation(bool vsync, bool cap30)
{
    if (restoreFrames > 0 && --restoreFrames == 0)
    {
        SetWindowSize(windowWidth, windowHeight);
        SetWindowPosition(static_cast<int>(windowPosition.x), static_cast<int>(windowPosition.y));
    }
    // Set the swap interval on the presentation context, not the renderer's
    // worker context. raylib reapplies it when changing fullscreen mode.
    if (vsync != IsWindowState(FLAG_VSYNC_HINT))
    {
        if (vsync) SetWindowState(FLAG_VSYNC_HINT);
        else ClearWindowState(FLAG_VSYNC_HINT);
    }

    const double now = GetTime();
    if (vsync && now >= nextRefreshCheck)
    {
        refreshRate = GetMonitorRefreshRate(GetCurrentMonitor());
        if (refreshRate <= 0) refreshRate = 60;
        nextRefreshCheck = now + 1.0;
    }
    // Leave headroom above the monitor clock to avoid a second limiter
    // making synchronized swaps miss refreshes. Bound the frame rate even
    // when a driver or virtual display ignores the requested swap interval.
    const int limit = cap30 ? 30 : (vsync ? refreshRate + std::max(5, refreshRate / 10) : 60);
    if (limit != frameLimit)
    {
        frameLimit = limit;
        SetTargetFPS(limit);
    }
}

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

void draw(Texture2D texture, int width, int height, bool flipY, int mode, bool upscaleFsr)
{
    const Rectangle target = destination(GetScreenWidth(), GetScreenHeight(), width, height, mode);
    const float outputSize[]{target.width * GetRenderWidth() / std::max(1, GetScreenWidth()),
                             target.height * GetRenderHeight() / std::max(1, GetScreenHeight())};
    Rectangle source{0, 0, static_cast<float>(width), static_cast<float>(flipY ? -height : height)};
    const int outputWidth = static_cast<int>(std::lround(outputSize[0]));
    const int outputHeight = static_cast<int>(std::lround(outputSize[1]));
    const bool needsUpscale = upscaleFsr && mode != hostmenu::kScreenInteger &&
        outputWidth >= width && outputHeight >= height && (outputWidth > width || outputHeight > height);
    if (!needsUpscale && fsrWidth != 0)
        releaseFsrTargets();
    if (needsUpscale && prepareFsr(outputWidth, outputHeight))
    {
        const Rectangle passTarget{0, 0, static_cast<float>(outputWidth), static_cast<float>(outputHeight)};
        const int sourceSize[]{width, height};
        BeginTextureMode(easuTarget);
        SetShaderValue(easuShader, easuSourceLocation, sourceSize, SHADER_UNIFORM_IVEC2);
        BeginShaderMode(easuShader);
        DrawTexturePro(texture, source, passTarget, {0, 0}, 0, WHITE);
        EndShaderMode();
        EndTextureMode();

        const Rectangle upscaledSource{0, 0, passTarget.width, -passTarget.height};
        if (!shader.id)
        {
            BeginShaderMode(rcasShader);
            DrawTexturePro(easuTarget.texture, upscaledSource, target, {0, 0}, 0, WHITE);
            EndShaderMode();
            return;
        }

        // Imported shaders still see the presented picture before the host UI.
        BeginTextureMode(rcasTarget);
        BeginShaderMode(rcasShader);
        DrawTexturePro(easuTarget.texture, upscaledSource, passTarget, {0, 0}, 0, WHITE);
        EndShaderMode();
        EndTextureMode();
        texture = rcasTarget.texture;
        source = upscaledSource;
    }
    if (shader.id)
    {
        const float sourceSize[]{source.width, std::abs(source.height)};
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
    if (easuShader.id) UnloadShader(easuShader);
    if (rcasShader.id) UnloadShader(rcasShader);
    shader = {};
    easuShader = {};
    rcasShader = {};
    easuSourceLocation = -1;
    fsrInitialized = false;
    releaseFsrTargets();
    windowResolution = 0;
    restoreFrames = frameLimit = 0;
    nextRefreshCheck = 0.0;
}
}
#endif
