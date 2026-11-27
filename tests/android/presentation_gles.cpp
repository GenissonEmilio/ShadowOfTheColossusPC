// ROM-free GLES 3 regression. Build with Android clang++ -std=c++20,
// -I PS2Recomp/ps2xRuntime/src/lib and -lEGL -lGLESv3; run via adb shell.
#include "desktop_antialias.h"
#include "desktop_fsr.h"
#include "desktop_interpolation.h"
#include "presentation_shader_source.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
using Pixel = std::array<uint8_t, 4>;

void check(bool ok, const char *label)
{
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", label);
    std::exit(1);
}

void checkEgl(bool ok, const char *label)
{
    if (ok) return;
    std::fprintf(stderr, "EGL error 0x%x: ", eglGetError());
    check(false, label);
}

void checkGl(const char *label)
{
    const GLenum error = glGetError();
    if (error == GL_NO_ERROR) return;
    std::fprintf(stderr, "GL error 0x%x: ", error);
    check(false, label);
}

class EglContexts
{
public:
    EglContexts()
    {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        checkEgl(display != EGL_NO_DISPLAY, "get default EGL display");
        checkEgl(eglInitialize(display, nullptr, nullptr), "initialize EGL");
        checkEgl(eglBindAPI(EGL_OPENGL_ES_API), "bind GLES API");
        const EGLint configAttributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLConfig config;
        EGLint count = 0;
        checkEgl(eglChooseConfig(display, configAttributes, &config, 1, &count) && count == 1,
                 "choose GLES3 pbuffer config");
        const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        const EGLint surfaceAttributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        for (size_t i = 0; i < contexts.size(); ++i)
        {
            const EGLContext shared = i == 0 ? EGL_NO_CONTEXT : contexts[0];
            contexts[i] = eglCreateContext(display, config, shared, contextAttributes);
            checkEgl(contexts[i] != EGL_NO_CONTEXT, "create GLES3 context");
            surfaces[i] = eglCreatePbufferSurface(display, config, surfaceAttributes);
            checkEgl(surfaces[i] != EGL_NO_SURFACE, "create EGL pbuffer");
        }
        makeCurrent(0);
        GLint major = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        checkGl("query GLES version");
        check(major >= 3, "driver supplies GLES 3 or later");
        std::printf("GL_RENDERER: %s\nGL_VERSION: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    }

    ~EglContexts()
    {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        for (size_t i = 0; i < contexts.size(); ++i)
        {
            eglDestroyContext(display, contexts[i]);
            eglDestroySurface(display, surfaces[i]);
        }
        eglTerminate(display);
    }

    void makeCurrent(size_t index)
    {
        checkEgl(eglMakeCurrent(display, surfaces[index], surfaces[index], contexts[index]),
                 "make GLES context current");
        glDisable(GL_DITHER);
    }

private:
    EGLDisplay display{};
    std::array<EGLContext, 2> contexts{};
    std::array<EGLSurface, 2> surfaces{};
};

GLuint compile(GLenum stage, const char *source, const char *label)
{
    const GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[4096]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "%s shader log:\n%s\n", label, log);
        check(false, "compile presentation shader");
    }
    return shader;
}

struct Texture
{
    GLuint id;
    int width;
    int height;
};

Texture texture(int width, int height, const Pixel *pixels = nullptr)
{
    GLuint id;
    glGenTextures(1, &id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    checkGl("allocate RGBA8 texture");
    return {id, width, height};
}

GLuint attach(Texture image)
{
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image.id, 0);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "RGBA8 framebuffer complete");
    return fbo;
}

class Program
{
public:
    Program(GLuint vertex, const char *fragment, const char *label) : name(label), id(glCreateProgram())
    {
        const std::string es = desktop_video::glesPresentationFragment(fragment);
        const GLuint shader = compile(GL_FRAGMENT_SHADER, es.c_str(), label);
        glAttachShader(id, vertex);
        glAttachShader(id, shader);
        glLinkProgram(id);
        glDeleteShader(shader);
        GLint ok = GL_FALSE;
        glGetProgramiv(id, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[4096]{};
            glGetProgramInfoLog(id, sizeof(log), nullptr, log);
            std::fprintf(stderr, "%s program log:\n%s\n", label, log);
            check(false, "link presentation program");
        }
    }

    ~Program() { glDeleteProgram(id); }
    void use() const { glUseProgram(id); }

    GLint uniform(const char *label) const
    {
        const GLint location = glGetUniformLocation(id, label);
        if (location < 0)
        {
            std::fprintf(stderr, "%s uniform %s: ", name, label);
            check(false, "expected an active uniform");
        }
        return location;
    }

    GLuint attribute(const char *label) const
    {
        const GLint location = glGetAttribLocation(id, label);
        check(location >= 0, label);
        return static_cast<GLuint>(location);
    }

