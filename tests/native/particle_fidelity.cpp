// Synthetic particle packets and emitter inputs; no ROM, save or GPU.
#ifndef SOTC_NATIVE_TEST_SOURCE
#define SOTC_NATIVE_TEST_SOURCE "../../game/overrides/sotc_native.cpp"
#endif
#include SOTC_NATIVE_TEST_SOURCE
#include <array>
#include <limits>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x10000;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0x4000;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[0x4000] = {};

namespace {
    int checks = 0, failures = 0;
    void check(bool condition, const char *message) {
        ++checks;
        std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
        failures += !condition;
    }
    constexpr uint32_t kPacket = 0x9000, kParticle = 0xa000, kCamera = 0xb000;
    struct Args { uint32_t blend; float u, v; } expected{};
    unsigned matCalls = 0, vtxCalls = 0;
    bool originalInputsIntact = true;
    std::vector<native_renderer::DrawState> draws;
    std::vector<native_renderer::Vertex> vertices;

    uint32_t bits(float f) { uint32_t value; std::memcpy(&value, &f, 4); return value; }
    void store64(uint8_t *ram, uint32_t address, uint64_t value) {
        std::memcpy(ram + address, &value, sizeof(value));
    }

    // A synthetic GS packet with the documented material contract. The actual
    // builder writes TEST_1 / CLAMP_1 at +0x20 / +0x30 (A+D register IDs follow
    // at +8). No instructions or data from the game are included here.
    void originalMaterial(uint8_t *ram, R5900Context *ctx, PS2Runtime *) {
        ++matCalls;
        originalInputsIntact &= GPR_U32(ctx, 4) == expected.blend &&
                               bits(ctx->f[12]) == bits(expected.u) &&
                               bits(ctx->f[13]) == bits(expected.v);
        const uint64_t clamp = (FPU_C_OLE_S(ctx->f[12], 1.0f) ? 1u : 0u) |
                               (FPU_C_OLE_S(ctx->f[13], 1.0f) ? 4u : 0u);
        store64(ram, kPacket + 0x20, 0x51011u);
        store64(ram, kPacket + 0x28, 0x47u); // TEST_1
        store64(ram, kPacket + 0x30, clamp);
        store64(ram, kPacket + 0x38, 0x08u); // CLAMP_1
        // Caller-saved registers are not the material arguments after return.
        SET_GPR_U32(ctx, 4, 99);
        ctx->f[12] = ctx->f[13] = 8.0f;
    }
    void originalVertices(uint8_t *, R5900Context *ctx, PS2Runtime *) {
        ++vtxCalls;
        originalInputsIntact &= GPR_U32(ctx, 4) == kParticle && GPR_U32(ctx, 5) == 1;
        SET_GPR_U32(ctx, 4, 0);
        SET_GPR_U32(ctx, 5, 0);
    }
}

// Observe the hook's renderer boundary, before any backend or GPU is created.
extern "C" void __wrap__ZN15native_renderer4drawEmPKfRKNS_9DrawStateE(
    uint64_t, const float *, const native_renderer::DrawState &state) {
    draws.push_back(state);
}
extern "C" void __wrap__ZN15native_renderer10uploadMeshEmPKNS_6VertexEjb(
    uint64_t, const native_renderer::Vertex *p, uint32_t count, bool) {
    vertices.assign(p, p + count);
}

