// Native renderer on Metal (see runtime/native_renderer.h).
#include "runtime/native_renderer.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <unordered_map>

namespace
{
    const char *kShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VIn
{
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv [[attribute(3)]];
    uchar4 color [[attribute(2)]];
};

struct VOut
{
    float4 position [[position]];
    float3 normal;
    float2 uv;
    float4 color;
};

struct Light
{
    float4x4 directions; // columns: the normal's x, y, z, 1
    float4x4 colours;    // columns: light 0, 1, 2, ambient
    int lit;
    float opacity;
};

vertex VOut nativeVertex(VIn in [[stage_in]], constant float4x4 &clip [[buffer(1)]], constant Light &light [[buffer(2)]])
{
    VOut out;
    float4 c = clip * float4(in.position, 1.0);
    // Depth from w (near plane at w = 1): z / w = 1 - 1 / w.
    out.position = float4(c.x, c.y, c.w - 1.0, c.w);
    out.normal = in.normal;
    out.uv = in.uv;
    out.color = float4(in.color) / 128.0; // 0x80 = 1.0
    out.color.a *= light.opacity;
    if (light.lit != 0)
    {
        float4 d = max(light.directions * float4(in.normal, 1.0), 0.0);
        d.w = 1.0;
        out.color.rgb = min(out.color.rgb * (light.colours * d).rgb, 255.0 / 128.0);
    }
    return out;
}

fragment float4 nativeFragment(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], constant int &textured [[buffer(0)]])
{
    constexpr sampler repeatLinear(address::repeat, filter::linear);
    float3 shade = in.color.rgb;
    float4 t = textured != 0 ? tex.sample(repeatLinear, in.uv) : float4(1.0);
    float alpha = saturate(t.a * (textured != 0 ? 2.0 : 1.0) * in.color.a); // texture alpha 0x80 = opaque
    if (alpha < 0.02)
        discard_fragment();
    return float4(saturate(t.rgb * shade), alpha);
}
)MSL";

    struct Mesh
    {
        id<MTLBuffer> buffer = nil;
        uint32_t count = 0;
    };

    struct Draw
    {
        uint64_t mesh;
        native_renderer::DrawState state;
        float clip[16];
    };

    struct Renderer
    {
        id<MTLDevice> device = nil;
        id<MTLCommandQueue> queue = nil;
        id<MTLRenderPipelineState> pipeline = nil;  // normal blend
        id<MTLRenderPipelineState> additive = nil;
        id<MTLRenderPipelineState> opaque = nil;
        id<MTLDepthStencilState> depthOff = nil;
        id<MTLDepthStencilState> depthReadOnly = nil;
        id<MTLDepthStencilState> depthState = nil;
        id<MTLTexture> color = nil, depth = nil;
        uint32_t width = 0, height = 0;
        std::unordered_map<uint64_t, Mesh> meshes;
        std::unordered_map<uint64_t, id<MTLTexture>> textures;
        std::vector<Draw> draws;

        std::mutex outputMutex;
        std::vector<uint8_t> output;
        uint32_t outputWidth = 0, outputHeight = 0;
        bool hasOutput = false;

        bool init()
        {
            device = MTLCreateSystemDefaultDevice();
            if (!device)
                return false;
            queue = [device newCommandQueue];
            NSError *error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kShaders] options:nil error:&error];
            if (!library)
            {
                std::cerr << "[native] shader compile failed: " << [[error localizedDescription] UTF8String] << std::endl;
                return false;
            }
            MTLVertexDescriptor *layout = [MTLVertexDescriptor vertexDescriptor];
            layout.attributes[0].format = MTLVertexFormatFloat3;
            layout.attributes[0].offset = offsetof(native_renderer::Vertex, x);
            layout.attributes[1].format = MTLVertexFormatFloat3;
            layout.attributes[1].offset = offsetof(native_renderer::Vertex, nx);
            layout.attributes[2].format = MTLVertexFormatUChar4;
            layout.attributes[2].offset = offsetof(native_renderer::Vertex, r);
            layout.attributes[3].format = MTLVertexFormatFloat2;
            layout.attributes[3].offset = offsetof(native_renderer::Vertex, u);
            layout.layouts[0].stride = sizeof(native_renderer::Vertex);
            MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
            desc.vertexFunction = [library newFunctionWithName:@"nativeVertex"];
            desc.fragmentFunction = [library newFunctionWithName:@"nativeFragment"];
            desc.vertexDescriptor = layout;
            desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            auto blend = desc.colorAttachments[0];
            blend.blendingEnabled = YES;
            blend.rgbBlendOperation = MTLBlendOperationAdd;
            blend.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            blend.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            blend.alphaBlendOperation = MTLBlendOperationAdd;
            blend.sourceAlphaBlendFactor = MTLBlendFactorOne;
            blend.destinationAlphaBlendFactor = MTLBlendFactorZero;
            pipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
            blend.destinationRGBBlendFactor = MTLBlendFactorOne;
            additive = [device newRenderPipelineStateWithDescriptor:desc error:&error];
            blend.blendingEnabled = NO;
            opaque = [device newRenderPipelineStateWithDescriptor:desc error:&error];
            [desc release];
            [library release];
            if (!pipeline)
            {
                std::cerr << "[native] pipeline failed: " << [[error localizedDescription] UTF8String] << std::endl;
                return false;
            }
            MTLDepthStencilDescriptor *depthDesc = [[MTLDepthStencilDescriptor alloc] init];
            depthDesc.depthCompareFunction = MTLCompareFunctionLess;
            depthDesc.depthWriteEnabled = YES;
            depthState = [device newDepthStencilStateWithDescriptor:depthDesc];
            depthDesc.depthWriteEnabled = NO;
            depthReadOnly = [device newDepthStencilStateWithDescriptor:depthDesc];
            depthDesc.depthCompareFunction = MTLCompareFunctionAlways;
            depthOff = [device newDepthStencilStateWithDescriptor:depthDesc];
            [depthDesc release];
            return true;
        }

        void ensureTargets(uint32_t w, uint32_t h)
        {
            if (color && width == w && height == h)
                return;
            [color release];
            [depth release];
            MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
            cd.usage = MTLTextureUsageRenderTarget;
            cd.storageMode = MTLStorageModeShared;
            color = [device newTextureWithDescriptor:cd];
            MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:w height:h mipmapped:NO];
            dd.usage = MTLTextureUsageRenderTarget;
            dd.storageMode = MTLStorageModePrivate;
            depth = [device newTextureWithDescriptor:dd];
            width = w;
            height = h;
        }

        void render(uint32_t w, uint32_t h)
        {
            @autoreleasepool
            {
                ensureTargets(w, h);
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = color;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].clearColor = MTLClearColorMake(0.08, 0.08, 0.1, 1.0);
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.depthAttachment.texture = depth;
                pass.depthAttachment.loadAction = MTLLoadActionClear;
                pass.depthAttachment.clearDepth = 1.0;
                pass.depthAttachment.storeAction = MTLStoreActionDontCare;
                id<MTLCommandBuffer> commands = [queue commandBuffer];
                id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
                [encoder setCullMode:MTLCullModeNone];
                std::stable_sort(draws.begin(), draws.end(), [](const Draw &a, const Draw &b) { return a.state.order < b.state.order; });
                int bound = -1;
                for (const Draw &d : draws)
                {
                    const int blendMode = static_cast<int>(d.state.blend);
                    const int mode = blendMode | (d.state.screen ? 4 : 0);
                    if (mode != bound)
                    {
                        [encoder setRenderPipelineState:blendMode == 1 ? additive : (blendMode == 2 ? opaque : pipeline)];
                        // Additive glows do not hide what is drawn after them; 2D ignores depth.
                        [encoder setDepthStencilState:d.state.screen ? depthOff : (blendMode == 1 ? depthReadOnly : depthState)];
                        bound = mode;
                    }
                    auto it = meshes.find(d.mesh);
                    if (it == meshes.end() || it->second.count == 0)
                        continue;
                    [encoder setVertexBuffer:it->second.buffer offset:0 atIndex:0];
                    [encoder setVertexBytes:d.clip length:sizeof(d.clip) atIndex:1];
                    struct
                    {
                        float light[32];
                        int lit;
                        float opacity;
                        int pad[2];
                    } light;
                    std::memcpy(light.light, d.state.light, sizeof(light.light));
                    light.lit = d.state.lit ? 1 : 0;
                    light.opacity = d.state.opacity;
                    [encoder setVertexBytes:&light length:sizeof(light) atIndex:2];
                    auto tex = d.state.texture ? textures.find(d.state.texture) : textures.end();
                    const int textured = tex != textures.end() ? 1 : 0;
                    if (textured)
                        [encoder setFragmentTexture:tex->second atIndex:0];
                    [encoder setFragmentBytes:&textured length:sizeof(textured) atIndex:0];
                    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:it->second.count];
                }
                [encoder endEncoding];
                [commands commit];
                [commands waitUntilCompleted];
                std::lock_guard<std::mutex> lock(outputMutex);
                output.resize(static_cast<size_t>(w) * h * 4u);
                [color getBytes:output.data() bytesPerRow:w * 4u fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
                outputWidth = w;
                outputHeight = h;
                hasOutput = true;
            }
        }
    };

    Renderer *g_renderer = nullptr;
    bool g_failed = false;
    std::atomic<bool> g_shown{false};

    Renderer *renderer()
    {
        if (!g_renderer && !g_failed)
        {
            g_renderer = new Renderer();
            if (!g_renderer->init())
            {
                delete g_renderer;
                g_renderer = nullptr;
                g_failed = true;
            }
        }
        return g_renderer;
    }
}

