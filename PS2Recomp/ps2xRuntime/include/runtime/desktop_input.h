#pragma once
#include <cstdint>

namespace desktop_input
{
    struct State {
        uint16_t buttons = 0; // pressed PS2 bits, merged with the physical pad
        uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    };
    // All window/cursor operations run on the host thread, once per frame.
    void initialize();
    void poll(bool menuBlocked);
    // Guest thread reads only this synchronized snapshot, never GLFW state.
    State read();
}
