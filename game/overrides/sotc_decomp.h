// Decompiled game code: guest functions rewritten by hand in C++, put in
// place of their recompiled versions.
//
// Each decomp_*.cpp registers its functions with SOTC_DECOMP_FUNCTION. They
// replace the recompiled code by default (SOTC_DECOMP=0: none of them,
// SOTC_DECOMP_SKIP=name,name: those left recompiled).
//
// Differential test, SOTC_DIFFTEST=all | name,name: each listed function is
// checked against its recompiled version on the same input. The native one
// runs first on the live state; its registers and every byte it changed in
// RAM and the scratchpad are kept aside; the state is put back and the
// recompiled one runs (the game goes on with its result). Return registers
// (v0, v1, f0), the callee-saved ones (s0-s7, fp, gp, sp, ra) and memory are
// compared; the caller-saved scratch registers may differ. Every call is
// checked until SOTC_DIFFTEST_FIRST (default 200) calls, then one in
// SOTC_DIFFTEST_EVERY (default 1000): a check copies 32 MB twice.
#pragma once

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdint>
#include <vector>

namespace sotc_decomp
{
    struct Function
    {
        uint32_t address;
        const char *name;
        PS2Runtime::RecompiledFunction native;
        bool returnsFloat = false; // the result in f0 (v0 is scratch then)
    };

    std::vector<Function> &registry();

    struct Registrar
    {
        Registrar(uint32_t address, const char *name, PS2Runtime::RecompiledFunction native, bool returnsFloat = false)
        {
            registry().push_back({address, name, native, returnsFloat});
        }
    };

    // Puts the registered functions in place (and the checks SOTC_DIFFTEST asks for).
    void install(PS2Runtime &runtime);

    // Returning from a decompiled function: the jr ra the recompiled code ends with.
    inline void returnTo(R5900Context *ctx) { ctx->pc = GPR_U32(ctx, 31); }
}

#define SOTC_DECOMP_FUNCTION(address, name, fn) \
    static const ::sotc_decomp::Registrar sotc_decomp_registrar_##name{address, #name, fn}
#define SOTC_DECOMP_FLOAT_FUNCTION(address, name, fn) \
    static const ::sotc_decomp::Registrar sotc_decomp_registrar_##name{address, #name, fn, true}
