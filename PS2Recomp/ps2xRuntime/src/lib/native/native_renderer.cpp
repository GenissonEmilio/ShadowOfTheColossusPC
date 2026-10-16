// The native renderer's front: picks a backend (native_backend.h) the first
// time it is used. macOS: Metal, or OpenGL with SOTC_NATIVE_GL=1 (to check
// the GL path against Metal); the Vita: OpenGL (vitaGL); elsewhere none.
#include "native_backend.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdlib>
#include <functional>
#include <iostream>

#if defined(__APPLE__) || (defined(__vita__) && defined(PS2X_VITA_SDL2_SHIM))
#define PS2X_NATIVE_GL 1
#endif

namespace native_renderer::detail
{
#if !defined(PS2X_HAVE_METAL)
    Backend *createMetalBackend() { return nullptr; }
#endif
#if !defined(PS2X_NATIVE_GL)
    Backend *createGlBackend() { return nullptr; }
#endif
}

namespace
{
    // SOTC_NATIVE_GL=check (macOS): Metal draws what is shown and OpenGL the
    // same frames alongside; every 60 frames the two pictures are compared
    // ([native:check] mean difference per channel, and the share of pixels
    // off by more than 8), the GL one written next to it (PS2X_NATIVE_CHECK_DIR).
    class CheckBackend final : public native_renderer::detail::Backend
    {
    public:
        CheckBackend(native_renderer::detail::Backend *shown, native_renderer::detail::Backend *other) : a(shown), b(other) {}
        bool hasMesh(uint64_t id) override { return a->hasMesh(id) && b->hasMesh(id); }
        void uploadMesh(uint64_t id, const native_renderer::Vertex *v, uint32_t n) override { a->uploadMesh(id, v, n), b->uploadMesh(id, v, n); }
        bool hasTexture(uint64_t id) override { return a->hasTexture(id) && b->hasTexture(id); }
        void uploadTexture(uint64_t id, uint32_t w, uint32_t h, const uint8_t *p, uint32_t l) override { a->uploadTexture(id, w, h, p, l), b->uploadTexture(id, w, h, p, l); }
        void beginFrame() override { a->beginFrame(), b->beginFrame(); }
        void draw(uint64_t m, const float c[16], const native_renderer::DrawState &s) override { a->draw(m, c, s), b->draw(m, c, s); }
        void setFog(const native_renderer::Fog &f) override { a->setFog(f), b->setFog(f); }
        void setBloom(const native_renderer::Bloom &x) override { a->setBloom(x), b->setBloom(x); }
        void addScreenBlur(const native_renderer::ScreenBlur &x) override { a->addScreenBlur(x), b->addScreenBlur(x); }
        void setGlowComposite(const native_renderer::GlowComposite &x) override { a->setGlowComposite(x), b->setGlowComposite(x); }
        void setDepthOfField(const native_renderer::DepthOfField &x) override { a->setDepthOfField(x), b->setDepthOfField(x); }
        void endFrame(uint32_t w, uint32_t h) override
        {
            a->endFrame(w, h);
            b->endFrame(w, h);
            if (++m_frames % 60u != 0u)
                return;
            std::vector<uint8_t> x, y;
            uint32_t xw = 0, xh = 0, yw = 0, yh = 0;
            if (!a->latestFrame(x, xw, xh) || !b->latestFrame(y, yw, yh) || xw != yw || xh != yh)
                return;
            uint64_t sum = 0, off = 0;
            for (size_t i = 0; i < x.size(); i += 4)
            {
                int worst = 0;
                for (int c = 0; c < 3; ++c)
                {
                    const int d = std::abs(static_cast<int>(x[i + c]) - static_cast<int>(y[i + c]));
                    sum += static_cast<uint64_t>(d);
                    worst = std::max(worst, d);
                }
                off += worst > 8 ? 1u : 0u;
            }
            const double pixels = static_cast<double>(x.size() / 4u);
            std::cerr << "[native:check] frame " << m_frames << " mean " << static_cast<double>(sum) / (pixels * 3.0) << " off>8 " << 100.0 * static_cast<double>(off) / pixels
                      << "%" << std::endl;
            if (const char *dir = std::getenv("PS2X_NATIVE_CHECK_DIR"))
            {
                for (int k = 0; k < 2; ++k)
                {
                    const std::string path = std::string(dir) + "/" + (k ? "gl_" : "metal_") + std::to_string(m_frames) + ".rgba";
                    if (FILE *f = std::fopen(path.c_str(), "wb"))
                    {
                        const auto &p = k ? y : x;
                        std::fwrite(p.data(), 1, p.size(), f);
                        std::fclose(f);
                    }
                }
            }
        }
        bool latestFrame(std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) override { return a->latestFrame(rgba, w, h); }

