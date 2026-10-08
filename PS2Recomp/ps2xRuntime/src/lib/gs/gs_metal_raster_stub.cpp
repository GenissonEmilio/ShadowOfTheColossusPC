// Platforms without Metal: the GPU rasterizer is never created, so the CPU
// backend keeps rasterizing (GSMetalRaster::create returns null).
#if !defined(PS2X_HAVE_METAL)

#include "runtime/gs/gs_metal_raster.h"

struct GSMetalRaster::Impl
{
};

GSMetalRaster::GSMetalRaster(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
GSMetalRaster::~GSMetalRaster() = default;

std::unique_ptr<GSMetalRaster> GSMetalRaster::create(uint8_t *, size_t) { return nullptr; }
void GSMetalRaster::draw(const GSGpuPrim *, const uint64_t *, size_t, const GSGpuState *, size_t, const uint16_t *, size_t, uint64_t, uint64_t) {}
void GSMetalRaster::upload(const GSGpuUpload &, const uint8_t *, size_t) {}
uint64_t GSMetalRaster::pipelineKey(uint32_t, const GSGpuState &) { return 0; }
void GSMetalRaster::waitIdle() {}

#endif
