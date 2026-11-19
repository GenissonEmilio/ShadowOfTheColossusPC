// ROM-free pixel regression for the GS distinction between untextured RGB
// (vertex / 255) and texture modulation (texture * vertex / 128).
#define GL_GLEXT_PROTOTYPES 1
#define GLFW_INCLUDE_NONE
#include "GLFW/glfw3.h"
#include "raylib.h"
#include "runtime/native_renderer.h"
#include "runtime/native_tuning.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
    void check(bool value,const char *message) {
        if (!value) {std::fprintf(stderr,"FAIL: %s\n",message);std::fflush(nullptr);std::_Exit(1);}
    }
}
int main() {
    setenv("SOTC_NATIVE_GL","1",1);SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);InitWindow(160,120,"SotC GS colour regression");
    check(IsWindowReady(),"window context");auto *window=glfwGetCurrentContext();
    glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
    auto *worker=glfwCreateWindow(16,16,"colour worker",nullptr,nullptr);check(worker,"renderer context");
    glfwMakeContextCurrent(window);native_renderer::setGlContext([worker]{glfwMakeContextCurrent(worker);});
    native_renderer::tuning().renderThread=false;native_renderer::tuning().asyncBuilds=false;
    const float matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const uint8_t texel[]={64,32,16,128};native_renderer::uploadTexture(1,1,1,texel);
    using V=native_renderer::Vertex;
    auto frame=[&](const char *name,bool textured,uint8_t r,uint8_t g,uint8_t b,uint8_t a,std::array<int,3> expected,bool blend=false) {
        V quad[]={{-1,-1,0,0,0,1,0,0,r,g,b,a},{1,-1,0,0,0,1,1,0,r,g,b,a},{1,1,0,0,0,1,1,1,r,g,b,a},
                  {-1,-1,0,0,0,1,0,0,r,g,b,a},{1,1,0,0,0,1,1,1,r,g,b,a},{-1,1,0,0,0,1,0,1,r,g,b,a}};
        native_renderer::uploadMesh(1,quad,6);native_renderer::beginFrame();
        native_renderer::DrawState state;state.texture=textured?1:0;
        state.blend=blend?native_renderer::Blend::Normal:native_renderer::Blend::Opaque;
        native_renderer::draw(1,matrix,state);native_renderer::endFrame(64,64);
        std::vector<uint8_t> rgba;uint32_t w=0,h=0;
        check(native_renderer::latestFrame(rgba,w,h)&&w==64&&h==64,"native frame readback");
        const size_t pixel=(32u*64u+32u)*4u;bool good=true;
        for (int i=0;i<3;++i)good&=std::abs(int(rgba[pixel+i])-expected[i])<=1;
        std::printf("%s: RGB %u %u %u; expected %d %d %d\n",name,rgba[pixel],rgba[pixel+1],rgba[pixel+2],expected[0],expected[1],expected[2]);
        check(good,name);
    };
    frame("untextured midtones",false,128,64,32,128,{128,64,32});
    frame("textured modulation unchanged",true,128,64,32,128,{64,16,4});
    frame("untextured full range",false,255,128,64,128,{255,128,64});
    frame("untextured alpha unchanged",false,128,64,32,64,{64,32,16},true);

    check(native_renderer::gpuSkinning(), "OpenGL skinning program is available");
    native_renderer::SkinVertex triangle[3]{};
    triangle[0].x = -0.18f; triangle[0].y = -0.23f;
    triangle[1].x = 0.18f; triangle[1].y = -0.23f;
    triangle[2].y = 0.23f;
    for (auto &vertex : triangle) {
        vertex.r = 240;
        vertex.a = 128;
        vertex.weight[0] = 1;
    }
    const uint32_t indices[] = {0, 1, 2};
    native_renderer::uploadSkinnedMesh(2, triangle, 3, indices, 3);
    native_renderer::beginFrame();
    float movingBone[16];
    std::copy(matrix, matrix + 16, movingBone);
    movingBone[12] = -0.5f;
    native_renderer::DrawState state;
    state.blend = native_renderer::Blend::Opaque;
    state.skinPaletteCount = 4;
    state.skinPalette = native_renderer::framePalette(movingBone, 4);
    native_renderer::draw(2, matrix, state);
    const auto movingHandle = state.skinPalette;
    state.skinPalette = native_renderer::framePalette(matrix, 4);
    native_renderer::draw(2, matrix, state);

    movingBone[12] = 0.5f;
    native_renderer::rewriteFramePalette(movingHandle, movingBone, 4);
    native_renderer::endFrame(64, 64);
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    check(native_renderer::latestFrame(rgba, w, h) && w == 64 && h == 64,
          "updated skin palette frame readback");
    const auto red = [&](uint32_t x) { return rgba[(32u * w + x) * 4u]; };
    check(red(16) == 0 && red(48) >= 239, "GPU renders the pose written after draw submission");
    check(red(32) >= 239, "rewriting the palette leaves the next draw's palette intact");
    std::puts("PASS: GS colour, alpha and GPU pose cases");std::fflush(nullptr);std::_Exit(0);
}
