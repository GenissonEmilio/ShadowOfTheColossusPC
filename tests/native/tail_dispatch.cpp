// Compile this harness with the two functions emitted from the synthetic ELF.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_recompiled_functions.h"
#include <cstdio>
#include <vector>

namespace {
    constexpr uint32_t entry=0x100000,target=0x100100;
    constexpr uint32_t stack=0x1800000,continuation=0x100108;
    constexpr uint64_t ra=0xabc0000000500000ull;
    unsigned failures=0,calls=0;
    PS2Runtime::RecompiledFunction original=nullptr;
    void check(bool ok,const char *what) {
        if(!ok){++failures;std::fprintf(stderr,"FAIL: %s\n",what);}
    }
    void inspect(R5900Context *c) {
        ++calls;
        check(c->pc==target,"hook sees target PC after delay slot");
        check(GPR_U64(c,31)==ra,"tail J keeps full ra");
        check(GPR_U32(c,29)==stack-16,"delay-slot SP adjustment precedes hook");
    }
    void replacement(uint8_t *,R5900Context *c,PS2Runtime *) {
        inspect(c);SET_GPR_U32(c,2,77);c->pc=uint32_t(ra);
    }
    void trampoline(uint8_t *r,R5900Context *c,PS2Runtime *rt) {
        inspect(c);original(r,c,rt);SET_GPR_U32(c,2,GPR_U32(c,2)+1);
    }
    void yielding(uint8_t *,R5900Context *c,PS2Runtime *) {
        inspect(c);SET_GPR_U32(c,2,99);c->pc=continuation;
    }
}
int main() {
    PS2Runtime runtime;
    std::vector<uint8_t> ram(PS2_RAM_SIZE);
    original=runtime.guestFunctionAt(target);auto start=runtime.guestFunctionAt(entry);
    check(original && start,"synthetic functions registered");
    if(!original || !start)return 1;
    struct Case {PS2Runtime::RecompiledFunction function;uint32_t expected,pc,calls;};
    const Case cases[]={{original,42,uint32_t(ra),0},{replacement,77,uint32_t(ra),1},
                        {trampoline,43,uint32_t(ra),1},{yielding,99,continuation,1},
                        {nullptr,42,uint32_t(ra),0}};
    for(const auto &test:cases) {
        runtime.registerFunction(target,test.function);
        calls=0;R5900Context c{};c.pc=entry;
        SET_GPR_U64(&c,31,ra);SET_GPR_U32(&c,29,stack);
        start(ram.data(),&c,&runtime);
        check(GPR_U32(&c,2)==test.expected,"tail J dispatches the installed target");
        check(calls==test.calls,"hook called exactly once");
        check(c.pc==test.pc,"callee return/continuation PC is retained");
        check(GPR_U64(&c,31)==ra,"return address remains unchanged");
        check(GPR_U32(&c,29)==stack-16,"only delay slot changes guest stack");
    }
    runtime.registerFunction(target,original);
    if(failures){std::fprintf(stderr,"FAIL: %u tail-dispatch checks\n",failures);return 1;}
    std::puts("PASS: generated tail J follows hooks/trampolines and preserves delay-slot, PC, RA and SP (5 cases)");
}
