#include "runtime/desktop_input.h"
#if defined(__linux__) && !defined(__ANDROID__)
#include "raylib.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace desktop_input
{
namespace {
    // Positive codes are raylib/GLFW keys; -(button+1) denotes a mouse button.
    struct Binding { const char *name; int code; uint16_t bit; };
    Binding bindings[] = {
        {"move_up", KEY_W, 0}, {"move_down", KEY_S, 0},
        {"move_left", KEY_A, 0}, {"move_right", KEY_D, 0},
        {"jump", KEY_SPACE, 0x4000}, {"attack", -1, 0x8000},
        {"grab", -2, 0x0800}, {"action", -3, 0x2000},
        {"horse", KEY_C, 0x1000}, {"weapon_left", KEY_Q, 0x0080},
        {"weapon_right", KEY_E, 0x0020}, {"look", KEY_F, 0x0400},
        {"camera_reset", KEY_R, 0x0100}, {"crouch", KEY_LEFT_SHIFT, 0x0200},
        {"start", KEY_ESCAPE, 0x0008}, {"select", KEY_I, 0x0001},
        {"dpad_up", KEY_UP, 0x0010}, {"dpad_down", KEY_DOWN, 0x0040},
        {"l3", KEY_LEFT_CONTROL, 0x0002}, {"r3", KEY_V, 0x0004},
        {"camera_left", KEY_LEFT, 0}, {"camera_right", KEY_RIGHT, 0},
        {"camera_up", KEY_PAGE_UP, 0}, {"camera_down", KEY_PAGE_DOWN, 0},
    };
    bool mouseCamera = true, invertX = false, invertY = false;
    float sensitivity = 1.0f;
    bool captured = false, waitingRelease = true;
    std::mutex mutex;
    State snapshot;
    using Clock = std::chrono::steady_clock;
    Clock::time_point sampledAt;

    bool validCode(int code) {
        return (code >= -7 && code <= 0) ||
            (code >= 32 && code <= 348 && code != KEY_F1 && code != KEY_F11);
    }
    bool down(int code) {
        return code < 0 ? IsMouseButtonDown(-code - 1) : code > 0 && IsKeyDown(code);
    }
    uint8_t axis(float value) {
        return static_cast<uint8_t>(std::clamp(128.0f + value * 127.0f, 0.0f, 255.0f));
    }
}

void initialize()
{
    // Esc belongs to the guest Start button. Quitting is an explicit F1 action.
    SetExitKey(KEY_NULL);
    const char *path = std::getenv("PS2X_DESKTOP_INPUT");
    if (!path) return;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream row(line);
        std::string key;
        float value;
        if (!std::getline(row, key, '=') || !(row >> value) || !std::isfinite(value)) continue;
        if (key == "mouse_camera") mouseCamera = value != 0;
        else if (key == "invert_x") invertX = value != 0;
        else if (key == "invert_y") invertY = value != 0;
        else if (key == "sensitivity") sensitivity = std::clamp(value, 0.1f, 5.0f);
        else if (value >= -7 && value <= 348 && value == std::floor(value)) {
            for (auto &binding : bindings)
                if (key == binding.name && validCode(static_cast<int>(value)))
                    binding.code = static_cast<int>(value);
        }
    }
}

void poll(bool menuBlocked)
{
    const bool active = IsWindowFocused() && !menuBlocked;
    const bool wantCapture = active && mouseCamera;
    const bool changed = captured != wantCapture;
    if (changed) {
        if (wantCapture) DisableCursor(); else EnableCursor();
        captured = wantCapture;
    }
    State next;
    if (!active) waitingRelease = true;
    else {
        bool held = IsKeyDown(KEY_F1);
        for (const auto &binding : bindings) held |= down(binding.code);
        if (waitingRelease && !held) waitingRelease = false;
        if (!waitingRelease) {
            for (const auto &binding : bindings)
                if (binding.bit && down(binding.code)) next.buttons |= binding.bit;
            next.lx = axis(float(down(bindings[3].code)) - float(down(bindings[2].code)));
            next.ly = axis(float(down(bindings[1].code)) - float(down(bindings[0].code)));
            float x = float(down(bindings[21].code)) - float(down(bindings[20].code));
            float y = float(down(bindings[23].code)) - float(down(bindings[22].code));
            if (captured && !changed) {
                const Vector2 delta = GetMouseDelta();
                // Convert relative motion to stick velocity, independent of host FPS.
                const float scale = sensitivity / (std::clamp(GetFrameTime(), 0.001f, 0.1f) * 600.0f);
                if (delta.x != 0) x = delta.x * scale * (invertX ? -1 : 1);
                if (delta.y != 0) y = delta.y * scale * (invertY ? -1 : 1);
            }
            next.rx = axis(x); next.ry = axis(y);
        }
    }
    std::lock_guard<std::mutex> lock(mutex);
    snapshot = next;
    sampledAt = Clock::now();
}

State read()
{
    std::lock_guard<std::mutex> lock(mutex);
    // Do not leave a button/stick latched if the window thread stalls.
    return Clock::now() - sampledAt < std::chrono::milliseconds(250) ? snapshot : State{};
}
}
#else
namespace desktop_input {
void initialize() {}
void poll(bool) {}
State read() { return {}; }
}
#endif
