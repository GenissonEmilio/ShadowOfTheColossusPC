#pragma once

// Native replacements for hot VU1 microcode loops. A game registers a
// function for (microcode image hash, program entry pc); when VU1 starts that
// program, the function runs instead of the microcode up to some pc and
// leaves registers and VU data memory exactly as the microcode would there;
// the microcode then continues from the returned pc.
//
// PS2X_VU1_NATIVE=0 disables them; =verify runs the microcode as well (on the
// interpreter, up to the returned pc) and reports any difference.

#include <cstdint>

struct VU1State;
class GS;

namespace vu1native
{
    // Returns the pc to continue at, or 0 to decline (the microcode runs).
    // cycles: VU1 cycles the replaced code would have taken (an estimate).
    using LoopFn = uint32_t (*)(VU1State &state, uint8_t *vuData, uint32_t dataSize, uint64_t &cycles);

    void registerLoop(uint64_t imageHash, uint32_t entryPc, LoopFn fn, bool generated = false);
    // A hand-written loop (game overrides) for that entry; generated fast
    // regions (vu1fast_*.cpp) do not count.
    bool isRegistered(uint64_t imageHash, uint32_t entryPc);

    // XGKICK from native code: the GIF packet at that VU data qword goes out
    // whole, now (the microcode double-buffers, so nothing it writes later
    // lands in a packet in flight). No-op while verifying (copies).
    void xgkick(uint32_t qwordAddress);

    // The GS this VU1 run kicks to (nullptr while verifying).
    GS *gs();

    struct Registrar
    {
        Registrar(uint64_t imageHash, uint32_t entryPc, LoopFn fn) { registerLoop(imageHash, entryPc, fn); }
    };
    struct GeneratedRegistrar
    {
        GeneratedRegistrar(uint64_t imageHash, uint32_t entryPc, LoopFn fn) { registerLoop(imageHash, entryPc, fn, true); }
    };
}
