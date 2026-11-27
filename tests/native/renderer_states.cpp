// Real GL-context regression: restore histories, then render the same next
// whole frame. Uses no game data and owns its detached renderer until exit.
#define GLFW_INCLUDE_NONE
#include "GLFW/glfw3.h"
#include "raylib.h"
#include "runtime/native_renderer.h"
#include "runtime/native_tuning.h"
#include "native/native_history_state.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    unsigned checks = 0;
    void check(bool ok, const char *message) {
        ++checks;
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::fflush(nullptr); std::_Exit(1); }
    }
    void check(bool ok, const std::string &message) { check(ok, message.c_str()); }
}
int main(int argc, char **argv) {
    bool oldBloom = false, tryShared = false, failedShared = false;
    for (int i = 1; i < argc; ++i) {
        oldBloom |= std::string_view(argv[i]) == "legacy-bloom";
        tryShared |= std::string_view(argv[i]) == "shared";
        failedShared |= std::string_view(argv[i]) == "failed-shared";
    }
    tryShared |= failedShared;
    const bool shared = tryShared && !failedShared;
#if defined(_WIN32)
    check(_putenv_s("SOTC_NATIVE_GL", "1") == 0, "set native OpenGL");
    check(_putenv_s("SOTC_BLOOM_OLD", oldBloom ? "1" : "") == 0, "set bloom path");
#else
    setenv("SOTC_NATIVE_GL", "1", 1);
    if (oldBloom) setenv("SOTC_BLOOM_OLD", "1", 1); else unsetenv("SOTC_BLOOM_OLD");
#endif
    SetTraceLogLevel(LOG_WARNING); SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(160, 120, "SotC temporal history state fixture"); check(IsWindowReady(), "host GL context");
    auto *host = glfwGetCurrentContext();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3); glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    auto *worker = glfwCreateWindow(16, 16, "history RenderThread", nullptr, tryShared ? host : nullptr); check(worker, "worker GL context");
    GLFWwindow *unshared = nullptr;
    std::function<void()> bindUnshared;
    unsigned sharedAttempts = 0, fallbackAttempts = 0;
    if (tryShared) {
        unshared = glfwCreateWindow(16, 16, "history RenderThread readback", nullptr, nullptr);
        check(unshared, "fallback GL context");
        bindUnshared = [unshared, &fallbackAttempts] {
            ++fallbackAttempts;
            glfwMakeContextCurrent(unshared);
        };
    }
    glfwMakeContextCurrent(host);
    native_renderer::setGlContext([&] {
        if (tryShared)
            ++sharedAttempts;
        if (!failedShared)
            glfwMakeContextCurrent(worker);
    }, tryShared, std::move(bindUnshared));
    native_renderer::tuning().renderThread = true; native_renderer::tuning().asyncBuilds = false;
    const float clip[] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    unsigned frameNumber = 0;
    uint64_t presentationRevision = 0;
    uint64_t windowRevision = 0;
    native_renderer::PresentationFrame current{}, retained{};
    std::vector<uint8_t> retainedPixels;
    const auto textureView = [](const native_renderer::PresentationFrame &value) {
        return Texture2D{value.texture, static_cast<int>(value.width), static_cast<int>(value.height),
                         1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    };
    auto frame = [&](uint8_t r, uint8_t g, uint8_t b, bool held = false, uint32_t side = 64, bool readPixels = true) {
        using V = native_renderer::Vertex;
        V vertices[] = {{-1,-1,0,0,0,1,0,0,b,r,g,63}, {1,-1,0,0,0,1,1,0,b,r,g,63}, {1,1,0,0,0,1,1,1,r,g,b,63},
                        {-1,-1,0,0,0,1,0,0,b,r,g,63}, {1,1,0,0,0,1,1,1,r,g,b,63}, {-1,1,0,0,0,1,0,1,r,g,b,63}};
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
        native_renderer::endFrame(side, side);
        std::string error; check(native_renderer::stateBarrier(error), error);
        std::vector<uint8_t> pixels; uint32_t w = 0, h = 0;
        using native_renderer::FrameRead;
        const auto expectedRead = held ? FrameRead::Unchanged : FrameRead::Updated;
        native_renderer::PresentationFrame windowFrame;
        check(native_renderer::readPresentationFrame(windowFrame, pixels, windowRevision, shared && held) == expectedRead &&
                  windowFrame.width == side && windowFrame.height == side, "finished window frame");
        check(native_renderer::sharedFrameReady() == shared && bool(windowFrame.texture) == shared,
              "window and renderer agree on the presentation path");
        if (shared) {
            check(pixels.empty(), "window latches the frame without receiving CPU pixels");
            if (held)
                check(windowFrame.texture == current.texture, "discarded partial frame retains the window texture");
            else {
                if (current.texture != retained.texture)
                    native_renderer::releasePresentationFrame(current.texture);
                current = windowFrame;
            }
        } else
            check(pixels.size() == (held ? 0u : side * side * 4u), "unshared window takes the CPU fallback");
        if (!readPixels)
            return pixels;
        check(native_renderer::readFrame(pixels, w, h, presentationRevision) == expectedRead && w == side && h == side,
              held ? "discarded partial frame keeps the previous picture" : "finished frame readback");
        std::vector<uint8_t> untouched;
        check(native_renderer::readFrame(untouched, w, h, presentationRevision) == FrameRead::Unchanged && untouched.empty(),
              "the presentation context can keep the last texture without copying the same frame again");
        return pixels;
    };
    retainedPixels = frame(220, 20, 10);
    {
        using native_renderer::FrameRead;
        using native_renderer::PresentationWait;
        native_renderer::PresentationFrame repeated;
        std::vector<uint8_t> pixels;
        check(native_renderer::readPresentationFrame(repeated, pixels, windowRevision, true) == FrameRead::Unchanged,
              "the latest frame repeats if the renderer has no new work");
        check(shared ? repeated.wait == PresentationWait::TimedOut && repeated.waitUs > 0
                     : repeated.wait == PresentationWait::NotRequested && repeated.waitUs == 0,
              "only shared presentation waits for a fresh publication");
    }
    if (shared) {
        retained = current;
        auto target = LoadRenderTexture(retained.width, retained.height);
        check(target.id != 0, "window sampling target");
        BeginTextureMode(target);
        ClearBackground(BLACK);
        DrawTexture(textureView(retained), 0, 0, WHITE);
        EndTextureMode();
        Image sampled = LoadImageFromTexture(target.texture);
        check(sampled.data, "read window sampling target");
        const auto *actual = static_cast<const uint8_t *>(sampled.data);
        for (uint32_t y = 0; y < retained.height; ++y)
            for (uint32_t x = 0; x < retained.width; ++x) {
                const auto in = (y * retained.width + x) * 4u;
                const auto out = ((retained.height - 1u - y) * retained.width + x) * 4u;
                check(std::equal(actual + out, actual + out + 3u, retainedPixels.data() + in) && actual[out + 3u] == 255,
                      "the window samples the same RGB and orientation with opaque alpha");
            }
        check(retainedPixels[3] != 255 && retainedPixels[0] > retainedPixels[(retained.height - 1u) * retained.width * 4u],
              "the fixture reads the bright red top and nonopaque alpha first");
        UnloadImage(sampled);
        UnloadRenderTexture(target);
    }
    frame(180, 50, 20);
    std::string error; std::vector<uint8_t> saved, expectedHistory, actualHistory;
    check(native_renderer::captureState(saved, error), error);
    check(native_renderer::validateState(saved, error), error);
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
    frame(25, 200, 40, true); // hidden partial guest frame; must not touch histories
    check(native_renderer::captureState(actualHistory, error) && actualHistory == saved, "discarded partial frame preserves restored history");
    const auto actual = frame(25, 200, 40);
    check(actual == expected, "same next whole frame renders identical RGBA8 after restore");
    check(native_renderer::captureState(actualHistory, error) && actualHistory == expectedHistory, "next-frame temporal state matches uninterrupted execution");
    frame(10, 20, 230, false, 96, false);
    check(native_renderer::resetStateFrame(error), error);
    check(native_renderer::restoreState(saved, error), error);
    check(native_renderer::captureState(actualHistory, error) && actualHistory == saved, "restoring at a different target size rebuilds the saved history");
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> heldPixels;
    check(native_renderer::latestFrame(heldPixels, w, h) && w == 96 && h == 96,
          "a diagnostic read sees the displayed frame after restoring a smaller state");
    if (shared) {
        Image savedFrame = LoadImageFromTexture(textureView(retained));
        const auto *bytes = static_cast<const uint8_t *>(savedFrame.data);
        check(bytes && std::equal(retainedPixels.begin(), retainedPixels.end(), bytes),
              "a leased texture retains all pixels across multiple publications and a resize");
        UnloadImage(savedFrame);
        native_renderer::releasePresentationFrame(retained.texture);
        native_renderer::releasePresentationFrame(current.texture);
    }
    check(sharedAttempts == unsigned(tryShared), "attempt the shared worker once when requested");
    check(fallbackAttempts == unsigned(failedShared), "retry with the unshared worker only after a shared bind failure");
    native_renderer::shutdownGl();
    glfwDestroyWindow(worker);
    if (unshared) glfwDestroyWindow(unshared);
    CloseWindow();
    std::printf("PASS: %u native GPU state checks, %s bloom, %s presentation\n",
                checks, oldBloom ? "legacy" : "GS", shared ? "shared" : failedShared ? "CPU fallback" : "CPU");
}
