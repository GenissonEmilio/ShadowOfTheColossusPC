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
#include "../../game/overrides/sotc_button_prompts.h"

namespace fixture {
    bool keys[512]{}, mouse[8]{}, focused = true, menu = false, cursor = false;
    Vector2 delta{};
    float dt = 1.0f/60.0f;
    ps2pad::GamepadState physical[4];
    const char *padNames[4]{"Controller", "Wireless Controller", "Sony virtual controller", "Controller"};
    const char *padGuids[4]{"", "050000004c050000e60c000000010000", "030000005e0400008e02000014010000", ""};
    const char *mappingNames[4]{"Generic pad", "Generic pad", "Sony DualShock 4", "Sony DualShock 3"};
    int exitKey = KEY_ESCAPE;
    const auto host = std::this_thread::get_id();
    void onHost() { assert(std::this_thread::get_id() == host); }
    void clear() { std::memset(keys, 0, sizeof(keys)); std::memset(mouse, 0, sizeof(mouse)); delta = {}; }
    void poll() { ps2pad::pollGamepads(); desktop_input::poll(menu); }
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
bool IsWindowReady() { fixture::onHost(); return true; }
bool IsGamepadAvailable(int slot) { fixture::onHost(); return fixture::physical[slot].index >= 0; }
bool IsGamepadButtonDown(int slot, int button) { fixture::onHost(); return fixture::physical[slot].buttons[button]; }
float GetGamepadAxisMovement(int slot, int axis) { fixture::onHost(); return fixture::physical[slot].axes[axis]; }
int SetGamepadMappings(const char *) { return 0; }
const char *GetGamepadName(int slot) { fixture::onHost(); return fixture::padNames[slot]; }
int glfwJoystickIsGamepad(int) { fixture::onHost(); return 1; }
const char *glfwGetGamepadName(int slot) { fixture::onHost(); return fixture::mappingNames[slot]; }
const char *glfwGetJoystickGUID(int slot) { fixture::onHost(); return fixture::padGuids[slot]; }
void SetExitKey(int key) { fixture::exitKey = key; }
void DisableCursor() { fixture::onHost(); fixture::cursor = true; }
void EnableCursor() { fixture::onHost(); fixture::cursor = false; }
Vector2 GetMouseDelta() { fixture::onHost(); return fixture::delta; }
float GetFrameTime() { fixture::onHost(); return fixture::dt; }
double GetTime() { return 0; }
}
namespace hostmenu { bool inputBlocked() { return fixture::menu; } }

void testPromptAtlas()
{
    using namespace desktop_input;
    auto profile = prompts();
    profile.style = PromptStyle::Keyboard;
    assert(profile.code(Control::Attack) == KEY_B);
    assert(profile.code(Control::Grab) == -4);
    assert(profile.code(Control::Start) == KEY_Y);

    constexpr uint64_t sourceId = 0x0400000001123456ull;
    const auto keyboardId = sotc_button_prompts::textureKey(sourceId, profile.variant());
    const auto fadedId = sotc_button_prompts::textureKey(sourceId ^ 31, profile.variant());
    const auto xboxId = sotc_button_prompts::textureKey(sourceId, 1);
    assert(keyboardId != sourceId && keyboardId != xboxId && keyboardId != fadedId);
    assert(sotc_button_prompts::textureKey(sourceId, 0) == sourceId);
    assert(sotc_button_prompts::textureKey(sourceId, profile.variant()) == keyboardId);

    std::array<uint8_t, 128 * 128 * 4> source;
    source.fill(23);
    auto painted = source;
    sotc_button_prompts::apply(painted.data(), painted.size(), profile);
    assert(painted != source);
    assert(std::equal(painted.begin() + (128 * 80 * 4), painted.end(), source.begin() + (128 * 80 * 4)));
    for (size_t i = 3; i < painted.size(); i += 4) assert(painted[i] == 23 || painted[i] == 0);
    // A remap changes the attack cell and its cache identity; other cells round-trip.
    profile.codes[static_cast<size_t>(Control::Attack)] = MOUSE_BUTTON_LEFT - 1;
    assert(sotc_button_prompts::textureKey(sourceId, profile.variant()) != keyboardId);
    auto remapped = source;
    sotc_button_prompts::apply(remapped.data(), remapped.size(), profile);
    assert(remapped != painted);
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x)
            if (x < 60 || x >= 79 || y < 20 || y >= 38)
                assert(std::equal(&remapped[(128 * y + x) * 4], &remapped[(128 * y + x) * 4] + 4, &painted[(128 * y + x) * 4]));
    profile.style = PromptStyle::PlayStation;
    sotc_button_prompts::apply(remapped.data(), remapped.size(), profile);
    assert(remapped != source);
    auto original = source;
    sotc_button_prompts::apply(original.data(), original.size(), profile);
    assert(original == source);
    profile.style = PromptStyle::Xbox;
    sotc_button_prompts::apply(original.data(), original.size(), profile);
    assert(original != source);
}

