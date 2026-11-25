// Synthetic NTO2 textures and transfer work only; no ROM, save or game data.
#ifndef SOTC_NATIVE_TEST_SOURCE
#define SOTC_NATIVE_TEST_SOURCE "../../game/overrides/sotc_native.cpp"
#endif
#include SOTC_NATIVE_TEST_SOURCE

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x10000;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0x4000;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[0x4000] = {};

namespace {
    int failures = 0;
    unsigned checks = 0;
    void check(bool ok, const char *message) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
        ++checks;
        if (!ok) ++failures;
    }
    struct Upload { uint64_t id; std::vector<uint8_t> rgba; };
    std::vector<Upload> uploads;
}

extern "C" void __wrap__ZN15native_renderer13uploadTextureEmjjPKhj(uint64_t id, uint32_t w, uint32_t h, const uint8_t *pixels, uint32_t) {
    uploads.push_back({id, std::vector<uint8_t>(pixels, pixels+size_t(w)*h*4u)});
}
extern "C" bool __wrap__ZN15native_renderer10hasTextureEm(uint64_t id) {
    return std::any_of(uploads.begin(), uploads.end(), [id](const auto &u) { return u.id == id; });
}

int main() {
    using namespace sotc_native;
    std::vector<uint8_t> ram(PS2_RAM_SIZE);g_ram=ram.data();
    auto put=[&](uint32_t at, uint32_t value) { std::memcpy(ram.data()+at,&value,4); };
    constexpr uint32_t h=0x1000, pixels=0x2000, original=0x3000;
    constexpr uint32_t work=0x4000, active=0x5000, other=0x6000;
    auto setup=[&](uint32_t psm, uint8_t index) {
        std::fill(ram.begin(),ram.begin()+0x7000,0);
        put(h,0x324F544Eu);put(h+0xCu,work);put(h+0x14u,pixels-h);put(h+0x18u,original-h);
        put(h+0x1Cu,psm | 1u<<12 | 2u<<16 | 2u<<20); // one 4x4 level, linear
        put(work+0xCu,active);put(work+0x40u,other); // deliberately different copies
        std::fill(ram.begin()+pixels,ram.begin()+pixels+16,index);
        for (uint32_t i=0;i<256;++i) {
            put(original+i*4u,0x80502010u);
            put(active+i*4u,0x00306090u);
            put(other+i*4u,0x80604020u);
        }
    };
    auto decoded=[&](uint32_t colour, const char *message) {
        TexSource source;std::vector<uint8_t> fast,reference;uint32_t fn=0,rn=0;
        const bool a=texSource(h,source) && decodeTexture(source,fast,fn);
        const bool b=decodeTextureReference(h,reference,rn);
        uint32_t first=0;if(fast.size()>=4)std::memcpy(&first,fast.data(),4);
        check(a && b && fn==1 && rn==1 && fast==reference && first==colour,message);
    };
    auto uploadedColour=[&](uint64_t key, uint32_t expected) {
        const auto it=std::find_if(uploads.begin(),uploads.end(),[key](const auto &u){return u.id==key;});
        uint32_t first=0;if(it!=uploads.end() && it->rgba.size()>=4)std::memcpy(&first,it->rgba.data(),4);
        return it!=uploads.end() && first==expected;
    };
    auto &t=native_renderer::tuning();t.asyncBuilds=false;t.fastHooks=true;

    setup(0x14u,0x11);
    decoded(0x00306090u,"4-bit fast/reference decoders use active transfer colours and zero alpha");
    uint64_t zero=decodedTexture(h);
    check(zero && uploadedColour(zero,0x00306090u),"GPU upload uses the same active CLUT as decoding");
    const size_t count=uploads.size();++g_frame;
    check(decodedTexture(h)==zero && uploads.size()==count,"unchanged active palette reuses its content key");
    put(active+4u,0x40306090u);++g_frame;
    uint64_t faded=decodedTexture(h);
    check(faded && faded!=zero && uploadedColour(faded,0x40306090u),"in-place alpha fade changes the key and upload while file CLUT stays fixed");
    put(work+0xCu,other);++g_frame;
    decoded(0x80604020u,"scroll buffer selection follows work+0x0c rather than a fixed base copy");
    uint64_t switched=decodedTexture(h);
    check(switched && switched!=faded && uploadedColour(switched,0x80604020u),"switching the active buffer refreshes GPU colours");
    TexSource captured;const bool copied=texSource(h,captured);
    put(other+4u,0x10203040u); // guest changes after dispatch to an async worker
    std::vector<uint8_t> capturedPixels;uint32_t capturedLevels=0;
    const bool built=copied && decodeTexture(captured,capturedPixels,capturedLevels);
    uint32_t capturedFirst=0;if(capturedPixels.size()>=4)std::memcpy(&capturedFirst,capturedPixels.data(),4);
    check(built && capturedFirst==0x80604020u && capturedLevels==1,
          "TexSource keeps its palette snapshot after guest memory changes");
    decoded(0x10203040u,"a later decode sees the changed palette without mutating an earlier snapshot");

    setup(0x13u,207); // CSM1 physical entry 215, outside a sparse 16-entry sample
    put(active+215u*4u,0x70123456u);++g_frame;
    decoded(0x70123456u,"8-bit active CLUT keeps CSM1 entry ordering");
    uint64_t eight=decodedTexture(h);put(active+215u*4u,0x20123456u);++g_frame;
    uint64_t changed=decodedTexture(h);
    check(eight && changed && eight!=changed && uploadedColour(changed,0x20123456u),"all 256 active entries participate in palette invalidation");

    setup(0x14u,0x11);put(h+0xCu,0);
    decoded(0x80502010u,"uninitialized work falls back to the file palette");
    put(h+0xCu,work);put(work+0xCu,0);
    decoded(0x80502010u,"null active selection falls back to the file palette");
    put(h+0xCu,0x10000000u);
    decoded(0x80502010u,"IO work address is rejected instead of wrapping into RAM");
    put(h+0xCu,0x1FFFFF8u);
    decoded(0x80502010u,"work record crossing RAM end is rejected before dereference");
    put(h+0xCu,work);put(work+0xCu,0x1FFFFE0u);
    decoded(0x80502010u,"active CLUT crossing RAM end falls back without wrapping");
    put(work+0xCu,active+1u);
    decoded(0x80502010u,"unaligned active CLUT is rejected");
    put(h+0xCu,0x20000000u|work);put(work+0xCu,0x80000000u|active);
    decoded(0x00306090u,"EE uncached work and KSEG RAM palette aliases are accepted");
    put(h+0xCu,0x30100000u);put(0x100000u+0xCu,0x30101000u);
    for(uint32_t i=0;i<16;++i)put(0x101000u+i*4u,0x70503010u);
    decoded(0x70503010u,"accelerated 0x30100000 work and CLUT aliases select their RAM copies");
    put(h+0xCu,work);
    put(work+0xCu,0x10000000u|active);
    decoded(0x80502010u,"non-RAM active pointer is rejected instead of masked into RAM");
    for(uint32_t i=0;i<16;++i)put(0x1FFFFC0u+i*4u,0x60504030u);
    put(work+0xCu,0x1FFFFC0u);
    decoded(0x60504030u,"a complete CLUT ending exactly at RAM end is valid");
    put(work+0xCu,0);put(h+0x18u,0xFFFFFFFFu);
    TexSource invalid;std::vector<uint8_t> rgba;uint32_t levels=0;++g_frame;
    check(!texSource(h,invalid) && !decodeTextureReference(h,rgba,levels) && decodedTexture(h)==0,
          "invalid active and overflowing file CLUT reject the texture consistently");

    setup(0x00u,0);put(h+0xCu,0xFFFFFFFFu);put(pixels,0x40302010u);
    decoded(0x40302010u,"direct RGBA textures do not require any CLUT work");
    std::printf("%s: %u transfer-palette checks, %d failures\n",failures?"FAIL":"PASS",checks,failures);
    return failures ? 1 : 0;
}