    void sample(const char *label, Texture image, GLint unit) const
    {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, image.id);
        glUniform1i(uniform(label), unit);
    }

    const char *name;

private:
    GLuint id;
};

enum class Orientation { Direct, FlipV };

Texture draw(const Program &program, Texture source, int width, int height,
             Orientation orientation = Orientation::Direct)
{
    const Texture result = texture(width, height);
    const GLuint fbo = attach(result);
    const float lowV = orientation == Orientation::Direct ? 0.0f : 1.0f;
    const float highV = 1.0f - lowV;
    const float vertices[] = {-1, -1, 0, lowV, 1, -1, 1, lowV,
                             -1,  1, 0, highV, 1, 1, 1, highV};
    const float identity[] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    const GLuint position = program.attribute("vertexPosition");
    const GLuint uv = program.attribute("vertexTexCoord");
    glEnableVertexAttribArray(position);
    glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(uv);
    glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<const void *>(2 * sizeof(float)));
    glUniformMatrix4fv(program.uniform("mvp"), 1, GL_FALSE, identity);
    program.sample("texture0", source, 0);
    glViewport(0, 0, width, height);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    checkGl(program.name);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    return result;
}

std::vector<Pixel> read(Texture image)
{
    const GLuint fbo = attach(image);
    std::vector<Pixel> pixels(image.width * image.height);
    glReadPixels(0, 0, image.width, image.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    checkGl("read GLES framebuffer");
    glDeleteFramebuffers(1, &fbo);
    return pixels;
}

void expect(Pixel actual, Pixel expected, const char *label, int tolerance = 1)
{
    for (size_t channel = 0; channel < expected.size(); ++channel)
        if (std::abs(int(actual[channel]) - int(expected[channel])) > tolerance)
        {
            std::fprintf(stderr, "%s: expected RGBA (%u, %u, %u, %u), got (%u, %u, %u, %u)\n",
                         label, expected[0], expected[1], expected[2], expected[3],
                         actual[0], actual[1], actual[2], actual[3]);
            std::exit(1);
        }
}

void expectSolid(Texture image, Pixel color, const char *label)
{
    for (const auto &pixel : read(image)) expect(pixel, color, label);
}

void checkFilters(const Program &fxaa, const Program &easu, const Program &rcas, const Program &area)
{
    const Pixel color{73, 119, 207, 101};
    const Pixel opaque{73, 119, 207, 255};
    const std::vector<Pixel> flat(8 * 6, color);
    const Texture source = texture(8, 6, flat.data());

    fxaa.use();
    glUniform2i(fxaa.uniform("sourceSize"), 8, 6);
    expectSolid(draw(fxaa, source, 8, 6), color, "FXAA preserves constant RGBA");
    easu.use();
    glUniform2i(easu.uniform("sourceSize"), 8, 6);
    const Texture upscaled = draw(easu, source, 15, 11);
    expectSolid(upscaled, opaque, "EASU upscales constant RGB and outputs opaque alpha");
    rcas.use();
    expectSolid(draw(rcas, upscaled, 15, 11), opaque, "RCAS preserves constant RGB and opaque alpha");
    expectSolid(draw(rcas, source, 8, 6), opaque, "RCAS outputs opaque alpha for nonopaque input");

    area.use();
    glUniform2i(area.uniform("sourceSize"), 8, 6);
    glUniform2i(area.uniform("targetSize"), 4, 6);
    expectSolid(draw(area, source, 4, 6), color, "area filter preserves constant RGBA");

    // Every destination texel integrates one known 2x2 block, including alpha.
    const Pixel pattern[] = {{0, 4, 32, 16}, {80, 8, 48, 32}, {32, 12, 64, 48}, {64, 16, 80, 64},
                             {8, 20, 96, 80}, {88, 24, 112, 96}, {40, 28, 128, 112}, {72, 32, 144, 128}};
    glUniform2i(area.uniform("sourceSize"), 4, 2);
    glUniform2i(area.uniform("targetSize"), 2, 2);
    const Texture horizontal = draw(area, texture(4, 2, pattern), 2, 2);
    glUniform2i(area.uniform("sourceSize"), 2, 2);
    glUniform2i(area.uniform("targetSize"), 2, 1);
    const auto pixels = read(draw(area, horizontal, 2, 1));
    expect(pixels[0], {44, 14, 72, 56}, "area filter integrates the left block");
    expect(pixels[1], {52, 22, 104, 88}, "area filter integrates the right block");
}

void checkInterpolation(const Program &motion, const Program &midpoint)
{
    constexpr int size = 64;
    constexpr int tiles = size / 16;
    std::vector<Pixel> before(size * size), shifted(size * size);
    const auto ramp = [](int x) -> Pixel {
        const auto level = static_cast<uint8_t>(x * 4);
        return {level, static_cast<uint8_t>(255 - level), level, 71};
    };
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            before[y * size + x] = ramp(x);
            shifted[y * size + x] = ramp(y < size / 2 ? std::max(0, x - 4) : x);
        }
    const Texture previous = texture(size, size, before.data());

    // Like Raylib render textures, the motion pass writes the first source row
    // at the top of an FBO. The resolve must account for that orientation.
    const auto fieldFor = [&](Texture current) {
        motion.use();
        glUniform2f(motion.uniform("sourceSize"), size, size);
        motion.sample("previousFrame", previous, 1);
        return draw(motion, current, tiles, tiles, Orientation::FlipV);
    };
    const auto resolve = [&](Texture current, Texture field) {
        midpoint.use();
        glUniform2f(midpoint.uniform("sourceSize"), size, size);
        midpoint.sample("previousFrame", previous, 1);
        midpoint.sample("motionField", field, 2);
        return read(draw(midpoint, current, size, size, Orientation::FlipV));
    };

    const Texture duplicate = texture(size, size, before.data());
    const Texture stillField = fieldFor(duplicate);
    expectSolid(stillField, {128, 128, 0, 255}, "identical frames encode zero motion and confidence");
    const auto still = resolve(duplicate, stillField);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            Pixel color = before[(size - 1 - y) * size + x];
            color[3] = 255;
            expect(still[y * size + x], color, "identical frames resolve to the original RGB with opaque alpha");
        }

    const Texture moved = texture(size, size, shifted.data());
    const Texture movedField = fieldFor(moved);
    const auto vectors = read(movedField);
    expect(vectors[2 * tiles + 1], {159, 128, 255, 255}, "moving half encodes four pixels of rightward motion");
    expect(vectors[1 * tiles + 1], {128, 128, 0, 255}, "stationary half encodes no movement");
    const auto halfway = resolve(moved, movedField);
    expect(halfway[(size - 1 - 23) * size + 27], {100, 155, 100, 255}, "moving half advances two pixels at the midpoint", 2);
    expect(halfway[(size - 1 - 40) * size + 27], {108, 147, 108, 255}, "stationary half retains its RGB at the midpoint");
}

