#include "runtime/desktop_video.h"
#ifdef PS2X_DESKTOP_VIDEO
#include "desktop_antialias.h"
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
    int sourceLocation = -1, outputLocation = -1, timeLocation = -1, fsrSharpenedLocation = -1;
    int windowWidth = 1024, windowHeight = 720;
    Vector2 windowPosition{};
    int windowResolution = 0;
    int restoreFrames = 0;
    int frameLimit = 0;
    int refreshRate = 60;
    double nextRefreshCheck = 0.0;

    struct BuiltinShader
    {
        Shader shader{};
        int sourceSize = -1;
        int targetSize = -1;
        bool attempted = false;

        bool initialize(const char *fragment, const char *name, bool needsSourceSize, bool needsTargetSize = false)
        {
            if (!attempted)
            {
                attempted = true;
                Shader candidate = LoadShaderFromMemory(nullptr, fragment);
                if (candidate.id != rlGetShaderIdDefault())
                {
                    if (IsShaderValid(candidate))
                    {
                        if (needsSourceSize) sourceSize = GetShaderLocation(candidate, "sourceSize");
                        if (needsTargetSize) targetSize = GetShaderLocation(candidate, "targetSize");
                        if ((!needsSourceSize || sourceSize >= 0) && (!needsTargetSize || targetSize >= 0))
                            shader = candidate;
                    }
                    if (candidate.id && !shader.id) UnloadShader(candidate);
                }
                if (!shader.id) TraceLog(LOG_WARNING, "VIDEO: could not initialize %s", name);
            }
            return shader.id != 0;
        }

        void release()
        {
            if (shader.id) UnloadShader(shader);
            *this = {};
        }
    } easu, rcas, area, fxaaShader;

    struct Pass
    {
        const BuiltinShader *program;
        int width, height;
    };

    struct Target
    {
        RenderTexture2D frame{};
        int width = 0, height = 0;
        int filter = TEXTURE_FILTER_POINT;

        void release()
        {
            if (frame.texture.id) UnloadTexture(frame.texture);
            if (frame.id) rlUnloadFramebuffer(frame.id);
            *this = {};
        }

        bool prepare(int w, int h)
        {
            if (w == width && h == height) return frame.id != 0;
            // The presentation passes only need one RGBA8 attachment each.
            frame.id = rlLoadFramebuffer();
            if (frame.id)
            {
                constexpr int format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                frame.texture = {rlLoadTexture(nullptr, w, h, format, 1), w, h, 1, format};
                rlFramebufferAttach(frame.id, frame.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
                if (!frame.texture.id || !rlFramebufferComplete(frame.id)) release();
            }
            // Remember a failed size until the layout changes.
            width = w;
            height = h;
            if (!frame.id)
                TraceLog(LOG_WARNING, "VIDEO: could not allocate presentation target (%dx%d)", w, h);
            return frame.id != 0;
        }

        void setFilter(int wanted)
        {
            if (wanted == filter) return;
            SetTextureFilter(frame.texture, wanted);
            filter = wanted;
        }
    };

    std::array<Target, 3> targets;
    bool overTargetBudget = false;

    bool prepareTargets(const std::array<Pass, 3> &passes, int count)
    {
        // 96 MiB of optional color buffers, enough for two 4K buffers and
        // a typical resolve intermediate. Fall back to the original picture
        // for unusually large windows or insufficient GPU resources.
        constexpr long long maxTargetPixels = 96LL * 1024 * 1024 / 4;
        long long pixels = 0;
        for (int i = 0; i < count; ++i)
            pixels += static_cast<long long>(passes[i].width) * passes[i].height;
        const bool exceedsBudget = pixels > maxTargetPixels;
        if (exceedsBudget && !overTargetBudget)
            TraceLog(LOG_WARNING, "VIDEO: presentation targets exceed the 96 MiB budget");
        overTargetBudget = exceedsBudget;
        if (exceedsBudget) count = 0;
        for (int i = 0; i < static_cast<int>(targets.size()); ++i)
            if (i >= count || targets[i].width != passes[i].width || targets[i].height != passes[i].height)
                targets[i].release();
        if (exceedsBudget) return false;

        for (int i = 0; i < count; ++i)
            if (!targets[i].prepare(passes[i].width, passes[i].height)) return false;
        return true;
    }

    void drawPass(const Pass &pass, Texture2D texture, Rectangle source, Rectangle destination)
    {
        const int sourceSize[]{static_cast<int>(source.width), static_cast<int>(std::abs(source.height))};
        const int targetSize[]{pass.width, pass.height};
        const auto &program = *pass.program;
        if (program.sourceSize >= 0)
            SetShaderValue(program.shader, program.sourceSize, sourceSize, SHADER_UNIFORM_IVEC2);
        if (program.targetSize >= 0)
            SetShaderValue(program.shader, program.targetSize, targetSize, SHADER_UNIFORM_IVEC2);
        BeginShaderMode(program.shader);
        DrawTexturePro(texture, source, destination, {0, 0}, 0, WHITE);
        EndShaderMode();
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

bool fxaaAvailable()
{
    return IsWindowReady() && fxaaShader.initialize(kFxaaFragment, "FXAA", true);
}

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
    fsrSharpenedLocation = GetShaderLocation(shader, "fsrSharpened");
    return true;
}

void draw(Texture2D texture, int width, int height, bool flipY, int mode, bool upscaleFsr, bool fxaa)
{
    const Rectangle target = destination(GetScreenWidth(), GetScreenHeight(), width, height, mode);
    const float scaleX = static_cast<float>(GetRenderWidth()) / std::max(1, GetScreenWidth());
    const float scaleY = static_cast<float>(GetRenderHeight()) / std::max(1, GetScreenHeight());
    const float outputSize[]{target.width * scaleX, target.height * scaleY};
    Rectangle source{0, 0, static_cast<float>(width), static_cast<float>(flipY ? -height : height)};
    const int outputWidth = std::max(1, static_cast<int>(std::lround(outputSize[0])));
    const int outputHeight = std::max(1, static_cast<int>(std::lround(outputSize[1])));
    const bool needsUpscale = upscaleFsr && mode != hostmenu::kScreenInteger &&
        outputWidth >= width && outputHeight >= height && (outputWidth > width || outputHeight > height);

    std::array<Pass, 3> passes{};
    int passCount = 0;
    if (needsUpscale && easu.initialize(kEasuFragment, "FSR 1 EASU", true)
                     && rcas.initialize(kRcasFragment, "FSR 1 RCAS", false))
    {
        width = outputWidth;
        height = outputHeight;
        passes[passCount++] = {&easu, width, height};
        passes[passCount++] = {&rcas, width, height};
    }
    else if ((outputWidth < width || outputHeight < height) &&
             area.initialize(kAreaFragment, "supersampling resolve", true, true))
    {
        // Resolve only the axes that actually shrink, even for mixed-aspect
        // presentation. Magnification on the remaining axis happens last.
        if (outputWidth < width)
        {
            width = outputWidth;
            passes[passCount++] = {&area, width, height};
        }
        if (outputHeight < height)
        {
            height = outputHeight;
            passes[passCount++] = {&area, width, height};
        }
    }
    if (fxaa && fxaaAvailable()) passes[passCount++] = {&fxaaShader, width, height};

    // A full-size last pass can go straight to a pixel-aligned window.
    const auto pixelAligned = [](float x) { return std::abs(x - std::round(x)) < 0.001f; };
    const bool direct = passCount > 0 && !shader.id && width == outputWidth && height == outputHeight &&
        pixelAligned(target.x * scaleX) && pixelAligned(target.y * scaleY) &&
        pixelAligned(outputSize[0]) && pixelAligned(outputSize[1]);
    int targetCount = passCount - (direct ? 1 : 0);
    // Reuse the EASU buffer for FXAA when a user shader also needs the output.
    const bool reuseFirst = targetCount == 3 && passes[0].width == passes[2].width && passes[0].height == passes[2].height;
    if (reuseFirst) --targetCount;

    int fsrSharpened = 0;
    if (prepareTargets(passes, targetCount))
    {
        const bool integerMagnification = mode == hostmenu::kScreenInteger &&
            outputWidth >= width && outputHeight >= height;
        for (int i = 0; i < passCount; ++i)
        {
            const Pass &pass = passes[i];
            if (direct && i == passCount - 1)
            {
                drawPass(pass, texture, source, target);
                return;
            }

            auto &buffer = targets[reuseFirst && i == 2 ? 0 : i];
            const Rectangle passTarget{0, 0, static_cast<float>(pass.width), static_cast<float>(pass.height)};
            buffer.setFilter(integerMagnification ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_BILINEAR);
            BeginTextureMode(buffer.frame);
            rlDisableColorBlend();
            drawPass(pass, texture, source, passTarget);
            if (pass.program == &rcas) fsrSharpened = 1;
            EndTextureMode();
            rlEnableColorBlend();
            texture = buffer.frame.texture;
            source = {0, 0, passTarget.width, -passTarget.height};
        }
    }
    if (shader.id)
    {
        const float sourceSize[]{source.width, std::abs(source.height)};
        const float seconds = static_cast<float>(GetTime());
        if (sourceLocation >= 0) SetShaderValue(shader, sourceLocation, sourceSize, SHADER_UNIFORM_VEC2);
        if (outputLocation >= 0) SetShaderValue(shader, outputLocation, outputSize, SHADER_UNIFORM_VEC2);
        if (timeLocation >= 0) SetShaderValue(shader, timeLocation, &seconds, SHADER_UNIFORM_FLOAT);
        if (fsrSharpenedLocation >= 0) SetShaderValue(shader, fsrSharpenedLocation, &fsrSharpened, SHADER_UNIFORM_INT);
        BeginShaderMode(shader);
    }
    DrawTexturePro(texture, source, target, {0, 0}, 0, WHITE);
    if (shader.id) EndShaderMode();
}

void shutdown()
{
    if (shader.id) UnloadShader(shader);
    shader = {};
    easu.release();
    rcas.release();
    area.release();
    fxaaShader.release();
    for (auto &target : targets) target.release();
    overTargetBudget = false;
    windowResolution = 0;
    restoreFrames = frameLimit = 0;
    nextRefreshCheck = 0.0;
}
}
#endif
