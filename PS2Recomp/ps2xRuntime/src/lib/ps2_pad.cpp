#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include <cstdlib>
#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include <cstring>

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
        data[6] = static_cast<uint8_t>(128 + lx * 127);
        data[7] = static_cast<uint8_t>(128 + ly * 127);
        data[4] = static_cast<uint8_t>(128 + rx * 127);
        data[5] = static_cast<uint8_t>(128 + ry * 127);
    }

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
    auto setStick = [&data](int xIndex, int yIndex, int x, int y)
    {
        if (x == 0 && y == 0)
            return;
        data[xIndex] = static_cast<uint8_t>(x < 0 ? 0x00 : (x > 0 ? 0xFF : kPadStickCenter));
        data[yIndex] = static_cast<uint8_t>(y < 0 ? 0x00 : (y > 0 ? 0xFF : kPadStickCenter));
    };
    setStick(6, 7, keyAxis(KEY_A, KEY_D), keyAxis(KEY_W, KEY_S));

    // PS2X_INPUT_SCRIPT="<sec>:<BUTTON>:<duration>;..." scripted presses for testing.
    // Besides buttons, LS_UP/LS_DOWN/LS_LEFT/LS_RIGHT push the left stick.
    static constexpr uint16_t kStickUp = 0xF001u, kStickDown = 0xF002u, kStickLeft = 0xF003u, kStickRight = 0xF004u;
    static const std::vector<std::tuple<double, uint16_t, double>> script = []
    {
        std::vector<std::tuple<double, uint16_t, double>> out;
        const char *env = std::getenv("PS2X_INPUT_SCRIPT");
        if (!env)
            return out;
        const std::map<std::string, uint16_t> names = {
            {"UP", PAD_UP}, {"DOWN", PAD_DOWN}, {"LEFT", PAD_LEFT}, {"RIGHT", PAD_RIGHT},
            {"CROSS", PAD_CROSS}, {"CIRCLE", PAD_CIRCLE}, {"SQUARE", PAD_SQUARE}, {"TRIANGLE", PAD_TRIANGLE},
            {"L1", PAD_L1}, {"L2", PAD_L2}, {"R1", PAD_R1}, {"R2", PAD_R2},
            {"START", PAD_START}, {"SELECT", PAD_SELECT},
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
                const auto it = names.find(name);
                if (it != names.end())
                    out.emplace_back(std::atof(at.c_str()), it->second, dur.empty() ? 0.2 : std::atof(dur.c_str()));
            }
        }
        return out;
    }();
    if (!script.empty())
    {
        const double now = GetTime();
        for (const auto &[at, mask, duration] : script)
        {
            if (!(now >= at && now < at + duration))
                continue;
            switch (mask)
            {
            case kStickUp: setStick(6, 7, 0, -1); break;
            case kStickDown: setStick(6, 7, 0, 1); break;
            case kStickLeft: setStick(6, 7, -1, 0); break;
            case kStickRight: setStick(6, 7, 1, 0); break;
            default: clearBit(mask); break;
            }
        }
    }

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}
