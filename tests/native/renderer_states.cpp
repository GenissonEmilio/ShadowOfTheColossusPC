// Real GL-context regression: restore histories, then render the same next
// whole frame. Uses no game data and owns its detached renderer until exit.
#define GLFW_INCLUDE_NONE
#include "GLFW/glfw3.h"
#include "raylib.h"
#include "runtime/native_renderer.h"
#include "runtime/native_tuning.h"
#include "native/native_history_state.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
    unsigned checks = 0;
    void check(bool ok, const char *message) {
        ++checks;
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::fflush(nullptr); std::_Exit(1); }
    }
    void check(bool ok, const std::string &message) { check(ok, message.c_str()); }
}
int main(int argc, char **) {
    const bool oldBloom = argc > 1;
    setenv("SOTC_NATIVE_GL", "1", 1);
    if (oldBloom) setenv("SOTC_BLOOM_OLD", "1", 1); else unsetenv("SOTC_BLOOM_OLD");
    SetTraceLogLevel(LOG_WARNING); SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(160, 120, "SotC temporal history state fixture"); check(IsWindowReady(), "host GL context");
    auto *host = glfwGetCurrentContext();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3); glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    auto *worker = glfwCreateWindow(16, 16, "history RenderThread", nullptr, nullptr); check(worker, "worker GL context");
    glfwMakeContextCurrent(host);
    native_renderer::setGlContext([worker] { glfwMakeContextCurrent(worker); });
    native_renderer::tuning().renderThread = true; native_renderer::tuning().asyncBuilds = false;
    const float clip[] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    unsigned frameNumber = 0;
    auto frame = [&](uint8_t r, uint8_t g, uint8_t b) {
        using V = native_renderer::Vertex;
        V vertices[] = {{-1,-1,0,0,0,1,0,0,r,g,b,128}, {1,-1,0,0,0,1,1,0,r,g,b,128}, {1,1,0,0,0,1,1,1,r,g,b,128},
                        {-1,-1,0,0,0,1,0,0,r,g,b,128}, {1,1,0,0,0,1,1,1,r,g,b,128}, {-1,1,0,0,0,1,0,1,r,g,b,128}};
        native_renderer::beginFrame(); native_renderer::uploadMesh(1, vertices, 6);
        native_renderer::DrawState draw; draw.blend = native_renderer::Blend::Opaque; draw.order = 2;
        native_renderer::draw(1, clip, draw);
        native_renderer::Bloom bloom; bloom.enabled = true; bloom.temporal = 0.75f;
        bloom.threshold = 0.5f; bloom.zA = 1; bloom.zB = 0;
        bloom.maskColor[0] = r / 255.0f; bloom.maskColor[1] = g / 255.0f; bloom.maskColor[2] = b / 255.0f;
        bloom.color[0] = bloom.color[1] = bloom.color[2] = 0.25f; bloom.color[3] = 0.4f;
        native_renderer::setBloom(bloom);
        if (frameNumber++) {
            draw.previousFrame = true; draw.blend = native_renderer::Blend::Normal;
            draw.opacity = 0.2f; draw.order = 29;
            native_renderer::draw(1, clip, draw);
        }
        native_renderer::endFrame(64, 64);
        std::string error; check(native_renderer::stateBarrier(error), error);
        std::vector<uint8_t> pixels; uint32_t w = 0, h = 0;
        check(native_renderer::latestFrame(pixels, w, h) && w == 64 && h == 64, "finished frame readback");
        return pixels;
    };
    frame(220, 20, 10); frame(180, 50, 20);
    std::string error; std::vector<uint8_t> saved, expectedHistory, actualHistory;
    check(native_renderer::captureState(saved, error), error);
    native_renderer::detail::HistoryState decoded;
    check(native_renderer::detail::decodeHistoryState(saved, decoded, error), error);
    check(oldBloom ? decoded.bloom : decoded.gsBloom, "the exercised bloom history is active");
    check(!decoded.previous.empty(), "previous-frame feedback is captured");
    const auto expected = frame(25, 200, 40);
    check(native_renderer::captureState(expectedHistory, error), error);
    frame(10, 20, 230); frame(10, 20, 200);
    check(native_renderer::captureState(actualHistory, error) && actualHistory != saved, "later timeline changes actual GPU history");
    auto broken = saved; broken.pop_back();
    check(!native_renderer::restoreState(broken, error), "malformed GPU history rejected before mutation");
    std::vector<uint8_t> intact;
    check(native_renderer::captureState(intact, error) && intact == actualHistory, "invalid GPU state leaves histories intact");
    check(native_renderer::resetStateFrame(error), error);
    check(native_renderer::restoreState(saved, error), error);
    check(native_renderer::captureState(actualHistory, error) && actualHistory == saved, "RGBA8 GPU readback matches every restored temporal buffer");
    frame(25, 200, 40); // hidden partial guest frame; must not touch histories
    check(native_renderer::captureState(actualHistory, error) && actualHistory == saved, "discarded partial frame preserves restored history");
    const auto actual = frame(25, 200, 40);
    check(actual == expected, "same next whole frame renders identical RGBA8 after restore");
    check(native_renderer::captureState(actualHistory, error) && actualHistory == expectedHistory, "next-frame temporal state matches uninterrupted execution");
    std::printf("PASS: %u native GPU state checks, %s bloom, owner-thread capture/restore\n", checks, oldBloom ? "legacy" : "GS");
    std::fflush(nullptr); std::_Exit(0);
}
