#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>

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

    inline void apply(uint8_t *rgba, size_t size)
    {
        constexpr int pitch = 128;
        if (size != pitch * pitch * 4) return;
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
}
