#include <cstdio>
#include <cmath>
#include <algorithm>
#include "runtime/host_menu.h"
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include <cstdlib>
#include "runtime/ps2_pad.h"
#include "runtime/ps2_stats.h"
#include "ps2_host_backend.h"
#include <atomic>
#include <cstring>
#include <utility>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;
}

namespace
{
    const uint8_t *g_scriptRam = nullptr;
}

void PSPadBackend::setScriptMemory(const uint8_t *rdram) { g_scriptRam = rdram; }

namespace
{
    std::atomic<bool> g_mouseCaptured{false};
    std::atomic<bool> g_captureClickHeld{false}; // the click that captured the mouse is not a game button
    std::atomic<float> g_mouseDx{0.0f}, g_mouseDy{0.0f}; // pixels moved since the pad last read them
    std::atomic<int> g_wheelDir{0};          // the last wheel step: -1 down (d-pad left), +1 up (d-pad right)
    std::atomic<double> g_wheelUntil{0.0};   // held as a d-pad press until then

    void addTo(std::atomic<float> &total, float value)
    {
        float old = total.load(std::memory_order_relaxed);
        while (!total.compare_exchange_weak(old, old + value, std::memory_order_relaxed))
        {
        }
    }
}

void PSPadBackend::hostUpdate()
{
    bool captured = g_mouseCaptured.load(std::memory_order_relaxed);
    if (!captured && !hostmenu::isOpen() && IsWindowFocused() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        DisableCursor();
        captured = true;
        g_captureClickHeld = true;
    }
    else if (captured && (IsKeyPressed(KEY_ESCAPE) || hostmenu::isOpen() || !IsWindowFocused()))
    {
        EnableCursor();
        captured = false;
    }
    if (g_captureClickHeld.load(std::memory_order_relaxed) && !IsMouseButtonDown(MOUSE_BUTTON_LEFT))
        g_captureClickHeld = false;
    g_mouseCaptured.store(captured, std::memory_order_relaxed);
    if (captured)
    {
        const Vector2 d = GetMouseDelta();
        addTo(g_mouseDx, d.x);
        addTo(g_mouseDy, d.y);
        if (const float wheel = GetMouseWheelMove(); wheel != 0.0f)
        {
            g_wheelDir = wheel > 0.0f ? 1 : -1;
            g_wheelUntil = GetTime() + 0.12; // long enough for the game's next pad reads to see it
        }
    }
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    constexpr int kGamepad = 0;
    if (hostmenu::inputBlocked())
        return true; // the settings menu owns the controller: nothing pressed
    const bool useGamepad = IsGamepadAvailable(kGamepad);
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    if (useGamepad)
    {
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_UP))
            clearBit(PAD_UP);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN))
            clearBit(PAD_DOWN);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))
            clearBit(PAD_LEFT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            clearBit(PAD_CROSS);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
            clearBit(PAD_CIRCLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT))
            clearBit(PAD_SQUARE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))
            clearBit(PAD_TRIANGLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1))
            clearBit(PAD_L1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
            clearBit(PAD_R1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))
            clearBit(PAD_L2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
            clearBit(PAD_R2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            clearBit(PAD_START);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_LEFT))
            clearBit(PAD_SELECT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_THUMB))
            clearBit(PAD_L3);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_THUMB))
            clearBit(PAD_R3);

        float lx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
        // A modern stick at rest reads a few percent off centre and never
        // still; the DualShock 2 came back to 0x80. Games aim with the raw
        // values (SotC's sword follows them every frame), so a radial dead
        // zone rescaled to the full range: PS2X_STICK_DEADZONE (0..0.9, 0.18).
        static const float deadZone = []
        {
            const char *env = std::getenv("PS2X_STICK_DEADZONE");
            const float v = env ? static_cast<float>(std::atof(env)) : 0.18f;
            return std::clamp(v, 0.0f, 0.9f);
        }();
        auto shape = [](float &x, float &y)
        {
            const float m = std::sqrt(x * x + y * y);
            if (m <= deadZone)
            {
                x = y = 0.0f;
                return;
            }
            const float scale = std::min(1.0f, (m - deadZone) / (1.0f - deadZone)) / m;
            x *= scale;
            y *= scale;
        };
        static const bool logAxes = std::getenv("PS2X_LOG_PAD") != nullptr;
        if (logAxes)
        {
            static int every = 0;
            if (++every % 30 == 0)
                std::fprintf(stderr, "[pad] raw L %+.3f %+.3f R %+.3f %+.3f\n", lx, ly, rx, ry);
        }
        shape(lx, ly);
        shape(rx, ry);
        auto toByte = [](float v) { return static_cast<uint8_t>(std::clamp(128.0f + v * 127.5f, 0.0f, 255.0f)); };
        data[6] = toByte(lx);
        data[7] = toByte(ly);
        data[4] = toByte(rx);
        data[5] = toByte(ry);
    }

    // Keyboard and mouse, PC layout (works alongside a gamepad; table in README.md):
    //   WASD move (Left Alt held: walk), mouse (captured by a click): camera, arrows: d-pad,
    //   Space triangle (jump), left button square (weapon), right button / F R1 (grab),
    //   E / Enter cross (call Agro, confirm), Left Shift circle, Tab / middle button L1,
    //   wheel or 1/2 d-pad left/right (weapon), Z L2, X R2, C L3, V R3,
    //   Esc Start (pause; also frees the mouse), Backspace Select.
    const bool mouseButtons = g_mouseCaptured.load(std::memory_order_relaxed) && !g_captureClickHeld.load(std::memory_order_relaxed);
    const std::pair<int, uint16_t> keys[] = {
        {KEY_SPACE, PAD_TRIANGLE}, {KEY_F, PAD_R1}, {KEY_E, PAD_CROSS}, {KEY_ENTER, PAD_CROSS},
        {KEY_LEFT_SHIFT, PAD_CIRCLE}, {KEY_TAB, PAD_L1}, {KEY_ONE, PAD_LEFT}, {KEY_TWO, PAD_RIGHT},
        {KEY_LEFT, PAD_LEFT}, {KEY_RIGHT, PAD_RIGHT}, {KEY_UP, PAD_UP}, {KEY_DOWN, PAD_DOWN}, {KEY_Z, PAD_L2}, {KEY_X, PAD_R2}, {KEY_C, PAD_L3},
        {KEY_V, PAD_R3}, {KEY_ESCAPE, PAD_START}, {KEY_BACKSPACE, PAD_SELECT}};
    for (const auto &[key, mask] : keys)
        if (IsKeyDown(key))
            clearBit(mask);
    if (mouseButtons)
    {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            clearBit(PAD_SQUARE);
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
            clearBit(PAD_R1);
        if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE))
            clearBit(PAD_L1);
    }
    if (GetTime() < g_wheelUntil.load(std::memory_order_relaxed))
        clearBit(g_wheelDir.load(std::memory_order_relaxed) > 0 ? PAD_RIGHT : PAD_LEFT);

    auto keyAxis = [](int negative, int positive) -> int
    { return (IsKeyDown(positive) ? 1 : 0) - (IsKeyDown(negative) ? 1 : 0); };
    auto toStick = [](float v) { return static_cast<uint8_t>(std::clamp(128.0f + v * 127.5f, 0.0f, 255.0f)); };
    const float walk = IsKeyDown(KEY_LEFT_ALT) ? 0.5f : 1.0f;
    // Keyboard and mouse add to the controller's sticks (both can be used at once).
    auto fromStick = [](uint8_t b) { return (static_cast<float>(b) - 128.0f) / 127.5f; };
    auto addStick = [&](int xIndex, int yIndex, float x, float y)
    {
        data[xIndex] = toStick(std::clamp(fromStick(data[xIndex]) + x, -1.0f, 1.0f));
        data[yIndex] = toStick(std::clamp(fromStick(data[yIndex]) + y, -1.0f, 1.0f));
    };
    if (const int x = keyAxis(KEY_A, KEY_D), y = keyAxis(KEY_W, KEY_S); x || y)
        addStick(6, 7, static_cast<float>(x) * walk, static_cast<float>(y) * walk);
    // Camera: the mouse's movement since the last read (PS2X_MOUSE_SENS, PS2X_MOUSE_INVERT_Y=1), smoothed.
    static const float sensitivity = std::getenv("PS2X_MOUSE_SENS") ? static_cast<float>(std::atof(std::getenv("PS2X_MOUSE_SENS"))) : 1.0f;
    static const float invertY = std::getenv("PS2X_MOUSE_INVERT_Y") && std::atoi(std::getenv("PS2X_MOUSE_INVERT_Y")) ? -1.0f : 1.0f;
    static float camX = 0.0f, camY = 0.0f;
    const float dx = g_mouseDx.exchange(0.0f, std::memory_order_relaxed), dy = g_mouseDy.exchange(0.0f, std::memory_order_relaxed);
    camX = std::clamp(camX * 0.5f + dx * 0.04f * sensitivity, -1.0f, 1.0f);
    camY = std::clamp(camY * 0.5f + dy * 0.04f * sensitivity * invertY, -1.0f, 1.0f);
    if (std::fabs(camX) > 0.02f || std::fabs(camY) > 0.02f)
        addStick(4, 5, camX, camY);
    auto setStick = [&data](int xIndex, int yIndex, int x, int y)
    {
        if (x == 0 && y == 0)
            return;
        data[xIndex] = static_cast<uint8_t>(x < 0 ? 0x00 : (x > 0 ? 0xFF : kPadStickCenter));
        data[yIndex] = static_cast<uint8_t>(y < 0 ? 0x00 : (y > 0 ? 0xFF : kPadStickCenter));
    };

    // PS2X_INPUT_SCRIPT="<sec>:<BUTTON>:<duration>;..." scripted presses for testing.
    // Besides buttons, LS_UP/LS_DOWN/LS_LEFT/LS_RIGHT push the left stick.
    // PS2X_INPUT_CLOCK=frames: times and durations count game frames instead
    // of seconds, so the run is the same at any speed (the pad itself is read
    // every VBlank: a press shorter than a slow frame would be lost).
    // A duration with a "v" suffix counts guest VBlanks from the press, and a
    // start of "+<n>v" is n VBlanks after the previous entry's start ("<n>v":
    // at guest VBlank n): SotC
    // moves Wander by VBlank time, so a walk timed in VBlanks covers the same
    // ground at any frame rate. Each entry's first and last game frame go to
    // the log ([input]). A start of "@<addr>=<value>" (hex) waits for that
    // guest RAM word to hold the value (WAIT presses nothing: an anchor for
    // the "+<n>v" entries after it), e.g. "@d7d824=0:WAIT;+300v:START:12".
    // SNAP saves the next presented frame as snap_<entry>.ppm (GPU backend,
    // PS2X_SNAPSHOT_DIR). "+<n>v@<k>" starts n VBlanks after entry k's start.
    // A start of "@3d" waits for 3D after a loading
    // screen: the first VBlank with VU1 programs after 60 without any.
    // A fourth field "r<n>" presses again every n VBlanks until 3D comes
    // after a loading screen (a menu that shows up after a varying wait,
    // e.g. the memory card's file list: "+150v:CROSS:8:r250").
    struct ScriptEntry
    {
        double at = 0.0;        // start (seconds or frames), or VBlanks after the previous entry's start
        bool afterPrevious = false;
        int anchor = -1;        // "+<n>v@<k>": n VBlanks after entry k's start instead of the previous one's
        bool atVblank = false;  // start at an absolute guest VBlank ("<n>v")
        bool onMemory = false;  // start when the word at memoryAddress holds memoryValue
        bool on3d = false;      // start at 3D after a loading screen ("@3d")
        uint32_t memoryAddress = 0, memoryValue = 0;
        uint16_t mask = 0;
        double duration = 0.0;
        bool vblankDuration = false;
        uint64_t startVblank = 0; // set when the entry starts
        double startNow = 0.0;
        int state = 0;            // 0 waiting, 1 pressed, 2 done, 3 waiting to press again ("r<n>")
        uint64_t repeatEvery = 0, firstVblank = 0, nextVblank = 0;
    };
    static constexpr uint16_t kStickUp = 0xF001u, kStickDown = 0xF002u, kStickLeft = 0xF003u, kStickRight = 0xF004u, kSnap = 0xF005u;
    static std::vector<ScriptEntry> script = []
    {
        std::vector<ScriptEntry> out;
        const char *env = std::getenv("PS2X_INPUT_SCRIPT");
        if (!env)
            return out;
        const std::map<std::string, uint16_t> names = {
            {"UP", PAD_UP}, {"DOWN", PAD_DOWN}, {"LEFT", PAD_LEFT}, {"RIGHT", PAD_RIGHT},
            {"CROSS", PAD_CROSS}, {"CIRCLE", PAD_CIRCLE}, {"SQUARE", PAD_SQUARE}, {"TRIANGLE", PAD_TRIANGLE},
            {"L1", PAD_L1}, {"L2", PAD_L2}, {"R1", PAD_R1}, {"R2", PAD_R2},
            {"START", PAD_START}, {"SELECT", PAD_SELECT}, {"WAIT", 0}, {"SNAP", kSnap},
            {"LS_UP", kStickUp}, {"LS_DOWN", kStickDown}, {"LS_LEFT", kStickLeft}, {"LS_RIGHT", kStickRight}};
        std::stringstream all(env);
        std::string item;
        while (std::getline(all, item, ';'))
        {
            std::stringstream fields(item);
            std::string at, name, dur;
            if (std::getline(fields, at, ':') && std::getline(fields, name, ':'))
            {
                std::getline(fields, dur, ':');
                std::string extra;
                std::getline(fields, extra, ':');
                const auto it = names.find(name);
                if (it == names.end())
                    continue;
                ScriptEntry e;
                e.afterPrevious = !at.empty() && at[0] == '+';
                e.on3d = at == "@3d";
                e.onMemory = !e.on3d && !at.empty() && at[0] == '@';
                if (e.onMemory)
                {
                    const size_t eq = at.find('=');
                    e.memoryAddress = static_cast<uint32_t>(std::strtoul(at.c_str() + 1, nullptr, 16));
                    e.memoryValue = eq == std::string::npos ? 0u : static_cast<uint32_t>(std::strtoul(at.c_str() + eq + 1, nullptr, 16));
                }
                else
                {
                    const size_t atSign = at.find('@');
                    if (e.afterPrevious && atSign != std::string::npos)
                        e.anchor = std::atoi(at.c_str() + atSign + 1);
                    e.at = std::atof(at.c_str() + (e.afterPrevious ? 1 : 0));
                    e.atVblank = !e.afterPrevious && !at.empty() && at.back() == 'v';
                }
                e.mask = it->second;
                e.vblankDuration = !dur.empty() && dur.back() == 'v';
                e.duration = dur.empty() ? 0.2 : std::atof(dur.c_str());
                if (!extra.empty() && extra[0] == 'r')
                    e.repeatEvery = std::strtoull(extra.c_str() + 1, nullptr, 10);
                out.push_back(e);
            }
        }
        return out;
    }();
    static const bool frameClock = []
    {
        const char *env = std::getenv("PS2X_INPUT_CLOCK");
        return env && std::strcmp(env, "frames") == 0;
    }();
    if (!script.empty())
    {
        const uint64_t frame = ps2_stats::vif1Chains.load(std::memory_order_relaxed);
        const uint64_t vblank = ps2_stats::vblanks.load(std::memory_order_relaxed);
        const double now = frameClock ? static_cast<double>(frame) : GetTime();
        // The last VBlank with 3D after a loading screen ("r<n>" entries stop there).
        static uint64_t loadedAt = 0, seenPrograms = 0, seenVblank = 0, quiet = 0;
        if (vblank != seenVblank)
        {
            const uint64_t programs = ps2_stats::vu1Programs.load(std::memory_order_relaxed);
            const bool drew = programs != seenPrograms;
            if (drew && quiet >= 60u)
                loadedAt = vblank;
            quiet = drew ? 0u : quiet + (vblank - seenVblank);
            seenPrograms = programs;
            seenVblank = vblank;
        }
        for (size_t i = 0; i < script.size(); ++i)
        {
            ScriptEntry &e = script[i];
            if (e.state == 3)
            {
                if (loadedAt > e.firstVblank)
                    e.state = 2;
                else if (vblank >= e.nextVblank)
                {
                    e.state = 1;
                    e.startVblank = vblank;
                    e.startNow = now;
                    std::fprintf(stderr, "[input] entry %zu pressed again at frame %llu vblank %llu\n", i, (unsigned long long)frame, (unsigned long long)vblank);
                }
            }
            if (e.state == 0)
            {
                const ScriptEntry *previous = i > 0 ? &script[i - 1] : nullptr;
                bool due = false;
                if (e.on3d)
                {
                    // VBlanks in a row without a VU1 program (a loading screen), then one with.
                    static uint64_t lastPrograms = 0, lastVblank = 0, quietVblanks = 0;
                    const uint64_t programs = ps2_stats::vu1Programs.load(std::memory_order_relaxed);
                    if (vblank != lastVblank)
                    {
                        const bool drew = programs != lastPrograms;
                        if (previous == nullptr || previous->state != 0)
                            due = drew && quietVblanks >= 60u;
                        quietVblanks = drew ? 0u : quietVblanks + (vblank - lastVblank);
                        lastPrograms = programs;
                        lastVblank = vblank;
                    }
                }
                else if (e.onMemory)
                {
                    // In script order: only once the entries before it started.
                    uint32_t word = 0;
                    if (g_scriptRam && (previous == nullptr || previous->state != 0))
                        std::memcpy(&word, g_scriptRam + (e.memoryAddress & 0x1FFFFFCu), sizeof(word));
                    due = g_scriptRam && (previous == nullptr || previous->state != 0) && word == e.memoryValue;
                }
                else
                {
                    if (e.afterPrevious && e.anchor >= 0 && static_cast<size_t>(e.anchor) < script.size())
                        previous = &script[static_cast<size_t>(e.anchor)];
                    due = e.afterPrevious ? (previous && previous->state != 0 && vblank >= previous->startVblank + static_cast<uint64_t>(e.at))
                          : e.atVblank  ? static_cast<double>(vblank) >= e.at
                                        : now >= e.at;
                }
                if (!due)
                    continue;
                e.state = 1;
                e.startVblank = vblank;
                e.firstVblank = vblank;
                e.startNow = now;
                if (e.mask == kSnap)
                    ps2_stats::snapshotRequest.store(static_cast<int>(i), std::memory_order_relaxed);
                std::fprintf(stderr, "[input] entry %zu pressed at frame %llu vblank %llu\n", i, (unsigned long long)frame, (unsigned long long)vblank);
            }
            if (e.state != 1)
                continue;
            const bool over = e.vblankDuration ? vblank >= e.startVblank + static_cast<uint64_t>(e.duration)
                                               : now >= e.startNow + e.duration;
            if (over)
            {
                e.state = 2;
                if (e.repeatEvery && loadedAt <= e.firstVblank)
                {
                    e.state = 3;
                    e.nextVblank = e.startVblank + e.repeatEvery;
                }
                std::fprintf(stderr, "[input] entry %zu released at frame %llu vblank %llu\n", i, (unsigned long long)frame, (unsigned long long)vblank);
                continue;
            }
            switch (e.mask)
            {
            case 0:
            case kSnap:
                break;
            case kStickUp: setStick(6, 7, 0, -1); break;
            case kStickDown: setStick(6, 7, 0, 1); break;
            case kStickLeft: setStick(6, 7, -1, 0); break;
            case kStickRight: setStick(6, 7, 1, 0); break;
            default: clearBit(e.mask); break;
            }
        }
    }

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}
