#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

void installSotcPalNativeComparison(PS2Runtime &) {}

namespace
{
    constexpr uint32_t kGamecoreEntry = 0x01355490u;
    constexpr uint32_t kBootChooseLanguage = 0x0138C2A8u;
    constexpr uint32_t kBootReturn = 0x01361954u;
    constexpr uint32_t kLanguageChosen = 0x01477F3Cu;

    unsigned originalCalls = 0;

    void original(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ++originalCalls;
        SET_GPR_U64(ctx, 2, 0);
        ctx->pc = GPR_U32(ctx, 31);
    }

    void check(PS2Runtime &runtime, const char *language, bool capture, uint32_t expectedLanguage)
    {
        if (language)
            assert(setenv("SOTC_CAMPAIGN_PAL_LANGUAGE", language, 1) == 0);
        else
            assert(unsetenv("SOTC_CAMPAIGN_PAL_LANGUAGE") == 0);
        if (capture)
            assert(setenv("PS2X_MODULE_CAPTURE", "1", 1) == 0);
        else
            assert(unsetenv("PS2X_MODULE_CAPTURE") == 0);

        std::vector<uint8_t> ram(PS2_RAM_SIZE, 0x9A);
        R5900Context ctx{};
        SET_GPR_U32(&ctx, 31, kBootReturn);
        const auto before = originalCalls;
        assert(runtime.callGuest(ram.data(), &ctx, kBootChooseLanguage, kBootReturn,
                                 kBootReturn - 8, PS2Runtime::GuestBranchKind::DirectCall, "JAL"));
        assert(ctx.pc == kBootReturn);
        assert(originalCalls == before + (expectedLanguage == 0));
        assert(GPR_U32(&ctx, 2) == (expectedLanguage != 0));

        std::array<uint32_t, 4> words{};
        std::memcpy(words.data(), ram.data() + kLanguageChosen - 4, sizeof(words));
        assert(words[0] == 0x9A9A9A9Au && words[3] == 0x9A9A9A9Au);
        assert(words[1] == (expectedLanguage ? 1u : 0x9A9A9A9Au));
        assert(words[2] == (expectedLanguage ? expectedLanguage : 0x9A9A9A9Au));
    }
}

int main()
{
    auto runtime = std::make_unique<PS2Runtime>();
    assert(runtime->registerFunction(kGamecoreEntry, original));
    assert(runtime->registerFunction(kBootChooseLanguage, original));

    ps2_game_overrides::applyMatching(*runtime, "SCUS_974.72", 0x00100010u, 0x5DC13BBCu, true);
    assert(runtime->lookupFunction(kBootChooseLanguage) == original);
    ps2_game_overrides::applyMatching(*runtime, "SCES_533.26", 0x00100010u, 0x5DC13BBCu, true);
    assert(runtime->lookupFunction(kBootChooseLanguage) != original);

    check(*runtime, nullptr, false, 0);
    check(*runtime, "pt-BR", false, 0);
    check(*runtime, "fr", true, 0);
    check(*runtime, "en", false, 1);
    check(*runtime, "fr", false, 2);
    check(*runtime, "es", false, 3);
    check(*runtime, "de", false, 4);
    check(*runtime, "it", false, 5);
}
