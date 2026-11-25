// ROM-free regression: a guest callback observes its native caller's frame.
// The handles/objects/callbacks below are synthetic, not game-derived assets.
#ifndef CHARA_SOURCE
#define CHARA_SOURCE "../../game/overrides/decomp_chara.cpp"
#endif
#include CHARA_SOURCE
#include <cstdio>
#include <vector>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {};

namespace {
    constexpr uint32_t character=0x180000, self=0x170000, other=0x175000;
    constexpr uint32_t target=0x176000, listHandle=0x160000, listTable=0x160100;
    constexpr uint32_t entries=0x160200, noTableGlobal=0x200000, selfGlobal=0x200008;
    constexpr uint32_t stack=0x1200000, frame=stack-0x70, heap=0x380000;
    constexpr uint64_t argument=0x1234567800180200ull, returnAddress=0x1234000000004000ull;
    std::vector<uint8_t> ram(PS2_RAM_SIZE);
    uint64_t saved[9];
    unsigned failures=0, calls=0, count=0;
    int mode=0;
    void check(bool condition,const char *what) {
        if(!condition){++failures;std::fprintf(stderr,"FAIL: mode=%d call=%u %s\n",mode,calls,what);}
    }
    template<typename T> T read(uint32_t a){T v;std::memcpy(&v,ram.data()+a,sizeof(v));return v;}
    template<typename T> void put(uint32_t a,T v){std::memcpy(ram.data()+a,&v,sizeof(v));}
    void put32(uint32_t a,uint32_t v){put(a,v);}
    void relocation(uint32_t hi,uint32_t lo,uint32_t address) {
        // Synthetic LUI/load reference, including signed low-immediate handling.
        put32(hi,0x3c020000u|((address+0x8000u)>>16));
        put32(lo,address&65535u);
    }
    void checkFrame(R5900Context *c) {
        check(GPR_U32(c,29)==frame,"callee sees native caller's guest SP");
        for(unsigned i=0;i<8;++i)
            check(read<uint64_t>(frame+0x10+i*8)==saved[i],"callee sees saved s-register in frame");
        check(read<uint64_t>(frame+0x50)==saved[8],"callee sees saved fp in frame");
        check(read<uint64_t>(frame+0x58)==returnAddress,"callee sees saved 64-bit ra in frame");
        check(read<uint32_t>(frame+0x60)==0x3f700123u,"callee sees saved f20 in frame");
        check(read<uint32_t>(frame)==target,"callee sees target argument in frame");
        check(GPR_U64(c,30)==argument,"callee sees full-width fp = original a0");
        check(c->f[20]==0.625f,"callee sees f20 = f12");
    }
    void guest(R5900Context *c,uint32_t address) {
        checkFrame(c);
        if(address==0x010C0A70) {
            SET_GPR_U64(c,2,character);
            return;
        }
        const unsigned index=calls++;
        const uint32_t entry=entries+index*28;
        check(GPR_U64(c,16)==entry,"callback sees s0 = current entry");
        check(GPR_U64(c,17)==(mode==1?count-index:index),"callback sees live loop counter s1");
        check(GPR_U64(c,18)==character,"callback sees s2 = character");
        check(GPR_U64(c,19)==(mode==1?noTableGlobal:count),"callback sees live s3");
        check(GPR_U64(c,20)==(mode==1?0x3ffffu:entries),"callback sees live s4");
        check(GPR_U64(c,21)==0x3ffff,"callback sees handle mask s5");
        check(GPR_U64(c,22)==noTableGlobal,"callback sees relocated empty-table high half s6");
        check(GPR_U64(c,23)==self,"callback sees s7 = self");
        check(GPR_U64(c,4)==argument,"callback keeps full-width a0");
        check(GPR_U64(c,6)==entry,"callback receives the current entry");
        check(c->f[12]==0.625f,"callback receives original float argument");
        if(mode==1) {
            check(address==0x010CE138,"self-link callback selected");
            check(GPR_U64(c,5)==character,"self-link receives character");
            check(GPR_U64(c,7)==(index?0xfeed000000000000ull+index:self),"a3 is retained from preceding callback");
            check(read<uint32_t>(frame+4)==0,"self-link frame keeps called-slot flag clear");
        } else {
            check(address==0x010CE2E8,"slot-link callback selected");
            check(GPR_U64(c,5)==0,"slot-link receives slot zero");
            check(GPR_U64(c,7)==other,"slot-link receives related object");
            check(read<uint32_t>(frame+4)==1,"slot-link flag is stored in caller frame");
        }
        // A callee can retain a caller's saved register in persistent object memory.
        // This was zero/stale with host-only saves, even though the epilogue restored s2.
        put<uint64_t>(heap+index*8,read<uint64_t>(frame+0x20));
        SET_GPR_U64(c,7,0xfeed000000000001ull+index);
        c->f[12]=-100.0f;
        SET_GPR_U64(c,2,mode==2?1u:0u);
    }
    void runCase(int which) {
        mode=which;calls=0;count=mode?3:0;
        std::fill(ram.begin(),ram.end(),0);
        R5900Context c{};
        for(int i=0;i<8;++i){saved[i]=0xabc0000000000000ull+i;SET_GPR_U64(&c,16+i,saved[i]);}
        saved[8]=0xfedc987600004321ull;SET_GPR_U64(&c,30,saved[8]);
        SET_GPR_U64(&c,31,returnAddress);SET_GPR_U32(&c,29,stack);
        SET_GPR_U64(&c,4,argument);SET_GPR_U32(&c,5,target);
        c.f[12]=0.625f;uint32_t f20=0x3f700123;std::memcpy(&c.f[20],&f20,4);
        relocation(0x10ce5a8,0x10ce5b0,selfGlobal);put32(selfGlobal,self);
        for(auto pair:std::initializer_list<std::pair<uint32_t,uint32_t>>{
                {0x10ce5d8,0x10ce5dc},{0x10ce620,0x10ce624},{0x10ce64c,0x10ce678},
                {0x10ce64c,0x10ce6d0},{0x10ce7a0,0x10ce7c8},{0x10ce7a0,0x10ce820}})
            relocation(pair.first,pair.second,noTableGlobal);
        put32(noTableGlobal,0x123000);put32(character+0x10ec,listHandle);
        put32(listHandle,16);put32(listHandle-16,listTable);
        put32(listTable+12,count);put32(listTable+16,entries);
        for(unsigned i=0;i<count;++i) {
            const uint32_t e=entries+i*28;
            put32(e,16);put32(e-16,0x170100);put32(e+4,16);put32(e-12,0x170200);
        }
        put32(0x170100+16,mode==1?self:other);put32(0x170200+16,target);
        put32(0x170100+8,0x170300);put32(0x170200+8,0x170300);put32(0x170300+12,64);
        put32(character+0x3fc,1);put32(character+0x30c,0x190000);
        put32(0x190000+4,other);put32(0x190000+0x44,0);
        decomp_chara::charaLinks(ram.data(),&c,nullptr);
        check(calls==(mode==2?1:count),"all expected callbacks executed");
        for(unsigned i=0;i<calls;++i)check(read<uint64_t>(heap+i*8)==saved[2],"heap retained correct caller save");
        for(unsigned i=0;i<8;++i)check(GPR_U64(&c,16+i)==saved[i],"epilogue restores s-register");
        check(GPR_U64(&c,30)==saved[8],"epilogue restores fp");
        check(GPR_U64(&c,31)==returnAddress,"epilogue restores full ra");
        check(GPR_U32(&c,29)==stack && c.pc==uint32_t(returnAddress),"epilogue restores SP and return PC");
        uint32_t actual;std::memcpy(&actual,&c.f[20],4);check(actual==f20,"epilogue restores f20 bits");
    }
}
namespace sotc_decomp {
    std::vector<Function> &registry(){static std::vector<Function> r;return r;}
    void callGuest(uint8_t *,R5900Context *c,PS2Runtime *,uint32_t target,uint32_t ra,uint32_t sp) {
        SET_GPR_U32(c,31,ra);SET_GPR_U32(c,29,sp);guest(c,target);c->pc=ra;
    }
}
int main() {
    runCase(0); // empty list / early epilogue
    runCase(1); // three self callbacks / live a3 retained
    runCase(2); // slot callback succeeds / early epilogue
    runCase(3); // slot callbacks fail / no second-loop call
    if(failures){std::fprintf(stderr,"FAIL: %u callee-state checks\n",failures);return 1;}
    std::puts("PASS: native character links preserve callee-visible guest frame, live registers, a3 and epilogue (4 cases)");
}
