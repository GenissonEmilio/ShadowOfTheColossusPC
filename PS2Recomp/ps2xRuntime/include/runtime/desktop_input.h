#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace desktop_input
{
    struct State {
        uint16_t buttons = 0; // pressed PS2 bits, merged with the physical pad
        uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    };
    enum class PromptStyle { PlayStation, Xbox, Keyboard };
    enum class Control : size_t {
        MoveUp, MoveDown, MoveLeft, MoveRight, Jump, Attack, Grab, Action,
        Horse, WeaponLeft, WeaponRight, Look, CameraReset, Crouch, Start, Select,
        DpadUp, DpadDown, L3, R3, CameraLeft, CameraRight, CameraUp, CameraDown,
        Confirm, Cancel, Count
    };
    struct PromptProfile {
        PromptStyle style = PromptStyle::PlayStation;
        // Raylib/GLFW key code, -(button + 1) for mouse, or zero for unbound.
        std::array<int, static_cast<size_t>(Control::Count)> codes{};
        int code(Control control) const { return codes[static_cast<size_t>(control)]; }
        uint64_t variant() const;
    };
    // All window/cursor operations run on the host thread, once per frame.
    void initialize();
    void poll(bool menuBlocked);
    void gamepadActivity(PromptStyle style);
    // Guest thread reads only this synchronized snapshot, never GLFW state.
    State read();
    PromptProfile prompts();
    void resetAfterStateLoad();
}
