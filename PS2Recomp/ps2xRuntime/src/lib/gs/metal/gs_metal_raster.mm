#include "runtime/gs/gs_metal_raster.h"
#include "runtime/gs/ps2_gs_memory.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unordered_map>

namespace
{
    const char *kShaderSource =
#include "gs_metal_shaders.inc"
        ;

    // Mirrors SwizzleInfo in the shaders.
    struct SwizzleInfo
    {
        uint32_t pw[8], ph[8], colW[8], colH[8], blkW[8], blkH[8];
        uint8_t block[8][32];
        uint16_t column[8][512];
    };

    // GS coordinates span 0..2047; the shaders map them onto this target.
    constexpr NSUInteger kTargetSize = 2048;
}

struct GSMetalRaster::Impl
{
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLLibrary> library = nil;
    id<MTLBuffer> vram = nil;
    id<MTLBuffer> swizzle = nil;
    std::unordered_map<uint64_t, id<MTLRenderPipelineState>> pipelines;
    id<MTLComputePipelineState> uploadPipeline = nil;
    // Work encoded but not committed yet (uploads ride along with the next draw).
    id<MTLCommandBuffer> open = nil;
    // Pre-draw copy of the texture memory feedback draws sample.
    id<MTLBuffer> snapshot = nil;

    id<MTLCommandBuffer> openCommands()
    {
        if (!open)
            open = [[queue commandBuffer] retain];
        return open;
    }

    void commitOpen()
    {
        if (!open)
            return;
        [open commit];
        [last release];
        last = open; // keeps the reference
        open = nil;
    }

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

    // Pipeline specialized on a key from pipelineKey (function constants).
    id<MTLRenderPipelineState> pipelineFor(uint64_t key)
    {
        auto it = pipelines.find(key);
        if (it != pipelines.end())
            return it->second;
        @autoreleasepool
        {
            MTLFunctionConstantValues *values = [[MTLFunctionConstantValues new] autorelease];
            const uint32_t constants[6] = {
                static_cast<uint32_t>(key & 1u),
                static_cast<uint32_t>((key >> 1) & 0x1FFFu),
                static_cast<uint32_t>((key >> 14) & 0x3Fu),
                static_cast<uint32_t>((key >> 20) & 0x3Fu),
                static_cast<uint32_t>((key >> 26) & 0x3Fu),
                static_cast<uint32_t>(key >> 32),
            };
            for (NSUInteger i = 0; i < 6; ++i)
                [values setConstantValue:&constants[i] type:MTLDataTypeUInt atIndex:i];
            NSError *error = nil;
            MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor new] autorelease];
            desc.vertexFunction = [[library newFunctionWithName:@"gsVertex"] autorelease];
            desc.fragmentFunction = [[library newFunctionWithName:@"gsFragment" constantValues:values error:&error] autorelease];
            desc.rasterSampleCount = 1;
            id<MTLRenderPipelineState> pipeline =
                desc.fragmentFunction ? [device newRenderPipelineStateWithDescriptor:desc error:&error] : nil;
            if (!pipeline)
                std::cerr << "[gs:metal] pipeline " << std::hex << key << std::dec << " failed: "
                          << (error ? [[error localizedDescription] UTF8String] : "?") << std::endl;
            pipelines.emplace(key, pipeline);
            return pipeline;
        }
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
        for (auto &[key, pipeline] : pipelines)
            [pipeline release];
        if (open)
        {
            [open commit];
            [open waitUntilCompleted];
            [open release];
        }
        [snapshot release];
        [uploadPipeline release];
        [library release];
        [queue release];
        [vram release];
        [swizzle release];
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

        // Swizzle geometry and tables, the same ones the CPU builds its page tables from.
        SwizzleInfo info{};
        for (uint32_t i = 0; i < 8; ++i)
        {
            uint32_t entries = 0, pw = 0, ph = 0;
            GSMem::PageTableData(i, entries, pw, ph);
            const GSMem::SwizzleTables tables = GSMem::SwizzleTableData(i);
            info.pw[i] = pw;
            info.ph[i] = ph;
            info.colW[i] = tables.columnWidth;
            info.colH[i] = tables.columnHeight;
            info.blkW[i] = tables.blockWidth;
            info.blkH[i] = tables.blockHeight;
            std::memcpy(info.block[i], tables.block, tables.blockWidth * tables.blockHeight);
            std::memcpy(info.column[i], tables.column, tables.columnWidth * tables.columnHeight * sizeof(uint16_t));
        }
        impl->swizzle = [impl->device newBufferWithBytes:&info length:sizeof(info) options:MTLResourceStorageModeShared];

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
        impl->library = [impl->device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource]
                                                   options:options
                                                     error:&error];
        if (!impl->library)
        {
            std::cerr << "[gs:metal] shader compile failed: " << [[error localizedDescription] UTF8String] << std::endl;
            return nullptr;
        }
        impl->queue = [impl->device newCommandQueue];
        std::cerr << "[gs:metal] GPU rasterizer on " << [[impl->device name] UTF8String] << std::endl;
        return std::unique_ptr<GSMetalRaster>(new GSMetalRaster(std::move(impl)));
    }
}

