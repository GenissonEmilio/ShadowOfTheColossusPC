#include "decomp_ground_support.h"

#ifndef SOTC_GROUND_SUPPORT_GEOMETRY_ONLY
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

namespace sotc_ground_support {
namespace {

Vec3 sub(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec3 add(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Vec3 mul(Vec3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }
double dot(Vec3 a, Vec3 b) { return double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z; }
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
bool finite(Vec3 a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }
struct Face {
    Vec3 n{}, center{};
    double area=0, altitude=0, d=0;
    int longest=0;
};
bool describe(const MeshView& mesh, uint32_t id, Face& out) {
    if (id>=mesh.triangleCount) return false;
    const auto& t=mesh.triangles[id];
    Vec3 p[3];
    for (int e=0;e<3;++e) {
        if(t.vertex[e]>=mesh.vertexCount) return false;
        p[e]=mesh.vertices[t.vertex[e]];
        if(!finite(p[e])) return false;
    }
    const Vec3 n=cross(sub(p[1],p[0]),sub(p[2],p[0]));
    const double length=std::sqrt(dot(n,n));
    if(!std::isfinite(length)||length<1e-8) return false;
    out.n=mul(n,float(1.0/length)); out.area=length*.5;
    out.center=mul(add(add(p[0],p[1]),p[2]),1.f/3.f);
    out.d=-dot(out.n,p[0]);
    double longest=0;
    for(int e=0;e<3;++e) {
        auto edge=sub(p[e],p[(e+1)%3]); double l=dot(edge,edge);
        if(l>longest) { longest=l; out.longest=e; }
    }
    out.altitude=length/std::sqrt(longest);
    return finite(out.n)&&std::isfinite(out.d);
}
bool reciprocal(const MeshView& mesh,uint32_t id,int e) {
    const auto& t=mesh.triangles[id]; int32_t neighbor=t.neighbor[e];
    if(neighbor<0||uint32_t(neighbor)>=mesh.triangleCount||uint32_t(neighbor)==id) return false;
    const auto& n=mesh.triangles[neighbor];
    const uint32_t a=t.vertex[e],b=t.vertex[(e+1)%3]; int matches=0;
    for(int j=0;j<3;++j) {
        // Consistent winding is necessary; coincident surfaces are not support.
        if(n.vertex[j]==b&&n.vertex[(j+1)%3]==a&&n.neighbor[j]==int32_t(id)) ++matches;
    }
    return matches==1;
}
bool closed(const MeshView& mesh,uint32_t id) {
    return reciprocal(mesh,id,0)&&reciprocal(mesh,id,1)&&reciprocal(mesh,id,2);
}

} // namespace

static bool resolveStrictSupportPlane(const MeshView& mesh,uint32_t face,Vec3 contact,
                                     float radius,std::array<float,4>& plane) {
    if(!mesh.vertices||!mesh.triangles||mesh.vertexCount>8192||mesh.triangleCount>8192||
       !finite(contact)||!std::isfinite(radius)||radius<.05f||radius>1.f) return false;
    Face seed;
    if(!describe(mesh,face,seed)||seed.n.y<=.15f||seed.n.y>=.85f||!closed(mesh,face)) return false;
    const double r2=double(radius)*radius;
    auto thin=[&](const Face& f) {
        return f.n.y>.15f&&f.altitude<=.75*radius&&f.area<=6*r2;
    };
    auto broad=[&](const Face& f) { return f.n.y>=.85f&&f.area>=8*r2; };
    if(!thin(seed)) return false;
    const auto& start=mesh.triangles[face];
    if(start.attribute==std::numeric_limits<uint32_t>::max()) return false;
    const auto edge=sub(mesh.vertices[start.vertex[(seed.longest+1)%3]],
                        mesh.vertices[start.vertex[seed.longest]]);
    Vec3 across={-edge.z,0,edge.x}; double al=std::sqrt(dot(across,across));
    if(al<radius) return false;
    across=mul(across,float(1/al));
    struct Node { uint32_t face; int parent,depth; Face geometry; };
    std::array<Node,32> nodes{};
    int count=1; nodes[0]={face,-1,0,seed};
    for(int at=0;at<count;++at) {
        // Search at most four shared edges toward each broad support. Thin
        // width/area and plane-deviation limits remain tied to the actor radius;
        // this hop limit additionally bounds work on finely tessellated meshes.
        if(broad(nodes[at].geometry)||nodes[at].depth>=4) continue;
        const auto& t=mesh.triangles[nodes[at].face];
        for(int e=0;e<3;++e) {
            if(!reciprocal(mesh,nodes[at].face,e)) return false;
            const uint32_t next=uint32_t(t.neighbor[e]);
            bool seen=false; for(int k=0;k<count;++k) seen|=nodes[k].face==next;
            if(seen) continue;
            if(mesh.triangles[next].attribute!=start.attribute) continue;
            Face f; if(!describe(mesh,next,f)) return false;
            if(!broad(f)&&(!thin(f)||!closed(mesh,next))) continue;
            if(count==int(nodes.size())) return false;
            nodes[count++]={next,at,nodes[at].depth+1,f};
        }
    }
    for(int i=1;i<count;++i) for(int j=i+1;j<count;++j) {
        const Face &a=nodes[i].geometry,&b=nodes[j].geometry;
        if(!broad(a)||!broad(b)||dot(a.n,b.n)<.99) continue;
        const double sa=dot(sub(a.center,seed.center),across);
        const double sb=dot(sub(b.center,seed.center),across);
        if(sa*sb>=0||std::min(std::abs(sa),std::abs(sb))<radius) continue;
        const double ya=-(a.n.x*double(contact.x)+a.n.z*double(contact.z)+a.d)/a.n.y;
        const double yb=-(b.n.x*double(contact.x)+b.n.z*double(contact.z)+b.d)/b.n.y;
        if(std::abs(ya-yb)>.5*radius) continue;
        bool fits=true;
        double foldLow=std::numeric_limits<double>::infinity(),foldHigh=-foldLow;
        for(int endpoint : {i,j}) {
            for(int n=nodes[endpoint].parent;n>=0;n=nodes[n].parent) {
                const auto& t=mesh.triangles[nodes[n].face];
                for(uint32_t v:t.vertex) {
                    const auto p=mesh.vertices[v];
                    const double acrossPosition=dot(sub(p,seed.center),across);
                    foldLow=std::min(foldLow,acrossPosition);foldHigh=std::max(foldHigh,acrossPosition);
                    if(std::abs(dot(a.n,p)+a.d)>.6*radius||
                       std::abs(dot(b.n,p)+b.d)>.6*radius) fits=false;
                }
            }
        }
        if(!fits||foldHigh-foldLow>2*radius) continue;
        Vec3 normal=add(mul(a.n,float(a.area)),mul(b.n,float(b.area)));
        normal=mul(normal,float(1/std::sqrt(dot(normal,normal))));
        plane={normal.x,normal.y,normal.z,float(-dot(normal,contact))};
        return true;
    }
    return false;
}

namespace {

// An XZ disk may overlap an edge, but a closest vertex alone cannot establish
// a strip crossing. Use a positive radial margin and refuse corner donors.
bool footprintDonor(const MeshView& mesh,uint32_t face,Vec3 contact,
                    float radius,double& distanceSquared) {
    const auto& t=mesh.triangles[face];
    const Vec3 a=mesh.vertices[t.vertex[0]],b=mesh.vertices[t.vertex[1]],c=mesh.vertices[t.vertex[2]];
    const double ux=double(b.x)-a.x,uz=double(b.z)-a.z;
    const double vx=double(c.x)-a.x,vz=double(c.z)-a.z;
    const double determinant=ux*vz-uz*vx;
    if(std::abs(determinant)<1e-10) return false;
    const double px=double(contact.x)-a.x,pz=double(contact.z)-a.z;
    const double s=(px*vz-pz*vx)/determinant;
    const double u=(ux*pz-uz*px)/determinant;
    constexpr double epsilon=1e-6;
    const double bary[3]={1-s-u,s,u};
    if(*std::min_element(bary,bary+3)>=-epsilon) {
        int onEdges=0; for(double v:bary) onEdges+=v<=epsilon;
        if(onEdges>=2) return false;
        distanceSquared=0;
        return true;
    }
    distanceSquared=std::numeric_limits<double>::infinity();
    bool interiorEdge=false;
    for(int e=0;e<3;++e) {
        const Vec3 p=mesh.vertices[t.vertex[e]],q=mesh.vertices[t.vertex[(e+1)%3]];
        const double dx=double(q.x)-p.x,dz=double(q.z)-p.z;
        const double lengthSquared=dx*dx+dz*dz;
        if(lengthSquared<1e-12) return false;
        const double along=std::clamp(((double(contact.x)-p.x)*dx+(double(contact.z)-p.z)*dz)/lengthSquared,0.,1.);
        const double ex=double(contact.x)-p.x-along*dx,ez=double(contact.z)-p.z-along*dz;
        const double d=ex*ex+ez*ez;
        if(d<distanceSquared) {
            distanceSquared=d;
            interiorEdge=along>epsilon&&along<1-epsilon;
        }
    }
    return interiorEdge&&distanceSquared<=.75*.75*double(radius)*radius;
}

bool resolveFootprintSupportPlane(const MeshView& mesh,uint32_t face,Vec3 contact,
                                 float radius,std::array<float,4>& plane) {
    if(!mesh.vertices||!mesh.triangles||mesh.vertexCount>8192||mesh.triangleCount>8192||
       !finite(contact)||!std::isfinite(radius)||radius<.05f||radius>1.f) return false;
    Face seed;
    if(!describe(mesh,face,seed)||seed.n.y<=.15f||seed.n.y>=.85f||!closed(mesh,face)) return false;
    const auto& start=mesh.triangles[face];
    constexpr uint32_t unsafe=std::numeric_limits<uint32_t>::max();
    if(start.attribute==unsafe) return false;
    const double r2=double(radius)*radius;
    const auto thin=[&](const Face& f) {
        return f.n.y>.01f&&f.altitude<=.75*radius&&f.area<=6*r2;
    };
    const auto broad=[&](const Face& f) { return f.n.y>=.85f&&f.area>=8*r2; };
    if(!thin(seed)) return false;
    const auto edge=sub(mesh.vertices[start.vertex[(seed.longest+1)%3]],
                        mesh.vertices[start.vertex[seed.longest]]);
    Vec3 across={-edge.z,0,edge.x}; const double length=std::sqrt(dot(across,across));
    if(length<radius) return false;
    across=mul(across,float(1/length));
    struct Node { uint32_t face; int parent,depth; Face geometry; };
    std::array<Node,64> nodes{};
    int count=1; nodes[0]={face,-1,0,seed};
    for(int at=0;at<count;++at) {
        if(broad(nodes[at].geometry)||nodes[at].depth>=6) continue;
        const auto& t=mesh.triangles[nodes[at].face];
        for(int e=0;e<3;++e) {
            if(!reciprocal(mesh,nodes[at].face,e)) return false;
            const uint32_t next=uint32_t(t.neighbor[e]);
            bool seen=false; for(int k=0;k<count;++k) seen|=nodes[k].face==next;
            if(seen||mesh.triangles[next].attribute==unsafe) continue;
            Face f; if(!describe(mesh,next,f)) return false;
            if(!closed(mesh,next)) continue;
            // A different material is allowed only as a terminal geometric
            // exit. It is never traversed, normalized or used as the donor.
            if(!broad(f)&&(mesh.triangles[next].attribute!=start.attribute||!thin(f))) continue;
            if(count==int(nodes.size())) return false;
            nodes[count++]={next,at,nodes[at].depth+1,f};
        }
    }
    std::array<bool,64> viable{};
    std::array<double,64> distances{};
    const auto branch=[&](int n) {
        while(nodes[n].parent>0) n=nodes[n].parent;
        return n;
    };
    for(int i=1;i<count;++i) {
        const Face& a=nodes[i].geometry;
        if(!broad(a)||mesh.triangles[nodes[i].face].attribute!=start.attribute||
           !footprintDonor(mesh,nodes[i].face,contact,radius,distances[i])) continue;
        for(int j=1;j<count;++j) {
            const Face& b=nodes[j].geometry;
            // Opposite centroids alone do not prove enclosure: both paths
            // must leave the seed across distinct edges, not fork later on
            // the same side of a wall or an excluded material boundary.
            if(i==j||!broad(b)||branch(i)==branch(j)||dot(a.n,b.n)<.99) continue;
            const double sa=dot(sub(a.center,seed.center),across);
            const double sb=dot(sub(b.center,seed.center),across);
            if(sa*sb>=0||std::min(std::abs(sa),std::abs(sb))<radius) continue;
            const double ya=-(a.n.x*double(contact.x)+a.n.z*double(contact.z)+a.d)/a.n.y;
            const double yb=-(b.n.x*double(contact.x)+b.n.z*double(contact.z)+b.d)/b.n.y;
            if(std::abs(ya-yb)>.5*radius) continue;
            bool fits=true;
            double low=std::numeric_limits<double>::infinity(),high=-low;
            for(int endpoint:{i,j}) for(int n=nodes[endpoint].parent;n>=0;n=nodes[n].parent) {
                const auto& t=mesh.triangles[nodes[n].face];
                for(uint32_t v:t.vertex) {
                    const auto p=mesh.vertices[v];
                    const double position=dot(sub(p,seed.center),across);
                    low=std::min(low,position);high=std::max(high,position);
                    if(std::abs(dot(a.n,p)+a.d)>.6*radius||
                       std::abs(dot(b.n,p)+b.d)>.6*radius) fits=false;
                }
            }
            if(fits&&high-low<=radius) viable[i]=true;
        }
    }
    int chosen=-1;
    for(int i=1;i<count;++i) if(viable[i]) {
        // Consider every viable donor before selecting one. Nearby but
        // disagreeing overlapping floors are ambiguous, not a first-match win.
        for(int j=1;j<i;++j) if(viable[j]) {
            const Face& a=nodes[i].geometry; const Face& b=nodes[j].geometry;
            const double ya=-(a.n.x*double(contact.x)+a.n.z*double(contact.z)+a.d)/a.n.y;
            const double yb=-(b.n.x*double(contact.x)+b.n.z*double(contact.z)+b.d)/b.n.y;
            if(dot(a.n,b.n)<.9999||std::abs(ya-yb)>.05*radius) return false;
        }
        if(chosen<0||distances[i]<distances[chosen]) chosen=i;
    }
    if(chosen<0) return false;
    const Vec3 normal=nodes[chosen].geometry.n;
    // Orientation only: the caller's actual contact remains the anchor. No
    // extrapolated floor height and no exit normal enters the result.
    plane={normal.x,normal.y,normal.z,float(-dot(normal,contact))};
    return true;
}

} // namespace

bool resolveSupportPlane(const MeshView& mesh,uint32_t face,Vec3 contact,
                         float radius,std::array<float,4>& plane,bool allowFootprint) {
    if(resolveStrictSupportPlane(mesh,face,contact,radius,plane)) return true;
    if(!allowFootprint||!mesh.vertices||!mesh.triangles||mesh.vertexCount>8192||mesh.triangleCount>8192||
       !finite(contact)||!std::isfinite(radius)||radius<.05f||radius>1.f) return false;
    return resolveFootprintSupportPlane(mesh,face,contact,radius,plane);
}

#ifndef SOTC_GROUND_SUPPORT_GEOMETRY_ONLY
namespace {

constexpr uint32_t kRam=0x2000000, kMaxFaces=8192, kMaxVertices=8192;
constexpr size_t kMaxBytes=512*1024;
PS2Runtime::RecompiledFunction originalPlane=nullptr;
bool range(uint32_t p,size_t n) { return p>=0x100000&&n<=kRam&&p<=kRam-n; }
template<class T> T read(const uint8_t* ram,uint32_t p) {
    T value{}; if(range(p,sizeof(T))) std::memcpy(&value,ram+p,sizeof(T)); return value;
}
struct Block {
    uint32_t address=0; std::vector<uint8_t> bytes;
    bool matches(const uint8_t* ram,uint32_t p,size_t size) const {
        return p==address&&size==bytes.size()&&range(p,size)&&!std::memcmp(bytes.data(),ram+p,size);
    }
    void set(const uint8_t* ram,uint32_t p,size_t size) {
        address=p; bytes.assign(ram+p,ram+p+size);
    }
};
struct Part {
    uint32_t object=0,resource=0,record=0,matrixAddress=0,vertexAddress=0,faceAddress=0;
    uint32_t nv=0,nf=0,formats=0,decompression=0,node=0;
    std::array<float,16> matrix{};
    std::array<float,8> decompressionData{};
    Block vertexBytes,faceBytes,attributeBytes;
    std::vector<Vec3> vertices;
    std::vector<Triangle> triangles;
    bool valid=false;
};
Part cached;
struct Edge { uint32_t a,b,face; int slot; };

bool canonicalAttribute(std::array<uint8_t,16>& record) {
    uint64_t bits; std::memcpy(&bits,record.data(),8);
    constexpr uint64_t highMask=0xF80000000ull,lowMask=0x7C000000ull;
    const uint64_t high=bits&highMask,low=bits&lowMask;
    // These four combinations take the same unconditional default admission
    // in manager_sub_010d41e0 (character probe) and10dc808 (slide exception).
    // This is a deliberately narrow category whitelist, NOT a camera-bit mask:
    // Their source dispatches do not branch on either accepted value.
    // In particular10dc844/868 excludes high0x480000000 and10dc878
    // excludes low0x14000000; neither is eligible here.
    // Unknown/conditional categories fail closed, even if another filter
    // configuration might admit them. Preserve every other bit and all8
    // trailing material bytes; original guest attributes are never modified.
    if((high!=0&&high!=0xA00000000ull)||(low!=0&&low!=0x7C000000ull)) return false;
    bits&=~(highMask|lowMask); std::memcpy(record.data(),&bits,8);
    return true;
}

bool loadPart(const uint8_t* ram,uint32_t gp,uint32_t& face) {
    if(!range(gp,0x34)||read<uint32_t>(ram,gp+0x30)!=2) return false;
    const uint32_t object=read<uint32_t>(ram,gp),packed=read<uint32_t>(ram,gp+4);
    if(!(packed&0x80000000)||!range(object,0x220)||read<uint8_t>(ram,object+8)!=10||
       read<uint32_t>(ram,0x1194074)!=0x10F1A58) return false;
    const uint32_t layout=object+0x20,handle=read<uint32_t>(ram,layout+0x184);
    if(!range(handle,0x24)) return false;
    const uint32_t resource=read<uint32_t>(ram,handle+0x20);
    if(!range(resource,0x38)) return false;
    const uint32_t parts=read<uint32_t>(ram,resource+4),node=(packed>>24)&127;
    const uint32_t records=read<uint32_t>(ram,resource+8);
    if(!parts||parts>128||node>=parts||!range(records,size_t(parts)*0x90)) return false;
    const uint32_t record=records+node*0x90;
    const uint32_t nv=read<uint32_t>(ram,record),vp=read<uint32_t>(ram,record+4);
    const uint32_t formats=read<uint32_t>(ram,record+8),dec=read<uint32_t>(ram,record+12);
    const uint32_t nf=read<uint32_t>(ram,record+16),fp=read<uint32_t>(ram,record+20);
    const uint32_t na=read<uint32_t>(ram,resource+0x30),ap=read<uint32_t>(ram,resource+0x34);
    face=packed&0xffffff;
    if(!nv||nv>kMaxVertices||!nf||nf>kMaxFaces||face>=nf) return false;
    const bool compact=(formats&255)!=0,compressed=((formats>>8)&255)!=0;
    const size_t vs=compressed?6:(compact?12:16),fs=compact?20:52;
    if(!na||na>4096||size_t(nv)*vs+size_t(nf)*fs+size_t(na)*16>kMaxBytes||
       !range(vp,size_t(nv)*vs)||!range(fp,size_t(nf)*fs)||!range(ap,size_t(na)*16)) return false;
    const uint32_t matrices=read<uint32_t>(ram,layout+0x15c);
    if(!range(matrices,size_t(parts)*64)) return false;
    const uint32_t ma=matrices+node*64;
    std::array<float,16> m; std::memcpy(m.data(),ram+ma,64);
    for(float f:m) if(!std::isfinite(f)) return false;
    if(std::abs(m[3])>1e-5||std::abs(m[7])>1e-5||std::abs(m[11])>1e-5||std::abs(m[15]-1)>1e-5) return false;
    std::array<float,8> d{};
    if(compressed) {
        if(!range(dec,32)) return false;
        std::memcpy(d.data(),ram+dec,32);
        for(float f:d) if(!std::isfinite(f)) return false;
    }
    if(cached.valid&&cached.object==object&&cached.resource==resource&&cached.record==record&&
       cached.nv==nv&&cached.nf==nf&&cached.formats==formats&&cached.node==node&&
       cached.matrixAddress==ma&&cached.decompression==dec&&
       !std::memcmp(cached.matrix.data(),m.data(),64)&&!std::memcmp(cached.decompressionData.data(),d.data(),32)&&
       cached.vertexBytes.matches(ram,vp,size_t(nv)*vs)&&cached.faceBytes.matches(ram,fp,size_t(nf)*fs)&&
       cached.attributeBytes.matches(ram,ap,size_t(na)*16)) return true;
    cached.valid=false;
    cached.object=object;cached.resource=resource;cached.record=record;cached.node=node;
    cached.nv=nv;cached.nf=nf;cached.formats=formats;cached.decompression=dec;
    cached.matrixAddress=ma;cached.matrix=m;cached.decompressionData=d;
    cached.vertexBytes.set(ram,vp,size_t(nv)*vs); cached.faceBytes.set(ram,fp,size_t(nf)*fs);
    cached.attributeBytes.set(ram,ap,size_t(na)*16);
    std::map<std::array<uint8_t,16>,uint32_t> materials;
    std::vector<uint32_t> materialKeys(na,std::numeric_limits<uint32_t>::max());
    for(uint32_t i=0;i<na;++i) {
        std::array<uint8_t,16> material;std::memcpy(material.data(),ram+ap+i*16,16);
        if(canonicalAttribute(material)) {
            const auto result=materials.emplace(material,uint32_t(materials.size()));
            materialKeys[i]=result.first->second;
        }
    }
    cached.vertices.resize(nv); cached.triangles.resize(nf);
    for(uint32_t i=0;i<nv;++i) {
        float local[4]={0,0,0,1};
        if(compressed) for(int k=0;k<3;++k) local[k]=d[k]+read<int16_t>(ram,vp+i*6+k*2)*d[k+4]/32768.f;
        else std::memcpy(local,ram+vp+i*vs,vs);
        Vec3 p{}; float* xyz=&p.x;
        for(int k=0;k<3;++k) xyz[k]=m[k]*local[0]+m[4+k]*local[1]+m[8+k]*local[2]+m[12+k]*local[3];
        if(!finite(p)) return false;
        cached.vertices[i]=p;
    }
    std::vector<Edge> edges; edges.reserve(size_t(nf)*3);
    for(uint32_t i=0;i<nf;++i) {
        auto& t=cached.triangles[i]; const uint32_t p=fp+uint32_t(i*fs);
        const uint32_t attribute=compact?read<uint16_t>(ram,p):read<uint32_t>(ram,p+0x30);
        if(attribute>=na) return false;
        t.attribute=materialKeys[attribute];
        t.neighbor={-1,-1,-1};
        for(int e=0;e<3;++e) {
            t.vertex[e]=compact?read<uint16_t>(ram,p+2+e*2):read<uint32_t>(ram,p+e*4);
            if(t.vertex[e]>=nv) return false;
        }
        if(t.vertex[0]==t.vertex[1]||t.vertex[1]==t.vertex[2]||t.vertex[2]==t.vertex[0]) return false;
        for(int e=0;e<3;++e) {
            const uint32_t a=t.vertex[e],b=t.vertex[(e+1)%3];
            edges.push_back({std::min(a,b),std::max(a,b),i,e});
        }
    }
    std::sort(edges.begin(),edges.end(),[](const Edge& a,const Edge& b) {
        return a.a<b.a||(a.a==b.a&&a.b<b.b);
    });
    for(size_t begin=0;begin<edges.size();) {
        size_t end=begin+1; while(end<edges.size()&&edges[end].a==edges[begin].a&&edges[end].b==edges[begin].b) ++end;
        if(end-begin==2) {
            const auto &a=edges[begin],&b=edges[begin+1];
            cached.triangles[a.face].neighbor[a.slot]=int32_t(b.face);
            cached.triangles[b.face].neighbor[b.slot]=int32_t(a.face);
        } else if(end-begin>2) for(size_t k=begin;k<end;++k) cached.triangles[edges[k].face].neighbor[edges[k].slot]=-2;
        begin=end;
    }
    cached.valid=true;
    return true;
}

// GP+20 is the probe/root, not the terrain hit. The selected rigid GP keeps
// its actual local hit at +10. Accept it only after transforming and checking
// the original triangle; a stale or unrelated contact cannot enable fallback.
bool terrainAnchor(const uint8_t* ram,uint32_t gp,const MeshView& mesh,
                   uint32_t face,const Face& geometry,Vec3 probe,float radius,
                   Vec3& anchor) {
    const Vec3 local=read<Vec3>(ram,gp+0x10);
    const float w=read<float>(ram,gp+0x1c);
    if(!finite(local)||!std::isfinite(w)||std::abs(w-1.f)>1e-4f) return false;
    const auto& m=cached.matrix;
    anchor={m[0]*local.x+m[4]*local.y+m[8]*local.z+m[12],
            m[1]*local.x+m[5]*local.y+m[9]*local.z+m[13],
            m[2]*local.x+m[6]*local.y+m[10]*local.z+m[14]};
    if(!finite(anchor)||!finite(probe)||
       std::abs(dot(geometry.n,anchor)+geometry.d)>.015) return false;
    const double dx=double(anchor.x)-probe.x,dz=double(anchor.z)-probe.z;
    if(dx*dx+dz*dz>.05*.05*double(radius)*radius) return false;
    const auto& t=mesh.triangles[face];
    const Vec3 u=sub(mesh.vertices[t.vertex[1]],mesh.vertices[t.vertex[0]]);
    const Vec3 v=sub(mesh.vertices[t.vertex[2]],mesh.vertices[t.vertex[0]]);
    const Vec3 p=sub(anchor,mesh.vertices[t.vertex[0]]);
    const double uu=dot(u,u),uv=dot(u,v),vv=dot(v,v),pu=dot(p,u),pv=dot(p,v);
    const double determinant=uu*vv-uv*uv;
    if(determinant<=1e-12) return false;
    const double a=(pu*vv-pv*uv)/determinant,b=(pv*uu-pu*uv)/determinant;
    return a>=-.002&&b>=-.002&&a+b<=1.002;
}

void planeHook(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    const uint32_t directRA=GPR_U32(ctx,31),sp=GPR_U32(ctx,29),gp=GPR_U32(ctx,4);
    const uint32_t outerRA=range(sp,16)?read<uint32_t>(ram,sp):0;
    const bool xz=directRA==0x1132898&&outerRA==0x10E785C;
    const bool slide=directRA==0x1132898&&outerRA==0x10DCB98;
    const bool slope=directRA==0x1132840&&outerRA==0x10DCB8C;
    const uint32_t player=read<uint32_t>(ram,0x1465EA4),owner=player+0x90;
    const bool eligible=(xz||slide||slope)&&range(player,0xe00)&&read<uint8_t>(ram,player+8)==9&&gp==owner+0xc0;
    originalPlane(ram,ctx,runtime);
    if(!eligible||ctx->pc!=directRA||GPR_U32(ctx,29)!=sp) return;
    uint32_t face=0;
    if(!loadPart(ram,gp,face)) return;
    const auto& t=cached.triangles[face];
    Face current;
    MeshView mesh{cached.vertices.data(),cached.vertices.size(),cached.triangles.data(),cached.triangles.size()};
    if(!describe(mesh,face,current)) return;
    const uint32_t raw=GPR_U32(ctx,2);
    if(!range(raw,16)) return;
    std::array<float,4> original; std::memcpy(original.data(),ram+raw,16);
    const Vec3 originalNormal{original[0],original[1],original[2]};
    if(!finite(originalNormal)||!std::isfinite(original[3])||
       std::abs(dot(originalNormal,originalNormal)-1)>.01||dot(current.n,originalNormal)<.999) return;
    for(uint32_t v:t.vertex) if(std::abs(dot(originalNormal,cached.vertices[v])+original[3])>.015) return;
    const Vec3 contact=read<Vec3>(ram,gp+0x20);
    // XZ10e79ac loads this field into f20;10e7a38 passes it as the
    // slide-query f12 radius. +6d0 is a movement offset vector, not radius.
    const float radius=read<float>(ram,owner+0xe0c);
    std::array<float,4> support;
    bool corrected=resolveSupportPlane(mesh,face,contact,radius,support);
    bool usedFootprint=false;
    // XZ rotates the requested movement using the normal's XYZ components.
    // Keep that orientation coherent with both subsequent slide projections.
    if(!corrected&&read<uint32_t>(ram,player+0x1b4)) {
        Vec3 terrain;
        if(terrainAnchor(ram,gp,mesh,face,current,contact,radius,terrain)) {
            usedFootprint=resolveFootprintSupportPlane(mesh,face,terrain,radius,support);
            corrected=usedFootprint;
        }
    }
    if(!corrected) return;
    // Both accessors reserve 16 bytes and save the outer RA at SP. Their
    // callers own these output slots throughout the following consumption:
    // XZ: SP+180 is unused (frame260; later vectors end at170, scalars at1a0).
    // AddSlide: SP+30 is first written at10dcc08, AFTER its plane projection.
    const uint32_t output=sp+16+(xz?0x180:0x30);
    if(!range(output,16)||(output&15)) return;
    std::memcpy(ram+output,support.data(),16);
    SET_GPR_U32(ctx,2,output);
    static const bool log=std::getenv("SOTC_GROUND_SUPPORT_LOG")!=nullptr;
    static const char* logFace=std::getenv("SOTC_GROUND_SUPPORT_LOG_FACE");
    static const uint32_t filter=logFace?uint32_t(std::strtoul(logFace,nullptr,0)):
        std::numeric_limits<uint32_t>::max();
    static unsigned accepted=0;
    if(log&&(filter==std::numeric_limits<uint32_t>::max()||filter==face)&&accepted++<64)
        std::fprintf(stderr,"[ground-support] face=%u caller=%08x footprint=%u normal=(%.6f,%.6f,%.6f)->(%.6f,%.6f,%.6f)\n",
            face,outerRA,unsigned(usedFootprint),original[0],original[1],original[2],support[0],support[1],support[2]);
}
} // namespace

void installGroundSupport(PS2Runtime& runtime) {
    const char* setting=std::getenv("SOTC_GROUND_SUPPORT");
    if(setting&&std::strcmp(setting,"0")==0) return;
    auto fn=runtime.guestFunctionAt(0x11257A0);
    if(!fn||fn==planeHook) return;
    cached=Part{}; originalPlane=fn;
    runtime.registerFunction(0x11257A0,planeHook);
}
#endif

} // namespace sotc_ground_support
