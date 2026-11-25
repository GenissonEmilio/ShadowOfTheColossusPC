// ROM-free exercise of the installed guest hooks and the desktop fitter.
#include "../../game/overrides/sotc_widescreen.cpp"
#include "runtime/desktop_video.h"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace hostmenu
{
    Settings &settings()
    {
        static Settings settings;
        return settings;
    }
}

int main()
{
    using namespace sotc_widescreen;
    std::vector<uint8_t> rdram(kProgressiveOption + 4);
    const auto ratio = [&] {
        uint32_t bits;
        std::memcpy(&bits, rdram.data() + kWideRatio, sizeof(bits));
        return bits;
    };
    hostmenu::settings().screenMode = hostmenu::kScreenWidescreen;
    refreshProjection(rdram.data());
    assert(active() && ratio() == 0x3FAAAAAAu);
    rdram[kProgressiveOption] = 1;
    refreshProjection(rdram.data());
    assert(active() && ratio() == 0x3F800000u);
    hostmenu::settings().screenMode = hostmenu::kScreenFit;
    refreshProjection(rdram.data());
    assert(!active() && ratio() == 0x3F400000u);
    rdram[kProgressiveOption] = 0;
    refreshProjection(rdram.data());
    assert(!active() && ratio() == 0x3F800000u);

    assert(uiX(-4096) == -3072 && uiX(0) == 0 && uiX(4096) == 3072);
    assert(isUiCoordinate(0x013F6B24u)); // HUD
    assert(isUiCoordinate(0x0134B494u)); // subtitle
    assert(!isUiCoordinate(0x0108A58Cu)); // cinema bar
    assert(!isUiCoordinate(0x0108A40Cu)); // fade
    assert(!isUiCoordinate(0x010F37CCu)); // screen copy

    const auto view = [](int width, int height, hostmenu::ScreenMode mode) {
        return desktop_video::destination(width, height, 512, 448, mode);
    };
    const auto expect = [](Rectangle actual, Rectangle expected) {
        const auto equal = [](float a, float b) { return std::abs(a - b) < 0.01f; };
        assert(equal(actual.x, expected.x) && equal(actual.y, expected.y));
        assert(equal(actual.width, expected.width) && equal(actual.height, expected.height));
    };
    expect(view(1920, 1080, hostmenu::kScreenWidescreen), {0, 0, 1920, 1080});
    expect(view(3440, 1440, hostmenu::kScreenWidescreen), {440, 0, 2560, 1440});
    expect(view(800, 600, hostmenu::kScreenWidescreen), {0, 75, 800, 450});
    expect(view(1920, 1080, hostmenu::kScreenFit), {240, 0, 1440, 1080});
    expect(view(3440, 1440, hostmenu::kScreenStretch), {0, 0, 3440, 1440});
    expect(view(1920, 1080, hostmenu::kScreenInteger), {448, 92, 1024, 896});
    std::puts("PASS: guest aspect, UI proportions, full-frame effects and desktop presentation");
}
