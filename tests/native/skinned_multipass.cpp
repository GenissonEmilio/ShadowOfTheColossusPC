// Synthetic VIF/material integration fixture. No game bytes, assets, save or GPU.
#ifndef SOTC_NATIVE_TEST_SOURCE
#define SOTC_NATIVE_TEST_SOURCE "../../game/overrides/sotc_native.cpp"
#endif
#include SOTC_NATIVE_TEST_SOURCE
#include <array>
#include <map>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x10000;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0x4000;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[0x4000] = {};

namespace {
    struct CapturedMesh {
        std::vector<native_renderer::Vertex> vertices;
        std::vector<native_renderer::SkinVertex> skin;
        std::vector<uint32_t> indices;
    };
    struct CapturedDraw {
        uint64_t mesh;
        std::array<float,16> transform;
        native_renderer::DrawState state;
    };
    std::map<uint64_t,CapturedMesh> meshes;
    std::vector<CapturedDraw> draws;
    std::vector<native_renderer::Vertex> frameVertices;
    std::vector<uint32_t> frameIndices;
    std::vector<float> palette;
    bool useGpu = false;
    unsigned failures = 0, checks = 0;
    void check(bool ok, const char *message) {
        ++checks;
        if (!ok) { ++failures; std::fprintf(stderr,"FAIL: %s\n",message); }
    }
    bool near(float a,float b) { return std::fabs(a-b)<0.0001f; }
    void originalModel(uint8_t *,R5900Context *,PS2Runtime *) {}
    constexpr uint32_t instance=0x1000, object=0x2000, header=0x3000;
    constexpr uint32_t surface=0x4000, packets=0x5000, data=0x6000;
    constexpr uint32_t own=0x8000, ret=0x9000, camera=0xa000, matrix=0xb000;
    constexpr uint32_t textureTable=0xc000, textureObjects=0xc100, textureHeaders=0xd000;
    const std::array<float,16> identity={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const std::array<float,16> bone={2,0,0,0, 0,3,0,0, 0,0,4,0, 10,20,30,1};
    // All coordinates and weights below are deliberately invented.
    const float positions[3][3]={{1,2,3},{2,2,3},{1,3,3}};
    const int16_t normals[3][4]={{0,0,4096,0},{0,0,4096,0},{0,0,4096,0}};
    const int16_t uv[3][4]={{0,0,2048,1024},{4096,0,3072,1024},{0,4096,2048,3072}};
    const float weights[3][4]={{0,0,0,0},{0.25f,1.75f,0,0},{1,0,0,0}};
    std::vector<uint8_t> ram(PS2_RAM_SIZE);
    template<class T> void put(uint32_t at,const T &value) { std::memcpy(ram.data()+at,&value,sizeof(value)); }
    template<class T> void append(std::vector<uint8_t> &out,const T &value) {
        const auto *p=reinterpret_cast<const uint8_t *>(&value);out.insert(out.end(),p,p+sizeof(value));
    }
    template<class T> void unpack(std::vector<uint8_t> &out,uint32_t command,uint32_t field,const T &payload) {
        append(out,(command<<24)|(3u<<16)|field);append(out,payload);
        while(out.size()%4)out.push_back(0);
    }
    uint32_t packet(bool skin,bool separateUv) {
        std::vector<uint8_t> bytes;
        append(bytes,0x01000606u); // STCYCL; parser consumes the following UNPACKs.
        unpack(bytes,0x68,1,positions);
        if(skin || separateUv) unpack(bytes,0x6d,2,normals);
        else {
            // Rigid lit packets carry a float normal beside packed ST1/ST2.
            const float floatNormals[3][3]={{0,0,1},{0,0,1},{0,0,1}};
            unpack(bytes,0x68,2,floatNormals);
        }
        if(separateUv) {
            const int16_t st[3][2]={{0,0},{4096,0},{0,4096}};
            unpack(bytes,0x65,3,st);
        } else unpack(bytes,0x6d,3,uv);
        const uint8_t colours[3][4]={{128,128,128,128},{128,128,128,128},{128,128,128,128}};
        unpack(bytes,0x6e,4,colours);
        if(skin) {
            std::array<std::array<uint32_t,4>,3> packed{};
            for(unsigned i=0;i<3;++i) {
                std::memcpy(packed[i].data(),weights[i],12);
                packed[i][3]=i==1 ? 2u:1u;
            }
            unpack(bytes,0x6c,5,packed);
        }
        append(bytes,0x17000000u); // MSCNT ends the batch.
        std::memcpy(ram.data()+data,bytes.data(),bytes.size());
        return static_cast<uint32_t>(bytes.size());
    }
    void setup(bool skin,bool two,bool fast,bool gpu) {
        std::fill(ram.begin(),ram.end(),0);
        meshes.clear();draws.clear();frameVertices.clear();frameIndices.clear();palette.clear();useGpu=gpu;
        using namespace sotc_native;
        g_ram=ram.data();g_frame=0;ps2_stats::vif1Chains.store(0);
        g_cameraContext=camera;g_layers={2};g_modelDisp=originalModel;g_skinnedDraws=0;
        g_skinnedPackets.clear();g_skinnedIds={};g_litPackets={};g_packetMemo={};g_skinnedPacketBytes=0;
        auto &t=native_renderer::tuning();t.fastHooks=fast;t.gpuSkinning=gpu;t.asyncBuilds=false;
        t.ps2Lists=true;t.modelDetail=0;t.packetQuickCheck=false;
        put(instance,object);put(instance+12,own);put(object+32,header);
        put(header+0x18,1u);put(header+0x1c,1u);put(header+0x50,surface);put(header+0x54,1u);put(header+0x60,packets);
        put(header+0x40,textureTable);put(header+0x44,2u);
        for(unsigned i=0;i<2;++i) {
            const uint32_t obj=textureObjects+i*0x40,nto=textureHeaders+i*0x200;
            put(textureTable+i*0x20+0x10,obj);put(obj+0x20,nto);
            put(nto,0x324f544eu);put(nto+0x14,0x100u);put(nto+0x1c,0x1000u); // one 1x1 RGBA level
            put(nto+0x100,0x80808080u);
            put(nto+0x80,5u);put(nto+0x84,i?193u:37u);put(nto+0x88,1u);put(nto+0x8c,0u);
        }
        put(surface+0x2c,0u);put(surface+0x3c,1u);
        for(auto &memo:g_textureMemo)memo={};
        put(surface+0x1c,two ? 2u:1u);
        // Different pass states make a duplicated first-pass draw fail.
        put(surface+0x60,0x48u);put(surface+0x68,0x42u); // ALPHA_1 additive
        put(surface+0x70,0x42u);put(surface+0x78,0x43u); // ALPHA_2 subtractive
        put(surface+0x80,5u);put(surface+0x88,8u);      // CLAMP_1 both
        put(surface+0x90,1u);put(surface+0x98,9u);      // CLAMP_2 U only
        put(matrix,identity);put(camera+0x40,identity);put(camera+0x5a0,identity);
        put(own,0x2000000fu);put(own+4,ret);put(ret,10u);
        put(ret+8,0x6c080000u); // Eight qwords: two synthetic palette matrices.
        put(ret+12,bone);put(ret+12+64,bone);
        put(packets,packet(skin,!two));put(packets+12,0xffffffffu);put(packets+16,data);
    }
    void verifyCase(const char *label,bool skin,bool two,bool fast,bool gpu,float opacity=1.0f) {
        setup(skin,two,fast,gpu);
        put(instance+0x244,1.0f-opacity);
        std::vector<sotc_native::Strip> decoded;
        sotc_native::readBatches(data,sotc_native::word(packets),decoded);
        check(decoded.size()==1 && decoded.front().size()==3,"one synthetic VIF strip decoded");
        if(decoded.empty())return;
        check(near(decoded[0][0].nz,1),"V4-16 normal survives a separate packed UV field");
        if(two)check(near(decoded[0][0].u2,0.5f)&&near(decoded[0][0].v2,0.25f),"packed ST2 stays distinct from ST1");
        R5900Context ctx{};SET_GPR_U32(&ctx,4,instance);SET_GPR_U32(&ctx,5,matrix);SET_GPR_U32(&ctx,31,0x1234);
        sotc_native::modelDispHook(ram.data(),&ctx,nullptr);
        check(draws.size()==(two?2u:1u),"material emits exactly its required number of passes");
        if(draws.size()!=(two?2u:1u)) {
            std::fprintf(stderr,"CASE: %s, draws=%zu\n",label,draws.size());return;
        }
        check(draws[0].state.framebufferOnlyAlphaFail && draws[0].state.alphaReference==37,"first texture forwards its dynamic GS alpha reference");
        check(near(draws[0].state.opacity,opacity),"first pass retains instance opacity");
        check(draws[0].state.blend==native_renderer::Blend::Additive && draws[0].state.clamp==3,"first pass preserves its blend and clamp");
        if(two) {
            check(draws[1].state.framebufferOnlyAlphaFail && draws[1].state.alphaReference==193,"second texture keeps its independent GS alpha reference");
            check(near(draws[1].state.opacity,opacity),"second pass retains instance opacity");
            check(draws[1].state.blend==native_renderer::Blend::Subtract && draws[1].state.clamp==1,"second pass uses its own blend and clamp");
            check(draws[0].state.writeDepth && !draws[1].state.writeDepth && draws[1].state.overlay,"second pass overlays the first without writing depth");
            check(draws[0].transform==draws[1].transform,"both passes use the same transform");
        }
        for(unsigned pass=0;pass<draws.size();++pass) {
            const auto &draw=draws[pass];const auto found=meshes.find(draw.mesh);
            check(found!=meshes.end(),"each draw references an uploaded mesh");if(found==meshes.end())continue;
            const auto &mesh=found->second;const float expectedU=pass?0.5f:0,expectedV=pass?0.25f:0;
            if(skin&&fast&&gpu) {
                check(!mesh.skin.empty() && draw.state.skinPalette!=0,"GPU path retains skinned mesh and palette");
                if(mesh.skin.empty())continue;const auto &v=mesh.skin[0];
                check(near(v.u,expectedU)&&near(v.v,expectedV),"GPU pass selects the correct UV pair");
                check(near(v.x,1)&&near(v.y,2)&&near(v.z,3)&&near(v.nz,1),"GPU bind pose and normal remain intact");
                check(palette.size()==32 && near(palette[12],10),"GPU palette contains both supplied matrices");
                if(two)check(draw.state.skinPalette==draws[0].state.skinPalette && draw.state.skinPaletteCount==draws[0].state.skinPaletteCount,"GPU passes share the same palette slice");
            } else {
                check(!mesh.vertices.empty(),"CPU/rigid path supplies vertices");if(mesh.vertices.empty())continue;
                const auto &v=mesh.vertices[0];
                check(near(v.u,expectedU)&&near(v.v,expectedV),"CPU/rigid pass selects the correct UV pair");
                check(near(v.x,skin?12:1)&&near(v.y,skin?26:2)&&near(v.z,skin?42:3),"CPU pose/rigid positions match synthetic matrix arithmetic");
                check(near(v.nz,skin?4:1),"CPU pose/rigid normal stays separate from UV data");
                check(draw.state.skinPalette==0,"CPU and rigid draws do not request a GPU palette");
            }
        }
    }
    void verifyAlphaMetadata() {
        setup(true,true,true,true);
        native_renderer::DrawState state;
        auto resolve=[&](uint32_t field=0x2cu) {
            state.framebufferOnlyAlphaFail=true;state.alphaReference=123;
            sotc_native::surfaceAlphaTest(header,0,state,field);
        };
        for(uint32_t reference:{0u,1u,64u,128u,255u}) {
            put(textureHeaders+0x84,reference);resolve();
            check(state.framebufferOnlyAlphaFail && state.alphaReference==reference,"all valid GS references survive without a fixed threshold");
        }
        const auto invalid=[&](uint32_t offset,uint32_t value,const char *message) {
            const uint32_t old=sotc_native::word(textureHeaders+offset);
            put(textureHeaders+offset,value);resolve();
            check(!state.framebufferOnlyAlphaFail && state.alphaReference==0,message);
            put(textureHeaders+offset,old);
        };
        invalid(0x80,4,"different alpha comparison retains legacy behavior");
        invalid(0x84,256,"out-of-range AREF cannot leak into AFAIL");
        invalid(0x88,0,"AFAIL KEEP is not treated as FB_ONLY");
        invalid(0x88,5,"raw AFAIL bits that enable DATE are rejected");
        invalid(0x8c,1,"Z ALWAYS override is not treated as GEQUAL");
        invalid(0,0,"invalid NTO2 signature retains legacy behavior");
        put(surface+0x18,1u<<17);resolve();
        check(!state.framebufferOnlyAlphaFail,"boss-mask TEST override cannot inherit placeholder NTO2 TEST");
        put(surface+0x18,0u);put(surface+0x2c,2u);resolve();
        check(!state.framebufferOnlyAlphaFail,"out-of-range texture index retains legacy behavior");
        put(surface+0x2c,0u);put(textureObjects+0x20,0x1fffffcu);resolve();
        check(!state.framebufferOnlyAlphaFail,"truncated NTO2 cannot wrap into RAM");
        put(textureObjects+0x20,textureHeaders);put(surface+0x1c,1u);resolve(0x3c);
        check(!state.framebufferOnlyAlphaFail,"single-texture mode has no second TEST");
        // Real modelDisp integration: a bad context-2 configuration must clear
        // the copied context-1 metadata, without losing either draw.
        setup(true,true,true,true);put(textureHeaders+0x200+0x84,256u);
        R5900Context ctx{};SET_GPR_U32(&ctx,4,instance);SET_GPR_U32(&ctx,5,matrix);
        sotc_native::modelDispHook(ram.data(),&ctx,nullptr);
        check(draws.size()==2 && draws[0].state.framebufferOnlyAlphaFail && !draws[1].state.framebufferOnlyAlphaFail && draws[1].state.alphaReference==0,"invalid second TEST cannot inherit the first pass threshold");
        setup(true,true,true,true);put(textureHeaders+0x88,0u);
        sotc_native::modelDispHook(ram.data(),&ctx,nullptr);
        check(draws.size()==2 && !draws[0].state.framebufferOnlyAlphaFail && draws[1].state.framebufferOnlyAlphaFail && draws[1].state.alphaReference==193,"valid second TEST is resolved independently of invalid first TEST");
    }
}

extern "C" bool __wrap__ZN2GS10skipRasterEv(){return true;}
extern "C" bool __wrap__ZN15native_renderer7hasMeshEm(uint64_t id){return meshes.count(id)!=0;}
extern "C" bool __wrap__ZN15native_renderer11gpuSkinningEv(){return useGpu;}
extern "C" void __wrap__ZN15native_renderer4drawEmPKfRKNS_9DrawStateE(uint64_t id,const float *m,const native_renderer::DrawState &s){
    CapturedDraw d{id,{},s};std::copy(m,m+16,d.transform.begin());draws.push_back(d);
}
extern "C" void __wrap__ZN15native_renderer10uploadMeshEmPKNS_6VertexEjb(uint64_t id,const native_renderer::Vertex *v,uint32_t n,bool){meshes[id].vertices.assign(v,v+n);}
extern "C" void __wrap__ZN15native_renderer17uploadSkinnedMeshEmPKNS_10SkinVertexEjPKjj(uint64_t id,const native_renderer::SkinVertex *v,uint32_t n,const uint32_t *ix,uint32_t ni){
    meshes[id].skin.assign(v,v+n);meshes[id].indices.assign(ix,ix+ni);
}
extern "C" native_renderer::Vertex *__wrap__ZN15native_renderer9frameMeshEj(uint32_t n){frameVertices.resize(n);frameIndices.clear();return frameVertices.data();}
extern "C" uint32_t *__wrap__ZN15native_renderer16frameMeshIndicesEjRj(uint32_t n,uint32_t &base){base=0;frameIndices.resize(n);return frameIndices.data();}
extern "C" void __wrap__ZN15native_renderer15commitFrameMeshEmjj(uint64_t id,uint32_t n,uint32_t ni){
    meshes[id].vertices.assign(frameVertices.begin(),frameVertices.begin()+n);meshes[id].indices.assign(frameIndices.begin(),frameIndices.begin()+ni);
}
extern "C" uint32_t __wrap__ZN15native_renderer12framePaletteEPKfj(const float *v,uint32_t n){palette.assign(v,v+n*4);return 1;}

int main(){
    verifyCase("GPU two textures",true,true,true,true);
    verifyCase("CPU fast two textures",true,true,true,false);
    verifyCase("CPU slow two textures",true,true,false,false);
    verifyCase("GPU single texture",true,false,true,true);
    verifyCase("CPU fast single texture",true,false,true,false);
    verifyCase("CPU slow single texture",true,false,false,false);
    verifyCase("rigid two textures",false,true,false,false);
    verifyCase("rigid single texture",false,false,false,false);
    verifyCase("GPU dual opacity",true,true,true,true,0.5f);
    verifyCase("CPU dual opacity",true,true,true,false,0.75f);
    verifyCase("single opacity",true,false,true,true,0.25f);
    verifyAlphaMetadata();
    std::printf("%s: %u checks; %u failures (synthetic skinned/rigid materials, no GPU)\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