int main(int argc, char **argv)
{
    using namespace fixture;
    using desktop_input::PromptStyle;
    const auto prompt = [] { return desktop_input::prompts().style; };
    assert(argc == 2);
    unsetenv("PS2X_INPUT_SCRIPT");
    unsetenv("PS2X_DESKTOP_INPUT");
    unsetenv("SOTC_BUTTON_PROMPTS");
    desktop_input::initialize();
    assert(exitKey == KEY_NULL); // Escape no longer closes the window.
    neutral();
    assert(cursor);
    assert(prompt() == PromptStyle::PlayStation);
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
    assert(packet(data) == (0x1000|0x8000|0x0800));
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
    // Loading releases keyboard/mouse latches, including a held quickload key.
    mouse[0] = true; keys[KEY_F8] = true; poll();
    desktop_input::resetAfterStateLoad(); assert(packet(data) == 0);
    poll(); assert(packet(data) == 0); poll(); assert(packet(data) == 0);
    neutral(); mouse[0] = true; poll(); assert(packet(data) == 0x8000);
    for (int key : {KEY_F1, KEY_F5, KEY_F8, KEY_F9, KEY_F11}) assert(!desktop_input::validCode(key));
    neutral();
    // Real user configuration replaces defaults for both keyboard and mouse.
    std::ofstream(argv[1]) << "attack=66\ngrab=-4\nstart=89\nsensitivity=2\ninvert_x=1\ninvert_y=1\nmove_up=84\n";
    setenv("PS2X_DESKTOP_INPUT", argv[1], 1);
    desktop_input::initialize();
    neutral(); mouse[0] = true; keys[KEY_W] = keys[KEY_ESCAPE] = true; poll();
    assert(packet(data) == 0 && data[7] == 128);
    neutral(); keys[KEY_B] = keys[KEY_T] = keys[KEY_Y] = true; mouse[3] = true; poll();
    assert(packet(data) == (0x8000|0x0800|0x0008) && data[7] == 1);
    neutral(); delta = {3,-3}; poll(); packet(data);
    assert(data[4] < 60 && data[5] > 195);
    // Physical pad remains usable alongside the desktop profile.
    neutral();
    assert(prompt() == PromptStyle::Keyboard);
    testPromptAtlas();
    physical[0].index = 0;
    physical[0].axes[GAMEPAD_AXIS_LEFT_X] = 0.12f;
    physical[0].buttons[GAMEPAD_BUTTON_RIGHT_FACE_DOWN] = true;
    poll(); assert(prompt() == PromptStyle::Keyboard); // connect, even with a button held
    physical[0].buttons[GAMEPAD_BUTTON_RIGHT_FACE_DOWN] = false;
    poll(); assert(prompt() == PromptStyle::Keyboard); // release and drift
    physical[0].axes[GAMEPAD_AXIS_LEFT_X] = 0.7f;
    poll(); assert(prompt() == PromptStyle::Xbox); // positive intent on a generic pad
    delta = {0.45f, 0.0f}; poll(); assert(prompt() == PromptStyle::Xbox);
    delta = {3.0f, 0.0f}; poll(); assert(prompt() == PromptStyle::Keyboard);
    delta = {}; poll(); assert(prompt() == PromptStyle::Keyboard); // stick held, no new input
    physical[1].index = 1;
    poll(); assert(prompt() == PromptStyle::Keyboard);
    physical[1].buttons[GAMEPAD_BUTTON_RIGHT_FACE_UP] = true;
    poll(); assert(prompt() == PromptStyle::PlayStation); // real Sony USB/Bluetooth ID
    neutral(); keys[KEY_B] = true; poll(); assert(prompt() == PromptStyle::Keyboard);
    physical[1].buttons[GAMEPAD_BUTTON_RIGHT_FACE_UP] = false;
    physical[1].axes[GAMEPAD_AXIS_RIGHT_TRIGGER] = 0.3f;
    poll(); assert(prompt() == PromptStyle::PlayStation);
    neutral();
    physical[2].index = 2;
    poll(); assert(prompt() == PromptStyle::PlayStation);
    focused = false;
    physical[2].buttons[GAMEPAD_BUTTON_RIGHT_FACE_UP] = true;
    poll(); assert(prompt() == PromptStyle::PlayStation);
    focused = true;
    poll(); assert(prompt() == PromptStyle::PlayStation);
    physical[2].buttons[GAMEPAD_BUTTON_RIGHT_FACE_UP] = false;
    poll();
    physical[2].buttons[GAMEPAD_BUTTON_RIGHT_FACE_UP] = true;
    menu = true;
    poll(); assert(prompt() == PromptStyle::PlayStation);
    menu = false;
    poll(); assert(prompt() == PromptStyle::PlayStation);
    physical[2].buttons[GAMEPAD_BUTTON_RIGHT_FACE_UP] = false;
    poll();
    physical[2].buttons[GAMEPAD_BUTTON_RIGHT_FACE_UP] = true;
    poll(); assert(prompt() == PromptStyle::Xbox); // Xbox VID overrides the emulated pad's Sony label
    physical[2] = {};
    poll(); assert(prompt() == PromptStyle::Xbox); // disconnect retains the last active style
    assert(gamepadStyle(3) == PromptStyle::PlayStation); // identifiable mapping without a vendor ID
    padGuids[3] = "78696e70757401000000000000000000";
    assert(gamepadStyle(3) == PromptStyle::Xbox); // XInput GUID overrides the mapping name
    for (const auto fixed : {PromptStyle::PlayStation, PromptStyle::Xbox, PromptStyle::Keyboard}) {
        setenv("SOTC_BUTTON_PROMPTS", fixed == PromptStyle::PlayStation ? "playstation" : fixed == PromptStyle::Xbox ? "xbox" : "keyboard", 1);
        desktop_input::initialize();
        neutral();
        physical[1].buttons[GAMEPAD_BUTTON_RIGHT_FACE_UP] = true;
        poll(); assert(prompt() == fixed);
        physical[1].buttons[GAMEPAD_BUTTON_RIGHT_FACE_UP] = false;
        keys[KEY_B] = true; poll(); assert(prompt() == fixed);
    }
    for (auto &pad : physical) pad = {};
    neutral();
    { std::lock_guard<std::mutex> lock(gamepadMutex);
      gamepads[2].index = 2;
      gamepads[2].buttons[GAMEPAD_BUTTON_RIGHT_FACE_UP] = true;
      gamepads[2].axes[GAMEPAD_AXIS_RIGHT_X] = 1; }
    assert(packet(data) == 0x1000 && data[4] == 255);
    ps2pad::resetAfterStateLoad(); assert(packet(data) == 0 && data[4] == 128);
    assert(packet(data) == 0);
    { std::lock_guard<std::mutex> lock(gamepadMutex); gamepads = {}; }
    keys[KEY_B] = true; poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(260));
    assert(packet(data) == 0 && data[4] == 128);
    std::puts("PASS: desktop packets, remaps, desktop/pad activity, manual prompts and atlas identity");
}
