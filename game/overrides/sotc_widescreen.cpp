#if defined(__ANDROID__)

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_android_runtime.h"

#include <cmath>
#include <cstring>

namespace sotc_widescreen
{
namespace
{
    constexpr uint32_t kSetScreenWide = 0x01164468u; // set_screen_wide_and_progressive
    constexpr uint32_t kWideRatio = 0x0118FE64u;
    constexpr uint32_t kGetOption = 0x0134EF80u; // _optionWorkChk
    constexpr uint32_t kWideOptionIndex = 2;
    constexpr uint32_t kProgressiveOption = 0x011DF760u;
    constexpr uint32_t kGsReg = 0x001C5B00u;
    constexpr uint32_t kFtoi4 = 0x001BE700u; // iosFTOI4Vector
    constexpr uint32_t kReleaseFrame = 0x001C8078u;
    bool projectionReady = false;

    template <uint32_t Address>
    PS2Runtime::RecompiledFunction original = nullptr;

    void refreshProjection(uint8_t *rdram, float aspect)
    {
        uint32_t progressive = 0;
        std::memcpy(&progressive, rdram + kProgressiveOption, sizeof(progressive));
        // Keep the game's vertical projection and expand its horizontal frustum.
        // The guest's progressive mode uses a different base ratio.
        const float baseRatio = progressive ? 0.75f : 1.0f;
        const float ratio = aspect > 0.0f ? baseRatio * (aspect / ps2x::android::kOriginalAspect) : baseRatio;
        std::memcpy(rdram + kWideRatio, &ratio, sizeof(ratio));
        projectionReady = true;
        ps2x::android::setActiveWidescreenAspect(aspect);
    }

    template <uint32_t Address>
    void cameraHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == Address)
            refreshProjection(rdram, ps2x::android::requestedWidescreenAspect());
        original<Address>(rdram, ctx, runtime);
    }

    void releaseFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // The title and static menus can draw frames without rebuilding a camera.
        const float aspect = ps2x::android::requestedWidescreenAspect();
        if (projectionReady && aspect != ps2x::android::activeWidescreenAspect())
            refreshProjection(rdram, aspect);
        original<kReleaseFrame>(rdram, ctx, runtime);
    }

    void setScreenWide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const float aspect = ps2x::android::requestedWidescreenAspect();
        SET_GPR_U32(ctx, 4, aspect > 0.0f ? 1u : 0u);
        original<kSetScreenWide>(rdram, ctx, runtime);
        refreshProjection(rdram, aspect);
    }

    void getOption(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool wideOption = GPR_U32(ctx, 4) == kWideOptionIndex;
        original<kGetOption>(rdram, ctx, runtime);
        if (wideOption)
            SET_GPR_U32(ctx, 2, ps2x::android::requestedWidescreenAspect() > 0.0f ? 1u : 0u);
    }

    int32_t uiX(int32_t x, float aspect)
    {
        // Keep the original 4:3 layout centered in the expanded picture.
        const double scale = static_cast<double>(ps2x::android::kOriginalAspect) / aspect;
        return static_cast<int32_t>(std::lround(x * scale));
    }

    bool isUiCoordinate(uint32_t caller)
    {
        // Full-frame fades, cinema bars and screen copies fill the expanded picture.
        return (caller >= 0x010A24C0u && caller < 0x010A5270u) || // menu and map sprites
               (caller >= 0x0134B490u && caller < 0x0134B6F8u) || // subtitles
               (caller >= 0x013F6B20u && caller < 0x013F8AA0u) || // HUD gauges
               (caller >= 0x0114FD00u && caller < 0x011500F8u) || // spriteDraw and variants
               (caller >= 0x01155B98u && caller < 0x01155ED8u);   // window sprites
    }

    void gsReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = GPR_U32(ctx, 4);
        const float aspect = ps2x::android::activeWidescreenAspect();
        if (aspect <= 0.0f || (reg != 0x4u && reg != 0x5u && reg != 0xCu && reg != 0xDu) || !isUiCoordinate(GPR_U32(ctx, 31)))
            return original<kGsReg>(rdram, ctx, runtime);

        const uint64_t value = GPR_U64(ctx, 5);
        constexpr int32_t center = 2048 * 16; // GS 12.4
        const auto x = static_cast<int32_t>(value & 0xFFFFu) - center;
        SET_GPR_U64(ctx, 5, (value & ~uint64_t{0xFFFFu}) | static_cast<uint64_t>(center + uiX(x, aspect)));
        original<kGsReg>(rdram, ctx, runtime);
        SET_GPR_U64(ctx, 5, value);
    }

    void fontCoordinates(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t caller = GPR_U32(ctx, 31);
        const uint32_t output = GPR_U32(ctx, 4) & PS2_RAM_MASK & ~0xFu;
        original<kFtoi4>(rdram, ctx, runtime);
        const float aspect = ps2x::android::activeWidescreenAspect();
        if (aspect <= 0.0f || caller < 0x0108FF60u || caller >= 0x010905A0u)
            return;

        // The font packs its two relative X coordinates as (x0, y0, x1, y1).
        for (const uint32_t offset : {0u, 8u})
        {
            int32_t x;
            std::memcpy(&x, rdram + output + offset, sizeof(x));
            x = uiX(x, aspect);
            std::memcpy(rdram + output + offset, &x, sizeof(x));
        }
    }

    template <uint32_t Address>
    void hook(PS2Runtime &runtime, PS2Runtime::RecompiledFunction replacement)
    {
        if (runtime.hasFunction(Address))
        {
            original<Address> = runtime.lookupFunction(Address);
            runtime.registerFunction(Address, replacement);
        }
    }
}

    void install(PS2Runtime &runtime)
    {
        projectionReady = false;
        ps2x::android::setActiveWidescreenAspect(0.0f);
        if (!runtime.hasFunction(kSetScreenWide))
            return;
        hook<kReleaseFrame>(runtime, releaseFrame);
        hook<kSetScreenWide>(runtime, setScreenWide);
        hook<kGetOption>(runtime, getOption);
        // The game builds separate draw, clip and culling matrices.
        hook<0x01162E30u>(runtime, cameraHook<0x01162E30u>); // update_camera_obj
        hook<0x01164500u>(runtime, cameraHook<0x01164500u>); // get_aspect_ratio
        hook<0x011645B0u>(runtime, cameraHook<0x011645B0u>); // create_original_clip_matrix
        hook<0x01164710u>(runtime, cameraHook<0x01164710u>); // update_clip_screen
        hook<0x01164858u>(runtime, cameraHook<0x01164858u>); // distance to screen
        hook<0x01164AC0u>(runtime, cameraHook<0x01164AC0u>); // enlarged frustum
        // Installed after the native renderer so both renderers see the corrected GS coordinates.
        hook<kGsReg>(runtime, gsReg);
        hook<kFtoi4>(runtime, fontCoordinates);
    }
}

