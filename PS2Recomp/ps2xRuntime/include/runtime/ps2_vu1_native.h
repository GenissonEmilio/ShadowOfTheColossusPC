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
class PS2Memory;
class VU1Interpreter;

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

    // A VIF1 batch that never reaches VU1. When an MSCNT would start the
    // program at (image hash, pc), the VIF codes from the batch's first
    // UNPACK up to that MSCNT (included) go to the batch function instead of
    // being unpacked; it returns true when it drew them itself. VU1 then does
    // not run: its registers and data memory stay as they were, so this is
    // only for programs whose effects end with their GS output.
    struct VifBatch
    {
        const uint8_t *codes; // the VIF codes and their data
        uint32_t size;        // bytes, up to the end of the MSCNT
        const uint32_t *row;  // VIF1 ROW, CYCLE and MODE at the first UNPACK
        uint32_t cycle;
        uint32_t mode;
        GS *gs;
    };
    using BatchFn = bool (*)(const VU1State &state, const VifBatch &batch);
    void registerBatch(uint64_t imageHash, uint32_t pc, BatchFn fn);
    // The VU1 whose program state decides (set by the runtime).
    void setBatchTarget(VU1Interpreter *vu1, PS2Memory *memory, GS *gs);
    bool hasBatchFns();
    bool tryBatch(VifBatch &batch);

    struct BatchRegistrar
    {
        BatchRegistrar(uint64_t imageHash, uint32_t pc, BatchFn fn) { registerBatch(imageHash, pc, fn); }
    };

    struct Registrar
    {
        Registrar(uint64_t imageHash, uint32_t entryPc, LoopFn fn) { registerLoop(imageHash, entryPc, fn); }
    };
    struct GeneratedRegistrar
    {
        GeneratedRegistrar(uint64_t imageHash, uint32_t entryPc, LoopFn fn) { registerLoop(imageHash, entryPc, fn, true); }
    };
}
