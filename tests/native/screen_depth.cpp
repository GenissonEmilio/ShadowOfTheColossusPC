// Synthetic GS packets and GPU pixels; no guest instructions or game assets.
#define GL_GLEXT_PROTOTYPES 1
#define GLFW_INCLUDE_NONE
#include "GLFW/glfw3.h"
#include "raylib.h"
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
    int failures = 0;
    void check(bool ok, const char *message) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) ++failures;
    }
}

int main() {
    using namespace sotc_native;
    std::vector<uint8_t> ram(PS2_RAM_SIZE); g_ram = ram.data();
    auto putFloat = [&](uint32_t at, float value) { std::memcpy(ram.data() + at, &value, 4); };
    constexpr uint32_t camera = 0x1000;
    auto projection = [&](float zA, float zB) {
        g_cameraContext = camera;
        putFloat(camera + 0x4A0u + 40u, zA);
        putFloat(camera + 0x4A0u + 56u, zB);
    };

    // Independently chosen projection whose closest GS depths are nearer
    // than the native clip plane. A world fragment at w=.105 is visible.
    projection(10.0f, 100.0f);
    const float near = viewDepth(0xFFFFFFu);
    const float worldW = 0.105f;
    check(near == 0.1f, "maximum GS Z lands on the native near boundary");
    check(viewDepth(0xFFFFFFFFu) == 0.1f, "maximum XYZ2 depth also lands on the near boundary");
    check(1.0f - 0.1f / near <= 1.0f - 0.1f / worldW,
          "maximum GS Z beats a visible world fragment at w=.105");
    check(viewDepth(60u) == 2.0f, "ordinary haze depth keeps the projection inverse");
    check(viewDepth(10u) == 1e6f && viewDepth(0u) == 1e6f, "nonpositive depth denominator keeps far fallback");
    projection(0.0f, std::numeric_limits<float>::infinity());
    check(viewDepth(1u) == 1e6f, "nonfinite inverse keeps far fallback");
    projection(0.0f, 1e9f);
    check(viewDepth(1u) == 1e6f, "finite far depth remains bounded");
    g_cameraContext = 0;
    check(viewDepth(0xFFFFFFu) == 1.0f, "missing camera keeps the existing neutral fallback");
    projection(10.0f, 100.0f);

    setenv("SOTC_NATIVE_GL", "1", 1);
    SetTraceLogLevel(LOG_WARNING); SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(160, 120, "SotC GS screen-depth regression");
    if (!IsWindowReady()) return 2;
    auto *window = glfwGetCurrentContext();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    auto *worker = glfwCreateWindow(16, 16, "depth worker", nullptr, nullptr);
    if (!worker) return 2;
    glfwMakeContextCurrent(window);
    native_renderer::setGlContext([worker] { glfwMakeContextCurrent(worker); });
    native_renderer::tuning().renderThread = false;
    native_renderer::tuning().asyncBuilds = false;
    native_renderer::tuning().midFrameRedraw = 0;
    native_renderer::tuning().hazeSource = 0;

    using V = native_renderer::Vertex;
    const V world[] = {{-1,-1,0,0,0,1,0,0,0,255,0,128}, {1,-1,0,0,0,1,1,0,0,255,0,128}, {1,1,0,0,0,1,1,1,0,255,0,128},
                       {-1,-1,0,0,0,1,0,0,0,255,0,128}, {1,1,0,0,0,1,1,1,0,255,0,128}, {-1,1,0,0,0,1,0,1,0,255,0,128}};
    native_renderer::uploadMesh(7, world, 6);
    float clip[16] = {worldW,0,0,0,0,worldW,0,0,0,0,1,0,0,0,0,worldW};
    auto frame = [&](bool overlay) {
        native_renderer::beginFrame();
        native_renderer::DrawState state; state.order = 2; state.blend = native_renderer::Blend::Opaque;
        native_renderer::draw(7, clip, state);
        if (overlay) {
            // Actual screen-copy assembler path: the copied green world is
            // modulated by red to black. A failed depth test leaves it green.
            g_2d = {}; g_layers = {19}; g_requested = kScreenCopy;
            feed2D(0, 0x54u); feed2D(0x42u, 0x44u);
            feed2D(1, (uint64_t{0x3F800000u} << 32) | 0x80000080u);
            auto vertex = [&](uint32_t x, uint32_t y, float u, float v) {
                uint64_t st; float uv[2] = {u,v}; std::memcpy(&st, uv, 8);
                feed2D(2, st);
                feed2D(4, uint64_t{x * 16u} | (uint64_t{y * 16u} << 16) | (uint64_t{0xFFFFFFu} << 32));
            };
            vertex(1792, 1824, 0, 0); vertex(2304, 1824, 1, 0);
            vertex(1792, 2272, 0, 1); vertex(2304, 2272, 1, 1);
            flush2D();
        }
        native_renderer::endFrame(64, 64);
        std::vector<uint8_t> rgba; uint32_t width = 0, height = 0;
        if (!native_renderer::latestFrame(rgba, width, height) || width != 64 || height != 64) {
            check(false, "GPU frame readback"); return std::array<int,3>{-1,-1,-1};
        }
        const size_t pixel = (32u * 64u + 32u) * 4u;
        return std::array<int,3>{rgba[pixel],rgba[pixel+1],rgba[pixel+2]};
    };
    const auto control = frame(false);
    check(control[0] <= 1 && control[1] >= 254 && control[2] <= 1, "GPU control: near world geometry is visible and writes depth");
    const auto corrected = frame(true);
    std::printf("screen-copy pixel: RGB %d %d %d\n", corrected[0], corrected[1], corrected[2]);
    check(corrected[0] <= 1 && corrected[0] >= 0 && corrected[1] <= 1 && corrected[2] <= 1,
          "GPU: maximum-Z screen copy passes in front of near world geometry");
    std::fflush(nullptr); std::_Exit(failures ? 1 : 0);
}
