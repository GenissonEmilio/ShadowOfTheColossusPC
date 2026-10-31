// Synthetic GIF packets and grid matrices; no ROM, guest functions or save.
#ifndef SOTC_NATIVE_TEST_SOURCE
#define SOTC_NATIVE_TEST_SOURCE "../../game/overrides/sotc_native.cpp"
#endif
#include SOTC_NATIVE_TEST_SOURCE
#include <array>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x10000;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0x4000;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[0x4000] = {};

namespace {
    int failures = 0;
    void check(bool value, const char *message) {
        std::printf("%s: %s\n", value ? "PASS" : "FAIL", message);
        if (!value) ++failures;
    }
    struct Upload { uint64_t id; uint32_t width, height, levels; std::vector<uint8_t> rgba; };
    std::vector<Upload> uploads;
    std::vector<std::array<float,16>> clips;
    void noop(uint8_t *, R5900Context *, PS2Runtime *) {}
}

// Observe the public renderer boundary without creating a GPU or modifying
// product code with test-only taps. These are GNU ld --wrap symbol names.
extern "C" void __wrap__ZN15native_renderer13uploadTextureEmjjPKhj(uint64_t id, uint32_t w, uint32_t h, const uint8_t *pixels, uint32_t levels) {
    uploads.push_back({id,w,h,levels,std::vector<uint8_t>(pixels,pixels+size_t(w)*h*4u)});
}
extern "C" bool __wrap__ZN15native_renderer10hasTextureEm(uint64_t id) {
    return std::any_of(uploads.begin(),uploads.end(),[id](const auto &u){return u.id==id;});
}
extern "C" void __wrap__ZN15native_renderer4drawEmPKfRKNS_9DrawStateE(uint64_t, const float *clip, const native_renderer::DrawState &) {
    std::array<float,16> copy;std::copy(clip,clip+16,copy.begin());clips.push_back(copy);
}

int main() {
    using namespace sotc_native;
    std::vector<uint8_t> ram(PS2_RAM_SIZE);g_ram=ram.data();
    R5900Context ctx{};g_frame=0;ps2_stats::vif1Chains.store(0);
    auto put32=[&](uint32_t at,uint32_t value){std::memcpy(ram.data()+at,&value,4);};
    auto put64=[&](uint32_t at,uint64_t value){std::memcpy(ram.data()+at,&value,8);};
    auto matrix=[&](uint32_t at,const std::array<float,16> &m){std::memcpy(ram.data()+at,m.data(),64);};

    // A black square fan in the off-screen 256x256 boss buffer. In the old
    // path glEndHook discarded it; affected models kept placeholder colours.
    constexpr uint32_t start=0x3000;
    uint32_t q=start+0x20;
    auto reg=[&](uint32_t name,uint64_t value){put64(q,value);put32(q+8,name);q+=16;};
    auto xy=[](unsigned x,unsigned y){return uint64_t((1920u+x)*16u)|uint64_t((1920u+y)*16u)<<16;};
    reg(0,5);reg(1,0x80000000u); // fan, black at PS2 alpha 0x80
    reg(5,xy(128,128));reg(5,xy(32,32));reg(5,xy(224,32));
    reg(5,xy(224,224));reg(5,xy(32,224));reg(5,xy(32,32));
    put32(0x1186398,start);put32(0x1186394,q);
    g_glEnd=noop;SET_GPR_U32(&ctx,31,0x01085700u);
    glEndHook(ram.data(),&ctx,nullptr);
    check(uploads.size()==1,"boss fan reaches a host texture instead of being discarded");
    if (!uploads.empty()) {
        const auto &u=uploads.back();
        check(u.width==256 && u.height==256 && u.levels==1,"boss mask uses the game's buffer dimensions and no mip chain");
        auto at=[&](unsigned x,unsigned y,unsigned c){return u.rgba[(y*256u+x)*4u+c];};
        check(at(128,128,3)==128 && at(0,0,3)==0,"mask interior is opaque black and exterior transparent");
        check(std::all_of(u.rgba.begin(),u.rgba.end(),[i=size_t(0)](uint8_t c) mutable{return (++i%4u)==0u || c==0;}),"mask never samples placeholder rainbow RGB");
        glEndHook(ram.data(),&ctx,nullptr);
        check(uploads.size()==1,"unchanged mask reuses its texture");
        put64(start+0x20u+3u*16u,xy(64,32));
        const auto before=u.rgba;
        glEndHook(ram.data(),&ctx,nullptr);
        check(uploads.size()==2 && uploads[0].id!=uploads[1].id && before!=uploads[1].rgba,"fan animation refreshes a separate texture slot");
    }

    // A mesh grid whose view differs from the world's view. This models the
    // coordinate-space distinction of death tendrils, without game assets.
    constexpr uint32_t camera=0x5000,object=0x7000,buffer=0x8000;
    std::array<float,16> projection={2,0,0,0,0,3,0,0,0,0,4,0,0,0,0,1};
    std::array<float,16> view={1,0,0,0,0,1,0,0,0,0,1,0,3,4,5,1};
    std::array<float,16> world={1,0,0,0,0,1,0,0,0,0,1,0,100,200,-300,1};
    std::array<float,16> expected={2,0,0,0,0,3,0,0,0,0,4,0,6,12,20,1};
    matrix(camera+0x40,projection);matrix(camera+0x5a0,world);matrix(buffer+0x40,view);
    ram[object+8]='#';put32(object+0x20,buffer);put32(object+0x28,2);put32(object+0x2c,2);
    g_cameraContext=camera;g_meshDisp=noop;g_requested=0;g_layers={18};g_haveViewLights=false;
    native_renderer::tuning().fastHooks=false;SET_GPR_U32(&ctx,4,object);
    meshDispHook(ram.data(),&ctx,nullptr);
    check(clips.size()==1 && clips.back()==expected,"meshDisp draws the grid by projection times its own view");
    world=expected;world[12]=std::nextafter(world[12],100.0f);matrix(camera+0x5a0,world);
    meshDispHook(ram.data(),&ctx,nullptr);
    check(clips.size()==2 && clips.back()==world,"equivalent cape view preserves the body's exact camera bits");
    return failures ? 1 : 0;
}
