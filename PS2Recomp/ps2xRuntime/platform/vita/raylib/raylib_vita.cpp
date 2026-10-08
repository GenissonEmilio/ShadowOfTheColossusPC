// raylib subset for the PS Vita on SDL2 (video through GXM, audio callback)
// and the Vita system libraries (controls). See raylib.h.

#include "raylib.h"

#include <SDL2/SDL.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace
{
    SDL_Window *g_window = nullptr;
    SDL_Renderer *g_renderer = nullptr;
    bool g_quit = false;
    int g_width = 960, g_height = 544;
    uint64_t g_startCounter = 0;

    struct TextureSlot
    {
        SDL_Texture *texture = nullptr;
        std::vector<uint8_t> pixels; // last upload, for LoadImageFromTexture
    };
    std::map<unsigned int, TextureSlot> g_textures;
    unsigned int g_nextTexture = 1;

    // FPS over the last second of EndDrawing() calls.
    int g_fps = 0;
    int g_framesThisSecond = 0;
    double g_fpsWindowStart = 0.0;

    // Controls: ports 0 (Vita / PS TV virtual) and 1 (PS TV first DS3/DS4) merged.
    SceCtrlData g_pad{};
    bool g_rearLeft = false, g_rearRight = false;

    // Audio
    SDL_AudioDeviceID g_audioDevice = 0;
    int g_audioBufferFrames = 1024;
    std::atomic<AudioCallback> g_audioCallback{nullptr};

    void pollInput()
    {
        SceCtrlData merged{};
        merged.lx = merged.ly = merged.rx = merged.ry = 128;
        for (int port = 0; port <= 1; ++port)
        {
            SceCtrlData data{};
            if (sceCtrlPeekBufferPositiveExt2(port, &data, 1) < 0)
                continue;
            merged.buttons |= data.buttons;
            auto pick = [](unsigned char current, unsigned char value)
            {
                return std::abs(int(value) - 128) > std::abs(int(current) - 128) ? value : current;
            };
            merged.lx = pick(merged.lx, data.lx);
            merged.ly = pick(merged.ly, data.ly);
            merged.rx = pick(merged.rx, data.rx);
            merged.ry = pick(merged.ry, data.ry);
        }
        g_pad = merged;

        // Vita 1000/2000 have no L2/R2: the rear touchpad's left and right
        // halves stand in for them.
        SceTouchData touch{};
        g_rearLeft = g_rearRight = false;
        if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch, 1) > 0)
        {
            for (unsigned i = 0; i < touch.reportNum; ++i)
            {
                if (touch.report[i].x < 960)
                    g_rearLeft = true;
                else
                    g_rearRight = true;
            }
        }
    }

    void audioThunk(void *, Uint8 *stream, int bytes)
    {
        const unsigned int frames = static_cast<unsigned int>(bytes) / 4u; // s16 stereo
        if (AudioCallback callback = g_audioCallback.load(std::memory_order_acquire))
            callback(stream, frames);
        else
            std::memset(stream, 0, static_cast<size_t>(bytes));
    }
}

void InitWindow(int width, int height, const char *title)
{
    g_width = width;
    g_height = height;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0)
    {
        std::fprintf(stderr, "[vita] SDL_Init failed: %s\n", SDL_GetError());
        return;
    }
    g_window = SDL_CreateWindow(title ? title : "", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, 0);
    g_renderer = g_window ? SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!g_renderer)
        std::fprintf(stderr, "[vita] SDL renderer failed: %s\n", SDL_GetError());
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    g_startCounter = SDL_GetPerformanceCounter();
}

void CloseWindow(void)
{
    for (auto &[id, slot] : g_textures)
        if (slot.texture)
            SDL_DestroyTexture(slot.texture);
    g_textures.clear();
    if (g_renderer)
        SDL_DestroyRenderer(g_renderer);
    if (g_window)
        SDL_DestroyWindow(g_window);
    g_renderer = nullptr;
    g_window = nullptr;
    SDL_Quit();
}

bool WindowShouldClose(void) { return g_quit; }
bool IsWindowReady(void) { return g_renderer != nullptr; }
void SetConfigFlags(unsigned int) {}
void SetTargetFPS(int) {} // presentation follows the display's vsync
int GetFPS(void) { return g_fps; }

double GetTime(void)
{
    if (g_startCounter == 0)
        g_startCounter = SDL_GetPerformanceCounter();
    return static_cast<double>(SDL_GetPerformanceCounter() - g_startCounter) /
           static_cast<double>(SDL_GetPerformanceFrequency());
}

