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
    float depth; // w
    float fog;   // GS vertex fog factor, 1: none
};

struct FogParams
{
    float start, width, steps;
    int fogged;
    float2 size;
    int vertexFog;
    float base, scale;
    float3 colour;
    float4 debug; // w > 0: flat debug colour
};

struct Light
{
    float4x4 directions; // columns: the normal's x, y, z, 1
    float4x4 colours;    // columns: light 0, 1, 2, ambient
    int lit;
    float opacity;
};

vertex VOut nativeVertex(VIn in [[stage_in]], constant float4x4 &clip [[buffer(1)]], constant Light &light [[buffer(2)]],
                         constant FogParams &fog [[buffer(3)]])
{
    VOut out;
    float4 c = clip * float4(in.position, 1.0);
    // Depth from w (near plane at w = 1): z / w = 1 - 1 / w.
    out.position = float4(c.x, c.y, c.w - 1.0, c.w);
    out.depth = c.w;
    out.fog = fog.vertexFog != 0 ? clamp(fog.base + fog.scale / max(c.w, 1e-3), 0.0, 255.0) / 255.0 : 1.0;
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

fragment float4 nativeFragment(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], texture2d<float> background [[texture(1)]],
                               constant int &textured [[buffer(0)]], constant FogParams &fog [[buffer(1)]])
{
    constexpr sampler repeatLinear(address::repeat, filter::linear);
    float3 shade = in.color.rgb;
    float4 t = textured != 0 ? tex.sample(repeatLinear, in.uv) : float4(1.0);
    float alpha = saturate(t.a * (textured != 0 ? 2.0 : 1.0) * in.color.a); // texture alpha 0x80 = opaque
    if (alpha < 0.02)
        discard_fragment();
    if (fog.debug.w > 0.0)
        return float4(fog.debug.rgb, 1.0);
    float3 rgb = saturate(t.rgb * shade);
    rgb = mix(fog.colour, rgb, in.fog);
    if (fog.fogged != 0)
    {
        // SotC's zFog: the scene fades into the background image (the sky)
        // in steps of depth.
        float f = saturate((in.depth - fog.start) / fog.width);
        if (fog.steps > 0.0)
            f = floor(f * fog.steps) / fog.steps;
        constexpr sampler clampLinear(address::clamp_to_edge, filter::linear);
        float3 bg = background.sample(clampLinear, in.position.xy / fog.size).rgb;
        rgb = mix(rgb, bg, f);
    }
    return float4(rgb, alpha);
}

// ---- full-screen passes (bloom) ----
struct Quad
{
    float4 position [[position]];
    float2 uv;
};

vertex Quad fullscreenVertex(uint id [[vertex_id]])
{
    Quad q;
    float2 p = float2((id << 1) & 2, id & 2);
    q.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    q.uv = float2(p.x, 1.0 - p.y);
    return q;
}

struct BloomParams
{
    float threshold, zA, zB, temporal;
    float4 maskColor;
    float4 color;
    float2 texel; // of the source
};

// The mask at a quarter resolution: the share of the 4x4 block beyond the threshold.
fragment float4 bloomMask(Quad in [[stage_in]], depth2d<float> depth [[texture(0)]], constant BloomParams &b [[buffer(0)]])
{
    float sum = 0.0;
    uint2 size = uint2(depth.get_width(), depth.get_height());
    uint2 base = uint2(in.uv * float2(size)) & ~uint2(3);
    for (uint y = 0; y < 4; ++y)
        for (uint x = 0; x < 4; ++x)
        {
            float d = depth.read(min(base + uint2(x, y), size - 1));
            // depth = 1 - 1 / w (cleared: 1, nothing drawn); GS Z = zA + zB / w.
            float gsz = d >= 1.0 ? 0.0 : b.zA + b.zB * (1.0 - d);
            sum += gsz <= b.threshold + 1.0 ? 1.0 : 0.0;
        }
    return float4(b.maskColor.rgb * (sum / 16.0), 1.0);
}

fragment float4 bloomBlur(Quad in [[stage_in]], texture2d<float> src [[texture(0)]], constant float2 &step [[buffer(0)]])
{
    constexpr sampler s(address::clamp_to_edge, filter::linear);
    const float w[4] = {0.30, 0.235, 0.12, 0.05};
    float4 c = src.sample(s, in.uv) * w[0];
    for (int i = 1; i < 4; ++i)
        c += (src.sample(s, in.uv + step * float(i)) + src.sample(s, in.uv - step * float(i))) * w[i];
    return c;
}

