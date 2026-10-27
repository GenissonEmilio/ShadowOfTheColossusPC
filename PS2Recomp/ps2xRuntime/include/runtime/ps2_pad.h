#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>

namespace ps2pad
{
    struct GamepadState {
        int index = -1;
        bool buttons[32] = {};
        float axes[8] = {};
    };
    // Poll on the window thread; the guest reads a synchronized copy.
    void pollGamepads();
    GamepadState readGamepad();
    // Shared by the game and the host menu; reconnects and non-zero slots work.
    int activeGamepad();
}

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size);
    // Guest RAM for PS2X_INPUT_SCRIPT entries that wait on a memory word.
    static void setScriptMemory(const uint8_t *rdram);
};

#endif
