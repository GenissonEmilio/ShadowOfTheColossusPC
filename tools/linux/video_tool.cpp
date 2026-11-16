// ROM-free GPU shader validator and presentation integration test.
#include "runtime/desktop_video.h"
#include "raylib.h"
#include "rlgl.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

static std::string quoted(const std::string &value)
{
    std::string result = "\"";
    for (char c : value)
    {
        if (c == '\\' || c == '"') result += '\\';
        if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else result += c;
    }
    return result + '"';
}

int main(int argc, char **argv)
{
    const bool validate = argc == 5 && std::string(argv[1]) == "--validate";
    const bool test = argc == 3 && std::string(argv[1]) == "--self-test";
    if (!validate && !test)
    {
        std::cerr << "--validate shader.frag --report report.json | --self-test output-directory\n";
        return 2;
    }
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | (validate ? FLAG_WINDOW_HIDDEN : 0));
    InitWindow(960, 540, "SotC video validation");
    if (!IsWindowReady()) return 3;
    SetTargetFPS(60);
    std::string error;
    int code = 0;
    if (validate)
    {
        const bool ok = desktop_video::loadShader(argv[2], error);
        std::ofstream(argv[4]) << "{\"ok\":" << (ok ? "true" : "false")
                               << ",\"error\":" << quoted(error) << "}\n";
        code = ok ? 0 : 2;
    }
    else
    {
        const std::filesystem::path out(argv[2]);
        std::filesystem::create_directories(out);
        Image image = GenImageColor(320, 240, RED);
        ImageDrawRectangle(&image, 160, 0, 160, 240, GREEN);
        Texture2D texture = LoadTextureFromImage(image);
        UnloadImage(image);
        std::ofstream report(out / "report.json");
        report << "{\"monitor_width\":" << GetMonitorWidth(GetCurrentMonitor())
               << ",\"monitor_height\":" << GetMonitorHeight(GetCurrentMonitor()) << ",\"cases\":[";
        auto capture = [&](const char *name, int mode)
        {
            Color edge{}, body{};
            for (int i = 0; i < 15; ++i)
            {
                BeginDrawing(); ClearBackground(BLACK);
                desktop_video::draw(texture, 320, 240, false, mode);
                if (i == 14)
                {
                    rlDrawRenderBatchActive();
                    Image shot = LoadImageFromScreen();
                    edge = GetImageColor(shot, 0, shot.height / 2);
                    body = GetImageColor(shot, shot.width / 3, shot.height / 2);
                    ExportImage(shot, (out / (std::string(name) + ".png")).string().c_str());
                    UnloadImage(shot);
                }
                EndDrawing();
            }
            const Rectangle r = desktop_video::destination(GetScreenWidth(), GetScreenHeight(), 320, 240, mode);
            report << "{\"name\":" << quoted(name) << ",\"fullscreen\":" << (IsWindowFullscreen() ? "true" : "false")
                   << ",\"vsync\":" << (IsWindowState(FLAG_VSYNC_HINT) ? "true" : "false")
                   << ",\"width\":" << GetRenderWidth() << ",\"height\":" << GetRenderHeight()
                   << ",\"x\":" << r.x << ",\"y\":" << r.y << ",\"picture_width\":" << r.width
                   << ",\"picture_height\":" << r.height
                   << ",\"edge\":[" << int(edge.r) << ',' << int(edge.g) << ',' << int(edge.b) << ']'
                   << ",\"body\":[" << int(body.r) << ',' << int(body.g) << ',' << int(body.b) << "]}";
        };
        capture("window-fit", 0); report << ',';
        desktop_video::updatePresentation(true, false);
        desktop_video::setFullscreen(true);
        desktop_video::updatePresentation(true, false);
        capture("fullscreen-fit", 0); report << ',';
        capture("fullscreen-stretch", 1); report << ',';
        const auto invert = out / "invert.frag";
        std::ofstream(invert) << "#version 330 core\nin vec2 fragTexCoord; uniform sampler2D texture0; out vec4 finalColor;"
                               "void main(){finalColor=vec4(vec3(1.0)-texture(texture0,fragTexCoord).rgb,1.0);}";
        const bool valid = desktop_video::loadShader(invert.string().c_str(), error);
        capture("fullscreen-shader", 1); report << ',';
        const auto invalid = out / "invalid.frag";
        std::ofstream(invalid) << "#version 330 core\nvoid main(){ bad syntax }";
        const bool rejected = !desktop_video::loadShader(invalid.string().c_str(), error);
        const bool retained = desktop_video::shaderActive();
        capture("invalid-keeps-shader", 1); report << ',';
        desktop_video::loadShader(nullptr, error);
        capture("shader-disabled", 1); report << ',';
        desktop_video::setFullscreen(false);
        desktop_video::updatePresentation(false, false);
        capture("window-restored", 0);
        desktop_video::setWindowResolution(2);
        const bool selectedWindow = GetScreenWidth() == 800 && GetScreenHeight() == 600;
        desktop_video::setFullscreen(true);
        desktop_video::setWindowResolution(4);
        const bool preservedFullscreen = GetScreenWidth() == GetMonitorWidth(GetCurrentMonitor())
            && GetScreenHeight() == GetMonitorHeight(GetCurrentMonitor());
        desktop_video::setFullscreen(false);
        for (int i = 0; i < 4; ++i)
        {
            desktop_video::updatePresentation(false, false);
            PollInputEvents();
        }
        const bool restoredPreset = GetScreenWidth() == 1280 && GetScreenHeight() == 720;
        desktop_video::setWindowResolution(2);

        // Include a diagonal edge, distinct top and bottom colors, and padding
        // outside the source rectangle to expose crop and orientation errors.
        Image pattern = GenImageColor(350, 300, MAGENTA);
        ImageDrawRectangle(&pattern, 0, 0, 320, 240, GREEN);
        ImageDrawRectangle(&pattern, 0, 0, 150, 120, RED);
        ImageDrawRectangle(&pattern, 0, 120, 150, 120, BLUE);
        for (int y = 0; y < 240; ++y)
            ImageDrawPixel(&pattern, 160 + y / 3, y, WHITE);
        Texture2D fsrTexture = LoadTextureFromImage(pattern);
        SetTextureFilter(fsrTexture, TEXTURE_FILTER_BILINEAR);
        UnloadImage(pattern);

        auto render = [&](bool fsr, bool flip, int mode)
        {
            BeginDrawing();
            ClearBackground(BLACK);
            desktop_video::draw(fsrTexture, 320, 240, flip, mode, fsr);
            rlDrawRenderBatchActive();
            Image shot = LoadImageFromScreen();
            EndDrawing();
            return shot;
        };
        auto hashPixels = [](Image shot)
        {
            Color *pixels = LoadImageColors(shot);
            uint64_t hash = 14695981039346656037ull;
            for (int i = 0; i < shot.width * shot.height; ++i)
            {
                hash ^= (uint32_t(pixels[i].r) << 16) | (uint32_t(pixels[i].g) << 8) | uint32_t(pixels[i].b);
                hash *= 1099511628211ull;
            }
            UnloadImageColors(pixels);
            return hash;
        };
        auto nearColor = [](Color pixel, Color expected)
        {
            return std::abs(int(pixel.r) - int(expected.r)) <= 2 && std::abs(int(pixel.g) - int(expected.g)) <= 2
                && std::abs(int(pixel.b) - int(expected.b)) <= 2;
        };
        const Image baseline = render(false, false, 1);
        const Image upscaled = render(true, false, 1);
        const Image flipped = render(true, true, 1);
        const Image integer = render(false, false, 2);
        const Image integerFsr = render(true, false, 2);
        const bool fsrChangesPixels = hashPixels(baseline) != hashPixels(upscaled);
        const bool fsrPreservesInteger = hashPixels(integer) == hashPixels(integerFsr);
        const bool fsrOrientation = nearColor(GetImageColor(upscaled, 10, 10), RED)
            && nearColor(GetImageColor(flipped, 10, 10), BLUE);
        const bool fsrCrop = nearColor(GetImageColor(upscaled, upscaled.width - 1, upscaled.height - 1), GREEN)
            && nearColor(GetImageColor(flipped, flipped.width - 1, flipped.height - 1), GREEN);
        const bool loaded = desktop_video::loadShader(invert.string().c_str(), error);
        const Image combined = render(true, false, 1);
        const bool fsrWithShader = loaded && nearColor(GetImageColor(combined, 10, 10), {25, 214, 200, 255});
        for (Image shot : {baseline, upscaled, flipped, integer, integerFsr, combined}) UnloadImage(shot);
        UnloadTexture(fsrTexture);

        report << "],\"window_presets\":" << (selectedWindow && preservedFullscreen && restoredPreset ? "true" : "false")
               << ",\"fsr_changes_pixels\":" << (fsrChangesPixels ? "true" : "false")
               << ",\"fsr_preserves_integer\":" << (fsrPreservesInteger ? "true" : "false")
               << ",\"fsr_orientation\":" << (fsrOrientation ? "true" : "false")
               << ",\"fsr_crop\":" << (fsrCrop ? "true" : "false")
               << ",\"fsr_with_shader\":" << (fsrWithShader ? "true" : "false")
               << ",\"valid_shader\":" << (valid ? "true" : "false")
               << ",\"invalid_rejected\":" << (rejected ? "true" : "false")
               << ",\"active_preserved\":" << (retained ? "true" : "false") << "}\n";
        if (!valid || !rejected || !retained || !selectedWindow || !preservedFullscreen || !restoredPreset
            || !fsrChangesPixels || !fsrPreservesInteger || !fsrOrientation || !fsrCrop || !fsrWithShader)
            code = 4;
        UnloadTexture(texture);
    }
    desktop_video::shutdown(); CloseWindow();
    return code;
}