#else

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/host_menu.h"

#include <bit>
#include <cstring>

namespace sotc_widescreen
{
namespace
{
    constexpr uint32_t kSetScreenWide = 0x01164468u; // set_screen_wide_and_progressive
    constexpr uint32_t kWideRatio = 0x0118FE64u;
    constexpr uint32_t kGetOption = 0x0134EF80u; // _optionWorkChk
    constexpr uint32_t kWideOptionIndex = 2;
    constexpr uint32_t kProgressiveOption = 0x011DF760u; // option 6
    constexpr uint32_t kGsReg = 0x001C5B00u;
    constexpr uint32_t kFtoi4 = 0x001BE700u; // iosFTOI4Vector

    template <uint32_t Address>
    PS2Runtime::RecompiledFunction original = nullptr;

    bool selected()
    {
        return hostmenu::settings().screenMode.load(std::memory_order_relaxed) == hostmenu::kScreenWidescreen;
    }

    bool active()
    {
        return hostmenu::settings().widescreenActive.load(std::memory_order_relaxed);
    }

    void refreshProjection(uint8_t *rdram)
    {
        const bool wide = selected();
        uint32_t progressive = 0;
        std::memcpy(&progressive, rdram + kProgressiveOption, sizeof(progressive));
        // These are the original set_screen_wide_and_progressive values.
        // Refresh before building camera matrices so a save state never
        // overrides the host's current aspect selection.
        constexpr float wideInterlaced = std::bit_cast<float>(0x3Faaaaaau);
        const float ratio = wide ? (progressive ? 1.0f : wideInterlaced) : (progressive ? 0.75f : 1.0f);
        std::memcpy(rdram + kWideRatio, &ratio, sizeof(ratio));
        hostmenu::settings().widescreenActive.store(wide, std::memory_order_relaxed);
    }