int GetScreenWidth(void) { return g_width; }
int GetScreenHeight(void) { return g_height; }

void BeginDrawing(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
        if (event.type == SDL_QUIT)
            g_quit = true;
    pollInput();
}

void EndDrawing(void)
{
    if (g_renderer)
        SDL_RenderPresent(g_renderer);
    // Logs go to files on ux0: keep them current in case the app is killed.
    std::fflush(stdout);
    std::fflush(stderr);
    ++g_framesThisSecond;
    const double now = GetTime();
    if (now - g_fpsWindowStart >= 1.0)
    {
        g_fps = g_framesThisSecond;
        g_framesThisSecond = 0;
        g_fpsWindowStart = now;
    }
}

void ClearBackground(Color color)
{
    if (!g_renderer)
        return;
    SDL_SetRenderDrawColor(g_renderer, color.r, color.g, color.b, color.a);
    SDL_RenderClear(g_renderer);
}

void DrawTexturePro(Texture2D texture, Rectangle source, Rectangle dest, Vector2, float, Color)
{
    const auto it = g_textures.find(texture.id);
    if (!g_renderer || it == g_textures.end() || !it->second.texture)
        return;
    const SDL_Rect src{static_cast<int>(source.x), static_cast<int>(source.y),
                       static_cast<int>(source.width), static_cast<int>(source.height)};
    const SDL_Rect dst{static_cast<int>(dest.x), static_cast<int>(dest.y),
                       static_cast<int>(dest.width), static_cast<int>(dest.height)};
    SDL_RenderCopy(g_renderer, it->second.texture, &src, &dst);
}

void DrawRectangle(int posX, int posY, int width, int height, Color color)
{
    if (!g_renderer)
        return;
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_renderer, color.r, color.g, color.b, color.a);
    const SDL_Rect rect{posX, posY, width, height};
    SDL_RenderFillRect(g_renderer, &rect);
}

void DrawText(const char *, int, int, int, Color) {} // no font on the Vita build
int MeasureText(const char *text, int fontSize) { return text ? static_cast<int>(std::strlen(text)) * fontSize / 2 : 0; }

Image GenImageColor(int width, int height, Color color)
{
    Image image{};
    image.width = width;
    image.height = height;
    image.mipmaps = 1;
    image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    auto *pixels = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(width) * height * 4u));
    for (int i = 0; pixels && i < width * height; ++i)
        std::memcpy(pixels + i * 4, &color, 4);
    image.data = pixels;
    return image;
}

void ImageCrop(Image *image, Rectangle crop)
{
    if (!image || !image->data)
        return;
    const int x = std::clamp(static_cast<int>(crop.x), 0, image->width);
    const int y = std::clamp(static_cast<int>(crop.y), 0, image->height);
    const int w = std::clamp(static_cast<int>(crop.width), 0, image->width - x);
    const int h = std::clamp(static_cast<int>(crop.height), 0, image->height - y);
    auto *out = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(w) * h * 4u));
    const auto *in = static_cast<const uint8_t *>(image->data);
    for (int row = 0; out && row < h; ++row)
        std::memcpy(out + row * w * 4, in + ((y + row) * image->width + x) * 4, static_cast<size_t>(w) * 4u);
    std::free(image->data);
    image->data = out;
    image->width = w;
    image->height = h;
}

void UnloadImage(Image image) { std::free(image.data); }
bool ExportImage(Image, const char *) { return false; }

Texture2D LoadTextureFromImage(Image image)
{
    Texture2D texture{};
    if (!g_renderer)
        return texture;
    TextureSlot slot;
    slot.texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, image.width, image.height);
    if (!slot.texture)
        return texture;
    texture.id = g_nextTexture++;
    texture.width = image.width;
    texture.height = image.height;
    texture.mipmaps = 1;
    texture.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    g_textures[texture.id] = std::move(slot);
    if (image.data)
        UpdateTexture(texture, image.data);
    return texture;
}

Image LoadImageFromTexture(Texture2D texture)
{
    Image image = GenImageColor(texture.width, texture.height, BLANK);
    const auto it = g_textures.find(texture.id);
    if (it != g_textures.end() && image.data && it->second.pixels.size() == static_cast<size_t>(texture.width) * texture.height * 4u)
        std::memcpy(image.data, it->second.pixels.data(), it->second.pixels.size());
    return image;
}

void UpdateTexture(Texture2D texture, const void *pixels)
{
    const auto it = g_textures.find(texture.id);
    if (it == g_textures.end() || !it->second.texture || !pixels)
        return;
    SDL_UpdateTexture(it->second.texture, nullptr, pixels, texture.width * 4);
}

