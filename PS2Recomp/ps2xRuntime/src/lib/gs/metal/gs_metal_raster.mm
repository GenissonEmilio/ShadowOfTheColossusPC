#include "runtime/gs/gs_metal_raster.h"
#include "runtime/gs/ps2_gs_memory.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
    const char *kShaderSource =
#include "gs_metal_shaders.inc"
        ;

    // Mirrors TableInfo in the shaders.
    struct TableInfo
    {
        uint32_t offset[8];
        uint32_t pw[8];
        uint32_t ph[8];
    };

    // GS coordinates span 0..2047; the shaders map them onto this target.
    constexpr NSUInteger kTargetSize = 2048;
}

struct GSMetalRaster::Impl
{
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLBuffer> vram = nil;
    id<MTLBuffer> tables = nil;
    TableInfo tableInfo{};

    // Per-submission inputs. A slot is reused only once the GPU is done with it.
    struct Slot
    {
        id<MTLBuffer> prims = nil;
        id<MTLBuffer> states = nil;
        id<MTLBuffer> cluts = nil;
        id<MTLCommandBuffer> commands = nil;
    };
    static constexpr size_t kSlots = 32;
    Slot slots[kSlots];
    size_t nextSlot = 0;
    id<MTLCommandBuffer> last = nil;

    // Grows a shared buffer to hold at least `bytes`.
    id<MTLBuffer> ensure(id<MTLBuffer> buffer, size_t bytes)
    {
        if (buffer && buffer.length >= bytes)
            return buffer;
        size_t size = 64 * 1024;
        while (size < bytes)
            size *= 2;
        [buffer release];
        return [device newBufferWithLength:size options:MTLResourceStorageModeShared];
    }

    ~Impl()
    {
        if (last)
            [last waitUntilCompleted];
        for (Slot &slot : slots)
        {
            [slot.prims release];
            [slot.states release];
            [slot.cluts release];
            [slot.commands release];
        }
        [last release];
        [pipeline release];
        [queue release];
        [vram release];
        [tables release];
        [device release];
    }
};

GSMetalRaster::GSMetalRaster(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
GSMetalRaster::~GSMetalRaster() = default;

std::unique_ptr<GSMetalRaster> GSMetalRaster::create(uint8_t *vramPtr, size_t vramSize)
{
    @autoreleasepool
    {
        auto impl = std::make_unique<Impl>();
        impl->device = MTLCreateSystemDefaultDevice();
        if (!impl->device)
        {
            std::cerr << "[gs:metal] no Metal device" << std::endl;
            return nullptr;
        }
        if (![impl->device supportsFamily:MTLGPUFamilyApple4])
        {
            std::cerr << "[gs:metal] raster order groups unsupported on " << [[impl->device name] UTF8String] << std::endl;
            return nullptr;
        }
        const NSUInteger pageSize = static_cast<NSUInteger>(getpagesize());
        if (reinterpret_cast<uintptr_t>(vramPtr) % pageSize != 0 || vramSize % pageSize != 0)
        {
            std::cerr << "[gs:metal] GS memory is not page aligned" << std::endl;
            return nullptr;
        }
        // GS local memory itself, shared with the CPU without copies.
        impl->vram = [impl->device newBufferWithBytesNoCopy:vramPtr
                                                      length:vramSize
                                                     options:MTLResourceStorageModeShared
                                                 deallocator:nil];

        // Swizzle tables, the same ones the CPU uses.
        std::vector<uint16_t> tableData;
        for (uint32_t i = 0; i < 8; ++i)
        {
            uint32_t entries = 0, pw = 0, ph = 0;
            const uint16_t *table = GSMem::PageTableData(i, entries, pw, ph);
            impl->tableInfo.offset[i] = static_cast<uint32_t>(tableData.size());
            impl->tableInfo.pw[i] = pw;
            impl->tableInfo.ph[i] = ph;
            tableData.insert(tableData.end(), table, table + entries);
        }
        impl->tables = [impl->device newBufferWithBytes:tableData.data()
                                                 length:tableData.size() * sizeof(uint16_t)
                                                options:MTLResourceStorageModeShared];

        NSError *error = nil;
        MTLCompileOptions *options = [[MTLCompileOptions new] autorelease];
        // The CPU renderer's float results must be reproduced: no fast math,
        // no reassociation (the float-pair Z arithmetic depends on it).
        if (@available(macOS 15.0, *))
        {
            options.mathMode = MTLMathModeSafe;
            options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
        }
        else
        {
            options.fastMathEnabled = NO;
        }
        id<MTLLibrary> library = [impl->device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource]
                                                           options:options
                                                             error:&error];
        if (!library)
        {
            std::cerr << "[gs:metal] shader compile failed: " << [[error localizedDescription] UTF8String] << std::endl;
            return nullptr;
        }
        MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor new] autorelease];
        desc.vertexFunction = [[library newFunctionWithName:@"gsVertex"] autorelease];
        desc.fragmentFunction = [[library newFunctionWithName:@"gsFragment"] autorelease];
        desc.rasterSampleCount = 1;
        impl->pipeline = [impl->device newRenderPipelineStateWithDescriptor:desc error:&error];
        [library release];
        if (!impl->pipeline)
        {
            std::cerr << "[gs:metal] pipeline failed: " << [[error localizedDescription] UTF8String] << std::endl;
            return nullptr;
        }
        impl->queue = [impl->device newCommandQueue];
        std::cerr << "[gs:metal] GPU rasterizer on " << [[impl->device name] UTF8String] << std::endl;
        return std::unique_ptr<GSMetalRaster>(new GSMetalRaster(std::move(impl)));
    }
}

