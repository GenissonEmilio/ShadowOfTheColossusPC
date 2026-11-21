// Synthetic inputs only: exercise the real native HUD/texture parser and registry.
#include "../../game/overrides/sotc_native.cpp"
#include "../../game/overrides/sotc_decomp.cpp"
#include "../../PS2Recomp/ps2xRuntime/src/lib/Kernel/GuestKernelMemory.h"
#include <cassert>
#include "runtime/ee_scheduler.h"

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x10000;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0x4000;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[0x4000] = {};

static void noop(uint8_t *, R5900Context *, PS2Runtime *) {}
static void originalPhysics(uint8_t *, R5900Context *c, PS2Runtime *) { SET_GPR_U32(c, 2, 1); }
static void experimentalPhysics(uint8_t *, R5900Context *c, PS2Runtime *) { SET_GPR_U32(c, 2, 2); }

namespace
{
    constexpr uint32_t kVectorEntry = 0x4000, kVectorResume = 0x4010, kVectorHandler = 0x4020;
    constexpr uint32_t kVectorNested = 0x4030, kVectorHandlerResume = 0x4040;
    bool vectorPreserved = false, nestedVectorPreserved = false;

    void vectorNested(uint8_t *, R5900Context *c, PS2Runtime *)
    {
        c->vu0_vf[20] = _mm_set1_ps(-1000.0f);
        c->vu0_q = 16.0f;
        c->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3f923456));
        c->pc = 0;
    }

    void vectorHandlerResume(uint8_t *, R5900Context *c, PS2Runtime *)
    {
        nestedVectorPreserved = _mm_cvtss_f32(c->vu0_vf[20]) == 500.0f && c->vu0_q == 4.0f;
        c->pc = 0;
    }

    void vectorHandler(uint8_t *, R5900Context *c, PS2Runtime *runtime)
    {
        c->vu0_vf[20] = _mm_set_ps(1.0f, -100.0f, -900.0f, 500.0f);
        c->vu0_acc = _mm_set1_ps(-1.0f);
        c->vu0_q = 4.0f;
        c->vi[7] = 99;
        c->vu0_clip_flags = 0x123456;
        c->pc = kVectorHandlerResume;
        GuestInvocation nested{};
        nested.kind = GuestInvocationKind::Interrupt;
        nested.context = *c;
        nested.context.pc = kVectorNested;
        SET_GPR_U32(&nested.context, 29, 0);
        SET_GPR_U32(&nested.context, 31, 0);
        runtime->eeScheduler().invokeCurrent(std::move(nested));
    }

    void vectorResume(uint8_t *, R5900Context *c, PS2Runtime *runtime)
    {
        alignas(16) float p[4], a[4];
        _mm_store_ps(p, c->vu0_vf[20]);
        _mm_store_ps(a, c->vu0_acc);
        vectorPreserved = p[0] == 1000.0f && p[1] == 5.0f && p[2] == -229.0f && p[3] == 1.0f &&
                          a[0] == 0.0f && a[1] == 1.0f && a[2] == 0.0f && a[3] == 0.0f &&
                          c->vu0_q == 0.5f && c->vi[7] == 7 && c->vu0_clip_flags == 0x42 &&
                          _mm_cvtsi128_si32(_mm_castps_si128(c->vu0_r)) == 0x3f923456;
        runtime->requestStop();
        c->pc = 0;
    }

    void vectorEntry(uint8_t *, R5900Context *c, PS2Runtime *runtime)
    {
        c->vu0_vf[20] = _mm_set_ps(1.0f, -229.0f, 5.0f, 1000.0f);
        c->vu0_acc = _mm_set_ps(0.0f, 0.0f, 1.0f, 0.0f);
        c->vu0_q = 0.5f;
        c->vi[7] = 7;
        c->vu0_clip_flags = 0x42;
        c->pc = kVectorResume;
        GuestInvocation irq{};
        irq.kind = GuestInvocationKind::Interrupt;
        irq.context = *c;
        irq.context.pc = kVectorHandler;
        SET_GPR_U32(&irq.context, 29, 0);
        SET_GPR_U32(&irq.context, 31, 0);
        runtime->eeScheduler().invokeCurrent(std::move(irq));
    }
}

