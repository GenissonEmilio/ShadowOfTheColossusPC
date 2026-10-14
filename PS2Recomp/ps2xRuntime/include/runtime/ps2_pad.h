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
};

#endif