    private:
        native_renderer::detail::Backend *a, *b;
        uint64_t m_frames = 0;
    };

    native_renderer::detail::Backend *g_backend = nullptr;
    bool g_tried = false;
    std::atomic<bool> g_shown{false};
    std::function<void()> g_glMakeCurrent;
    std::function<void(uint32_t, uint32_t, uint32_t)> g_presentHook;

    bool wantGl()
    {
#if defined(__vita__)
        return true;
#else
        const char *gl = std::getenv("SOTC_NATIVE_GL");
        return gl && (std::atoi(gl) != 0 || std::string(gl) == "check");
#endif
    }

    // OpenGL: created by the first call after available(), from the game
    // thread, which keeps the renderer's context current from then on.
    native_renderer::detail::Backend *backend()
    {
        if (!g_tried)
        {
            g_tried = true;
            if (wantGl() && g_glMakeCurrent)
            {
                g_glMakeCurrent();
                g_backend = native_renderer::detail::createGlBackend();
                std::cerr << "[native] OpenGL renderer " << (g_backend ? "up" : "failed") << std::endl;
                const char *mode = std::getenv("SOTC_NATIVE_GL");
                if (g_backend && mode && std::string(mode) == "check")
                {
                    if (auto *metal = native_renderer::detail::createMetalBackend())
                        g_backend = new CheckBackend(metal, g_backend);
                }
            }
            if (!g_backend && !wantGl())
                g_backend = native_renderer::detail::createMetalBackend();
        }
        return g_backend;
    }
}

namespace native_renderer::detail
{
    const std::function<void(uint32_t, uint32_t, uint32_t)> &presentHook() { return g_presentHook; }
}

namespace native_renderer
{
    void setGlContext(std::function<void()> makeCurrent)
    {
        g_glMakeCurrent = std::move(makeCurrent);
#if defined(__vita__)
        // The CG compiler needs the main thread's stack: build it now.
        backend();
#endif
    }
    void setPresentHook(std::function<void(uint32_t, uint32_t, uint32_t)> hook) { g_presentHook = std::move(hook); }

    bool available()
    {
        if (!g_tried && wantGl())
            return static_cast<bool>(g_glMakeCurrent); // not created here: this may not be the game thread
        return backend() != nullptr;
    }
    bool hasMesh(uint64_t id) { return backend() && g_backend->hasMesh(id); }
    void uploadMesh(uint64_t id, const Vertex *vertices, uint32_t count)
    {
        if (backend())
            g_backend->uploadMesh(id, vertices, count);
    }
    bool hasTexture(uint64_t id) { return backend() && g_backend->hasTexture(id); }
    void uploadTexture(uint64_t id, uint32_t width, uint32_t height, const uint8_t *rgba, uint32_t levels)
    {
        if (backend())
            g_backend->uploadTexture(id, width, height, rgba, levels);
    }
    void beginFrame()
    {
        if (backend())
            g_backend->beginFrame();
    }
    void draw(uint64_t mesh, const float clip[16], const DrawState &state)
    {
        if (backend())
            g_backend->draw(mesh, clip, state);
    }
    void setFog(const Fog &fog)
    {
        if (backend())
            g_backend->setFog(fog);
    }
    void setBloom(const Bloom &bloom)
    {
        if (backend())
            g_backend->setBloom(bloom);
    }
    void addScreenBlur(const ScreenBlur &blur)
    {
        if (backend())
            g_backend->addScreenBlur(blur);
    }
    void setGlowComposite(const GlowComposite &glow)
    {
        if (backend())
            g_backend->setGlowComposite(glow);
    }
    void setDepthOfField(const DepthOfField &dof)
    {
        if (backend())
            g_backend->setDepthOfField(dof);
    }
    void endFrame(uint32_t width, uint32_t height)
    {
        if (backend())
            g_backend->endFrame(width, height);
    }
    void setShown(bool shown) { g_shown.store(shown, std::memory_order_relaxed); }
    bool shown() { return g_shown.load(std::memory_order_relaxed); }
    bool latestFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height)
    {
        // Not backend(): the presenting thread must not create it.
        return g_backend && g_backend->latestFrame(rgba, width, height);
    }
}
