// ROM-free regression for the Android native renderer. Link native_renderer.cpp
// and native_renderer_gl.cpp with their usual includes, -lEGL -lGLESv3 and
// -Wl,--wrap=glDrawArrays to inspect the real worker's draw submissions.
// Run via adb shell, with optional argument "cpu" to use the unshared path.
#include "runtime/native_renderer.h"
#include "runtime/native_tuning.h"
#include "runtime/ps2_android_runtime.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

// The production hook publishes hardware capability to the Android UI.
void ps2x::android::setMaxAnisotropy(float) {}

namespace
{
using native_renderer::DrawState;
using native_renderer::FrameRead;
using native_renderer::PresentationFrame;
using native_renderer::Vertex;
using Pixel = std::array<uint8_t, 4>;
constexpr GLsizei kDisjointVertices = 18;
std::atomic<bool> countAlphaDraws{false};
std::atomic<unsigned> alphaDraws{0};
std::atomic<unsigned> groupedAlphaDraws{0};

void require(bool ok, const char *label)
{
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", label);
    std::fflush(nullptr);
    std::_Exit(1);
}

void requireEgl(bool ok, const char *label)
{
    if (ok) return;
    std::fprintf(stderr, "EGL error 0x%x: ", eglGetError());
    require(false, label);
}

void requireGl(const char *label)
{
    const GLenum error = glGetError();
    if (error == GL_NO_ERROR) return;
    std::fprintf(stderr, "GL error 0x%x: ", error);
    require(false, label);
}

class EGLFixture
{
public:
    explicit EGLFixture(bool shared)
    {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        requireEgl(display != EGL_NO_DISPLAY, "get default EGL display");
        requireEgl(eglInitialize(display, nullptr, nullptr), "initialize EGL");
        const EGLint configAttributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLConfig config;
        EGLint count = 0;
        requireEgl(eglChooseConfig(display, configAttributes, &config, 1, &count) && count == 1,
                   "choose GLES3 pbuffer config");
        const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        const EGLint surfaceAttributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        for (size_t i = 0; i < contexts.size(); ++i)
        {
            const EGLContext share = i == 1 && shared ? contexts[0] : EGL_NO_CONTEXT;
            contexts[i] = eglCreateContext(display, config, share, contextAttributes);
            requireEgl(contexts[i] != EGL_NO_CONTEXT, "create GLES context");
            surfaces[i] = eglCreatePbufferSurface(display, config, surfaceAttributes);
            requireEgl(surfaces[i] != EGL_NO_SURFACE, "create EGL pbuffer");
        }
        makeCurrent(0);
        std::printf("GL_RENDERER: %s\nGL_VERSION: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
        native_renderer::setGlContext([this] { makeCurrent(1); }, shared);
    }

    ~EGLFixture()
    {
        native_renderer::shutdownGl();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        for (size_t i = 0; i < contexts.size(); ++i)
        {
            eglDestroyContext(display, contexts[i]);
            eglDestroySurface(display, surfaces[i]);
        }
        eglTerminate(display);
    }

private:
    void makeCurrent(size_t index)
    {
        requireEgl(eglMakeCurrent(display, surfaces[index], surfaces[index], contexts[index]), "make GLES context current");
        glDisable(GL_DITHER);
    }

    EGLDisplay display{};
    std::array<EGLContext, 2> contexts{};
    std::array<EGLSurface, 2> surfaces{};
};

GLuint compile(GLenum stage, const char *source)
{
    const GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[2048]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "%s\n", log);
    }
    require(ok, "compile shared frame sampling shader");
    return shader;
}

GLuint sampleSharedTexture(PresentationFrame frame)
{
    const GLuint vs = compile(GL_VERTEX_SHADER, R"GLSL(#version 300 es
        void main() {
            vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
            gl_Position = vec4(2.0 * p - 1.0, 0.0, 1.0);
        }
    )GLSL");
    const GLuint fs = compile(GL_FRAGMENT_SHADER, R"GLSL(#version 300 es
        precision highp float;
        uniform sampler2D sourceFrame;
        out vec4 pixel;
        void main() {
            pixel = texelFetch(sourceFrame, ivec2(gl_FragCoord.xy), 0);
        }
    )GLSL");
    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    require(linked, "link shared frame sampling program");
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLuint color, fbo, vao;
    glGenTextures(1, &color);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, frame.width, frame.height);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "complete consumer framebuffer");
    glBindTexture(GL_TEXTURE_2D, frame.texture);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "sourceFrame"), 0);
    glViewport(0, 0, frame.width, frame.height);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    requireGl("sample texture leased from the GLES render thread");
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glUseProgram(0);
    glDeleteProgram(program);
    return color;
}

