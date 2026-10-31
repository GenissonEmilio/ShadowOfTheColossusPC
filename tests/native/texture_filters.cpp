// ROM-free regression: a palette/texture upload must not keep a stale sampler.
#define GL_GLEXT_PROTOTYPES 1
#define GLFW_INCLUDE_NONE
#include <GL/gl.h>
#include <GL/glext.h>
#include "GLFW/glfw3.h"
#include "raylib.h"
#include "runtime/native_renderer.h"
#include "runtime/native_tuning.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
    std::atomic<float> observed{1};
    bool anisotropySupported=false;
    void check(bool ok,const char *what) {
        if(!ok){std::fprintf(stderr,"FAIL: %s\n",what);std::fflush(nullptr);std::_Exit(1);}
    }
}
extern "C" void __real_glDrawArrays(GLenum,GLint,GLsizei);
extern "C" void __wrap_glDrawArrays(GLenum mode,GLint first,GLsizei count) {
    if(anisotropySupported) {
        GLfloat value=1;
        glGetTexParameterfv(GL_TEXTURE_2D,GL_TEXTURE_MAX_ANISOTROPY_EXT,&value);
        observed.store(std::max(observed.load(),value));
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
    V quad[]={{-1,-1,0,0,0,1,0,0,128,128,128,128},{1,-1,0,0,0,1,4,0,128,128,128,128},{1,1,0,0,0,1,4,4,128,128,128,128},
              {-1,-1,0,0,0,1,0,0,128,128,128,128},{1,1,0,0,0,1,4,4,128,128,128,128},{-1,1,0,0,0,1,0,4,128,128,128,128}};
    native_renderer::uploadMesh(1,quad,6);
    float matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    unsigned cases=0;
    auto frame=[&](int filter,bool upload,unsigned levels=5) {
        t.textureFilter=float(filter);observed.store(1);
        if(upload)native_renderer::uploadTexture(1,16,16,pixels.data(),levels);
        native_renderer::beginFrame();
        native_renderer::DrawState state;state.texture=1;state.blend=native_renderer::Blend::Opaque;state.maxLod=float(levels-1);
        native_renderer::draw(1,matrix,state);native_renderer::endFrame(64,64);
        std::vector<uint8_t> rgba;uint32_t w=0,h=0;
        check(native_renderer::latestFrame(rgba,w,h)&&w==64&&h==64&&rgba.size()==64*64*4,"native frame and F8 readback");
        const float wanted=anisotropySupported&&filter>=3&&levels>1?std::min(limit,float(1u<<(filter-2))):1;
        std::printf("filter=%d upload=%d levels=%u anisotropy=%g expected=%g\n",filter,upload,levels,observed.load(),wanted);
        check(std::fabs(observed.load()-wanted)<0.01f,"sampler follows selection after texture upload");
        ++cases;
    };
    frame(2,true);frame(0,false);frame(1,false);
    for(int filter=3;filter<=6;++filter)frame(filter,false);
    frame(2,true); // Original PR #6 failed here: UI trilinear, GPU still 16x.
    frame(6,true);frame(2,false);
    frame(6,true);frame(2,true,1);frame(6,true);frame(2,true);
    std::printf("PASS: %u sampler transitions, palette upload, mip changes, native readback; extension=%d\n",cases,anisotropySupported);
    std::fflush(nullptr);std::_Exit(0); // the fixture owns the detached renderer
}
