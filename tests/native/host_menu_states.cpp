// Actual menu implementation with deterministic host events, no ROM/window.
#include "raylib.h"
#include "runtime/state_manager.h"
#include "runtime/native_tuning.h"
#include "runtime/ps2_pad.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>

namespace fixture
{
    bool keys[512]{}, keyPress[512]{}, mousePress = false, working = false;
    Vector2 pointer{};
    ps2pad::GamepadState gamepad;
    std::array<savestates::Slot, savestates::SlotCount> stored{};
    std::vector<std::pair<savestates::Action, int>> requests;
    struct Text { std::string value; int x, y, size; };
    std::vector<Text> drawn;
    int width = 1024, height = 768;
    double clock = 0;
}

extern "C"
{
    bool IsKeyDown(int key) { return fixture::keys[key]; }
    bool IsKeyPressed(int key) { return fixture::keyPress[key]; }
    bool IsMouseButtonPressed(int button) { return button == MOUSE_BUTTON_LEFT && fixture::mousePress; }
    Vector2 GetMousePosition() { return fixture::pointer; }
    double GetTime() { return fixture::clock; }
    int MeasureText(const char *text, int size)
    {
        int glyphs = 0;
        for (const auto *p = reinterpret_cast<const unsigned char *>(text); *p; ++p)
            if ((*p & 0xC0) != 0x80) ++glyphs;
        return glyphs * size * 3 / 5;
    }
    void DrawText(const char *text, int x, int y, int size, Color)
    { fixture::drawn.push_back({text, x, y, size}); }
    void DrawRectangle(int, int, int, int, Color) {}
    bool CheckCollisionPointRec(Vector2 point, Rectangle box)
    { return point.x >= box.x && point.x < box.x + box.width && point.y >= box.y && point.y < box.y + box.height; }
}

namespace ps2pad
{
    void pollGamepads() {}
    GamepadState readGamepad() { return fixture::gamepad; }
}
namespace native_renderer
{
    Tuning *g_tuning = nullptr;
    Tuning &tuningFirst() { static Tuning value; g_tuning = &value; return value; }
    Tuning defaultTuning() { return {}; }
    const std::vector<TuningItem> &tuningItems() { static const std::vector<TuningItem> items; return items; }
    std::string tuningValueText(const TuningItem &, bool) { return {}; }
    bool tuningApplies(const TuningItem &) { return true; }
    const char *backendName() { return "OpenGL"; }
    void tuningStep(const TuningItem &, int) {}
    bool tuningRestore(const char *, float) { return false; }
}
namespace savestates
{
    void request(Action action, int slot) { assert(!fixture::working); fixture::requests.emplace_back(action, slot); }
    bool busy() { return fixture::working; }
    std::string status() { return fixture::working ? "Salvando estado..." : ""; }
    std::array<Slot, SlotCount> slots() { return fixture::stored; }
}

#include "../../PS2Recomp/ps2xRuntime/src/lib/host_menu.cpp"

namespace fixture
{
    void frame()
    {
        clock += 1.0 / 60.0;
        drawn.clear();
        hostmenu::update();
        hostmenu::draw(width, height);
        std::memset(keyPress, 0, sizeof(keyPress));
        mousePress = false;
    }
    void key(int key)
    {
        keys[key] = keyPress[key] = true; frame();
        keys[key] = false; frame();
    }
    void pad(int button)
    {
        gamepad.index = 0; gamepad.buttons[button] = true; frame();
        gamepad.buttons[button] = false; frame();
    }
    void click(Rectangle rectangle)
    { pointer = {rectangle.x + rectangle.width / 2, rectangle.y + rectangle.height / 2}; mousePress = true; frame(); frame(); }
    Rectangle action(int item)
    {
        for (const auto &[rectangle, value] : hostmenu::g_rowRects)
            if (value == item) return rectangle;
        assert(false && "missing action rectangle"); return {};
    }
    void bounds()
    {
        for (const auto &text : drawn)
        {
            if (text.x < 0 || text.y < 0 || text.x + MeasureText(text.value.c_str(), text.size) > width || text.y + text.size > height)
            {
                std::fprintf(stderr, "text outside %dx%d: %s (%d,%d,%d)\n", width, height, text.value.c_str(), text.x, text.y, text.size);
                assert(false);
            }
        }
        for (const auto &rectangle : hostmenu::g_tabRects)
            assert(rectangle.x >= 0 && rectangle.y >= 0 && rectangle.x + rectangle.width <= width && rectangle.y + rectangle.height <= height);
        for (const auto &[rectangle, item] : hostmenu::g_rowRects)
        {
            (void)item;
            assert(rectangle.x >= 0 && rectangle.y >= 0 && rectangle.x + rectangle.width <= width && rectangle.y + rectangle.height <= height);
        }
    }
}

