#include "runtime/desktop_input.h"
#include <cstdlib>
#include <optional>
#include <string_view>

namespace desktop_input {
namespace {
    std::optional<PromptStyle> manualStyle()
    {
        const auto *value = std::getenv("SOTC_BUTTON_PROMPTS");
        const std::string_view mode = value ? value : "auto";
        if (mode == "playstation") return PromptStyle::PlayStation;
        if (mode == "xbox") return PromptStyle::Xbox;
        if (mode == "keyboard") return PromptStyle::Keyboard;
        return std::nullopt;
    }
}

uint64_t PromptProfile::variant() const
{
    if (style == PromptStyle::PlayStation) return 0;
    if (style == PromptStyle::Xbox) return 1;
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const int code : codes)
        hash = (hash ^ static_cast<uint32_t>(code)) * 0x100000001b3ull;
    return (hash << 2) | 2;
}
}

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
#include "raylib.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace desktop_input
{
namespace {
    // Positive codes are raylib/GLFW keys; -(button+1) denotes a mouse button.
    struct Binding { const char *name; int code; uint16_t bit; };
    constexpr Binding defaultBindings[] = {
        {"move_up", KEY_W, 0}, {"move_down", KEY_S, 0},
        {"move_left", KEY_A, 0}, {"move_right", KEY_D, 0},
        {"jump", KEY_SPACE, 0x1000}, {"attack", -1, 0x8000},
        {"grab", -2, 0x0800}, {"action", -3, 0x2000},
        {"horse", KEY_C, 0x4000}, {"weapon_left", KEY_Q, 0x0080},
        {"weapon_right", KEY_E, 0x0020}, {"look", KEY_F, 0x0400},
        {"camera_reset", KEY_R, 0x0100}, {"crouch", KEY_LEFT_SHIFT, 0x0200},
        {"start", KEY_ESCAPE, 0x0008}, {"select", KEY_I, 0x0001},
        {"dpad_up", KEY_UP, 0x0010}, {"dpad_down", KEY_DOWN, 0x0040},
        {"l3", KEY_LEFT_CONTROL, 0x0002}, {"r3", KEY_V, 0x0004},
        {"camera_left", KEY_LEFT, 0}, {"camera_right", KEY_RIGHT, 0},
        {"camera_up", KEY_PAGE_UP, 0}, {"camera_down", KEY_PAGE_DOWN, 0},
        {"confirm", KEY_ENTER, 0x4000}, {"cancel", KEY_BACKSPACE, 0x2000},
    };
    static_assert(std::size(defaultBindings) == static_cast<size_t>(Control::Count));
    std::array<Binding, std::size(defaultBindings)> bindings;
    std::array<bool, bindings.size()> previous{};
    bool mouseCamera = true, invertX = false, invertY = false;
    float sensitivity = 1.0f;
    bool captured = false, waitingRelease = true;
    std::mutex mutex;
    State snapshot;
    PromptProfile promptSnapshot;
    PromptStyle lastStyle = PromptStyle::PlayStation, pendingPadStyle = PromptStyle::Xbox;
    bool automaticPrompts = true, pendingPad = false;
    std::atomic<bool> resetPending{false};
    using Clock = std::chrono::steady_clock;
    Clock::time_point sampledAt;

    bool validCode(int code) {
        return (code >= -7 && code <= 0) ||
            (code >= 32 && code <= 348 && code != KEY_F1 && code != KEY_F5 && code != KEY_F8 && code != KEY_F9 && code != KEY_F11);
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
    std::copy(std::begin(defaultBindings), std::end(defaultBindings), bindings.begin());
    previous = {};
    waitingRelease = true;
    mouseCamera = true; invertX = invertY = false; sensitivity = 1.0f;
    lastStyle = PromptStyle::PlayStation;
    pendingPad = false;
    const auto style = manualStyle();
    automaticPrompts = !style;
    const char *path = std::getenv("PS2X_DESKTOP_INPUT");
    std::ifstream file;
    if (path) file.open(path);
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
    std::lock_guard<std::mutex> lock(mutex);
    promptSnapshot.style = style.value_or(lastStyle);
    for (size_t i = 0; i < bindings.size(); ++i)
        promptSnapshot.codes[i] = bindings[i].code;
}

void gamepadActivity(PromptStyle style)
{
    pendingPad = true;
    pendingPadStyle = style;
}

void poll(bool menuBlocked)
{
    const bool reset = resetPending.exchange(false);
    const bool padActivity = pendingPad;
    pendingPad = false;
    if (reset) waitingRelease = true;
    const bool active = IsWindowFocused() && !menuBlocked && !reset;
    const bool wantCapture = active && mouseCamera;
    const bool changed = captured != wantCapture;
    if (changed) {
        if (wantCapture) DisableCursor(); else EnableCursor();
        captured = wantCapture;
    }
    State next;
    bool desktopActivity = false;
    if (!active) waitingRelease = true;
    else {
        bool held = IsKeyDown(KEY_F1) || IsKeyDown(KEY_F5) || IsKeyDown(KEY_F8);
        std::array<bool, bindings.size()> current{};
        for (size_t i = 0; i < bindings.size(); ++i) {
            current[i] = down(bindings[i].code);
            held |= current[i];
        }
        if (waitingRelease && !held) waitingRelease = false;
        if (!waitingRelease) {
            for (size_t i = 0; i < bindings.size(); ++i) {
                if (current[i]) next.buttons |= bindings[i].bit;
                desktopActivity |= current[i] && !previous[i];
            }
            const auto key = [&](Control control) { return float(current[static_cast<size_t>(control)]); };
            next.lx = axis(key(Control::MoveRight) - key(Control::MoveLeft));
            next.ly = axis(key(Control::MoveDown) - key(Control::MoveUp));
            float x = key(Control::CameraRight) - key(Control::CameraLeft);
            float y = key(Control::CameraDown) - key(Control::CameraUp);
            if (captured && !changed) {
                const Vector2 delta = GetMouseDelta();
                // Convert relative motion to stick velocity, independent of host FPS.
                const float scale = sensitivity / (std::clamp(GetFrameTime(), 0.001f, 0.1f) * 600.0f);
                if (delta.x != 0) x = delta.x * scale * (invertX ? -1 : 1);
                if (delta.y != 0) y = delta.y * scale * (invertY ? -1 : 1);
                // Subpixel motion and the capture transition leave prompts alone.
                desktopActivity |= delta.x * delta.x + delta.y * delta.y >= 4.0f;
            }
            next.rx = axis(x); next.ry = axis(y);
        }
        previous = current;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (resetPending.load()) return;
    snapshot = next;
    sampledAt = Clock::now();
    if (automaticPrompts && active) {
        // When both arrive during one host poll, a deliberate pad input wins
        // over a mouse delta sampled in that same interval.
        if (padActivity) lastStyle = pendingPadStyle;
        else if (desktopActivity) lastStyle = PromptStyle::Keyboard;
        promptSnapshot.style = lastStyle;
    }
}

void resetAfterStateLoad()
{
    std::lock_guard<std::mutex> lock(mutex);
    snapshot = {}; sampledAt = Clock::now(); resetPending.store(true);
}

State read()
{
    std::lock_guard<std::mutex> lock(mutex);
    // Do not leave a button/stick latched if the window thread stalls.
    return Clock::now() - sampledAt < std::chrono::milliseconds(250) ? snapshot : State{};
}

PromptProfile prompts()
{
    std::lock_guard<std::mutex> lock(mutex);
    return promptSnapshot;
}
}
#else
namespace desktop_input {
void initialize() {}
void poll(bool) {}
void gamepadActivity(PromptStyle) {}
State read() { return {}; }
PromptProfile prompts()
{
    static const PromptProfile profile{manualStyle().value_or(PromptStyle::PlayStation)};
    return profile;
}
void resetAfterStateLoad() {}
}
#endif
