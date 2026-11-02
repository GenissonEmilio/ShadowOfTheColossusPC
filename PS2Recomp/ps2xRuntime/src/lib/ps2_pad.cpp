#include <cstdio>
#include <cmath>
#include <algorithm>
#include "runtime/host_menu.h"
#include "runtime/desktop_input.h"
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include <cstdlib>
#include "runtime/ps2_pad.h"
#include "runtime/ps2_stats.h"
#include "ps2_host_backend.h"
#include <cstring>
#include <atomic>
#include <fstream>
#include <mutex>
#include <array>
#if defined(__linux__) && !defined(__ANDROID__)
#include "runtime/linux_gamepad_mappings.h"
// GLFW is linked by the desktop raylib backend. A present joystick may have
// no mapping: raylib still reports it available, but returns no gamepad input.
extern "C" int glfwJoystickIsGamepad(int jid);
#endif

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

    void loadGamepadMappings()
    {
#if defined(__linux__) && !defined(__ANDROID__)
        if (!IsWindowReady())
            return;
        // The Zikway HID / GameSir Nova Lite layout is exposed by SDL's HID
        // backend, but is missing from GLFW's bundled Linux mapping database.
        // Use GLFW's GUID (without SDL's name CRC) and its evdev axis order.
        static std::once_flag loaded;
        std::call_once(loaded, [] {
            SetGamepadMappings(ps2pad::kLinuxMappings);
            SetGamepadMappings(
                "03000000373500004110000011010000,Zikway HID gamepad,"
                "a:b0,b:b1,x:b3,y:b4,back:b10,guide:b12,start:b11,"
                "leftstick:b13,rightstick:b14,leftshoulder:b6,rightshoulder:b7,"
                "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
                "leftx:a0,lefty:a1,rightx:a2,righty:a3,"
                "lefttrigger:a5,righttrigger:a4,platform:Linux,");
            // A caller can supply a mapping from a controller configurator without
            // Steam, or reuse SDL's standard environment setting. User mappings win.
            if (const char *path = std::getenv("PS2X_GAMEPAD_MAPPINGS")) {
                std::ifstream input(path, std::ios::binary | std::ios::ate);
                if (input && input.tellg() >= 0 && input.tellg() <= 1024 * 1024) {
                    std::string text(static_cast<size_t>(input.tellg()), '\0');
                    input.seekg(0);
                    if (input.read(text.data(), static_cast<std::streamsize>(text.size())))
                        SetGamepadMappings(text.c_str());
                }
            }
            if (const char *text = std::getenv("SDL_GAMECONTROLLERCONFIG"))
                SetGamepadMappings(text);
        });
#endif
    }
}

namespace {
    std::mutex gamepadMutex;
    std::array<ps2pad::GamepadState, 4> gamepads;
    int selectedGamepad = -1;
    bool stateReleaseGate = false;
}

void ps2pad::pollGamepads()
{
    loadGamepadMappings();
    std::array<GamepadState, 4> snapshot;
    for (int i = 0; i < 4; ++i) {
        if (!IsGamepadAvailable(i)) continue;
#if defined(__linux__) && !defined(__ANDROID__)
        if (!glfwJoystickIsGamepad(i)) continue;
#endif
        snapshot[i].index = i;
        for (int button = GAMEPAD_BUTTON_LEFT_FACE_UP; button <= GAMEPAD_BUTTON_RIGHT_THUMB; ++button)
            snapshot[i].buttons[button] = IsGamepadButtonDown(i, button);
        for (int axis = GAMEPAD_AXIS_LEFT_X; axis <= GAMEPAD_AXIS_RIGHT_TRIGGER; ++axis)
            snapshot[i].axes[axis] = GetGamepadAxisMovement(i, axis);
    }
    std::lock_guard<std::mutex> lock(gamepadMutex);
    gamepads = snapshot;
}

void ps2pad::resetAfterStateLoad()
{
    std::lock_guard<std::mutex> lock(gamepadMutex);
    stateReleaseGate = true;
}

