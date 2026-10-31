// Deterministic host events -> actual PS2 packets, including guest-thread reads.
#include "raylib.h"
#include <cassert>
#include <thread>
#include <chrono>
#include <cstring>
#include <fstream>
#include <cstdio>
#include "../../PS2Recomp/ps2xRuntime/src/lib/desktop_input.cpp"
#include "../../PS2Recomp/ps2xRuntime/src/lib/ps2_pad.cpp"

namespace fixture {
    bool keys[512]{}, mouse[8]{}, focused = true, menu = false, cursor = false;
    Vector2 delta{};
    float dt = 1.0f/60.0f;
    int exitKey = KEY_ESCAPE;
    const auto host = std::this_thread::get_id();
    void onHost() { assert(std::this_thread::get_id() == host); }
    void clear() { std::memset(keys, 0, sizeof(keys)); std::memset(mouse, 0, sizeof(mouse)); delta = {}; }
    void poll() { desktop_input::poll(menu); }
    void neutral() { clear(); poll(); }
    uint16_t packet(uint8_t (&data)[32]) {
        std::thread guest([&] { PSPadBackend pad; assert(pad.readState(0,0,data,32)); });
        guest.join();
        return ~(uint16_t(data[2]) | uint16_t(data[3])<<8);
    }
}
extern "C" {
bool IsKeyDown(int key) { fixture::onHost(); return fixture::keys[key]; }
bool IsMouseButtonDown(int button) { fixture::onHost(); return fixture::mouse[button]; }
bool IsWindowFocused() { fixture::onHost(); return fixture::focused; }
void SetExitKey(int key) { fixture::exitKey = key; }
void DisableCursor() { fixture::onHost(); fixture::cursor = true; }
void EnableCursor() { fixture::onHost(); fixture::cursor = false; }
Vector2 GetMouseDelta() { fixture::onHost(); return fixture::delta; }
float GetFrameTime() { fixture::onHost(); return fixture::dt; }
double GetTime() { return 0; }
}
namespace hostmenu { bool inputBlocked() { return fixture::menu; } }

int main(int argc, char **argv)
{
    using namespace fixture;
    assert(argc == 2);
    unsetenv("PS2X_INPUT_SCRIPT");
    unsetenv("PS2X_DESKTOP_INPUT");
    desktop_input::initialize();
    assert(exitKey == KEY_NULL); // Escape no longer closes the window.
    neutral();
    assert(cursor);
    uint8_t data[32];
    for (const auto &binding : desktop_input::bindings) {
        neutral();
        if (binding.code < 0) mouse[-binding.code-1] = true;
        else keys[binding.code] = true;
        poll();
        auto pressed = packet(data);
        if (binding.bit) assert(pressed == binding.bit);
        else assert(pressed == 0);
    }
    neutral(); keys[KEY_W] = keys[KEY_D] = keys[KEY_SPACE] = true;
    mouse[0] = mouse[1] = true; poll();
    assert(packet(data) == (0x4000|0x8000|0x0800));
    assert(data[6] == 255 && data[7] == 1);
    neutral(); delta = {5, -5}; poll(); packet(data);
    auto rx = data[4], ry = data[5];
    assert(rx > 180 && ry < 80);
    dt = 1.0f/120.0f; delta = {2.5f,-2.5f}; poll(); packet(data);
    assert(data[4] == rx && data[5] == ry); // equal velocity at 60 / 120 Hz
    neutral(); packet(data); assert(data[4] == 128 && data[5] == 128);
    // F1 releases the cursor and blocks attacks; held input cannot leak on close.
    mouse[0] = true; menu = true; poll(); assert(!cursor);
    assert(packet(data) == 0);
    menu = false; poll(); assert(cursor); assert(packet(data) == 0);
    neutral(); mouse[0] = true; poll(); assert(packet(data) == 0x8000);
    focused = false; poll(); assert(!cursor); assert(packet(data) == 0);
    focused = true; poll(); assert(packet(data) == 0);
    neutral();
    // Real user configuration replaces defaults for both keyboard and mouse.
    std::ofstream(argv[1]) << "attack=66\ngrab=-4\nstart=257\nsensitivity=2\ninvert_x=1\ninvert_y=1\nmove_up=84\n";
    setenv("PS2X_DESKTOP_INPUT", argv[1], 1);
    desktop_input::initialize();
    neutral(); mouse[0] = true; keys[KEY_W] = keys[KEY_ESCAPE] = true; poll();
    assert(packet(data) == 0 && data[7] == 128);
    neutral(); keys[KEY_B] = keys[KEY_T] = keys[KEY_ENTER] = true; mouse[3] = true; poll();
    assert(packet(data) == (0x8000|0x0800|0x0008) && data[7] == 1);
    neutral(); delta = {3,-3}; poll(); packet(data);
    assert(data[4] < 60 && data[5] > 195);
    // Physical pad remains usable alongside the desktop profile.
    neutral();
    { std::lock_guard<std::mutex> lock(gamepadMutex);
      gamepads[2].index = 2;
      gamepads[2].buttons[GAMEPAD_BUTTON_RIGHT_FACE_UP] = true;
      gamepads[2].axes[GAMEPAD_AXIS_RIGHT_X] = 1; }
    assert(packet(data) == 0x1000 && data[4] == 255);
    { std::lock_guard<std::mutex> lock(gamepadMutex); gamepads = {}; }
    keys[KEY_B] = true; poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(260));
    assert(packet(data) == 0 && data[4] == 128);
    std::puts("PASS: desktop packet mappings, mouse velocity, focus/menu, remap, gamepad and thread ownership");
}