    template <uint32_t Address>
    void cameraHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == Address)
            refreshProjection(rdram);
        original<Address>(rdram, ctx, runtime);
    }

    void setScreenWide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SET_GPR_U32(ctx, 4, selected() ? 1u : 0u);
        original<kSetScreenWide>(rdram, ctx, runtime);
    }

    void getOption(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool wideOption = GPR_U32(ctx, 4) == kWideOptionIndex;
        original<kGetOption>(rdram, ctx, runtime);
        if (wideOption)
            SET_GPR_U32(ctx, 2, selected() ? 1u : 0u);
    }

    int32_t uiX(int32_t x)
    {
        // Center the original 4:3 layout in the anamorphic 16:9 picture.
        // Int64 covers the full signed input range before rounding.
        return static_cast<int32_t>((static_cast<int64_t>(x) * 3 + (x < 0 ? -2 : 2)) / 4);
    }

    bool isUiCoordinate(uint32_t caller)
    {
        // Full-frame fades, cinema bars, screen copies, and the title
        // backdrop keep their original screen-wide coordinates.
        return (caller >= 0x010A24C0u && caller < 0x010A5270u) || // menu and map sprites
               (caller >= 0x0134B490u && caller < 0x0134B6F8u) || // subtitles
               (caller >= 0x013F6B20u && caller < 0x013F8AA0u) || // HUD gauges
               (caller >= 0x0114FD00u && caller < 0x011500F8u) || // spriteDraw and variants
               (caller >= 0x01155B98u && caller < 0x01155ED8u);   // window sprites
    }

    void gsReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = GPR_U32(ctx, 4);
        if (!active() || (reg != 0x4u && reg != 0x5u && reg != 0xCu && reg != 0xDu) || !isUiCoordinate(GPR_U32(ctx, 31)))
            return original<kGsReg>(rdram, ctx, runtime);

        const uint64_t value = GPR_U64(ctx, 5);
        constexpr int32_t center = 2048 * 16; // GS 12.4
        const auto x = static_cast<int32_t>(value & 0xFFFFu) - center;
        SET_GPR_U64(ctx, 5, (value & ~uint64_t{0xFFFFu}) | static_cast<uint64_t>(center + uiX(x)));
        original<kGsReg>(rdram, ctx, runtime);
        SET_GPR_U64(ctx, 5, value);
    }

    void fontCoordinates(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t caller = GPR_U32(ctx, 31);
        const uint32_t output = GPR_U32(ctx, 4) & PS2_RAM_MASK & ~0xFu;
        original<kFtoi4>(rdram, ctx, runtime);
        if (!active() || caller < 0x0108FF60u || caller >= 0x010905A0u)
            return;

        // Two relative X values in the font's packed (x0, y0, x1, y1).
        for (const uint32_t offset : {0u, 8u})
        {
            int32_t x;
            std::memcpy(&x, rdram + output + offset, sizeof(x));
            x = uiX(x);
            std::memcpy(rdram + output + offset, &x, sizeof(x));
        }
    }

    template <uint32_t Address>
    void hook(PS2Runtime &runtime, PS2Runtime::RecompiledFunction replacement)
    {
        if (runtime.hasFunction(Address))
        {
            original<Address> = runtime.lookupFunction(Address);
            runtime.registerFunction(Address, replacement);
        }
    }
}

    void install(PS2Runtime &runtime)
    {
        if (!runtime.hasFunction(kSetScreenWide))
            return;
        hostmenu::settings().widescreenActive.store(false, std::memory_order_relaxed);
        hook<kSetScreenWide>(runtime, setScreenWide);
        hook<kGetOption>(runtime, getOption);
        // Entry points that build the render, scissor, and culling matrices,
        // or read the aspect to update dependent camera state.
        hook<0x01162E30u>(runtime, cameraHook<0x01162E30u>); // update_camera_obj
        hook<0x01164500u>(runtime, cameraHook<0x01164500u>); // get_aspect_ratio
        hook<0x011645B0u>(runtime, cameraHook<0x011645B0u>); // create_original_clip_matrix
        hook<0x01164710u>(runtime, cameraHook<0x01164710u>); // update_clip_screen
        hook<0x01164858u>(runtime, cameraHook<0x01164858u>); // distance to screen
        hook<0x01164AC0u>(runtime, cameraHook<0x01164AC0u>); // enlarged frustum
        // Wrap the native GS recorder when present so both paths use the
        // same corrected sprite positions.
        hook<kGsReg>(runtime, gsReg);
        hook<kFtoi4>(runtime, fontCoordinates);
    }
}

#endif
