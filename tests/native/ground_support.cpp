// Original synthetic geometry only. No game assets or saved player geometry.
#include "../../game/overrides/decomp_ground_support.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace sotc_ground_support;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Fixture {
    std::vector<Vec3> vertices;
    std::vector<Triangle> triangles;
    uint32_t face = 8;
    float radius = .3f;
    MeshView view() const { return {vertices.data(), vertices.size(), triangles.data(), triangles.size()}; }
    Vec3 contact() const {
        Vec3 p{};
        for (auto id : triangles[face].vertex) {
            p.x += vertices[id].x / 3; p.y += vertices[id].y / 3; p.z += vertices[id].z / 3;
        }
        return p;
    }
};
Fixture ribbon(float fold = .1f, bool peak = false, bool longPath = false) {
    Fixture f;
    constexpr float xs[] = {-2,-1,1,2};
    const std::vector<float> zs = longPath ? std::vector<float>{-1,-.025f,-.0125f,0,.0125f,.025f,1} : (peak ? std::vector<float>{-1,-.025f,0,.025f,1} : std::vector<float>{-1,-.025f,.025f,1});
    if (peak) f.face=longPath ? 15 : 9;
    for (float z : zs) for (float x : xs)
        f.vertices.push_back({x,.2f*z+((peak ? z==0 : z<0) ? fold : 0),z});
    for (uint32_t z=0; z+1<zs.size(); ++z) for (uint32_t x=0; x<3; ++x) {
        const uint32_t a=z*4+x,b=a+4,c=a+1,d=b+1;
        f.triangles.push_back({{a,b,c},{-1,-1,-1},1});
        f.triangles.push_back({{c,b,d},{-1,-1,-1},1});
    }
    std::map<std::pair<uint32_t,uint32_t>,std::pair<uint32_t,uint32_t>> edges;
    for (uint32_t i=0; i<f.triangles.size(); ++i) for (uint32_t e=0;e<3;++e) {
        auto key=std::minmax(f.triangles[i].vertex[e],f.triangles[i].vertex[(e+1)%3]);
        const std::pair<uint32_t,uint32_t> edge{key.first,key.second};
        auto [it, inserted]=edges.emplace(edge,std::pair{i,e});
        if (!inserted) {
            const auto [j,k]=it->second;
            f.triangles[i].neighbor[e]=int32_t(j); f.triangles[j].neighbor[k]=int32_t(i);
        }
    }
    return f;
}
constexpr std::array<float,4> sentinel{12,34,56,78};
std::array<float,4> resolve(const Fixture& f, bool expected) {
    auto plane=sentinel;
    const auto vertices=f.vertices; const auto triangles=f.triangles;
    const bool got=resolveSupportPlane(f.view(),f.face,f.contact(),f.radius,plane);
    require(got==expected, expected ? "eligible closed microfold was refused" : "unsafe geometry was smoothed");
    require(std::memcmp(vertices.data(),f.vertices.data(),vertices.size()*sizeof(Vec3))==0,"resolver mutated vertices");
    require(std::memcmp(triangles.data(),f.triangles.data(),triangles.size()*sizeof(Triangle))==0,"resolver mutated topology");
    if (!got) require(plane==sentinel,"rejected result changed output plane");
    return plane;
}
void expectedNormal(const std::array<float,4>& plane, Vec3 n, Vec3 contact) {
    const float length=std::sqrt(plane[0]*plane[0]+plane[1]*plane[1]+plane[2]*plane[2]);
    require(std::isfinite(length)&&std::fabs(length-1)<1e-4f,"support normal is not finite/unit");
    require(plane[0]*n.x+plane[1]*n.y+plane[2]*n.z>.999f,"support normal disagrees with broad ramp");
    require(std::fabs(plane[0]*contact.x+plane[1]*contact.y+plane[2]*contact.z+plane[3])<1e-4f,"orientation plane was not anchored at supplied contact");
}
void tests() {
    const float inv=1/std::sqrt(1.04f);
    auto f=ribbon(); expectedNormal(resolve(f,true),{0,inv,-.2f*inv},f.contact());
    resolve(ribbon(0),false); // broad gentle surface already has the correct normal
    auto multi=ribbon(.05f,true);
    expectedNormal(resolve(multi,true),{0,inv,-.2f*inv},multi.contact()); // 2/3 edge hops through a folded ribbon
    multi.face=8;
    expectedNormal(resolve(multi,true),{0,inv,-.2f*inv},multi.contact()); // four-hop support through three thin triangles
    resolve(ribbon(.05f,true,true),false); // one supporting ramp requires five hops
    for (auto& p:f.vertices) { const float x=p.x; p.x=2*p.z+12; p.y=2*p.y+3; p.z=-2*x-20; }
    f.radius*=2;
    expectedNormal(resolve(f,true),{-.2f*inv,inv,0},f.contact()); // transformed/scaled mesh

    f=ribbon(); f.triangles[f.face].neighbor[0]=-1; resolve(f,false); // open dropoff
    f=ribbon(); f.triangles[f.face].neighbor[0]=-2; resolve(f,false); // nonmanifold edge
    f=ribbon(); f.triangles[f.face].neighbor[0]=999; resolve(f,false); // bad adjacency index
    f=ribbon(); {
        const auto other=f.triangles[f.face].neighbor[0];
        for(auto& n:f.triangles[other].neighbor) if(n==int32_t(f.face)) n=-1;
    } resolve(f,false); // one-way neighbor must not masquerade as a shared edge
    f=ribbon(); for(auto& t:f.triangles) if(f.vertices[t.vertex[0]].z>=.025f) t.attribute=2;
    resolve(f,false); // different material beyond the crease
    resolve(ribbon(1),false); // real tall wall/ridge, not a microfold
    f=ribbon(); for(auto& p:f.vertices) if(p.z>=.025f)p.y+=.6f; resolve(f,false); // large step
    f=ribbon(); for(auto& p:f.vertices) {p.z*=.1f;p.y*=.1f;} resolve(f,false); // no broad support areas
    f=ribbon(); for(auto& t:f.triangles)std::swap(t.vertex[0],t.vertex[1]); resolve(f,false); // downward/invalid support winding
    f=ribbon(); f.vertices[f.triangles[f.face].vertex[0]].x=std::numeric_limits<float>::quiet_NaN(); resolve(f,false);
    f=ribbon(); f.radius=0; resolve(f,false);
    f=ribbon(); f.radius=-.3f; resolve(f,false);
    f=ribbon(); auto plane=sentinel;
    require(!resolveSupportPlane(f.view(),999,f.contact(),f.radius,plane)&&plane==sentinel,"invalid face changed output");
    auto bad=f.view(); bad.vertices=nullptr;
    require(!resolveSupportPlane(bad,f.face,f.contact(),f.radius,plane)&&plane==sentinel,"null vertices accepted");
    require(!resolveSupportPlane(f.view(),f.face,{0,std::numeric_limits<float>::infinity(),0},f.radius,plane)&&plane==sentinel,"infinite contact accepted");
}
}
int main() {
    try { tests(); std::cout<<"ground support: synthetic acceptance and safety boundaries passed\n"; return 0; }
    catch(const std::exception& e) { std::cerr<<"ground support: "<<e.what()<<'\n'; return 1; }
}