int main() {
    using namespace sotc_native;
    std::vector<uint8_t> ram(PS2_RAM_SIZE);
    g_ram = ram.data(); g_frame = 0; ps2_stats::vif1Chains.store(0);
    auto putFloat = [&](uint32_t at, float value) { std::memcpy(ram.data() + at, &value, 4); };
    const std::array<float, 16> identity = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::memcpy(ram.data() + kCamera + 0x5a0, identity.data(), 64);
    g_cameraContext = kCamera; g_requested = 0; g_layers = {18};
    g_ptclMat = originalMaterial; g_ptclVtx = originalVertices;
    native_renderer::tuning().particles = true;
    native_renderer::tuning().fastHooks = false;
    // One billboard; q2 also provides the second endpoint for streak types.
    putFloat(kParticle + 8, 4); putFloat(kParticle + 12, 1);
    for (unsigned i = 0; i < 4; ++i) putFloat(kParticle + 16 + i * 4, 0.5f);
    putFloat(kParticle + 36, 1); putFloat(kParticle + 40, 4);

    auto drawParticle = [&](uint32_t blend, float u, float v, uint32_t type = 1u) {
        draws.clear(); vertices.clear();
        expected = {blend, u, v};
        R5900Context ctx{};
        SET_GPR_U32(&ctx, 4, blend); ctx.f[12] = u; ctx.f[13] = v;
        ptclMatHook(ram.data(), &ctx, nullptr);
        g_ptclEnvState = {};
        auto &e = g_ptclEnvState;
        e.type = type;
        std::copy(identity.begin(), identity.end(), e.m);
        for (unsigned i = 0; i < 3; ++i) e.axes[i][i] = 1.0f;
        SET_GPR_U32(&ctx, 4, kParticle); SET_GPR_U32(&ctx, 5, 1);
        ptclVtxHook(ram.data(), &ctx, nullptr);
        return draws.size() == 1 && !vertices.empty();
    };
    auto packet64 = [&](uint32_t offset) {
        uint64_t value; std::memcpy(&value, ram.data() + kPacket + offset, 8); return value;
    };
    auto matchesClampPacket = [&]() {
        const uint64_t c = packet64(0x30);
        const unsigned hostClamp = (c & 1u) | ((c >> 1u) & 2u);
        return draws.size() == 1 && draws.back().clamp == hostClamp;
    };

    check(drawParticle(0, 1.0f, 1.0f), "normal particle emits the captured vertices after the original call");
    const uint64_t test = packet64(0x20);
    check(packet64(0x28) == 0x47u && packet64(0x38) == 0x08u &&
          (test & 1u) == 1u && ((test >> 1u) & 7u) == 0u &&
          ((test >> 12u) & 3u) == 1u && ((test >> 16u) & 1u) == 1u &&
          ((test >> 17u) & 3u) == 2u,
          "synthetic TEST_1 offsets encode NEVER / FB_ONLY and GEQUAL depth test");
    check(draws.size() == 1 && draws.back().noDepthWrite && !draws.back().screen &&
          !draws.back().background, "normal particles test scene depth without writing Z");
    check(matchesClampPacket(), "unit particle extents clamp both axes like the material packet");

    drawParticle(0, 2.0f, 1.0f);
    check(matchesClampPacket(), "particle U repeat and V clamp follow f12 / f13 independently");
    drawParticle(0, 1.0f, 2.0f);
    check(matchesClampPacket(), "particle U clamp and V repeat follow f12 / f13 independently");
    drawParticle(0, 2.0f, 3.0f);
    check(matchesClampPacket() && draws.back().clamp == 0,
          "repeated particle extents retain repeat on both axes");
    drawParticle(0, std::nextafter(1.0f, 2.0f), 1.0f);
    check(matchesClampPacket(), "particle clamp threshold keeps the original inclusive comparison");
    drawParticle(0, -1.0f, 0.0f);
    check(matchesClampPacket(), "nonpositive particle extents retain the original comparison result");
    drawParticle(0, std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::quiet_NaN());
    check(matchesClampPacket(), "particle clamp uses the EE comparison contract for special float bits");

    drawParticle(1, 1.0f, 1.0f, 3);
    check(draws.size() == 1 && draws.back().blend == native_renderer::Blend::Additive &&
          vertices.size() == 6,
          "additive streak retains its blend and six vertices");
    drawParticle(2, 1.0f, 1.0f, 5);
    check(draws.size() == 1 && draws.back().blend == native_renderer::Blend::Subtract &&
          draws.back().noDepthWrite && vertices.size() == 12,
          "subtractive lit streak keeps blend and twelve vertices with no Z writes");
    drawParticle(3, 1.0f, 1.0f, 7);
    check(draws.size() == 1 && draws.back().blend == native_renderer::Blend::Normal &&
          draws.back().noDepthWrite && draws.back().order == 18,
          "default particle blend keeps its layer and FB_ONLY depth behavior");
    check(originalInputsIntact && matCalls == 10 && vtxCalls == 10,
          "both original functions receive unchanged inputs exactly once per draw");
    check(native_renderer::DrawState{}.noDepthWrite == false && native_renderer::DrawState{}.clamp == 0,
          "ordinary draw defaults remain unchanged outside the particle hooks");
    std::printf("%s: %d particle-fidelity checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
