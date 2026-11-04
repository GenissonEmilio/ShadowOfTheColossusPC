// Synthetic guest RAM and mock original callee; no game assets/process needed.
#include "../../game/overrides/decomp_ground_support.cpp"
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace sotc_ground_support;
constexpr uint32_t player=0x180000,owner=player+0x90,object=0x190000;
constexpr uint32_t handle=0x1a0000,resource=0x1a0100,record=0x1a0200;
constexpr uint32_t vertices=0x1b0000,faces=0x1c0000,matrix=0x1d0000,decomp=0x1e0000;
constexpr uint32_t rawPlane=0x1f0000,stack=0x1200000,gp=owner+0xc0;
constexpr uint32_t attributes=0x1f1000;
std::vector<uint8_t> ram(kRam);
unsigned calls=0; bool unfinished=false; R5900Context originalEnd{};
template<class T> void put(uint32_t a,T value) {std::memcpy(ram.data()+a,&value,sizeof(value));}
void require(bool ok,const char* text) {if(!ok) throw std::runtime_error(text);}
void original(uint8_t* memory,R5900Context* ctx,PS2Runtime*) {
    ++calls;
    // Model arbitrary original register/FPU/VU side effects, not just ABI saves.
    SET_GPR_U64(ctx,7,0x9182736455ull); ctx->f[4]=2.125f;
    std::memcpy(memory+0x1f2000,&calls,sizeof(calls));
    SET_GPR_U32(ctx,2,rawPlane); ctx->pc=unfinished?0x1125838:GPR_U32(ctx,31);
    originalEnd=*ctx;
}
void fixture(bool compressed) {
    std::fill(ram.begin(),ram.end(),0); cached=Part{};
    const float xs[]={-2,-1,1,2},zs[]={-1,-.025f,.025f,1};
    std::vector<Vec3> points;
    for(float z:zs) for(float x:xs) points.push_back({x,.2f*z+(z>0?.1f:0),z});
    const float transform[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 10,20,-30,1};
    std::memcpy(ram.data()+matrix,transform,64);
    put<uint32_t>(0x1465EA4,player); put<uint8_t>(player+8,9);
    put<float>(owner+0xe0c,.3f); put<uint32_t>(gp,object);
    put<uint32_t>(gp+4,0x80000008); put<uint32_t>(gp+0x30,2);
    put<uint8_t>(object+8,10); put<uint32_t>(0x1194074,0x10F1A58);
    put<uint32_t>(object+0x20+0x184,handle);put<uint32_t>(handle+0x20,resource);
    put<uint32_t>(object+0x20+0x15c,matrix);
    put<uint32_t>(resource+4,1);put<uint32_t>(resource+8,record);
    put<uint32_t>(resource+0x30,32);put<uint32_t>(resource+0x34,attributes);
    for(uint32_t i=0;i<32;++i) put<uint64_t>(attributes+i*16,0x1000);
    put<uint64_t>(attributes+23*16,0x2000);
    put<uint32_t>(record,uint32_t(points.size()));put<uint32_t>(record+4,vertices);
    put<uint32_t>(record+8,compressed?0x101:1);put<uint32_t>(record+12,decomp);
    put<uint32_t>(record+16,18);put<uint32_t>(record+20,faces);
    if(compressed) {
        const float d[8]={0,0,0,1, 4,4,4,1};std::memcpy(ram.data()+decomp,d,32);
        for(uint32_t i=0;i<points.size();++i) {
            const float p[3]={points[i].x,points[i].y,points[i].z};
            for(int k=0;k<3;++k) put<int16_t>(vertices+i*6+k*2,int16_t(std::lround(p[k]*8192)));
        }
    } else std::memcpy(ram.data()+vertices,points.data(),points.size()*sizeof(Vec3));
    uint32_t id=0;
    for(uint16_t z=0;z<3;++z) for(uint16_t x=0;x<3;++x) {
        const uint16_t a=z*4+x,b=a+4,c=a+1,d=b+1;
        for(auto ids:{std::array<uint16_t,3>{a,b,c},std::array<uint16_t,3>{c,b,d}}) {
            put<uint16_t>(faces+id*20,19);
            std::memcpy(ram.data()+faces+id*20+2,ids.data(),6);++id;
        }
    }
    uint32_t face;
    require(loadPart(ram.data(),gp,face),"synthetic rigid part decodes");
    MeshView view{cached.vertices.data(),cached.vertices.size(),cached.triangles.data(),cached.triangles.size()};
    Face f;require(describe(view,face,f),"current triangle normal decodes");
    std::array<float,4> plane{f.n.x,f.n.y,f.n.z,float(f.d)};
    std::memcpy(ram.data()+rawPlane,plane.data(),16);put<Vec3>(gp+0x20,f.center);
    require(std::abs(cached.vertices[0].x-8)<1e-4&&std::abs(cached.vertices[0].z+31)<1e-4,
            "adapter applies matrix translation");
}
void footprintFixture() {
    fixture(false);
    const float xs[]={-2,-1,1,2},zs[]={-1,-.06f,-.058f,.035f,.06f,1};
    std::vector<Vec3> points;
    for(float z:zs) for(float x:xs)
        points.push_back({x,.2f*z+(z==-.058f?-.1f:0),z});
    std::memcpy(ram.data()+vertices,points.data(),points.size()*sizeof(Vec3));
    put<uint32_t>(record,uint32_t(points.size()));put<uint32_t>(record+16,30);
    put<uint64_t>(attributes+23*16,(uint64_t(1)<<59)|0x1000);
    uint32_t id=0;
    for(uint16_t z=0;z<5;++z) for(uint16_t x=0;x<3;++x) {
        const uint16_t a=z*4+x,b=a+4,c=a+1,d=b+1;
        for(auto ids:{std::array<uint16_t,3>{a,b,c},std::array<uint16_t,3>{c,b,d}}) {
            put<uint16_t>(faces+id*20,z==4?23:19);
            std::memcpy(ram.data()+faces+id*20+2,ids.data(),6);++id;
        }
    }
    put<uint32_t>(gp+4,0x8000000f);put<uint32_t>(player+0x1b4,1);
    uint32_t face;require(loadPart(ram.data(),gp,face),"footprint part decodes");
    MeshView view{cached.vertices.data(),cached.vertices.size(),cached.triangles.data(),cached.triangles.size()};
    Face f;require(describe(view,face,f),"footprint face normal decodes");
    const std::array<float,4> plane{f.n.x,f.n.y,f.n.z,float(f.d)};
    std::memcpy(ram.data()+rawPlane,plane.data(),16);
    put<Vec3>(gp+0x10,{f.center.x-10,f.center.y-20,f.center.z+30});put<float>(gp+0x1c,1);
    put<Vec3>(gp+0x20,{f.center.x,f.center.y+.75f,f.center.z});
    require(cached.triangles[3].attribute!=cached.triangles[26].attribute,
            "real bit59 remains distinct in donor/exit material keys");
}
void invoke(uint32_t outer,uint32_t direct,bool correction) {
    std::fill(ram.begin()+stack-16,ram.begin()+stack+0x1b0,0xa5);
    put<uint32_t>(stack,outer);
    R5900Context ctx{};
    for(int i=0;i<32;++i) {
        SET_GPR_U64(&ctx,i,0x8765432100000000ull+i);ctx.f[i]=float(i)+.25f;
        ctx.vu0_vf[i]=_mm_set1_ps(float(i)+.125f);
    }
    ctx.vu0_acc=_mm_set1_ps(-7.f);ctx.vu0_q=3.25f;ctx.f_acc=.75f;ctx.fcr31=0x00810001;
    SET_GPR_U32(&ctx,29,stack);SET_GPR_U32(&ctx,31,direct);SET_GPR_U32(&ctx,4,gp);
    originalPlane=original; const unsigned before=calls;
    auto bytes=ram;
    planeHook(ram.data(),&ctx,nullptr);
    require(calls==before+1,"original callee runs exactly once");
    std::memcpy(bytes.data()+0x1f2000,&calls,sizeof(calls));
    const uint32_t output=stack+16+(outer==0x10E785C?0x180:0x30);
    R5900Context expected=originalEnd;
    if(correction) SET_GPR_U32(&expected,2,output);
    require(!std::memcmp(&ctx,&expected,sizeof(ctx)),"all original context state preserved except corrected pointer");
    for(size_t i=0;i<ram.size();++i)
        if(!correction||i<output||i>=output+16) require(ram[i]==bytes[i],"only caller-owned output slot changes");
    if(correction) {
        const auto p=read<std::array<float,4>>(ram.data(),output);
        require(p[1]>.95f&&std::abs(p[0])<1e-4&&std::abs(p[2]+.196116f)<.002,
                "hook returns gentle unit support normal");
    }
}
}
int main() {
    try {
        fixture(false);
        invoke(0x10E785C,0x1132898,true);
        invoke(0x10DCB98,0x1132898,true);
        invoke(0x10DCB8C,0x1132840,true);
        invoke(0x10E785C,0x1132840,false);
        invoke(0x123456,0x1132898,false);
        unfinished=true;invoke(0x10E785C,0x1132898,false);unfinished=false;
        put<uint8_t>(object+8,11);invoke(0x10E785C,0x1132898,false);put<uint8_t>(object+8,10);
        put<uint32_t>(gp+0x30,3);invoke(0x10E785C,0x1132898,false);put<uint32_t>(gp+0x30,2);
        uint32_t face;require(loadPart(ram.data(),gp,face),"cache initially valid");
        const float before=cached.vertices[0].x;
        put<float>(matrix+48,11);require(loadPart(ram.data(),gp,face)&&cached.vertices[0].x==before+1,
                                      "same-address matrix edit invalidates cache");
        put<float>(vertices, -3);require(loadPart(ram.data(),gp,face)&&cached.vertices[0].x==8,
                                      "same-address vertex edit invalidates cache");
        const uint32_t oldMaterial=cached.triangles[8].attribute;
        put<uint16_t>(faces+8*20,23);require(loadPart(ram.data(),gp,face)&&cached.triangles[8].attribute!=oldMaterial,
                                          "same-address face edit invalidates cache");
        put<uint32_t>(record+16,8193);require(!loadPart(ram.data(),gp,face),"oversized resource rejected");
        fixture(true);invoke(0x10E785C,0x1132898,true);
        // Same index can change meaning in a save load; different indexes
        // with identical ground admission/material can be the same support.
        for(uint32_t i=0;i<18;++i) if(i<6||i>=12) put<uint16_t>(faces+i*20,26);
        put<uint64_t>(attributes+26*16,0xA7C001000ull);
        invoke(0x10E785C,0x1132898,true);
        const uint32_t materialBefore=cached.triangles[0].attribute;
        put<uint64_t>(attributes+26*16,0xA7C002000ull);
        require(loadPart(ram.data(),gp,face)&&cached.triangles[0].attribute!=materialBefore,
                "same-address material edit invalidates cache");
        invoke(0x10E785C,0x1132898,false);
        put<uint64_t>(attributes+26*16,0xA7C001000ull);
        put<uint64_t>(attributes+26*16+8,1);
        invoke(0x10E785C,0x1132898,false);
        put<uint64_t>(attributes+26*16+8,0);
        std::array<uint8_t,16> conditional{};
        for(uint64_t bits:{0x480000000ull,0x14000000ull,0x300000000ull,0x64000000ull,0x100000000ull}) {
            std::memcpy(conditional.data(),&bits,8);
            require(!canonicalAttribute(conditional),"special or unknown collision category fails closed");
        }
        put<uint64_t>(attributes+19*16,0x480001000ull);
        invoke(0x10E785C,0x1132898,false);
        const float decoded=cached.vertices[0].x;
        put<float>(decomp,1);require(loadPart(ram.data(),gp,face)&&cached.vertices[0].x==decoded+1,
                                   "same-address decompression edit invalidates cache");
        footprintFixture();
        invoke(0x10E785C,0x1132898,false); // new policy never enters XZ accessor
        invoke(0x10DCB98,0x1132898,true);
        invoke(0x10DCB8C,0x1132840,true);
        Vec3 anchor;
        MeshView view{cached.vertices.data(),cached.vertices.size(),cached.triangles.data(),cached.triangles.size()};
        Face f;require(describe(view,15,f),"footprint seed remains original");
        require(terrainAnchor(ram.data(),gp,view,15,f,read<Vec3>(ram.data(),gp+0x20),.3f,anchor),
                "terrain anchor is independently validated");
        const auto result=read<std::array<float,4>>(ram.data(),stack+16+0x30);
        require(std::abs(result[0]*anchor.x+result[1]*anchor.y+result[2]*anchor.z+result[3])<1e-4,
                "fallback plane is anchored at terrain, not elevated probe");
        put<uint32_t>(player+0x1b4,0);invoke(0x10DCB98,0x1132898,false);
        put<uint32_t>(player+0x1b4,1);
        const auto local=read<Vec3>(ram.data(),gp+0x10);
        put<float>(gp+0x14,local.y+.2f);invoke(0x10DCB98,0x1132898,false);
        put<Vec3>(gp+0x10,local);put<float>(gp+0x1c,0);invoke(0x10DCB98,0x1132898,false);
        put<float>(gp+0x1c,1);
        put<Vec3>(gp+0x10,{local.x+5,local.y,local.z});
        put<Vec3>(gp+0x20,{local.x+15,local.y+20.75f,local.z-30});
        invoke(0x10DCB98,0x1132898,false); // coplanar but outside selected face
        footprintFixture();put<float>(gp+0x20,read<float>(ram.data(),gp+0x20)+.1f);
        invoke(0x10DCB98,0x1132898,false); // probe and hit no longer correspond
        footprintFixture();put<uint64_t>(attributes+23*16,(uint64_t(1)<<59)|0x480001000ull);
        invoke(0x10DCB98,0x1132898,false); // unknown exit category stays rejected
        std::cout<<"PASS: rigid adapter, scoped hooks, original callee state, caller scratch, content invalidation\n";
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