uint64_t GSMetalRaster::pipelineKey(uint32_t kind, const GSGpuState &st)
{
    // Fields a draw does not use are left out, so they do not split pipelines.
    const bool tme = st.tme != 0u;
    const uint32_t ztst = (st.test >> 17) & 3u;
    const bool usesZ = ztst >= 2u || st.zmask == 0u;
    uint32_t flags = tme ? 1u : 0u;
    if (tme)
        flags |= (st.fst << 1) | (st.linear << 2) | (st.tcc << 9) | ((st.tfx & 3u) << 10);
    flags |= (st.iip << 3) | (st.fge << 4) | (st.abe << 5) | (st.pabe << 6) | (st.zmask << 7) | (st.fba << 8);
    flags |= (st.fbmsk != 0u ? 1u : 0u) << 12;
    return static_cast<uint64_t>(kind & 1u) | (static_cast<uint64_t>(flags) << 1) |
           (static_cast<uint64_t>(st.fpsm & 0x3Fu) << 14) | (static_cast<uint64_t>(usesZ ? st.zpsm & 0x3Fu : 0u) << 20) |
           (static_cast<uint64_t>(tme ? st.tpsm & 0x3Fu : 0u) << 26) | (static_cast<uint64_t>(st.test & 0x7F00Fu) << 32);
}

