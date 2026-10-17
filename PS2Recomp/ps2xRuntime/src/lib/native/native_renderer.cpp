// The native renderer's front: picks a backend (native_backend.h) the first
// time it is used. macOS: Metal, or OpenGL with SOTC_NATIVE_GL=1 (to check
// the GL path against Metal); the Vita: OpenGL (vitaGL); elsewhere none.
#include "native_backend.h"
#include "runtime/native_tuning.h"

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

namespace native_renderer
{
    Tuning defaultTuning()
    {
        Tuning d;
#if defined(__APPLE__)
        d.renderScale = 3;
#endif
        return d;
    }

    Tuning &tuning()
    {
        static Tuning t = []
        {
            Tuning d = defaultTuning();
            auto on = [](const char *name) { return std::getenv(name) != nullptr; };
            auto number = [](const char *name, float fallback) { return std::getenv(name) ? static_cast<float>(std::atof(std::getenv(name))) : fallback; };
            d.renderScale = static_cast<int>(number("SOTC_RENDER_SCALE", static_cast<float>(d.renderScale)));
            const float strength = number("SOTC_BLOOM_SCALE", d.bloomStrength);
            d.bloom = strength > 0.0f;
            if (d.bloom)
                d.bloomStrength = strength;
            d.bloomRounds = static_cast<int>(number("SOTC_BLOOM_ROUNDS", 1.0f));
            d.bloomSpread = number("SOTC_BLOOM_SPREAD", 1.0f);
            d.vertexFog = !on("SOTC_NATIVE_NOVFOG");
            d.vertexFogAmount = number("SOTC_FOG_AMOUNT", d.vertexFogAmount);
            d.zFog = !on("SOTC_NATIVE_ZFOG") || std::atoi(std::getenv("SOTC_NATIVE_ZFOG")) != 0;
            d.glow = !on("SOTC_NATIVE_NOGLOW");
            d.depthOfField = !on("SOTC_NATIVE_NODOF");
            d.shadows = !on("SOTC_NATIVE_NOSHADOWS");
            d.particles = !on("SOTC_NATIVE_NOPTCL");
            d.screenBlur = !on("SOTC_NATIVE_NOSBLUR");
            d.reflections = !on("SOTC_NATIVE_NOREFLECT");
            d.refraction = !on("SOTC_NATIVE_NOREFRACT");
            return d;
        }();
        return t;
    }

    std::string tuningAsEnv()
    {
        const Tuning &t = tuning(), d = defaultTuning();
        std::string out;
        auto add = [&](const std::string &s) { out += (out.empty() ? "" : " ") + s; };
        auto number = [](float v) { char b[32]; std::snprintf(b, sizeof(b), "%g", v); return std::string(b); };
        if (t.renderScale != d.renderScale)
            add("SOTC_RENDER_SCALE=" + std::to_string(t.renderScale));
        if (!t.bloom)
            add("SOTC_BLOOM_SCALE=0");
        else if (t.bloomStrength != d.bloomStrength)
            add("SOTC_BLOOM_SCALE=" + number(t.bloomStrength));
        if (t.bloomRounds != d.bloomRounds)
            add("SOTC_BLOOM_ROUNDS=" + std::to_string(t.bloomRounds));
        if (t.bloomSpread != d.bloomSpread)
            add("SOTC_BLOOM_SPREAD=" + number(t.bloomSpread));
        if (!t.vertexFog)
            add("SOTC_NATIVE_NOVFOG=1");
        if (t.vertexFogAmount != d.vertexFogAmount)
            add("SOTC_FOG_AMOUNT=" + number(t.vertexFogAmount));
        if (!t.zFog)
            add("SOTC_NATIVE_ZFOG=0");
        const std::pair<bool, const char *> offs[] = {{t.glow, "SOTC_NATIVE_NOGLOW=1"}, {t.depthOfField, "SOTC_NATIVE_NODOF=1"}, {t.shadows, "SOTC_NATIVE_NOSHADOWS=1"},
                                                      {t.particles, "SOTC_NATIVE_NOPTCL=1"}, {t.screenBlur, "SOTC_NATIVE_NOSBLUR=1"}, {t.reflections, "SOTC_NATIVE_NOREFLECT=1"},
                                                      {t.refraction, "SOTC_NATIVE_NOREFRACT=1"}};
        for (const auto &[value, env] : offs)
            if (!value)
                add(env);
        return out.empty() ? "(the defaults)" : out;
    }
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
        void uploadMesh(uint64_t id, const native_renderer::Vertex *v, uint32_t n, bool f) override { a->uploadMesh(id, v, n, f), b->uploadMesh(id, v, n, f); }
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
    // SOTC_NATIVE_LOG_UPLOADS=1: uploads per frame (meshes: static / per-frame
    // ones, the ids with a top bit set; textures), every 60 frames.
    struct UploadStats
    {
        bool on = std::getenv("SOTC_NATIVE_LOG_UPLOADS") != nullptr;
        uint64_t frames = 0, staticMeshes = 0, staticBytes = 0, frameMeshes = 0, frameBytes = 0, textures = 0, textureBytes = 0, draws = 0;
    } g_uploads;
    void uploadMesh(uint64_t id, const Vertex *vertices, uint32_t count, bool perFrame)
    {
        if (g_uploads.on)
        {
            (perFrame ? g_uploads.frameMeshes : g_uploads.staticMeshes) += 1;
            (perFrame ? g_uploads.frameBytes : g_uploads.staticBytes) += static_cast<uint64_t>(count) * sizeof(Vertex);
        }
        if (backend())
            g_backend->uploadMesh(id, vertices, count, perFrame);
    }
    bool hasTexture(uint64_t id) { return backend() && g_backend->hasTexture(id); }
    void uploadTexture(uint64_t id, uint32_t width, uint32_t height, const uint8_t *rgba, uint32_t levels)
    {
        if (g_uploads.on)
        {
            ++g_uploads.textures;
            g_uploads.textureBytes += static_cast<uint64_t>(width) * height * 4u;
        }
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
        ++g_uploads.draws;
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
        if (g_uploads.on && ++g_uploads.frames % 60u == 0u)
        {
            UploadStats &u = g_uploads;
            std::fprintf(stderr, "[uploads] per frame: static meshes %.1f (%.0f KB), per-frame meshes %.1f (%.0f KB), textures %.2f (%.0f KB), draws %.0f\n",
                         u.staticMeshes / 60.0, u.staticBytes / 60.0 / 1024.0, u.frameMeshes / 60.0, u.frameBytes / 60.0 / 1024.0,
                         u.textures / 60.0, u.textureBytes / 60.0 / 1024.0, u.draws / 60.0);
            u.staticMeshes = u.staticBytes = u.frameMeshes = u.frameBytes = u.textures = u.textureBytes = u.draws = 0;
        }
    }
    void setShown(bool shown) { g_shown.store(shown, std::memory_order_relaxed); }
    bool shown() { return g_shown.load(std::memory_order_relaxed); }
    bool latestFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height)
    {
        // Not backend(): the presenting thread must not create it.
        return g_backend && g_backend->latestFrame(rgba, width, height);
    }
}
