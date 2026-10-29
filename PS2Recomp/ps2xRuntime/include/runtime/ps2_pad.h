#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size);
    // Guest RAM for PS2X_INPUT_SCRIPT entries that wait on a memory word.
    static void setScriptMemory(const uint8_t *rdram);
    // The controller the game reads (-1: none).
    static int gamepadIndex();
    // Its name as the host reports it (nullptr: none).
    static const char *gamepadName();
    // DualShock 2 motors as the game sets them (libvib): small on/off, large 0..1.
    // Linux: played on the controller in use through evdev force feedback.
    static void setRumble(bool small, float large);
};

#endif