void compareToDiagnostic(GLuint texture, uint32_t side, const std::vector<uint8_t> &diagnostic)
{
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "complete consumer read framebuffer");
    std::vector<uint8_t> sampled(diagnostic.size());
    glReadPixels(0, 0, side, side, GL_RGBA, GL_UNSIGNED_BYTE, sampled.data());
    requireGl("read consumer sample");
    for (size_t at = 0; at < sampled.size(); at += 4)
        require(std::equal(sampled.data() + at, sampled.data() + at + 3, diagnostic.data() + at) && sampled[at + 3] == 255,
                "shared sampling preserves the top-row-first RGB and makes alpha opaque");
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &texture);
}

constexpr float projection[] = {1,0,0,0, 0,1,0,0, 0,0,0,1, 0,0,0,0};
constexpr uint32_t kSide = 64;
constexpr Pixel upper{216, 48, 72, 63}, lower{32, 104, 192, 97};
uint64_t nextMesh = 1;

Vertex vertex(float x, float y, float w, Pixel color, float u = 0.0f)
{
    return {x * w, y * w, w, 0, 0, 1, u, 0.5f, color[0], color[1], color[2], color[3]};
}

void emit(const Vertex *vertices, uint32_t count, const DrawState &state)
{
    const auto id = nextMesh++;
    native_renderer::uploadMesh(id, vertices, count, true);
    native_renderer::draw(id, projection, state);
}

void finish(uint32_t side)
{
    native_renderer::endFrame(side, side);
    std::string error;
    if (!native_renderer::stateBarrier(error)) require(false, error.c_str());
}

void bands(uint32_t side, Pixel top, Pixel bottom)
{
    native_renderer::beginFrame();
    DrawState state;
    state.blend = native_renderer::Blend::Opaque;
    state.order = 2;
    const Vertex rows[] = {
        vertex(-1, -1, 2, bottom), vertex(1, -1, 2, bottom), vertex(1, 0, 2, bottom),
        vertex(-1, -1, 2, bottom), vertex(1, 0, 2, bottom), vertex(-1, 0, 2, bottom),
        vertex(-1, 0, 2, top), vertex(1, 0, 2, top), vertex(1, 1, 2, top),
        vertex(-1, 0, 2, top), vertex(1, 1, 2, top), vertex(-1, 1, 2, top),
    };
    emit(rows, 12, state);
    finish(side);
}

std::vector<uint8_t> diagnosticFrame(uint32_t side)
{
    std::vector<uint8_t> pixels;
    uint32_t w = 0, h = 0;
    require(native_renderer::latestFrame(pixels, w, h) && w == side && h == side && pixels.size() == side * side * 4,
            "read diagnostic RGBA from native backend");
    return pixels;
}

void pixelIs(const std::vector<uint8_t> &pixels, uint32_t side, uint32_t x, uint32_t y, Pixel expected, const char *label)
{
    const auto *actual = pixels.data() + (y * side + x) * 4;
    for (size_t c = 0; c < expected.size(); ++c)
        if (std::abs(int(actual[c]) - int(expected[c])) > 1)
        {
            std::fprintf(stderr, "pixel (%u, %u): expected (%u,%u,%u,%u), got (%u,%u,%u,%u)\n",
                         x, y, expected[0], expected[1], expected[2], expected[3], actual[0], actual[1], actual[2], actual[3]);
            require(false, label);
        }
}

void checkPresentation(bool shared)
{
    uint64_t revision = 0;
    std::vector<uint8_t> pixels;
    PresentationFrame first;
    bands(kSide, upper, lower);
    require(native_renderer::sharedFrameReady() == shared, "native backend selects the configured presentation path");
    require(native_renderer::readPresentationFrame(first, pixels, revision) == FrameRead::Updated &&
                first.width == kSide && first.height == kSide && bool(first.texture) == shared,
            "presenter acquires the first frame");
    const auto diagnostic = diagnosticFrame(kSide);
    pixelIs(diagnostic, kSide, 32, 0, {216, 48, 72, 126}, "diagnostic begins with the top band and real alpha");
    pixelIs(diagnostic, kSide, 32, kSide - 1, {32, 104, 192, 193}, "diagnostic ends with the bottom band and real alpha");
    require(shared ? pixels.empty() : pixels == diagnostic, "presenter only receives CPU pixels on the fallback path");
    PresentationFrame unchanged;
    pixels.clear();
    require(native_renderer::readPresentationFrame(unchanged, pixels, revision) == FrameRead::Unchanged &&
                unchanged.texture == first.texture && pixels.empty(),
            "the same revision retains the presentation without a copy");
    if (!shared) return;

    for (const uint32_t side : {kSide, kSide + 16, kSide + 16})
    {
        bands(side, lower, upper);
        PresentationFrame latest;
        require(native_renderer::readPresentationFrame(latest, pixels, revision) == FrameRead::Updated &&
                    latest.texture && latest.texture != first.texture && latest.width == side && latest.height == side && pixels.empty(),
                "new publications and resize keep the first lease intact");
        native_renderer::releasePresentationFrame(latest.texture);
    }
    const GLuint sampled = sampleSharedTexture(first);
    native_renderer::releasePresentationFrame(first.texture);
    // The worker can now reuse the lease even while the consumer draw is queued.
    bands(kSide, lower, upper);
    compareToDiagnostic(sampled, kSide, diagnostic);
}