void clearRow(Texture target, int first, int height, Pixel color)
{
    glScissor(0, first, target.width, height);
    glClearColor(color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, color[3] / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLsync signal()
{
    const GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    check(fence != nullptr, "create shared GL fence");
    glFlush();
    return fence;
}

void waitFor(GLsync fence)
{
    glWaitSync(fence, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(fence);
    checkGl("wait on fence from the other EGL context");
}

void checkSharing(EglContexts &egl, const Program &fxaa)
{
    constexpr int side = 8;
    const Pixel lower{32, 104, 192, 97}, upper{216, 48, 72, 185}, replacement{17, 221, 83, 143};
    egl.makeCurrent(1);
    const Texture source = texture(side, side);
    const GLuint fbo = attach(source);
    glEnable(GL_SCISSOR_TEST);
    clearRow(source, 0, side / 2, lower);
    clearRow(source, side / 2, side / 2, upper);
    const GLsync ready = signal();

    egl.makeCurrent(0);
    waitFor(ready);
    fxaa.use();
    glUniform2i(fxaa.uniform("sourceSize"), side, side);
    const Texture consumed = draw(fxaa, source, side, side, Orientation::FlipV);
    const GLsync sampled = signal();

    egl.makeCurrent(1);
    waitFor(sampled);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    clearRow(source, 0, side, replacement);
    glDisable(GL_SCISSOR_TEST);
    const GLsync replaced = signal();
    glDeleteFramebuffers(1, &fbo);

    egl.makeCurrent(0);
    waitFor(replaced);
    const auto pixels = read(consumed);
    expect(pixels[side + 2], upper, "shared texture samples the flipped upper row with alpha");
    expect(pixels[(side - 2) * side + 2], lower, "shared texture samples the flipped lower row with alpha");
    fxaa.use();
    expectSolid(draw(fxaa, source, side, side), replacement, "shared texture samples the rewritten frame");
}
}

int main()
{
    using namespace desktop_video;
    EglContexts egl;
    const GLuint vertex = compile(GL_VERTEX_SHADER, kGlesPresentationVertex, "presentation vertex");
    const Program fxaa(vertex, kFxaaFragment, "FXAA");
    const Program easu(vertex, kEasuFragment, "FSR EASU");
    const Program rcas(vertex, kRcasFragment, "FSR RCAS");
    const Program area(vertex, kAreaFragment, "area resolve");
    const Program motion(vertex, kFrameMotionFragment, "frame generation motion");
    const Program midpoint(vertex, kFrameMidpointFragment, "frame generation midpoint");
    glDeleteShader(vertex);
    checkFilters(fxaa, easu, rcas, area);
    checkInterpolation(motion, midpoint);
    checkSharing(egl, fxaa);
    std::puts("PASS: six GLES 3 presentation shaders and bidirectional EGL texture sharing");
}
