// ROM-free GPU shader validator and presentation integration test.
#include "runtime/desktop_video.h"
#include "raylib.h"
#include "rlgl.h"
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
                    ExportImage(shot, (out / (std::string(name) + ".png")).c_str());
                    UnloadImage(shot);
                }
                EndDrawing();
            }
            const Rectangle r = desktop_video::destination(GetScreenWidth(), GetScreenHeight(), 320, 240, mode);
            report << "{\"name\":" << quoted(name) << ",\"fullscreen\":" << (desktop_video::isFullscreen() ? "true" : "false")
                   << ",\"width\":" << GetRenderWidth() << ",\"height\":" << GetRenderHeight()
                   << ",\"x\":" << r.x << ",\"y\":" << r.y << ",\"picture_width\":" << r.width
                   << ",\"picture_height\":" << r.height
                   << ",\"edge\":[" << int(edge.r) << ',' << int(edge.g) << ',' << int(edge.b) << ']'
                   << ",\"body\":[" << int(body.r) << ',' << int(body.g) << ',' << int(body.b) << "]}";
        };
        capture("window-fit", 0); report << ',';
        desktop_video::setFullscreen(true);
        capture("fullscreen-fit", 0); report << ',';
        capture("fullscreen-stretch", 1); report << ',';
        const auto invert = out / "invert.frag";
        std::ofstream(invert) << "#version 330 core\nin vec2 fragTexCoord; uniform sampler2D texture0; out vec4 finalColor;"
                               "void main(){finalColor=vec4(vec3(1.0)-texture(texture0,fragTexCoord).rgb,1.0);}";
        const bool valid = desktop_video::loadShader(invert.c_str(), error);
        capture("fullscreen-shader", 1); report << ',';
        const auto invalid = out / "invalid.frag";
        std::ofstream(invalid) << "#version 330 core\nvoid main(){ bad syntax }";
        const bool rejected = !desktop_video::loadShader(invalid.c_str(), error);
        const bool retained = desktop_video::shaderActive();
        capture("invalid-keeps-shader", 1); report << ',';
        desktop_video::loadShader(nullptr, error);
        capture("shader-disabled", 1); report << ',';
        desktop_video::setFullscreen(false);
        capture("window-restored", 0);
        report << "],\"valid_shader\":" << (valid ? "true" : "false")
               << ",\"invalid_rejected\":" << (rejected ? "true" : "false")
               << ",\"active_preserved\":" << (retained ? "true" : "false") << "}\n";
        if (!valid || !rejected || !retained) code = 4;
        UnloadTexture(texture);
    }
    desktop_video::shutdown(); CloseWindow();
    return code;
}
