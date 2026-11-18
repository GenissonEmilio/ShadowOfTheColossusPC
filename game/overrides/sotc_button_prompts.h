#pragma once

#include "runtime/desktop_input.h"
#include "raylib.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <string>

namespace sotc_button_prompts
{
    // Coordinates in the US game's 128 x 128 button_icon_set atlas.
    // The texture's existing silhouettes, directional arrows and Japanese
    // text remain useful to the guest's original sprite layouts.
    using Glyph = std::array<uint8_t, 7>;
    using Pixel = std::array<uint8_t, 4>;
    inline constexpr Glyph kA{14, 17, 17, 31, 17, 17, 17};
    inline constexpr Glyph kB{30, 17, 17, 30, 17, 17, 30};
    inline constexpr Glyph kS{15, 16, 16, 14, 1, 1, 30};
    inline constexpr Glyph kT{31, 4, 4, 4, 4, 4, 4};
    inline constexpr Glyph kX{17, 17, 10, 4, 10, 17, 17};
    inline constexpr Glyph kY{17, 17, 10, 4, 4, 4, 4};
    inline constexpr Glyph kMenu{0, 31, 0, 31, 0, 31, 0};
    inline constexpr Glyph kView{14, 10, 11, 31, 21, 21, 31};

    inline void applyXbox(uint8_t *rgba)
    {
        constexpr int pitch = 128;
        const auto pixelAt = [&](int x, int y) { return rgba + (y * pitch + x) * 4; };
        const auto paint = [&](int x, int y, Pixel color)
        {
            uint8_t *p = pixelAt(x, y);
            for (size_t i = 0; i < color.size(); ++i)
                p[i] = color[i];
        };
        const auto fill = [&](int x, int y, int w, int h, Pixel color)
        {
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i)
                    paint(x + i, y + j, color);
        };
        const auto draw = [&](int x, int y, int w, int h, const Glyph &glyph, Pixel color)
        {
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i)
                    if (glyph[j * 7 / h] & (16 >> (i * 5 / w)))
                        paint(x + i, y + j, color);
        };

        // Match the current atlas opacity if the game's CLUT is faded.
        const uint8_t opacity = pixelAt(9, 22)[3];
        const Pixel white{254, 254, 254, opacity};
        const Pixel gray{64, 64, 64, opacity};
        struct FaceButton { Glyph glyph; Pixel color; };
        const FaceButton buttons[]{
            {kY, {255, 212, 75, opacity}},
            {kB, {255, 105, 96, opacity}},
            {kA, {124, 231, 119, opacity}},
            {kX, {111, 191, 255, opacity}},
        };
        for (size_t i = 0; i < std::size(buttons); ++i)
        {
            const int x = static_cast<int>(i) * 20;
            for (int y = 23; y <= 32; ++y)
                for (int u = x + 2; u < x + 17; ++u)
                {
                    const uint8_t *p = pixelAt(u, y);
                    if (p[0] != p[1] || p[1] != p[2])
                        paint(u, y, gray);
                }
            draw(x + 6, 23, 7, 10, buttons[i].glyph, buttons[i].color);
        }

        // Retain L/R in the shoulder caps and replace their numeric suffix.
        for (int i = 0; i < 4; ++i)
        {
            const int x = 29 * i + 14;
            fill(x, 3, 11, 11, gray);
            draw(x + 1, 3, 9, 11, i % 2 == 0 ? kB : kT, white);
        }

        // Thumbsticks, View (Select) and Menu (Start).
        for (int x : {27, 64})
        {
            fill(x, 63, 8, 11, {});
            draw(x, 63, 8, 11, kS, white);
        }
        fill(0, 41, 20, 17, {});
        fill(2, 42, 15, 13, gray);
        draw(4, 44, 11, 9, kView, white);
        fill(98, 20, 21, 18, {});
        fill(101, 22, 15, 13, gray);
        draw(103, 24, 11, 9, kMenu, white);
    }

    // The variants share their source pixels/CLUT, but must occupy separate
    // GPU and pending-upload keys. The original keeps its existing ID.
    constexpr uint64_t textureKey(uint64_t original, uint64_t variant)
    {
        if (!variant) return original;
        return ((original ^ (variant * 0x9e3779b97f4a7c15ull)) & 0x03ffffffffffffffull) | (3ull << 59);
    }

    inline std::string keyLabel(int code)
    {
        if ((code >= KEY_A && code <= KEY_Z) || (code >= KEY_ZERO && code <= KEY_NINE))
            return std::string(1, static_cast<char>(code));
        if (code >= KEY_F1 && code <= KEY_F12) return "F" + std::to_string(code - KEY_F1 + 1);
        if (code >= KEY_KP_0 && code <= KEY_KP_9) return "KP" + std::to_string(code - KEY_KP_0);
        switch (code) {
        case KEY_NULL: return "-";
        case KEY_SPACE: return "SPC";
        case KEY_ESCAPE: return "ESC";
        case KEY_ENTER: return "ENT";
        case KEY_TAB: return "TAB";
        case KEY_BACKSPACE: return "BKSP";
        case KEY_INSERT: return "INS";
        case KEY_DELETE: return "DEL";
        case KEY_PAGE_UP: return "PGUP";
        case KEY_PAGE_DOWN: return "PGDN";
        case KEY_HOME: return "HOME";
        case KEY_END: return "END";
        case KEY_CAPS_LOCK: return "CAPS";
        case KEY_SCROLL_LOCK: return "SCRL";
        case KEY_NUM_LOCK: return "NUM";
        case KEY_PRINT_SCREEN: return "PRT";
        case KEY_PAUSE: return "PAUS";
        case KEY_LEFT_SHIFT: return "LSFT";
        case KEY_RIGHT_SHIFT: return "RSFT";
        case KEY_LEFT_CONTROL: return "LCTL";
        case KEY_RIGHT_CONTROL: return "RCTL";
        case KEY_LEFT_ALT: return "LALT";
        case KEY_RIGHT_ALT: return "RALT";
        case KEY_LEFT_SUPER: return "LSUP";
        case KEY_RIGHT_SUPER: return "RSUP";
        case KEY_KB_MENU: return "MENU";
        case KEY_KP_DECIMAL: return "KP.";
        case KEY_KP_DIVIDE: return "KP/";
        case KEY_KP_MULTIPLY: return "KP*";
        case KEY_KP_SUBTRACT: return "KP-";
        case KEY_KP_ADD: return "KP+";
        case KEY_KP_ENTER: return "KPEN";
        case KEY_KP_EQUAL: return "KP=";
        default:
            if (code > KEY_SPACE && code <= KEY_GRAVE) return std::string(1, static_cast<char>(code));
            return "#" + std::to_string(code);
        }
    }

    using Letter = std::array<uint8_t, 5>;
    inline constexpr Letter letter(char c)
    {
        constexpr Letter letters[]{
            {2,5,7,5,5}, {6,5,6,5,6}, {3,4,4,4,3}, {6,5,5,5,6}, {7,4,6,4,7}, {7,4,6,4,4},
            {3,4,5,5,3}, {5,5,7,5,5}, {7,2,2,2,7}, {1,1,1,5,2}, {5,5,6,5,5}, {4,4,4,4,7},
            {5,7,7,5,5}, {5,7,7,7,5}, {2,5,5,5,2}, {6,5,6,4,4}, {2,5,5,7,3}, {6,5,6,5,5},
            {3,4,2,1,6}, {7,2,2,2,2}, {5,5,5,5,7}, {5,5,5,5,2}, {5,5,7,7,5}, {5,5,2,5,5},
            {5,5,2,2,2}, {7,1,2,4,7},
        };
        constexpr Letter digits[]{
            {7,5,5,5,7}, {2,6,2,2,7}, {6,1,2,4,7}, {6,1,2,1,6}, {5,5,7,1,1},
            {7,4,6,1,6}, {3,4,6,5,2}, {7,1,2,2,2}, {2,5,2,5,2}, {2,5,3,1,6},
        };
        if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
        if (c >= '0' && c <= '9') return digits[c - '0'];
        switch (c) {
        case '-': return {0,0,7,0,0};
        case '=': return {0,7,0,7,0};
        case '+': return {0,2,7,2,0};
        case '*': return {5,2,7,2,5};
        case '.': return {0,0,0,0,2};
        case ',': return {0,0,0,2,4};
        case ';': return {0,2,0,2,4};
        case '\'': return {2,2,0,0,0};
        case '`': return {4,2,0,0,0};
        case '/': return {1,1,2,4,4};
        case '\\': return {4,4,2,1,1};
        case '[': return {3,2,2,2,3};
        case ']': return {6,2,2,2,6};
        default: return {5,7,5,7,5};
        }
    }

    struct Cell { int x, y, width, height; };

    struct KeyboardAtlas {
        uint8_t *pixels;

        uint8_t *at(int x, int y) const { return pixels + (y * 128 + x) * 4; }

        void fill(Cell area, Pixel color) const
        {
            for (int y = area.y; y < area.y + area.height; ++y)
                for (int x = area.x; x < area.x + area.width; ++x)
                    std::copy(color.begin(), color.end(), at(x, y));
        }

        uint8_t clear(Cell area) const
        {
            uint8_t alpha = 0;
            for (int y = area.y; y < area.y + area.height; ++y)
                for (int x = area.x; x < area.x + area.width; ++x)
                    alpha = std::max(alpha, at(x, y)[3]);
            fill(area, {});
            return alpha;
        }

        void text(Cell area, const std::string &label, Pixel ink) const
        {
            const int nominalWidth = static_cast<int>(label.size()) * 4 - 1;
            const int width = std::min(2 * nominalWidth, area.width);
            const int height = std::min(area.height, width >= 2 * nominalWidth ? 10 : 7);
            const int x = area.x + (area.width - width) / 2, y = area.y + (area.height - height) / 2;
            for (int i = 0; i < width; ++i) {
                const int column = i * nominalWidth / width;
                const auto glyph = letter(label[column / 4]);
                for (int j = 0; j < height; ++j)
                    if ((4 >> (column % 4)) & glyph[j * 5 / height])
                        std::copy(ink.begin(), ink.end(), at(x + i, y + j));
            }
        }

        void mouse(Cell area, int button, Pixel ink) const
        {
            if (area.width < (button <= 3 ? 7 : 12) || area.height < 10) {
                text(area, "M" + std::to_string(button), ink);
                return;
            }
            const int x = area.x + (area.width - (button <= 3 ? 7 : 12)) / 2;
            const int y = area.y + (area.height - 10) / 2;
            fill({x + 1, y, 5, 1}, ink);
            fill({x, y + 1, 1, 8}, ink);
            fill({x + 6, y + 1, 1, 8}, ink);
            fill({x + 1, y + 9, 5, 1}, ink);
            fill({x + 1, y + 4, 5, 1}, ink);
            if (button <= 3)
                fill({x + (button == 1 ? 1 : button == 2 ? 4 : 3), y + 1, button == 3 ? 1 : 2, 3}, ink);
            else
                text({x + 8, y, 4, 10}, std::to_string(button), ink);
        }

        void arrow(Cell area, int code, Pixel ink) const
        {
            const bool vertical = code == KEY_UP || code == KEY_DOWN;
            const bool reverse = code == KEY_DOWN || code == KEY_RIGHT;
            const int radius = std::min(area.width, area.height) >= 7 ? 2 : 1;
            const int x = area.x + area.width / 2, y = area.y + area.height / 2;
            for (int offset = -radius; offset <= radius; ++offset) {
                const auto point = [&](int u, int v) {
                    fill({x + (vertical ? u : v), y + (vertical ? v : u), 1, 1}, ink);
                };
                point(0, offset);
                point(offset, (reverse ? 1 : -1) * (radius - std::abs(offset)));
            }
        }

        void key(Cell area, int code, uint8_t alpha) const
        {
            if (!alpha) return;
            fill({area.x, area.y + 1, area.width, area.height - 2}, {30, 30, 32, alpha});
            fill({area.x + 1, area.y, area.width - 2, area.height - 1}, {97, 97, 102, alpha});
            fill({area.x + 1, area.y + 1, area.width - 2, area.height - 3}, {49, 49, 54, alpha});
            label({area.x + 1, area.y + 1, area.width - 2, area.height - 3}, code, alpha);
        }

        void label(Cell area, int code, uint8_t alpha) const
        {
            if (!alpha) return;
            const Pixel ink{254, 254, 254, alpha};
            if (code < 0) mouse(area, -code, ink);
            else if (code >= KEY_RIGHT && code <= KEY_UP) arrow(area, code, ink);
            else text(area, keyLabel(code), ink);
        }

        void replace(Cell area, int code) const { key(area, code, clear(area)); }

        void directions(Cell area, const std::array<int, 4> &controls) const
        {
            const auto alpha = clear(area);
            const int width = area.width / 3, height = area.height / 3;
            key({area.x + width, area.y, width, height}, controls[0], alpha);
            key({area.x + width, area.y + 2 * height, width, height}, controls[1], alpha);
            key({area.x, area.y + height, width, height}, controls[2], alpha);
            key({area.x + 2 * width, area.y + height, width, height}, controls[3], alpha);
        }
    };

    inline void apply(uint8_t *rgba, size_t size, const desktop_input::PromptProfile &profile)
    {
        using desktop_input::Control;
        using desktop_input::PromptStyle;
        if (size != 128 * 128 * 4) return;
        if (profile.style == PromptStyle::Xbox) applyXbox(rgba);
        if (profile.style != PromptStyle::Keyboard) return;

        const KeyboardAtlas atlas{rgba};
        const auto code = [&](Control name) { return profile.code(name); };
        const Control shoulders[]{Control::Look, Control::CameraReset, Control::Grab, Control::Crouch};
        for (int i = 0; i < 4; ++i) atlas.replace({29 * i, 0, 28, 19}, code(shoulders[i]));

        // The same face buttons are used for gameplay and for menu aliases.
        const Control faces[]{Control::Jump, Control::Action, Control::Horse, Control::Attack};
        for (int i = 0; i < 4; ++i) {
            int binding = code(faces[i]);
            if (i == 1 && !binding) binding = code(Control::Cancel);
            if (i == 2 && !binding) binding = code(Control::Confirm);
            atlas.replace({20 * i, 20, 19, 18}, binding);
        }
        atlas.directions({80, 20, 18, 18},
            {code(Control::MoveUp), code(Control::MoveDown), code(Control::MoveLeft), code(Control::MoveRight)});
        atlas.replace({98, 20, 21, 18}, code(Control::Start));
        atlas.replace({0, 40, 20, 18}, code(Control::Select));

        const auto up = code(Control::DpadUp), down = code(Control::DpadDown);
        const auto left = code(Control::WeaponLeft), right = code(Control::WeaponRight);
        atlas.directions({20, 39, 18, 19}, {up, down, left, right});
        atlas.replace({38, 39, 18, 19}, up);
        atlas.replace({56, 39, 18, 19}, right);
        atlas.replace({74, 39, 18, 19}, down);
        atlas.replace({92, 39, 18, 19}, left);

        // The map draws the stick diagrams without the click labels. Keep the
        // original artwork at x=1..16 and x=37..52 and replace only L3/R3 ink.
        constexpr Cell leftClick{18, 63, 17, 11}, rightClick{54, 63, 18, 11};
        atlas.label(leftClick, code(Control::L3), atlas.clear(leftClick));
        atlas.label(rightClick, code(Control::R3), atlas.clear(rightClick));
    }
}
