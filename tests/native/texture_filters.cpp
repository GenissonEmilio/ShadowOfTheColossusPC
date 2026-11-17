// ROM-free checks for texture sampling and the water passes on desktop GL.
#define GL_GLEXT_PROTOTYPES 1
#define GLFW_INCLUDE_NONE
#include <GL/gl.h>
#include <GL/glext.h>
#include "GLFW/glfw3.h"
#include "raylib.h"
#include "runtime/native_renderer.h"
#include "runtime/native_tuning.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
    std::atomic<float> observed{1};
    std::array<std::atomic<GLint>, 3> effectWidth{}; // reflection, refraction, scene copy
    bool anisotropySupported=false;
    void check(bool ok,const char *what) {
        if(!ok){std::fprintf(stderr,"FAIL: %s\n",what);std::fflush(nullptr);std::_Exit(1);}
    }
}
extern "C" void __real_glDrawArrays(GLenum,GLint,GLsizei);
extern "C" void __wrap_glDrawArrays(GLenum mode,GLint first,GLsizei count) {
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    const GLint location = program ? glGetUniformLocation(program, "uFogColour") : -1;
    if (location >= 0) {
        GLfloat fogColour[4] = {};
        glGetUniformfv(program, location, fogColour);
        const int textured = static_cast<int>(fogColour[3]);
        GLint active = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        if (textured == 1 && anisotropySupported) {
            glActiveTexture(GL_TEXTURE0);
            GLfloat value = 1.0f;
            glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, &value);
            observed.store(std::max(observed.load(), value));
        } else if (textured >= 2 && textured <= 4) {
            glActiveTexture(textured == 2 ? GL_TEXTURE2 : GL_TEXTURE3);
            GLint width = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
            effectWidth[textured - 2].store(width);
        }
        glActiveTexture(active);
    }
    __real_glDrawArrays(mode,first,count);
}
int main() {
    setenv("SOTC_NATIVE_GL","1",1);
    SetTraceLogLevel(LOG_WARNING);SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(160,120,"SotC texture-filter regression");check(IsWindowReady(),"window context");
    auto *window=glfwGetCurrentContext();
    GLfloat limit=1;glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT,&limit);
    anisotropySupported=glGetError()==GL_NO_ERROR && limit>=2;
    glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
    auto *context=glfwCreateWindow(16,16,"sampler worker",nullptr,nullptr);check(context,"renderer context");
    glfwMakeContextCurrent(window);
    native_renderer::setGlContext([context]{glfwMakeContextCurrent(context);});
    auto &t=native_renderer::tuning();t.renderThread=false;t.asyncBuilds=false;
    std::vector<uint8_t> pixels(2048,128);
    using V=native_renderer::Vertex;
    // At 64 x 64 the UV footprint is four texels across and half a texel high.
    V quad[]={{-1,-1,0,0,0,1,0,0,128,128,128,128},{1,-1,0,0,0,1,16,0,128,128,128,128},{1,1,0,0,0,1,16,2,128,128,128,128},
              {-1,-1,0,0,0,1,0,0,128,128,128,128},{1,1,0,0,0,1,16,2,128,128,128,128},{-1,1,0,0,0,1,0,2,128,128,128,128}};
    native_renderer::uploadMesh(1,quad,6);
    float matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    unsigned cases=0;
    auto readFrame = [] {
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        check(native_renderer::latestFrame(rgba, w, h) && w == 64 && h == 64 && rgba.size() == 64*64*4,
              "native frame and F8 readback");
        return rgba;
    };
    enum Sampling { World, Screen, AlphaDepth, NoMip, LimitedMips };
    auto frame=[&](int filter,bool upload,unsigned levels=5,Sampling sampling=World) {
        t.textureFilter=float(filter);observed.store(1);
        if (upload) {
            size_t offset = 0;
            for (unsigned level = 0, size = 16; level < levels; ++level, size = std::max(1u, size / 2u)) {
                const bool lastMip = level > 0 && level + 1 == levels;
                for (unsigned i = 0; i < size * size; ++i) {
                    pixels[offset++] = lastMip ? 0 : 255;
                    pixels[offset++] = 0;
                    pixels[offset++] = lastMip ? 255 : 0;
                    pixels[offset++] = 128;
                }
            }
            native_renderer::uploadTexture(1,16,16,pixels.data(),levels);
        }
        native_renderer::beginFrame();
        native_renderer::DrawState state;state.texture=1;state.blend=native_renderer::Blend::Opaque;state.maxLod=float(levels-1);
        state.lodK = 4.0f; // GS sampling reads the last mip, marked blue.
        state.screen = sampling == Screen;
        state.framebufferOnlyAlphaFail = sampling == AlphaDepth;
        state.alphaReference = 64;
        if (sampling == NoMip)
            state.maxLod = 0.0f;
        else if (sampling == LimitedMips)
            state.maxLod = 1.0f;
        native_renderer::draw(1,matrix,state);native_renderer::endFrame(64,64);
        const auto rgba = readFrame();
        const bool anisotropicWorld = anisotropySupported && filter >= 3 && levels > 1 && sampling == World;
        const float wanted = anisotropicWorld ? std::min(limit, float(1u << (filter-2))) : 1.0f;
        std::printf("filter=%d upload=%d levels=%u anisotropy=%g expected=%g\n",filter,upload,levels,observed.load(),wanted);
        check(std::fabs(observed.load()-wanted)<0.01f,"sampler follows selection after texture upload");
        // Footprint sampling selects a finer, red mip. The legacy fragment
        // path still read the blue mip even when the sampler was set to 16x.
        const bool red = anisotropicWorld || levels == 1 || sampling == NoMip || sampling == LimitedMips;
        const auto *pixel = rgba.data() + (32 * 64 + 32) * 4;
        check(pixel[red ? 0 : 2] > 240 && pixel[red ? 2 : 0] < 15, "sampled pixel selects expected mip level");
        ++cases;
    };
    frame(2,true);frame(0,false);frame(1,false);
    for(int filter=3;filter<=6;++filter)frame(filter,false);
    frame(2,true); // Original PR #6 failed here: UI trilinear, GPU still 16x.
    frame(6,true);frame(2,false);
    frame(6,true);frame(2,true,1);frame(6,true);frame(2,true);
    frame(6, false, 5, Screen);
    frame(6, false, 5, AlphaDepth);
    frame(6, false, 5, NoMip);
    frame(6, false, 5, LimitedMips);
    std::printf("PASS: %u sampler transitions, palette upload, mip changes, native readback; extension=%d\n",cases,anisotropySupported);

    auto effectsFrame = [&](unsigned detail, int waterFrom, bool redraw, bool haze) {
        t.effectDetail = static_cast<float>(detail);
        t.waterSource = static_cast<float>(waterFrom);
        t.hazeSource = 0.0f;
        t.midFrameRedraw = redraw ? 1.0f : 0.0f;
        for (auto &width : effectWidth)
            width.store(0);
        native_renderer::beginFrame();
        native_renderer::DrawState pass;
        pass.blend = native_renderer::Blend::Opaque;
        pass.order = 2;
        pass.debugColour = 0xFF0000; // blue main scene
        native_renderer::draw(1, matrix, pass);
        pass.order = 12;
        pass.reflectionSource = true;
        pass.debugColour = 0x0000FF; // red mirror scene
        native_renderer::draw(1, matrix, pass);
        pass.reflectionSource = false;
        pass.debugColour = 0;
        pass.order = 9;
        pass.refractionSample = true;
        const float left[16] = {0.5f,0,0,0,0,1,0,0,0,0,1,0,-0.5f,0,0,1};
        const float right[16] = {0.5f,0,0,0,0,1,0,0,0,0,1,0,0.5f,0,0,1};
        native_renderer::draw(1, left, pass);
        pass.refractionSample = false;
        pass.reflectionSample = true;
        pass.order = 14;
        native_renderer::draw(1, right, pass);
        if (haze) {
            pass.reflectionSample = false;
            pass.screenCopy = true;
            pass.order = 19;
            const float corner[16] = {0.1f,0,0,0,0,0.1f,0,0,0,0,1,0,-0.8f,-0.8f,0,1};
            native_renderer::draw(1, corner, pass);
        }
        native_renderer::endFrame(64,64);
        const auto rgba = readFrame();
        const GLint expected = 256 << detail;
        check(effectWidth[0].load() == expected, "mirror draw samples selected target");
        check(effectWidth[1].load() == (waterFrom == 0 ? expected : 64), "refraction draw samples selected source");
        check(!haze || effectWidth[2].load() == 256, "heat haze remains on its own low-resolution path");
        const auto *water = rgba.data() + (32 * 64 + 16) * 4;
        const auto *mirror = rgba.data() + (32 * 64 + 48) * 4;
        check(water[2] > 240 && water[0] < 15 && mirror[0] > 240 && mirror[2] < 15,
              "mirror and refraction sample the correct scenes");
    };
    effectsFrame(0, 0, false, true);
    effectsFrame(1, 0, false, true);
    effectsFrame(2, 0, true, false);
    effectsFrame(2, 1, false, true);
    effectsFrame(0, 0, false, true);
    std::puts("PASS: mirror and refraction target sizes, separate heat haze, redraw, source override and return to default");
    std::fflush(nullptr);std::_Exit(0); // the fixture owns the detached renderer
}