fragment float4 bloomTemporal(Quad in [[stage_in]], texture2d<float> current [[texture(0)]], texture2d<float> previous [[texture(1)]],
                              constant float &amount [[buffer(0)]])
{
    constexpr sampler s(address::clamp_to_edge, filter::nearest);
    return mix(previous.sample(s, in.uv), current.sample(s, in.uv), amount);
}

fragment float4 shadowDarken(Quad in [[stage_in]], constant float &amount [[buffer(0)]])
{
    return float4(0.0, 0.0, 0.0, amount);
}

fragment float4 bloomComposite(Quad in [[stage_in]], texture2d<float> src [[texture(0)]], constant BloomParams &b [[buffer(0)]])
{
    constexpr sampler s(address::clamp_to_edge, filter::linear);
    return float4(src.sample(s, in.uv).rgb * b.color.rgb * b.color.a, 1.0);
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
        id<MTLRenderPipelineState> subtract = nil;
        id<MTLRenderPipelineState> volume = nil, darken = nil; // shadows
        id<MTLDepthStencilState> volumeState = nil, shadowedState = nil;
        id<MTLDepthStencilState> depthEqualReadOnly = nil;
        id<MTLDepthStencilState> depthOff = nil;
        id<MTLDepthStencilState> depthReadOnly = nil;
        id<MTLDepthStencilState> depthState = nil;
        id<MTLTexture> color = nil, depth = nil, backgroundColor = nil;
        native_renderer::Fog fog;
        native_renderer::Bloom bloom;
        id<MTLTexture> bloomTex[3] = {nil, nil, nil}; // ping, pong, history
        bool bloomHistory = false;
        id<MTLRenderPipelineState> maskPipeline = nil, blurPipeline = nil, temporalPipeline = nil, compositePipeline = nil;
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
            desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            desc.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
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
            blend.rgbBlendOperation = MTLBlendOperationReverseSubtract;
            subtract = [device newRenderPipelineStateWithDescriptor:desc error:&error];
            blend.rgbBlendOperation = MTLBlendOperationAdd;
            blend.blendingEnabled = NO;
            opaque = [device newRenderPipelineStateWithDescriptor:desc error:&error];
            blend.writeMask = MTLColorWriteMaskNone;
            volume = [device newRenderPipelineStateWithDescriptor:desc error:&error];
            blend.writeMask = MTLColorWriteMaskAll;
            {
                // The darkening of the shadowed pixels: Cd x (1 - amount).
                id<MTLFunction> savedVertex = desc.vertexFunction, savedFragment = desc.fragmentFunction;
                MTLVertexDescriptor *savedLayout = [desc.vertexDescriptor retain];
                desc.vertexFunction = [library newFunctionWithName:@"fullscreenVertex"];
                desc.fragmentFunction = [library newFunctionWithName:@"shadowDarken"];
                desc.vertexDescriptor = nil;
                blend.blendingEnabled = YES;
                blend.sourceRGBBlendFactor = MTLBlendFactorZero;
                blend.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                blend.sourceAlphaBlendFactor = MTLBlendFactorZero;
                blend.destinationAlphaBlendFactor = MTLBlendFactorOne;
                darken = [device newRenderPipelineStateWithDescriptor:desc error:&error];
                [desc.vertexFunction release];
                [desc.fragmentFunction release];
                desc.vertexFunction = savedVertex;
                desc.fragmentFunction = savedFragment;
                desc.vertexDescriptor = savedLayout;
                [savedLayout release];
            }
            [desc release];
            auto fullscreen = [&](NSString *fragment, bool add) -> id<MTLRenderPipelineState>
            {
                MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
                d.vertexFunction = [library newFunctionWithName:@"fullscreenVertex"];
                d.fragmentFunction = [library newFunctionWithName:fragment];
                d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
                if (add)
                {
                    d.colorAttachments[0].blendingEnabled = YES;
                    d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
                    d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
                    d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
                    d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
                }
                NSError *e = nil;
                id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:&e];
                [d.vertexFunction release];
                [d.fragmentFunction release];
                [d release];
                if (!state)
                    std::cerr << "[native] bloom pipeline failed: " << [[e localizedDescription] UTF8String] << std::endl;
                return state;
            };
            maskPipeline = fullscreen(@"bloomMask", false);
            blurPipeline = fullscreen(@"bloomBlur", false);
            temporalPipeline = fullscreen(@"bloomTemporal", false);
            compositePipeline = fullscreen(@"bloomComposite", true);
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
            depthDesc.depthCompareFunction = MTLCompareFunctionLessEqual;
            depthEqualReadOnly = [device newDepthStencilStateWithDescriptor:depthDesc];
            depthDesc.depthCompareFunction = MTLCompareFunctionAlways;
            depthOff = [device newDepthStencilStateWithDescriptor:depthDesc];
            {
                // Shadow volumes: the count where the faces pass the depth test.
                MTLStencilDescriptor *front = [[MTLStencilDescriptor alloc] init], *back = [[MTLStencilDescriptor alloc] init];
                front.stencilCompareFunction = back.stencilCompareFunction = MTLCompareFunctionAlways;
                front.depthStencilPassOperation = MTLStencilOperationIncrementWrap;
                back.depthStencilPassOperation = MTLStencilOperationDecrementWrap;
                depthDesc.depthCompareFunction = MTLCompareFunctionLess;
                depthDesc.depthWriteEnabled = NO;
                depthDesc.frontFaceStencil = front;
                depthDesc.backFaceStencil = back;
                volumeState = [device newDepthStencilStateWithDescriptor:depthDesc];
                // The darkening: where the count is not 0; it clears the count.
                front.stencilCompareFunction = back.stencilCompareFunction = MTLCompareFunctionNotEqual;
                front.depthStencilPassOperation = back.depthStencilPassOperation = MTLStencilOperationZero;
                depthDesc.depthCompareFunction = MTLCompareFunctionAlways;
                depthDesc.frontFaceStencil = front;
                depthDesc.backFaceStencil = back;
                shadowedState = [device newDepthStencilStateWithDescriptor:depthDesc];
                [front release];
                [back release];
            }
            [depthDesc release];
            return true;
        }

        void ensureTargets(uint32_t w, uint32_t h)
        {
            if (color && width == w && height == h)
                return;
            [color release];
            [depth release];
            [backgroundColor release];
            MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
            cd.usage = MTLTextureUsageRenderTarget;
            cd.storageMode = MTLStorageModeShared;
            color = [device newTextureWithDescriptor:cd];
            cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            cd.storageMode = MTLStorageModePrivate;
            backgroundColor = [device newTextureWithDescriptor:cd];
            MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:w height:h mipmapped:NO];
            dd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            dd.storageMode = MTLStorageModePrivate;
            depth = [device newTextureWithDescriptor:dd];
            for (id<MTLTexture> &t : bloomTex)
            {
                [t release];
                MTLTextureDescriptor *bd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                              width:std::max(w / 4u, 1u) height:std::max(h / 4u, 1u) mipmapped:NO];
                bd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                bd.storageMode = MTLStorageModePrivate;
                t = [device newTextureWithDescriptor:bd];
            }
            bloomHistory = false;
            width = w;
            height = h;
        }

        void render(uint32_t w, uint32_t h)
        {
            @autoreleasepool
            {
                ensureTargets(w, h);
                std::stable_sort(draws.begin(), draws.end(), [](const Draw &a, const Draw &b) { return a.state.order < b.state.order; });
                id<MTLCommandBuffer> commands = [queue commandBuffer];
                const bool fogOn = fog.enabled && fog.width > 0.0f;
                auto pass = [&](id<MTLTexture> target, bool backgroundOnly, uint32_t fromOrder = 0, uint32_t toOrder = ~0u, bool load = false)
                {
                    MTLRenderPassDescriptor *desc = [MTLRenderPassDescriptor renderPassDescriptor];
                    desc.colorAttachments[0].texture = target;
                    desc.colorAttachments[0].loadAction = load ? MTLLoadActionLoad : MTLLoadActionClear;
                    desc.colorAttachments[0].clearColor = MTLClearColorMake(0.08, 0.08, 0.1, 1.0);
                    desc.colorAttachments[0].storeAction = MTLStoreActionStore;
                    desc.depthAttachment.texture = depth;
                    desc.depthAttachment.loadAction = load ? MTLLoadActionLoad : MTLLoadActionClear;
                    desc.depthAttachment.clearDepth = 1.0;
                    desc.depthAttachment.storeAction = MTLStoreActionStore;
                    desc.stencilAttachment.texture = depth;
                    desc.stencilAttachment.loadAction = load ? MTLLoadActionLoad : MTLLoadActionClear;
                    desc.stencilAttachment.clearStencil = 0;
                    desc.stencilAttachment.storeAction = MTLStoreActionStore;
                    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:desc];
                    [encoder setCullMode:MTLCullModeNone];
                    if (!backgroundOnly)
                        [encoder setFragmentTexture:backgroundColor atIndex:1];
                    int bound = -1;
                    // Shadow volumes counted so far, waiting for their darkening.
                    uint32_t shadowOrder = 0;
                    float shadowAmount = 0.0f;
                    auto resolveShadows = [&]
                    {
                        if (!shadowAmount)
                            return;
                        [encoder setRenderPipelineState:darken];
                        [encoder setDepthStencilState:shadowedState];
                        [encoder setStencilReferenceValue:0];
                        [encoder setFragmentBytes:&shadowAmount length:sizeof(shadowAmount) atIndex:0];
                        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                        shadowAmount = 0.0f;
                        bound = -1;
                    };
                    for (const Draw &d : draws)
                    {
                        if (backgroundOnly && !d.state.background)
                            continue;
                        if (d.state.order < fromOrder || d.state.order >= toOrder)
                            continue;
                        if (shadowAmount && d.state.order > shadowOrder)
                            resolveShadows();
                        if (d.state.shadowVolume)
                        {
                            if (backgroundOnly)
                                continue;
                            auto it = meshes.find(d.mesh);
                            if (it == meshes.end() || it->second.count == 0)
                                continue;
                            [encoder setRenderPipelineState:volume];
                            [encoder setDepthStencilState:volumeState];
                            [encoder setVertexBuffer:it->second.buffer offset:0 atIndex:0];
                            [encoder setVertexBytes:d.clip length:sizeof(d.clip) atIndex:1];
                            struct
                            {
                                float light[32];
                                int lit;
                                float opacity;
                                int pad[2];
                            } none = {};
                            none.opacity = 1.0f;
                            [encoder setVertexBytes:&none length:sizeof(none) atIndex:2];
                            struct
                            {
                                float values[24];
                            } nofog = {};
                            [encoder setVertexBytes:&nofog length:sizeof(nofog) atIndex:3];
                            [encoder setFragmentBytes:&nofog length:sizeof(nofog) atIndex:1];
                            const int untextured = 0;
                            [encoder setFragmentBytes:&untextured length:sizeof(untextured) atIndex:0];
                            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:it->second.count];
                            shadowOrder = d.state.order;
                            shadowAmount = d.state.shadow;
                            bound = -1;
                            continue;
                        }
                        const int blendMode = static_cast<int>(d.state.blend);
                        const bool backdrop = d.state.screen || d.state.background;
                        const int mode = blendMode | (backdrop ? 8 : 0) | (d.state.overlay ? 16 : 0) | (d.state.writeDepth ? 32 : 0);
                        if (mode != bound)
                        {
                            [encoder setRenderPipelineState:blendMode == 1 ? additive : (blendMode == 2 ? opaque : (blendMode == 3 ? subtract : pipeline))];
                            // Additive glows do not hide what is drawn after them; 2D and the
                            // background (the sky, a small dome drawn first) ignore depth; a
                            // second pass lands on its first.
                            [encoder setDepthStencilState:backdrop ? depthOff
                                                                   : (d.state.overlay ? depthEqualReadOnly
                                                                                      : (blendMode == 1 && !d.state.writeDepth ? depthReadOnly : depthState))];
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
                        struct
                        {
                            float start, width, steps;
                            int fogged;
                            float size[2];
                            int vertexFog;
                            float base, scale;
                            float pad[3];
                            float colour[4];
                            float debug[4];
                        } fp = {fog.start, fog.width, static_cast<float>(fog.steps), (!backgroundOnly && fogOn && d.state.fogged) ? 1 : 0,
                                {static_cast<float>(w), static_cast<float>(h)},
                                (fog.vertexFog && !d.state.screen && !d.state.background) ? 1 : 0, fog.base, fog.scale, {0, 0, 0},
                                {fog.colour[0], fog.colour[1], fog.colour[2], 0},
                                {static_cast<float>(d.state.debugColour & 0xFFu) / 255.0f, static_cast<float>((d.state.debugColour >> 8) & 0xFFu) / 255.0f,
                                 static_cast<float>((d.state.debugColour >> 16) & 0xFFu) / 255.0f, d.state.debugColour ? 1.0f : 0.0f}};
                        [encoder setFragmentBytes:&fp length:sizeof(fp) atIndex:1];
                        [encoder setVertexBytes:&fp length:sizeof(fp) atIndex:3];
                        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:it->second.count];
                    }
                    resolveShadows();
                    [encoder endEncoding];
                };
                if (fogOn)
                    pass(backgroundColor, true); // the sky, what distance fades into
                const bool bloomOn = bloom.enabled && maskPipeline && blurPipeline && temporalPipeline && compositePipeline;
                if (!bloomOn)
                {
                    pass(color, false);
                    bloomHistory = false;
                }
                else
                {
                    pass(color, false, 0, bloom.order);
                    struct
                    {
                        float threshold, zA, zB, temporal;
                        float maskColor[4];
                        float color[4];
                        float texel[2];
                        float pad[2];
                    } bp = {bloom.threshold, bloom.zA, bloom.zB, bloom.temporal,
                            {bloom.maskColor[0], bloom.maskColor[1], bloom.maskColor[2], 1.0f},
                            {bloom.color[0], bloom.color[1], bloom.color[2], bloom.color[3]}, {0, 0}, {0, 0}};
                    auto quad = [&](id<MTLTexture> target, id<MTLRenderPipelineState> state, bool load, auto bind)
                    {
                        MTLRenderPassDescriptor *desc = [MTLRenderPassDescriptor renderPassDescriptor];
                        desc.colorAttachments[0].texture = target;
                        desc.colorAttachments[0].loadAction = load ? MTLLoadActionLoad : MTLLoadActionDontCare;
                        desc.colorAttachments[0].storeAction = MTLStoreActionStore;
                        id<MTLRenderCommandEncoder> e = [commands renderCommandEncoderWithDescriptor:desc];
                        [e setRenderPipelineState:state];
                        bind(e);
                        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                        [e endEncoding];
                    };
                    const float lw = static_cast<float>(bloomTex[0].width), lh = static_cast<float>(bloomTex[0].height);
                    quad(bloomTex[0], maskPipeline, false, [&](id<MTLRenderCommandEncoder> e)
                    {
                        [e setFragmentTexture:depth atIndex:0];
                        [e setFragmentBytes:&bp length:sizeof(bp) atIndex:0];
                    });
                    // The GS chain reduces the mask in stages before blurring: a wide glow.
                    for (int i = 0; i < 3; ++i)
                    {
                        const float spread = 1.0f + static_cast<float>(i);
                        const float stepH[2] = {spread / lw, 0.0f}, stepV[2] = {0.0f, spread / lh};
                        quad(bloomTex[1], blurPipeline, false, [&](id<MTLRenderCommandEncoder> e)
                        {
                            [e setFragmentTexture:bloomTex[0] atIndex:0];
                            [e setFragmentBytes:stepH length:sizeof(stepH) atIndex:0];
                        });
                        quad(bloomTex[0], blurPipeline, false, [&](id<MTLRenderCommandEncoder> e)
                        {
                            [e setFragmentTexture:bloomTex[1] atIndex:0];
                            [e setFragmentBytes:stepV length:sizeof(stepV) atIndex:0];
                        });
                    }
                    // With the last frame's: bloomTex[1] = mix(history, current), kept as history.
                    const float amount = bloomHistory ? bloom.temporal : 1.0f;
                    quad(bloomTex[1], temporalPipeline, false, [&](id<MTLRenderCommandEncoder> e)
                    {
                        [e setFragmentTexture:bloomTex[0] atIndex:0];
                        [e setFragmentTexture:bloomTex[2] atIndex:1];
                        [e setFragmentBytes:&amount length:sizeof(amount) atIndex:0];
                    });
                    std::swap(bloomTex[1], bloomTex[2]);
                    bloomHistory = true;
                    quad(color, compositePipeline, true, [&](id<MTLRenderCommandEncoder> e)
                    {
                        [e setFragmentTexture:bloomTex[2] atIndex:0];
                        [e setFragmentBytes:&bp length:sizeof(bp) atIndex:0];
                    });
                    pass(color, false, bloom.order, ~0u, true);
                }
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

    void setBloom(const Bloom &bloom)
    {
        if (Renderer *r = renderer())
            r->bloom = bloom;
    }

    void setFog(const Fog &fog)
    {
        if (Renderer *r = renderer())
            r->fog = fog;
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