void GSMetalRaster::draw(const GSGpuPrim *prims, size_t primCount,
                         const GSGpuState *states, size_t stateCount,
                         const uint16_t *cluts, size_t clutCount)
{
    if (primCount == 0)
        return;
    Impl &impl = *m_impl;
    @autoreleasepool
    {
        Impl::Slot &slot = impl.slots[impl.nextSlot];
        impl.nextSlot = (impl.nextSlot + 1) % Impl::kSlots;
        if (slot.commands)
        {
            [slot.commands waitUntilCompleted];
            [slot.commands release];
            slot.commands = nil;
        }
        slot.prims = impl.ensure(slot.prims, primCount * sizeof(GSGpuPrim));
        slot.states = impl.ensure(slot.states, stateCount * sizeof(GSGpuState));
        slot.cluts = impl.ensure(slot.cluts, std::max<size_t>(clutCount, 1) * 512 * sizeof(uint16_t));
        std::memcpy(slot.prims.contents, prims, primCount * sizeof(GSGpuPrim));
        std::memcpy(slot.states.contents, states, stateCount * sizeof(GSGpuState));
        if (clutCount)
            std::memcpy(slot.cluts.contents, cluts, clutCount * 512 * sizeof(uint16_t));

        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.renderTargetWidth = kTargetSize;
        pass.renderTargetHeight = kTargetSize;
        pass.defaultRasterSampleCount = 1;

        id<MTLCommandBuffer> commands = [impl.queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:impl.pipeline];
        [encoder setViewport:MTLViewport{0, 0, double(kTargetSize), double(kTargetSize), 0, 1}];
        [encoder setVertexBuffer:slot.prims offset:0 atIndex:1];
        const uint32_t primBase = 0;
        [encoder setVertexBytes:&primBase length:sizeof(primBase) atIndex:6];
        [encoder setFragmentBuffer:impl.vram offset:0 atIndex:0];
        [encoder setFragmentBuffer:slot.prims offset:0 atIndex:1];
        [encoder setFragmentBuffer:slot.states offset:0 atIndex:2];
        [encoder setFragmentBuffer:impl.tables offset:0 atIndex:3];
        [encoder setFragmentBuffer:slot.cluts offset:0 atIndex:4];
        [encoder setFragmentBytes:&impl.tableInfo length:sizeof(TableInfo) atIndex:5];
        [encoder setFragmentBuffer:impl.vram offset:0 atIndex:7];
        // One bounding quad per primitive; raster order groups keep overlapping
        // pixels in submission order.
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:primCount];
        [encoder endEncoding];
        // PS2X_GS_PROFILE=1: GPU execution time per second.
        static const bool profile = std::getenv("PS2X_GS_PROFILE") != nullptr;
        if (profile)
        {
            [commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
                static std::atomic<double> busy{0.0};
                static std::atomic<uint64_t> buffers{0};
                static std::atomic<double> windowStart{0.0};
                const double gpu = done.GPUEndTime - done.GPUStartTime;
                busy.store(busy.load() + gpu);
                buffers.fetch_add(1);
                if (windowStart.load() == 0.0)
                    windowStart.store(done.GPUStartTime);
                if (done.GPUEndTime - windowStart.load() >= 1.0)
                {
                    std::fprintf(stderr, "[gs:metal] gpu busy %.0f ms/s over %llu command buffers\n", busy.load() * 1000.0,
                                 (unsigned long long)buffers.load());
                    busy.store(0.0);
                    buffers.store(0);
                    windowStart.store(done.GPUEndTime);
                }
            }];
        }
        [commands commit];
        slot.commands = [commands retain];
        [impl.last release];
        impl.last = [commands retain];
    }
}

void GSMetalRaster::waitIdle()
{
    Impl &impl = *m_impl;
    if (!impl.last)
        return;
    [impl.last waitUntilCompleted];
    if (impl.last.error)
        std::cerr << "[gs:metal] GPU error: " << [[impl.last.error localizedDescription] UTF8String] << std::endl;
    [impl.last release];
    impl.last = nil;
}
