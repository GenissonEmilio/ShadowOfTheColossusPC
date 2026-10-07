#pragma once

// GPU GS rasterizer (Metal, macOS). Draws queued GS primitives straight into
// the swizzled GS local memory, which the GPU shares with the CPU (unified
// memory), running the same per-pixel algorithm as GSCpuBackend.

#include <cstddef>
#include <cstdint>
#include <memory>

// Layouts shared with the shaders (gs_metal_shaders.inc): 32-bit fields only.
struct GSGpuPrim
{
    uint32_t kind = 0; // 0 = triangle, 1 = sprite
    uint32_t state = 0;
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float fx[3]{}, fy[3]{};
    float winding = 0.0f, invAbsDenom = 0.0f;
    float zhi[3]{}, zlo[3]{};
    uint32_t rgba[3]{};
    float s[3]{}, t[3]{}, q[3]{};
    float u[3]{}, v[3]{};
    float fog[3]{};
    float su0 = 0.0f, sv0 = 0.0f, su1 = 0.0f, sv1 = 0.0f;
};
static_assert(sizeof(GSGpuPrim) == 45 * 4, "GSGpuPrim layout must match the shader");

struct GSGpuState
{
    uint32_t fbp, fbw, fpsm, fbmsk;
    uint32_t zbp, zpsm, zmask, test;
    uint32_t asel, bsel, csel, dsel;
    uint32_t fix, abe, pabe, fba;
    uint32_t fge, fogR, fogG, fogB;
    uint32_t iip, tme, fst, linear;
    uint32_t tbp0, tbw, tpsm, texW;
    uint32_t texH, tcc, tfx, cpsm;
    uint32_t csm, csa, wrapU, wrapV;
    uint32_t minU, maxU, minV, maxV;
    uint32_t ta0, aem, ta1, clut;
};
static_assert(sizeof(GSGpuState) == 44 * 4, "GSGpuState layout must match the shader");

// An IMAGE (host to local) transfer piece: pixels firstPixel .. firstPixel +
// count - 1 of the rrw-wide rectangle at (dsax, dsay), packed as the GS
// receives them.
struct GSGpuUpload
{
    uint32_t dbp, dbw, dpsm, dsax, dsay, rrw, firstPixel, count;
};

class GSMetalRaster
{
public:
    // Returns null when Metal is unavailable or vram cannot be shared with the
    // GPU (it must be page aligned).
    static std::unique_ptr<GSMetalRaster> create(uint8_t *vram, size_t vramSize);
    ~GSMetalRaster();

    // Queues the primitives for rasterization (in submission order) without
    // waiting. With a feedback range, textures are sampled from a copy of
    // that GS memory taken before the draws. cluts holds clutCount CLUTs of 512 entries.
    // keys[i] selects the specialized pipeline of prims[i] (see pipelineKey).
    void draw(const GSGpuPrim *prims, const uint64_t *keys, size_t primCount,
              const GSGpuState *states, size_t stateCount,
              const uint16_t *cluts, size_t clutCount,
              uint64_t feedbackBegin = 0, uint64_t feedbackEnd = 0);

    // Writes transfer data into GS memory after the work queued so far,
    // without waiting.
    void upload(const GSGpuUpload &upload, const uint8_t *data, size_t bytes);

    // Specialization key of a primitive: kind plus the state's switches.
    static uint64_t pipelineKey(uint32_t kind, const GSGpuState &state);
    // Waits until all queued work has been executed.
    void waitIdle();

private:
    struct Impl;
    explicit GSMetalRaster(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
