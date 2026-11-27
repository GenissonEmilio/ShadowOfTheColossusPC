// ROM-free GPU/CPU contract for TEST GEQUAL/NEVER + AFAIL FB_ONLY, DATE off.
// The CPU reference processes every covered primitive in submission order.
#define GL_GLEXT_PROTOTYPES 1
#define GLFW_INCLUDE_NONE
#include <GL/gl.h>
#include "GLFW/glfw3.h"
#include "raylib.h"
#include "runtime/native_renderer.h"
#include "runtime/native_tuning.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
namespace {
using V=native_renderer::Vertex;
using B=native_renderer::Blend;
enum class MeshKind { Static, DynamicIndexed, Skinned };
struct Triangle {
    float w=2; // depth = 1 - 0.1/w in this renderer's projection
    std::array<uint8_t,3> rgb={128,0,0};
    uint8_t alpha=128;
    float u=1;
    float sampledTextureAlpha=128;
    float x=-1,y=-1,width=4,height=4;
};
struct Layer {
    std::vector<Triangle> triangles;
    unsigned textureKind=0; // 0: uniform128; 1: mixed0/128; 2/3:200/190; 4/5:mips; 6:mixed0/200
    B blend=B::Normal;
    bool alphaTest=true,noDepthWrite=false;
    uint8_t reference=64;
    float opacity=1;
};
unsigned checks=0,failures=0;
unsigned calls=0;
FILE *dump=nullptr;
uint64_t nextMesh=100;
const float projection[16]={1,0,0,0, 0,1,0,0, 0,0,0,1, 0,0,0,0};
const float skinPalette[32]={1,0,0,0, 0,1,0,0, 0,0,1,0, 4,-4,2,1,
                             1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
void require(bool ok,const char *what) {
    if(!ok){std::fprintf(stderr,"FATAL: %s\n",what);std::fflush(nullptr);std::_Exit(2);}
}
const char *kindName(MeshKind k) {
    return k==MeshKind::Static?"static":k==MeshKind::DynamicIndexed?"dynamic-indexed":"gpu-skinned";
}
std::array<uint8_t,3> reference(const std::vector<Layer>&layers,bool probe) {
    std::array<uint8_t,3> colour={0,0,0};float storedDepth=1;
    auto primitive=[&](const Triangle&t,const Layer&l) {
        const float x=(1.0f/64-t.x)/t.width,y=(-1.0f/64-t.y)/t.height;
        if(x<0 || y<0 || x+y>1)return;
        const float depth=1-.1f/t.w;
        if(depth>storedDepth)return;
        const float gsAlpha=std::clamp(t.sampledTextureAlpha*(float(t.alpha)/128)*l.opacity,0.f,255.f);
        // Match the existing GS-to-GL modulation convention; TEST uses the
        // unsaturated byte value above, not this blend alpha.
        const float blendAlpha=std::clamp((t.sampledTextureAlpha/255)*2*(float(t.alpha)/128)*l.opacity,0.f,1.f);
        if(!l.alphaTest && blendAlpha<.02f)return;
        for(unsigned i=0;i<3;++i) {
            const float src=std::clamp((128.f/255)*(float(t.rgb[i])/128),0.f,1.f);
            float value=src;
            if(l.blend==B::Normal)value=src*blendAlpha+(colour[i]/255.f)*(1-blendAlpha);
            else if(l.blend==B::Additive)value=src*blendAlpha+colour[i]/255.f;
            else if(l.blend==B::Subtract)value=colour[i]/255.f-src*blendAlpha;
            colour[i]=uint8_t(std::lround(std::clamp(value,0.f,1.f)*255));
        }
        if(!l.noDepthWrite && (!l.alphaTest || gsAlpha>=l.reference))storedDepth=depth;
    };
    for(const auto&l:layers)for(const auto&t:l.triangles)primitive(t,l);
    if(probe) {
        Layer marker;marker.blend=B::Opaque;marker.alphaTest=false;
        primitive(Triangle{2.5f,{0,0,255},128,1,128},marker);
    }
    return colour;
}
std::vector<V> vertices(const std::vector<Triangle>&triangles) {
    std::vector<V> v;
    for(const auto&t:triangles) {
        // A fullscreen triangle; all three vertices carry constant attributes.
        const float xy[3][2]={{t.x,t.y},{t.x+t.width,t.y},{t.x,t.y+t.height}};
        for(const auto&p:xy)v.push_back(V{p[0]*t.w,p[1]*t.w,t.w,0,0,1,t.u,.5f,t.rgb[0],t.rgb[1],t.rgb[2],t.alpha});
    }
    return v;
}
uint64_t upload(const std::vector<Triangle>&primitives,MeshKind kind,native_renderer::DrawState &state) {
    uint64_t id=nextMesh++;
    auto v=vertices(primitives);
    if(kind==MeshKind::Static) {
        native_renderer::uploadMesh(id,v.data(),uint32_t(v.size()));return id;
    }
    // Reverse storage, then index it back to the original primitive order.
    // This catches bounds classification based on storage order, not indices.
    std::vector<V> storage;std::vector<uint32_t> indices;
    for(size_t i=primitives.size();i>0;--i)storage.insert(storage.end(),v.begin()+(i-1)*3,v.begin()+i*3);
    for(size_t i=0;i<primitives.size();++i)for(unsigned j=0;j<3;++j)indices.push_back(uint32_t((primitives.size()-1-i)*3+j));
    if(kind==MeshKind::DynamicIndexed) {
        // Prefix both arenas with an undrawn committed mesh. Main indices
        // must use the nonzero vertex base and nonzero firstIndex offset.
        V *prefix=native_renderer::frameMesh(3);require(prefix,"prefix vertex arena");
        std::copy(v.begin(),v.begin()+3,prefix);
        uint32_t prefixBase=0;auto *pi=native_renderer::frameMeshIndices(3,prefixBase);require(pi,"prefix index arena");
        for(unsigned j=0;j<3;++j)pi[j]=prefixBase+j;
        native_renderer::commitFrameMesh((1ull<<63)|nextMesh++,3,3);
        V *target=native_renderer::frameMesh(uint32_t(storage.size()));require(target,"dynamic vertex arena");
        std::copy(storage.begin(),storage.end(),target);
        uint32_t base=0;auto *ix=native_renderer::frameMeshIndices(uint32_t(indices.size()),base);require(ix && base>=3,"nonzero dynamic vertex base");
        for(size_t i=0;i<indices.size();++i)ix[i]=base+indices[i];
        id|=1ull<<63;
        native_renderer::commitFrameMesh(id,uint32_t(storage.size()),uint32_t(indices.size()));return id;
    }
    std::vector<native_renderer::SkinVertex> skin(storage.size());
    for(size_t i=0;i<storage.size();++i) {
        std::memcpy(&skin[i],&storage[i],sizeof(V));
        skin[i].x-=1;skin[i].y+=1;skin[i].z-=.5f;
        skin[i].bone[0]=0;skin[i].bone[1]=4;
        skin[i].weight[0]=.25f;skin[i].weight[1]=.75f;
    }
    native_renderer::uploadSkinnedMesh(id,skin.data(),uint32_t(skin.size()),indices.data(),uint32_t(indices.size()));
    state.skinPalette=native_renderer::framePalette(skinPalette,8);state.skinPaletteCount=8;
    require(state.skinPalette,"GPU skin palette");return id;
}
void frame(const std::string&name,const std::vector<Layer>&layers,MeshKind kind,bool probe) {
    native_renderer::beginFrame();
    auto emit=[&](const Layer&l,MeshKind k,unsigned order) {
        native_renderer::DrawState state;state.texture=10+l.textureKind;state.blend=l.blend;
        state.framebufferOnlyAlphaFail=l.alphaTest;state.alphaReference=l.reference;
        state.opacity=l.opacity;state.noDepthWrite=l.noDepthWrite;state.writeDepth=true;state.order=order;state.clamp=3;
        if(l.textureKind==4 || l.textureKind==5){state.maxLod=1;state.lodK=1;state.lodScale=0;}
        uint64_t id=upload(l.triangles,k,state);native_renderer::draw(id,projection,state);
    };
    unsigned order=1;for(const auto&l:layers)emit(l,kind,order++);
    if(probe){Layer marker;marker.triangles={Triangle{2.5f,{0,0,255},128,1,128}};marker.blend=B::Opaque;marker.alphaTest=false;emit(marker,MeshKind::Static,order);}
    calls=0;
    native_renderer::endFrame(64,64);
    std::vector<uint8_t> rgba;uint32_t w=0,h=0;
    require(native_renderer::latestFrame(rgba,w,h)&&w==64&&h==64,"frame readback");
    if(dump)require(std::fwrite(rgba.data(),1,rgba.size(),dump)==rgba.size(),"write frame comparison");
    auto expected=reference(layers,probe);const size_t at=(32*64+32)*4;
    bool pass=true;for(unsigned c=0;c<3;++c)pass&=std::abs(int(rgba[at+c])-int(expected[c]))<=1;
    // For alpha=1, a tolerance of one must not turn a dropped nonzero colour
    // contribution into a false positive; this case uses opaque RGB writes.
    if(name=="low-alpha-one" && !probe)pass&=rgba[at]>0;
    ++checks;if(!pass)++failures;
    std::printf("%s: %s/%s/%s RGB=%u,%u,%u expected=%u,%u,%u calls=%u\n",pass?"PASS":"FAIL",name.c_str(),kindName(kind),probe?"depth-probe":"colour",rgba[at],rgba[at+1],rgba[at+2],expected[0],expected[1],expected[2],calls);
}
Layer one(Triangle t,unsigned texture=0,B blend=B::Normal,uint8_t ref=64,float opacity=1,bool mask=false) {
    Layer l;l.triangles={t};l.textureKind=texture;l.blend=blend;l.reference=ref;l.opacity=opacity;l.noDepthWrite=mask;return l;
}
}
extern "C" void __real_glDrawArrays(GLenum,GLint,GLsizei);
extern "C" void __wrap_glDrawArrays(GLenum mode,GLint first,GLsizei count) {
    ++calls;
    __real_glDrawArrays(mode,first,count);
}
extern "C" void __real_glDrawElements(GLenum,GLsizei,GLenum,const void*);
extern "C" void __wrap_glDrawElements(GLenum mode,GLsizei count,GLenum type,const void *indices) {
    ++calls;
    __real_glDrawElements(mode,count,type,indices);
}
int main() {
    setenv("SOTC_NATIVE_GL","1",1);SetTraceLogLevel(LOG_WARNING);SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(160,120,"SotC alpha and depth fixture");require(IsWindowReady(),"window context");
    auto *main=glfwGetCurrentContext();glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
    auto *worker=glfwCreateWindow(16,16,"alpha worker",nullptr,nullptr);require(worker,"worker context");
    glfwMakeContextCurrent(main);native_renderer::setGlContext([worker]{glfwMakeContextCurrent(worker);});
    if(const char *path=std::getenv("SOTC_ALPHA_TEST_FRAME_DUMP")) {
        dump=std::fopen(path,"wb");require(dump,"open frame comparison");
    }
    auto&t=native_renderer::tuning();t.renderThread=false;t.asyncBuilds=false;t.gpuSkinning=true;t.textureFilter=2;
    const uint8_t uniform[]={128,128,128,128},mixed[]={128,128,128,0,128,128,128,128};
    const uint8_t high[]={128,128,128,200},below[]={128,128,128,190};
    const uint8_t mixedHigh[]={128,128,128,0,128,128,128,200};
    const uint8_t mipHigh[]={128,128,128,0,128,128,128,0,128,128,128,128};
    const uint8_t mipLow[]={128,128,128,128,128,128,128,128,128,128,128,0};
    native_renderer::uploadTexture(10,1,1,uniform);native_renderer::uploadTexture(11,2,1,mixed);
    native_renderer::uploadTexture(12,1,1,high);native_renderer::uploadTexture(13,1,1,below);
    native_renderer::uploadTexture(14,2,1,mipHigh,2);native_renderer::uploadTexture(15,2,1,mipLow,2);
    native_renderer::uploadTexture(16,2,1,mixedHigh);
    require(native_renderer::gpuSkinning(),"GL GPU skinning must be active; CPU fallback is not a GPU check");
    using K=MeshKind;
    for(auto kind:{K::Static,K::DynamicIndexed,K::Skinned}) {
        auto run=[&](const char*name,const std::vector<Layer>&layers){frame(name,layers,kind,false);frame(name,layers,kind,true);};
        Layer scar=one(Triangle{4,{128,0,0},128,1,128},0,B::Opaque);scar.alphaTest=false;
        Layer fur=one(Triangle{2,{128,128,128},28,1,128});
        Layer glow=one(Triangle{3,{0,128,0},128,1,128},0,B::Additive);
        run("fur-scar-glow",{scar,fur,glow});
        // Dormin's fur writes colour with both low and high alpha, while
        // even the high-alpha primitive leaves Z available to the body.
        Layer neverFur=one(Triangle{2,{128,0,0},28,1,200},2,B::Normal,0,1,true);
        Layer body=one(Triangle{3,{0,128,0},128,1,128},0,B::Normal,127);
        run("never-fb-only-fur",{neverFur});
        neverFur.triangles[0].alpha=128;
        run("never-fb-only-fur-body",{neverFur,body});
        run("never-fb-only-low-alpha",{one(Triangle{2,{128,0,0},1,1,128},0,B::Opaque,0,1,true)});
        for(uint8_t alpha:{63,64,65})run((std::string("reference-64-alpha-")+std::to_string(alpha)).c_str(),{one(Triangle{2,{128,0,0},alpha,1,128},1)});
        run("reference193-pass200",{one(Triangle{2,{128,0,0},128,1,200},2,B::Normal,193)});
        run("reference193-mixed-pass200",{one(Triangle{2,{128,0,0},128,1,200},6,B::Normal,193)});
        run("reference193-fail190",{one(Triangle{2,{128,0,0},128,1,190},3,B::Normal,193)});
        run("low-alpha-one",{one(Triangle{2,{128,0,0},1,1,128},0,B::Opaque)});
        run("zmsk-one",{one(Triangle{2,{128,0,0},128,1,128},0,B::Normal,64,1,true)});
        Layer overlap;overlap.textureKind=1;
        overlap.triangles={Triangle{2,{128,0,0},96,1,128},Triangle{3,{0,128,0},96,.5f,64}};
        run("overlap-near-pass-far-fail-one-mesh",{overlap});
        std::reverse(overlap.triangles.begin(),overlap.triangles.end());run("overlap-far-fail-near-pass-one-mesh",{overlap});
        Layer islands;
        islands.textureKind=1;
        const float locations[8][2]={{-.18f,-.18f},{-.9f,-.9f},{-.45f,-.9f},{.1f,-.9f},
                                     {.55f,-.9f},{-.9f,.4f},{-.45f,.4f},{.4f,.4f}};
        for(unsigned i=0;i<8;++i)
            islands.triangles.push_back({2,{128,90,15},96,i%2?.5f:1.f,i%2?64.f:128.f,
                                         locations[i][0],locations[i][1],i?.34f:.40f,i?.34f:.40f});
        run("disjoint-mixed-coverage",{islands,glow});
        islands.blend=B::Subtract;
        run("disjoint-mixed-subtract",{scar,islands});
        islands.blend=B::Opaque;
        for(auto& t:islands.triangles){t.u=.25f;t.sampledTextureAlpha=0;}
        run("disjoint-mixed-zero-alpha-rgb",{islands});
        islands.alphaTest=false;
        run("ordinary-alpha-cutout",{islands});
        islands.alphaTest=true;
        for(auto& t:islands.triangles)t.x+=3;
        run("offscreen-mixed",{islands});
        for(unsigned i=0;i<8;++i){islands.triangles[i].x=locations[i][0];islands.triangles[i].w=.15f;}
        run("disjoint-near-clip-fallback",{islands});
        // The first two mixed triangles batch. The third shares samples
        // with the first and is farther, so its colour must see the first Z.
        Layer boundary;
        boundary.textureKind=1;
        boundary.triangles={Triangle{2,{128,0,0},96,1,128,-.18f,-.18f,.40f,.40f},
                            Triangle{2,{0,0,128},96,.5f,64,-.9f,-.9f,.36f,.36f},
                            Triangle{3,{0,128,0},96,.5f,64,-.2f,-.2f,.44f,.44f}};
        run("overlap-after-batched-range",{boundary});
        run("all-mips-high-level-one",{one(Triangle{2,{128,0,0},128,1,128},4)});
        run("all-mips-low-level-one",{one(Triangle{2,{128,0,0},128,1,0},5)});
        run("opacity-crosses-reference",{one(Triangle{2,{128,0,0},128,1,128},0,B::Normal,64,.25f)});
    }
    if(dump)require(!std::fclose(dump),"close frame comparison");
    std::printf("%s: %u alpha/depth GPU checks; %u failures (synthetic, no ROM)\n",failures?"FAIL":"PASS",checks,failures);
    std::fflush(nullptr);std::_Exit(failures?1:0);
}