ps2pad::GamepadState ps2pad::readGamepad()
{
    std::lock_guard<std::mutex> lock(gamepadMutex);
    if (stateReleaseGate) {
        bool held = false;
        for (const auto &pad : gamepads) {
            if (pad.index < 0) continue;
            for (bool button : pad.buttons) held |= button;
            for (int axis = GAMEPAD_AXIS_LEFT_X; axis <= GAMEPAD_AXIS_RIGHT_Y; ++axis)
                held |= std::fabs(pad.axes[axis]) > 0.35f;
            held |= pad.axes[GAMEPAD_AXIS_LEFT_TRIGGER] > 0.35f || pad.axes[GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.35f;
        }
        if (!held) stateReleaseGate = false;
        return {};
    }
    int first = -1;
    for (int i = 0; i < 4; ++i) {
        if (gamepads[i].index < 0) continue;
        if (first < 0) first = i;
        bool pressed = false;
        for (int button = GAMEPAD_BUTTON_LEFT_FACE_UP; button <= GAMEPAD_BUTTON_RIGHT_THUMB; ++button)
            pressed |= gamepads[i].buttons[button];
        for (int axis = GAMEPAD_AXIS_LEFT_X; axis <= GAMEPAD_AXIS_RIGHT_Y; ++axis)
            pressed |= std::fabs(gamepads[i].axes[axis]) > 0.35f;
        if (pressed) {
            selectedGamepad = i;
            return gamepads[i];
        }
        if (i == selectedGamepad) first = selectedGamepad;
    }
    selectedGamepad = first;
    return first >= 0 ? gamepads[first] : GamepadState{};
}

int ps2pad::activeGamepad() { return readGamepad().index; }

namespace
{
    const uint8_t *g_scriptRam = nullptr;
}

void PSPadBackend::setScriptMemory(const uint8_t *rdram) { g_scriptRam = rdram; }

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
    const auto gamepad = ps2pad::readGamepad();
    if (hostmenu::inputBlocked())
        return true; // the settings menu owns the controller: nothing pressed
    const bool useGamepad = gamepad.index >= 0;
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    if (useGamepad)
    {
        if (gamepad.buttons[GAMEPAD_BUTTON_LEFT_FACE_UP])
            clearBit(PAD_UP);
        if (gamepad.buttons[GAMEPAD_BUTTON_LEFT_FACE_DOWN])
            clearBit(PAD_DOWN);
        if (gamepad.buttons[GAMEPAD_BUTTON_LEFT_FACE_LEFT])
            clearBit(PAD_LEFT);
        if (gamepad.buttons[GAMEPAD_BUTTON_LEFT_FACE_RIGHT])
            clearBit(PAD_RIGHT);
        if (gamepad.buttons[GAMEPAD_BUTTON_RIGHT_FACE_DOWN])
            clearBit(PAD_CROSS);
        if (gamepad.buttons[GAMEPAD_BUTTON_RIGHT_FACE_RIGHT])
            clearBit(PAD_CIRCLE);
        if (gamepad.buttons[GAMEPAD_BUTTON_RIGHT_FACE_LEFT])
            clearBit(PAD_SQUARE);
        if (gamepad.buttons[GAMEPAD_BUTTON_RIGHT_FACE_UP])
            clearBit(PAD_TRIANGLE);
        if (gamepad.buttons[GAMEPAD_BUTTON_LEFT_TRIGGER_1])
            clearBit(PAD_L1);
        if (gamepad.buttons[GAMEPAD_BUTTON_RIGHT_TRIGGER_1])
            clearBit(PAD_R1);
        if (gamepad.buttons[GAMEPAD_BUTTON_LEFT_TRIGGER_2])
            clearBit(PAD_L2);
        if (gamepad.buttons[GAMEPAD_BUTTON_RIGHT_TRIGGER_2])
            clearBit(PAD_R2);
        if (gamepad.buttons[GAMEPAD_BUTTON_MIDDLE_RIGHT])
            clearBit(PAD_START);
        if (gamepad.buttons[GAMEPAD_BUTTON_MIDDLE_LEFT])
            clearBit(PAD_SELECT);
        if (gamepad.buttons[GAMEPAD_BUTTON_LEFT_THUMB])
            clearBit(PAD_L3);
        if (gamepad.buttons[GAMEPAD_BUTTON_RIGHT_THUMB])
            clearBit(PAD_R3);

        float lx = gamepad.axes[GAMEPAD_AXIS_LEFT_X];
        float ly = gamepad.axes[GAMEPAD_AXIS_LEFT_Y];
        float rx = gamepad.axes[GAMEPAD_AXIS_RIGHT_X];
        float ry = gamepad.axes[GAMEPAD_AXIS_RIGHT_Y];
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

    auto setStick = [&data](int xIndex, int yIndex, int x, int y)
    {
        if (x == 0 && y == 0)
            return;
        data[xIndex] = static_cast<uint8_t>(x < 0 ? 0x00 : (x > 0 ? 0xFF : kPadStickCenter));
        data[yIndex] = static_cast<uint8_t>(y < 0 ? 0x00 : (y > 0 ? 0xFF : kPadStickCenter));
    };
#if defined(__linux__) && !defined(__ANDROID__)
    const auto desktop = desktop_input::read();
    btns &= ~desktop.buttons;
    if (desktop.lx != 128 || desktop.ly != 128) {
        data[6] = desktop.lx; data[7] = desktop.ly;
    }
    if (desktop.rx != 128 || desktop.ry != 128) {
        data[4] = desktop.rx; data[5] = desktop.ry;
    }
#else
    // Keyboard, laid out like an arcade panel (works alongside a gamepad):
    //   WASD = left stick, arrows = D-pad
    //   Y U I O = Square Triangle L1 L2 (top row)
    //   H J K L = Cross  Circle   R1 R2 (bottom row)
    //   Z = Start, X = Select
    if (IsKeyDown(KEY_Y))
        clearBit(PAD_SQUARE);
    if (IsKeyDown(KEY_U))
        clearBit(PAD_TRIANGLE);
    if (IsKeyDown(KEY_I))
        clearBit(PAD_L1);
    if (IsKeyDown(KEY_O))
        clearBit(PAD_L2);
    if (IsKeyDown(KEY_H))
        clearBit(PAD_CROSS);
    if (IsKeyDown(KEY_J))
        clearBit(PAD_CIRCLE);
    if (IsKeyDown(KEY_K))
        clearBit(PAD_R1);
    if (IsKeyDown(KEY_L))
        clearBit(PAD_R2);
    if (IsKeyDown(KEY_Z))
        clearBit(PAD_START);
    if (IsKeyDown(KEY_X))
        clearBit(PAD_SELECT);
    if (IsKeyDown(KEY_UP))
        clearBit(PAD_UP);
    if (IsKeyDown(KEY_DOWN))
        clearBit(PAD_DOWN);
    if (IsKeyDown(KEY_LEFT))
        clearBit(PAD_LEFT);
    if (IsKeyDown(KEY_RIGHT))
        clearBit(PAD_RIGHT);

    auto keyAxis = [](int negative, int positive) -> int
    { return (IsKeyDown(positive) ? 1 : 0) - (IsKeyDown(negative) ? 1 : 0); };
    setStick(6, 7, keyAxis(KEY_A, KEY_D), keyAxis(KEY_W, KEY_S));

#endif

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