void checkAlphaDepth(bool probe)
{
    native_renderer::beginFrame();
    const uint8_t mixedAlpha[] = {128, 128, 128, 0, 128, 128, 128, 128};
    native_renderer::uploadTexture(1, 2, 1, mixedAlpha);
    const Pixel red{200, 0, 0, 128}, green{0, 180, 0, 128}, blue{0, 0, 220, 128};
    std::vector<Vertex> mixed;
    const auto triangle = [&](float x, float y, float size, float w, Pixel color, float u) {
        mixed.push_back(vertex(x, y, w, color, u));
        mixed.push_back(vertex(x + size, y, w, color, u));
        mixed.push_back(vertex(x, y + size, w, color, u));
    };
    // Six disjoint triangles are conservatively mixed because the texture has
    // both alpha values. The last triangle overlaps the first at greater depth.
    triangle(-0.22f, -0.22f, 0.64f, 2, red,   0.75f);
    triangle(-0.90f, -0.88f, 0.42f, 2, green, 0.25f);
    triangle( 0.51f, -0.88f, 0.38f, 2, red,   0.75f);
    triangle(-0.90f,  0.48f, 0.40f, 2, green, 0.25f);
    triangle( 0.51f,  0.49f, 0.38f, 2, red,   0.75f);
    triangle(-0.30f, -0.88f, 0.38f, 2, green, 0.25f);
    triangle(-0.24f, -0.24f, 0.68f, 3, green, 0.25f);
    DrawState state;
    state.texture = 1;
    state.clamp = 3;
    state.blend = native_renderer::Blend::Opaque;
    state.framebufferOnlyAlphaFail = true;
    state.alphaReference = 64;
    state.order = 2;
    emit(mixed.data(), mixed.size(), state);
    if (probe)
    {
        const Vertex marker[] = {vertex(-1, -1, 2.5f, blue), vertex(3, -1, 2.5f, blue), vertex(-1, 3, 2.5f, blue)};
        DrawState overlay;
        overlay.blend = native_renderer::Blend::Opaque;
        overlay.order = 3;
        emit(marker, 3, overlay);
    }
    alphaDraws = 0;
    groupedAlphaDraws = 0;
    countAlphaDraws = true;
    finish(kSide);
    countAlphaDraws = false;
    const auto pixels = diagnosticFrame(kSide);
    pixelIs(pixels, kSide, 32, 32, {200, 0, 0, 255}, "alpha-pass depth occludes the overlapping farther triangle");
    pixelIs(pixels, kSide, 8, 55, probe ? Pixel{0, 0, 220, 255} : Pixel{0, 180, 0, 0},
            probe ? "alpha-fail fragments leave depth available" : "alpha-fail fragments still write color");
    pixelIs(pixels, kSide, 52, 55, {200, 0, 0, 255}, "a second alpha-passing triangle keeps its depth and color");

    const char *option = std::getenv("SOTC_GL_ALPHA_BATCH");
    const bool batching = !option || std::string_view(option) != "0";
    uint64_t hash = 14695981039346656037ull;
    for (const uint8_t byte : pixels) hash = (hash ^ byte) * 1099511628211ull;
    std::printf("alpha/depth probe=%u: %u GL draws, %u six-triangle draws, RGBA hash %016llx\n",
                static_cast<unsigned>(probe), alphaDraws.load(), groupedAlphaDraws.load(), static_cast<unsigned long long>(hash));
    require(groupedAlphaDraws == (batching ? 2u : 0u),
            "batching groups six disjoint triangles for the color pass and depth replay");
}
}

extern "C" void __real_glDrawArrays(GLenum mode, GLint first, GLsizei count);
extern "C" void __wrap_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if (countAlphaDraws && mode == GL_TRIANGLES)
    {
        ++alphaDraws;
        if (count == kDisjointVertices) ++groupedAlphaDraws;
    }
    __real_glDrawArrays(mode, first, count);
}

int main(int argc, char **argv)
{
    const bool shared = argc == 1;
    require(shared || (argc == 2 && std::string_view(argv[1]) == "cpu"), "usage: shared_renderer_gles [cpu]");
    EGLFixture egl(shared);
    native_renderer::updateTuning([](native_renderer::Tuning &tuning) {
        tuning.renderThread = true;
        tuning.asyncBuilds = false;
        tuning.warmShaders = false;
        tuning.msaa = 0;
        tuning.textureFilter = 1;
    });
    checkPresentation(shared);
    checkAlphaDepth(false);
    checkAlphaDepth(true);
    std::printf("PASS: native Android GLES renderer, %s presentation and alpha/depth ordering\n", shared ? "shared" : "CPU");
}