void UnloadTexture(Texture2D texture)
{
    const auto it = g_textures.find(texture.id);
    if (it == g_textures.end())
        return;
    if (it->second.texture)
        SDL_DestroyTexture(it->second.texture);
    g_textures.erase(it);
}

bool IsKeyDown(int) { return false; }
bool IsKeyPressed(int) { return false; }
bool IsGamepadAvailable(int gamepad) { return gamepad == 0; }

bool IsGamepadButtonDown(int gamepad, int button)
{
    if (gamepad != 0)
        return false;
    const unsigned int b = g_pad.buttons;
    switch (button)
    {
    case GAMEPAD_BUTTON_LEFT_FACE_UP: return b & SCE_CTRL_UP;
    case GAMEPAD_BUTTON_LEFT_FACE_RIGHT: return b & SCE_CTRL_RIGHT;
    case GAMEPAD_BUTTON_LEFT_FACE_DOWN: return b & SCE_CTRL_DOWN;
    case GAMEPAD_BUTTON_LEFT_FACE_LEFT: return b & SCE_CTRL_LEFT;
    case GAMEPAD_BUTTON_RIGHT_FACE_UP: return b & SCE_CTRL_TRIANGLE;
    case GAMEPAD_BUTTON_RIGHT_FACE_RIGHT: return b & SCE_CTRL_CIRCLE;
    case GAMEPAD_BUTTON_RIGHT_FACE_DOWN: return b & SCE_CTRL_CROSS;
    case GAMEPAD_BUTTON_RIGHT_FACE_LEFT: return b & SCE_CTRL_SQUARE;
    case GAMEPAD_BUTTON_LEFT_TRIGGER_1: return b & SCE_CTRL_L1;
    case GAMEPAD_BUTTON_RIGHT_TRIGGER_1: return b & SCE_CTRL_R1;
    case GAMEPAD_BUTTON_LEFT_TRIGGER_2: return (b & SCE_CTRL_L2) || g_rearLeft;
    case GAMEPAD_BUTTON_RIGHT_TRIGGER_2: return (b & SCE_CTRL_R2) || g_rearRight;
    case GAMEPAD_BUTTON_MIDDLE_LEFT: return b & SCE_CTRL_SELECT;
    case GAMEPAD_BUTTON_MIDDLE_RIGHT: return b & SCE_CTRL_START;
    case GAMEPAD_BUTTON_LEFT_THUMB: return b & SCE_CTRL_L3;
    case GAMEPAD_BUTTON_RIGHT_THUMB: return b & SCE_CTRL_R3;
    default: return false;
    }
}

float GetGamepadAxisMovement(int gamepad, int axis)
{
    if (gamepad != 0)
        return 0.0f;
    auto norm = [](unsigned char v)
    {
        const float f = (static_cast<float>(v) - 128.0f) / 127.0f;
        return std::clamp(f, -1.0f, 1.0f);
    };
    switch (axis)
    {
    case GAMEPAD_AXIS_LEFT_X: return norm(g_pad.lx);
    case GAMEPAD_AXIS_LEFT_Y: return norm(g_pad.ly);
    case GAMEPAD_AXIS_RIGHT_X: return norm(g_pad.rx);
    case GAMEPAD_AXIS_RIGHT_Y: return norm(g_pad.ry);
    default: return 0.0f;
    }
}

void InitAudioDevice(void)
{
    if (g_audioDevice != 0)
        return;
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = static_cast<Uint16>(g_audioBufferFrames);
    want.callback = audioThunk;
    g_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audioDevice == 0)
        std::fprintf(stderr, "[vita] SDL audio failed: %s\n", SDL_GetError());
}

void CloseAudioDevice(void)
{
    if (g_audioDevice != 0)
        SDL_CloseAudioDevice(g_audioDevice);
    g_audioDevice = 0;
}

bool IsAudioDeviceReady(void) { return g_audioDevice != 0; }
void SetAudioStreamBufferSizeDefault(int size) { g_audioBufferFrames = std::max(256, size); }

AudioStream LoadAudioStream(unsigned int sampleRate, unsigned int sampleSize, unsigned int channels)
{
    AudioStream stream{};
    stream.sampleRate = sampleRate;
    stream.sampleSize = sampleSize;
    stream.channels = channels;
    return stream;
}

void SetAudioStreamCallback(AudioStream, AudioCallback callback) { g_audioCallback.store(callback, std::memory_order_release); }

void PlayAudioStream(AudioStream)
{
    if (g_audioDevice != 0)
        SDL_PauseAudioDevice(g_audioDevice, 0);
}
