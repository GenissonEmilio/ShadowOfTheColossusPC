// ROM-free GPU regression: every framebuffer-reading layer needs its own
// picture prefix, independent of the order in which draws were submitted.
#define GL_GLEXT_PROTOTYPES 1
#define GLFW_INCLUDE_NONE
#include <GL/gl.h>
#include "GLFW/glfw3.h"
#include "raylib.h"
#include "runtime/native_renderer.h"
#include "runtime/native_tuning.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
    int failures = 0;
    void check(bool ok, const char *message) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) ++failures;
    }
    void uploadQuad(uint64_t id, float left, uint8_t r, uint8_t g, uint8_t b) {
        using V = native_renderer::Vertex;
        const V vertices[] = {
            {left,-1,0,0,0,1,0,1,r,g,b,128}, {1,-1,0,0,0,1,1,1,r,g,b,128},
            {1,1,0,0,0,1,1,0,r,g,b,128}, {left,-1,0,0,0,1,0,1,r,g,b,128},
            {1,1,0,0,0,1,1,0,r,g,b,128}, {left,1,0,0,0,1,0,0,r,g,b,128}
        };
        native_renderer::uploadMesh(id, vertices, 6);
    }
}

int main() {
    setenv("SOTC_NATIVE_GL", "1", 1);
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(160, 120, "SotC screen-copy ordering regression");
    if (!IsWindowReady()) return 2;
    auto *window = glfwGetCurrentContext();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    auto *worker = glfwCreateWindow(16, 16, "copy worker", nullptr, nullptr);
    if (!worker) return 2;
    glfwMakeContextCurrent(window);
    native_renderer::setGlContext([worker] { glfwMakeContextCurrent(worker); });
    auto &t = native_renderer::tuning();
    t.renderThread = false;
    t.asyncBuilds = false;
    t.renderScale = 1;
    t.waterSource = 0;

    uploadQuad(1, -1, 255, 0, 0);   // stale prior frame
    uploadQuad(2, -1, 0, 255, 0);   // world before layer 19
    uploadQuad(3, 0, 0, 0, 255);    // opaque geometry between layers 19 and 23
    uploadQuad(4, -1, 128, 128, 128); // identity modulation of the copied image
    const float clip[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    auto draw = [&](uint64_t mesh, uint32_t order, bool copy = false) {
        native_renderer::DrawState state;
        state.order = order;
        state.blend = native_renderer::Blend::Opaque;
        state.screen = true;
        state.screenCopy = copy;
        state.clamp = 3;
        native_renderer::draw(mesh, clip, state);
    };
    auto finish = [&] {
        native_renderer::endFrame(64, 64);
        std::vector<uint8_t> rgba;
        uint32_t width = 0, height = 0;
        const bool read = native_renderer::latestFrame(rgba, width, height) &&
                          width == 64 && height == 64 && rgba.size() == 64 * 64 * 4;
        check(read, "GPU frame readback");
        std::array<int, 6> samples{-1,-1,-1,-1,-1,-1};
        if (read) {
            // Far from the half-screen boundary, triangle seams and texture edges.
            for (size_t side = 0; side < 2; ++side) {
                const size_t pixel = (24 * 64 + (side ? 48 : 16)) * 4;
                for (size_t c = 0; c < 3; ++c) samples[side * 3 + c] = rgba[pixel + c];
            }
        }
        check(glGetError() == GL_NO_ERROR, "no GL errors after frame");
        return samples;
    };
    auto correct = [](const std::array<int, 6> &p) {
        return p[0] >= 0 && p[0] <= 1 && p[1] >= 254 && p[2] <= 1 &&
               p[3] >= 0 && p[3] <= 1 && p[4] <= 1 && p[5] >= 254;
    };

    for (int redraw = 0; redraw <= 1; ++redraw) {
        for (int source = 0; source <= 1; ++source) {
            t.midFrameRedraw = float(redraw);
            t.hazeSource = float(source);
            native_renderer::beginFrame();
            draw(2, 2); draw(3, 21);
            check(correct(finish()), "opaque green/blue control");
            for (bool reversed : {false, true}) {
                // Initialize the exact source sampled by the next frame to red.
                // A missing early capture then produces a deterministic red left.
                native_renderer::beginFrame();
                draw(1, 2); draw(4, 19, true); draw(4, 23, true);
                const auto warm = finish();
                check(warm[0] >= 254 && warm[1] <= 1 && warm[2] <= 1 &&
                      warm[3] >= 254 && warm[4] <= 1 && warm[5] <= 1,
                      "prior framebuffer source initialized red");
                native_renderer::beginFrame();
                draw(2, 2); draw(3, 21);
                draw(4, reversed ? 23 : 19, true);
                draw(4, reversed ? 19 : 23, true);
                const auto p = finish();
                std::printf("source=%d redraw=%d submission=%s left=%d,%d,%d right=%d,%d,%d\n",
                            source, redraw, reversed ? "23,19" : "19,23",
                            p[0], p[1], p[2], p[3], p[4], p[5]);
                // Only capturing the earliest layer loses the intervening blue.
                check(correct(p), "both copy layers sample their current picture prefix");
            }
        }
    }
    std::fflush(nullptr);
    std::_Exit(failures ? 1 : 0);
}