int main()
{
    using namespace fixture;
    hostmenu::settings().fullscreen = true;
    hostmenu::settings().screenMode = hostmenu::kScreenStretch;
    hostmenu::settings().language = 1;
    key(KEY_F1);
    assert(hostmenu::isOpen());
    for (int i = 0; i < 6; ++i) key(KEY_E);
    assert(hostmenu::g_tab == hostmenu::kStatesTab);
    assert(hostmenu::g_rowRects.size() == 18);
    key(KEY_ENTER);
    assert(requests.size() == 1 && requests.back() == std::make_pair(savestates::Action::Save, 0));
    stored[0] = {true, 64u * 1024u * 1024u, "01/11 12:34:56"};
    key(KEY_ENTER);
    assert(hostmenu::g_stateConfirm && !hostmenu::g_stateConfirmYes);
    key(KEY_ENTER); // default Cancel
    assert(!hostmenu::g_stateConfirm && requests.size() == 1);
    key(KEY_ENTER); key(KEY_RIGHT); key(KEY_ENTER);
    assert(requests.size() == 2 && requests.back().first == savestates::Action::Save);
    key(KEY_RIGHT); key(KEY_ENTER);
    assert(hostmenu::g_stateConfirm && requests.size() == 2);
    key(KEY_BACKSPACE);
    assert(hostmenu::isOpen() && !hostmenu::g_stateConfirm && requests.size() == 2);
    key(KEY_ENTER); key(KEY_RIGHT); key(KEY_ENTER);
    assert(requests.size() == 3 && requests.back().first == savestates::Action::Load);
    key(KEY_RIGHT); key(KEY_ENTER); key(KEY_F1);
    assert(!hostmenu::isOpen() && !hostmenu::g_stateConfirm && requests.size() == 3);
    key(KEY_F1); key(KEY_ENTER); key(KEY_RIGHT); key(KEY_ENTER);
    assert(requests.size() == 4 && requests.back().first == savestates::Action::Delete);
    // Up/down selects slots; left/right selects the action within one slot.
    for (int i = 0; i < 5; ++i) pad(GAMEPAD_BUTTON_LEFT_FACE_DOWN);
    assert(hostmenu::g_item / 3 == 5);
    pad(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
    assert(!hostmenu::g_stateConfirm && requests.size() == 4); // empty delete
    pad(GAMEPAD_BUTTON_LEFT_FACE_RIGHT); // wraps Delete -> Save
    pad(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
    assert(requests.size() == 5 && requests.back() == std::make_pair(savestates::Action::Save, 5));
    // Mouse controls and modal buttons use the rendered hit rectangles.
    click(action(1));
    assert(hostmenu::g_stateConfirm);
    click(hostmenu::g_stateConfirmRects[0]);
    assert(requests.size() == 5 && !hostmenu::g_stateConfirm);
    click(action(1)); click(hostmenu::g_stateConfirmRects[1]);
    assert(requests.size() == 6 && requests.back() == std::make_pair(savestates::Action::Load, 0));
    working = true;
    click(action(0));
    assert(requests.size() == 6 && !hostmenu::g_stateConfirm);
    working = false;
    key(KEY_H); assert(hostmenu::g_help); key(KEY_BACKSPACE); assert(!hostmenu::g_help);
    for (const int language : {1, 2})
        for (const auto &[w, h] : {std::pair{640, 480}, std::pair{1024, 768}, std::pair{1920, 1080}})
        {
            hostmenu::settings().language = language; width = w; height = h;
            frame(); bounds();
            click(action(1)); assert(hostmenu::g_stateConfirm); bounds();
            key(KEY_BACKSPACE);
        }
    assert(hostmenu::settings().fullscreen.load());
    assert(hostmenu::settings().screenMode.load() == hostmenu::kScreenStretch);
    assert(!hostmenu::quitRequested());
    key(KEY_F1); assert(!hostmenu::isOpen());
    std::puts("PASS: state slots, keyboard/gamepad/mouse, confirmations, busy guard, PT/EN bounds and fullscreen preservation");
}