namespace native_renderer
{
    bool available() { return renderer() != nullptr; }

    bool hasMesh(uint64_t id)
    {
        Renderer *r = renderer();
        return r && r->meshes.count(id) != 0;
    }

    void uploadMesh(uint64_t id, const Vertex *vertices, uint32_t count)
    {
        Renderer *r = renderer();
        if (!r)
            return;
        Mesh &mesh = r->meshes[id];
        [mesh.buffer release];
        mesh.buffer = count ? [r->device newBufferWithBytes:vertices length:count * sizeof(Vertex) options:MTLResourceStorageModeShared] : nil;
        mesh.count = count;
    }

    bool hasTexture(uint64_t id)
    {
        Renderer *r = renderer();
        return r && r->textures.count(id) != 0;
    }

    void uploadTexture(uint64_t key, uint32_t width, uint32_t height, const uint8_t *rgba)
    {
        Renderer *r = renderer();
        if (!r || !width || !height)
            return;
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        id<MTLTexture> texture = [r->device newTextureWithDescriptor:desc];
        [texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:rgba bytesPerRow:width * 4u];
        auto it = r->textures.find(key);
        if (it != r->textures.end())
            [it->second release];
        r->textures[key] = texture;
    }

    void beginFrame()
    {
        if (Renderer *r = renderer())
            r->draws.clear();
    }

    void draw(uint64_t mesh, const float clip[16], const DrawState &state)
    {
        Renderer *r = renderer();
        if (!r)
            return;
        Draw d;
        d.mesh = mesh;
        d.state = state;
        std::memcpy(d.clip, clip, sizeof(d.clip));
        r->draws.push_back(d);
    }

    void endFrame(uint32_t width, uint32_t height)
    {
        if (Renderer *r = renderer())
            r->render(width, height);
    }

    void setShown(bool shown) { g_shown.store(shown, std::memory_order_relaxed); }
    bool shown() { return g_shown.load(std::memory_order_relaxed); }

    bool latestFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height)
    {
        Renderer *r = g_renderer;
        if (!r)
            return false;
        std::lock_guard<std::mutex> lock(r->outputMutex);
        if (!r->hasOutput)
            return false;
        rgba = r->output;
        width = r->outputWidth;
        height = r->outputHeight;
        return true;
    }
}
