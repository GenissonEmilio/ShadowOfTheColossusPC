// Synthetic inputs only: exercise the real native HUD/texture parser and registry.
#include "../../game/overrides/sotc_native.cpp"
#include "../../game/overrides/sotc_decomp.cpp"
#include <cassert>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x10000;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0x4000;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[0x4000] = {};

static void noop(uint8_t *, R5900Context *, PS2Runtime *) {}
static void originalPhysics(uint8_t *, R5900Context *c, PS2Runtime *) { SET_GPR_U32(c, 2, 1); }
static void experimentalPhysics(uint8_t *, R5900Context *c, PS2Runtime *) { SET_GPR_U32(c, 2, 2); }

int main()
{
    using namespace sotc_native;
    std::vector<uint8_t> ram(PS2_RAM_SIZE);
    R5900Context ctx{};
    g_ram = ram.data();
    const auto put = [&](uint32_t at, uint32_t v) { std::memcpy(ram.data() + at, &v, 4); };

    // Calls from every HUD helper must reach the same GS parser. These are
    // original test points in clip space, not extracted game instructions.
    g_gsReg = noop;
    g_requested = 0;
    g_layers = {25};
    const uint32_t callers[] = {0x013F7000, 0x013F7D38, 0x013F8238, 0x013F8540, 0x013F88E8};
    for (uint32_t caller : callers) {
        g_2d = {};
        SET_GPR_U32(&ctx, 31, caller);
        const auto feed = [&](uint32_t reg, uint64_t value) {
            SET_GPR_U32(&ctx, 4, reg); SET_GPR_U64(&ctx, 5, value);
            gsRegHook(ram.data(), &ctx, nullptr);
        };
        feed(0, 3); // untextured triangle
        feed(1, 0x80808080);
        feed(5, 0x80008000);
        feed(5, 0x80008020);
        feed(5, 0x80208000);
        assert(g_2d.batch.size() == 3);
    }
    g_2d = {};
    feed2D(0, 0); // point -> a one-pixel quad
    feed2D(1, 0x80808080);
    feed2D(5, 0x80008000);
    assert(g_2d.batch.size() == 6);
    const auto &a = g_2d.batch;
    assert(std::fabs((a[1].x - a[0].x) * 256 - 1) < 1e-6f);
    assert(std::fabs((a[2].y - a[0].y) * 224 + 1) < 1e-6f);

    // An animation can change any CLUT entry, including entries that the
    // old sparse sample never read. Every mutation must change the GPU key.
    constexpr uint32_t h = 0x2000, clut = h + 0x200;
    put(h, 0x324f544e); // NTO2 magic
    put(h + 0x14, 0x100); put(h + 0x18, 0x200);
    put(h + 0x1c, 0x00331013); // 8x8 PSMT8, one level
    std::fill(ram.begin() + h + 0x100, ram.begin() + h + 0x140, 1);
    native_renderer::tuning().fastHooks = true;
    for (unsigned i = 0; i < 256; ++i) put(clut + i * 4, 0x80010203);
    uint64_t previous = texture(h);
    for (unsigned i = 0; i < 256; ++i) {
        put(clut + i * 4, 0x20090807);
        ++g_frame;
        const auto changed = texture(h);
        if (previous == changed) {
            std::fprintf(stderr, "CLUT entry %u: unchanged key %016llx, format %08x\n", i, (unsigned long long)changed, word(h + 0x1c));
            return 1;
        }
        previous = changed;
    }
    // Palette-free 32-bit textures must include changes beyond the old
    // width*height-byte prefix in their invalidation sample.
    put(h + 0x1c, 0x00331000);
    ++g_frame;
    previous = texture(h);
    put(h + 0x100 + 200, 0x80abcdef);
    ++g_frame;
    assert(previous != texture(h));

    // Default physics stays authored guest code; opt-in and difftest remain
    // possible. Registry fallbacks retain the original callable function.
    PS2Runtime runtime;
    runtime.registerFunction(0x2000, originalPhysics);
    sotc_decomp::registry().push_back({0x2000, "testPhysics", experimentalPhysics, false, false, false, true});
    unsetenv("SOTC_NATIVE_COLLISION");
    unsetenv("SOTC_DECOMP"); unsetenv("SOTC_DECOMP_SKIP"); unsetenv("SOTC_DIFFTEST");
    sotc_decomp::install(runtime);
    assert(runtime.lookupFunction(0x2000) == originalPhysics);
    assert(sotc_decomp::recompiled(0x2000) == originalPhysics);
    setenv("SOTC_NATIVE_COLLISION", "1", 1);
    sotc_decomp::install(runtime);
    assert(runtime.lookupFunction(0x2000) == experimentalPhysics);
    std::puts("PASS: HUD helpers, GS points, every CLUT entry, RGBA texture tail, original collision dispatch");
}
