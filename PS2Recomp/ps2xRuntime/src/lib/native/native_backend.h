#pragma once

// The native renderer's backends (Metal on macOS, OpenGL on macOS and the
// Vita through vitaGL): runtime/native_renderer.h picks one (native_renderer.cpp).

#include "runtime/native_renderer.h"

#include <functional>

namespace native_renderer::detail
{
    class Backend
    {
    public:
        virtual ~Backend() = default;
        virtual bool hasMesh(uint64_t id) = 0;
        virtual void uploadMesh(uint64_t id, const Vertex *vertices, uint32_t count, bool perFrame) = 0;
        virtual bool hasTexture(uint64_t id) = 0;
        virtual void uploadTexture(uint64_t id, uint32_t width, uint32_t height, const uint8_t *rgba, uint32_t levels) = 0;
        virtual void beginFrame() = 0;
        virtual void draw(uint64_t mesh, const float clip[16], const DrawState &state) = 0;
        virtual void setFog(const Fog &fog) = 0;
        virtual void setBloom(const Bloom &bloom) = 0;
        virtual void addScreenBlur(const ScreenBlur &blur) = 0;
        virtual void setGlowComposite(const GlowComposite &glow) = 0;
        virtual void setDepthOfField(const DepthOfField &dof) = 0;
        virtual void endFrame(uint32_t width, uint32_t height) = 0;
        virtual bool latestFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height) = 0;
    };

    // nullptr when the platform or the device does not have it.
    Backend *createMetalBackend();
    Backend *createGlBackend();
    // The host's present hook (native_renderer::setPresentHook), for the GL backend.
    const std::function<void(uint32_t, uint32_t, uint32_t)> &presentHook();
}