void GSMetalRaster::draw(const GSGpuPrim *prims, const uint64_t *keys, size_t primCount,
                         const GSGpuState *states, size_t stateCount,
                         const uint16_t *cluts, size_t clutCount,
                         uint64_t feedbackBegin, uint64_t feedbackEnd)
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

        id<MTLCommandBuffer> commands = impl.openCommands();
        id<MTLBuffer> textureSource = impl.vram;
        if (feedbackEnd > feedbackBegin)
        {
            // Feedback draw: sample a copy of the texture taken before it.
            const NSUInteger size = impl.vram.length;
            if (!impl.snapshot)
                impl.snapshot = [impl.device newBufferWithLength:size options:MTLResourceStorageModePrivate];
            const NSUInteger length = static_cast<NSUInteger>(std::min<uint64_t>(feedbackEnd - feedbackBegin, size));
            const NSUInteger first = static_cast<NSUInteger>(feedbackBegin % size);
            const NSUInteger head = std::min(length, size - first);
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            [blit copyFromBuffer:impl.vram sourceOffset:first toBuffer:impl.snapshot destinationOffset:first size:head];
            if (head < length)
                [blit copyFromBuffer:impl.vram sourceOffset:0 toBuffer:impl.snapshot destinationOffset:0 size:length - head];
            [blit endEncoding];
            textureSource = impl.snapshot;
        }
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        [encoder setViewport:MTLViewport{0, 0, double(kTargetSize), double(kTargetSize), 0, 1}];
        [encoder setVertexBuffer:slot.prims offset:0 atIndex:1];
        const uint32_t primBase = 0;
        [encoder setVertexBytes:&primBase length:sizeof(primBase) atIndex:6];
        [encoder setFragmentBuffer:impl.vram offset:0 atIndex:0];
        [encoder setFragmentBuffer:slot.prims offset:0 atIndex:1];
        [encoder setFragmentBuffer:slot.states offset:0 atIndex:2];
        [encoder setFragmentBuffer:impl.swizzle offset:0 atIndex:3];
        [encoder setFragmentBuffer:slot.cluts offset:0 atIndex:4];
        [encoder setFragmentBuffer:textureSource offset:0 atIndex:7];
        // One bounding quad per primitive, in runs sharing a specialized
        // pipeline; raster order groups keep overlapping pixels in order.
        for (size_t start = 0; start < primCount;)
        {
            size_t end = start + 1;
            while (end < primCount && keys[end] == keys[start])
                ++end;
            if (id<MTLRenderPipelineState> pipeline = impl.pipelineFor(keys[start]))
            {
                [encoder setRenderPipelineState:pipeline];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6
                          instanceCount:end - start baseInstance:start];
            }
            start = end;
        }
        [encoder endEncoding];

        // PS2X_GS_PROFILE=1: GPU execution time per second.
        static const bool profile = std::getenv("PS2X_GS_PROFILE") != nullptr;
        if (profile)
        {
            const size_t pipelineCount = impl.pipelines.size();
            [commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
                static std::atomic<double> busy{0.0};
                static std::atomic<uint64_t> buffers{0};
                static std::atomic<double> windowStart{0.0};
                busy.store(busy.load() + (done.GPUEndTime - done.GPUStartTime));
                buffers.fetch_add(1);
                if (windowStart.load() == 0.0)
                    windowStart.store(done.GPUStartTime);
                if (done.GPUEndTime - windowStart.load() >= 1.0)
                {
                    std::fprintf(stderr, "[gs:metal] gpu busy %.0f ms/s over %llu command buffers, %zu pipelines\n",
                                 busy.load() * 1000.0, (unsigned long long)buffers.load(), pipelineCount);
                    busy.store(0.0);
                    buffers.store(0);
                    windowStart.store(done.GPUEndTime);
                }
            }];
        }
        slot.commands = [commands retain];
        impl.commitOpen();
    }
}

void GSMetalRaster::upload(const GSGpuUpload &params, const uint8_t *data, size_t bytes)
{
    if (params.count == 0 || bytes == 0)
        return;
    Impl &impl = *m_impl;
    @autoreleasepool
    {
        if (!impl.uploadPipeline)
        {
            NSError *error = nil;
            id<MTLFunction> function = [[impl.library newFunctionWithName:@"gsUpload"] autorelease];
            impl.uploadPipeline = [impl.device newComputePipelineStateWithFunction:function error:&error];
            if (!impl.uploadPipeline)
            {
                std::cerr << "[gs:metal] upload pipeline failed: "
                          << (error ? [[error localizedDescription] UTF8String] : "?") << std::endl;
                std::abort();
            }
        }
        // The command buffer keeps the staging buffer alive until it has run.
        id<MTLBuffer> staging = [[impl.device newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared] autorelease];
        id<MTLCommandBuffer> commands = impl.openCommands();
        id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
        [encoder setComputePipelineState:impl.uploadPipeline];
        [encoder setBuffer:impl.vram offset:0 atIndex:0];
        [encoder setBuffer:staging offset:0 atIndex:1];
        [encoder setBytes:&params length:sizeof(params) atIndex:2];
        [encoder setBuffer:impl.swizzle offset:0 atIndex:3];
        const NSUInteger width = std::min<NSUInteger>(256, impl.uploadPipeline.maxTotalThreadsPerThreadgroup);
        [encoder dispatchThreads:MTLSizeMake(params.count, 1, 1) threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        [encoder endEncoding];
    }
}

void GSMetalRaster::waitIdle()
{
    Impl &impl = *m_impl;
    impl.commitOpen();
    if (!impl.last)
        return;
    [impl.last waitUntilCompleted];
    if (impl.last.error)
        std::cerr << "[gs:metal] GPU error: " << [[impl.last.error localizedDescription] UTF8String] << std::endl;
    [impl.last release];
    impl.last = nil;
}
