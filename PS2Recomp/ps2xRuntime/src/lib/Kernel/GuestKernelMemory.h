#pragma once

#include <cstdint>

// Guest-visible structures the runtime owns (RPC packets and server
// descriptors, TLS blocks, boot-mode entries, stacks for handler and callback
// invocations) live in the EE kernel area, which games never touch. Placing
// them high in user RAM let game heaps grow over them (layout from the
// sotc-pc fork of PS2Recomp).
namespace ps2x::kernel_memory
{
    constexpr uint32_t kRpcPacketPoolBase = 0x00040000u;
    constexpr uint32_t kRpcPacketPoolBytes = 0x00001000u;
    constexpr uint32_t kRpcServerPoolBase = 0x00041000u;
    constexpr uint32_t kRpcServerPoolBytes = 0x00002000u;
    constexpr uint32_t kTlsPoolBase = 0x00043000u;
    constexpr uint32_t kTlsPoolBytes = 0x00004000u;
    constexpr uint32_t kBootModePoolBase = 0x00047000u;
    constexpr uint32_t kBootModePoolBytes = 0x00001000u;

    // The allocator skips [kRpcPacketPoolBase, end of kBootModePool):
    // 11 stacks above it and 12 below it, each 16 KiB.
    constexpr uint32_t kCallbackStackFloor = 0x00010000u;
    constexpr uint32_t kCallbackStackTop = 0x00074000u;
    constexpr uint32_t kCallbackStackSize = 0x00004000u;
}