int main()
{
    {
        PS2Runtime vectors;
        std::vector<uint8_t> memory(PS2_RAM_SIZE);
        vectors.registerFunction(kVectorEntry, vectorEntry);
        vectors.registerFunction(kVectorResume, vectorResume);
        vectors.registerFunction(kVectorHandler, vectorHandler);
        vectors.registerFunction(kVectorNested, vectorNested);
        vectors.registerFunction(kVectorHandlerResume, vectorHandlerResume);
        R5900Context initial{};
        initial.pc = kVectorEntry;
        vectors.eeScheduler().reset(memory.data(), initial);
        vectors.eeScheduler().run();
        assert(vectorPreserved && nestedVectorPreserved &&
               "interrupts must preserve suspended vector calculations and advance the shared RNG");
    }
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

    // An in-place edit outside BOTH sparse samples used to stay stale
    // forever. Bounded full checks must find it, then track animation on
    // every frame (and every draw once the packet is known to be mutable).
    constexpr uint32_t packet = 0x8000, packetSize = 4096;
    native_renderer::tuning().packetQuickCheck = true;
    ++g_frame;
    uint64_t packetKey = packetId(packet, packetSize, true);
    const auto sparse = sample(packet, packetSize, 32);
    const auto quick = quickSample(packet, packetSize);
    put(packet + 4, 0x3f801234);
    assert(sample(packet, packetSize, 32) == sparse);
    assert(quickSample(packet, packetSize) == quick);
    bool detected = false;
    for (unsigned i = 0; i < 16; ++i) {
        ++g_frame;
        const auto changed = packetId(packet, packetSize, true);
        if (changed != packetKey) {
            assert(g_packetChangedInPlace);
            packetKey = changed;
            detected = true;
            break;
        }
    }
    assert(detected);
    ++g_frame;
    put(packet + 8, 0x3f812345);
    auto changedPacket = packetId(packet, packetSize, true);
    assert(changedPacket != packetKey && g_packetChangedInPlace);
    put(packet + 12, 0x3f823456); // same frame, same mutable buffer
    packetKey = packetId(packet, packetSize, true);
    assert(packetKey != changedPacket && g_packetChangedInPlace);

    // Streaming reuse after an absent frame is checked immediately, even
    // when the four shortcut words match. So is quick-check disabled.
    constexpr uint32_t streamed = 0xa000;
    const auto streamedKey = packetId(streamed, packetSize, true);
    g_frame += 2;
    put(streamed + 4, 0x40567890);
    assert(packetId(streamed, packetSize, true) != streamedKey);
    constexpr uint32_t strict = 0xc000;
    packetKey = packetId(strict, packetSize, true);
    native_renderer::tuning().packetQuickCheck = false;
    ++g_frame;
    put(strict + 4, 0x40678901);
    assert(packetId(strict, packetSize, true) != packetKey);
    native_renderer::tuning().packetQuickCheck = true;
    packetKey = packetId(strict, packetSize, false);
    put(strict + 8, 0x40789012);
    assert(packetId(strict, packetSize, false) != packetKey);

    constexpr uint32_t animated = 0x10000;
    put(animated, 0x324f544e);
    put(animated + 0x14, 0x1000); put(animated + 0x18, 0x800);
    put(animated + 0x1c, 0x00771013); // 128x128 PSMT8
    put(animated + 0x800, 0x80000000);
    put(animated + 0x804, 0x80ffffff);
    ++g_frame;
    previous = texture(animated);
    const auto pixelsSample = sample(animated + 0x1000, 16384, 64);
    ram[animated + 0x1004] = 1;
    assert(sample(animated + 0x1000, 16384, 64) == pixelsSample);
    detected = false;
    for (unsigned i = 0; i < 16; ++i) {
        ++g_frame;
        const auto changed = texture(animated);
        if (changed != previous) {
            previous = changed;
            detected = true;
            break;
        }
    }
    assert(detected);
    ++g_frame;
    ram[animated + 0x1005] = 1;
    assert(texture(animated) != previous);
    native_renderer::tuning().fastHooks = false;
    previous = texture(animated);
    ram[animated + 0x1006] = 1;
    assert(texture(animated) != previous);
    native_renderer::tuning().fastHooks = true;

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

    // The twelfth 16 KiB callback stack used to overlap TLS; the next one
    // overlapped RPC packets/descriptors. Simulate real stack writes through
    // every allocation, keeping sentinel bytes in the service pools.
    using namespace ps2x::kernel_memory;
    constexpr uint32_t servicesEnd = kBootModePoolBase + kBootModePoolBytes;
    std::fill(ram.begin() + kRpcPacketPoolBase, ram.begin() + servicesEnd, 0xa5);
    assert(runtime.reserveAsyncCallbackStack(0, 16) == 0);
    unsigned stacks = 0;
    uint32_t previousBase = kCallbackStackTop;
    while (const uint32_t sp = runtime.reserveAsyncCallbackStack(kCallbackStackSize, 16)) {
        const uint32_t top = sp + 16, base = top - kCallbackStackSize;
        assert(base >= kCallbackStackFloor && top <= previousBase);
        assert(top <= kRpcPacketPoolBase || base >= servicesEnd);
        std::fill(ram.begin() + base, ram.begin() + top, 0x5a);
        previousBase = base;
        ++stacks;
        assert(stacks <= 25);
    }
    assert(stacks == 23);
    assert(runtime.reserveAsyncCallbackStack(kCallbackStackSize, 16) == 0);
    assert(std::all_of(ram.begin() + kRpcPacketPoolBase, ram.begin() + servicesEnd,
                       [](uint8_t value) { return value == 0xa5; }));
    PS2Runtime aligned;
    // A rejected oversized request must not consume the pool. Alignment
    // padding also belongs to the reserved stack interval.
    assert(aligned.reserveAsyncCallbackStack(0x40000, 16) == 0);
    assert(aligned.reserveAsyncCallbackStack(0x10000, 16) == kCallbackStackTop - 16);
    assert(aligned.reserveAsyncCallbackStack(0x10000, 16) == kCallbackStackTop - 0x10000 - 16);
    assert(aligned.reserveAsyncCallbackStack(0x10000, 0x10000) == kRpcPacketPoolBase - 16);
    std::puts("PASS: HUD, CLUT/RGBA, geometry/texture edits, original collision dispatch, isolated vector contexts/RNG and callback stacks");
}
