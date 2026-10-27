// Real raylib/GLFW input -> DualShock packet. Driven by virtual USB devices
// in the local integration test; no game files or keyboard injection.
#include "../../PS2Recomp/ps2xRuntime/src/lib/ps2_pad.cpp"
#include "raylib.h"
#include <thread>
#include <atomic>
namespace hostmenu { bool inputBlocked() { return false; } }

int main()
{
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(320, 200, "Shadow controller integration");
    std::atomic<bool> stop{false};
    std::atomic<unsigned> observed{0};
    // Match the runtime's threading: the guest consumes packets on a worker,
    // while GLFW is polled only by the thread that owns the window.
    std::thread guest([&] {
        PSPadBackend pad;
        unsigned seen = 0;
        while (!stop.load()) {
            uint8_t packet[32];
            pad.readState(0, 0, packet, sizeof(packet));
            const auto bits = static_cast<uint16_t>(packet[2] | (packet[3] << 8));
            if (!(bits & 0x4000)) seen |= 1;
            if (!(bits & 0x0010)) seen |= 2;
            if (packet[6] > 200) seen |= 4;
            if ((seen & 1) && bits == 0xffff && packet[6] == 128) seen |= 8;
            if (ps2pad::activeGamepad() == 1) seen |= 16;
            observed.store(seen);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    for (int frame = 0; frame < 420; ++frame) {
        PollInputEvents();
        ps2pad::pollGamepads();
        if (observed.load() == 31) break;
        WaitTime(0.016);
    }
    stop.store(true);
    guest.join();
    CloseWindow();
    unsigned seen = observed.load();
    std::printf("PAD_RESULT cross=%d up=%d analog=%d release=%d slot1=%d\n", !!(seen & 1), !!(seen & 2), !!(seen & 4), !!(seen & 8), !!(seen & 16));
    return seen == 31 ? 0 : 1;
}
