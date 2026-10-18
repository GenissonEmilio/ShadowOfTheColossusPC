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
#include <cstring>
#include <cmath>
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
        d.renderScale = 3.0f;
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
            d.renderScale = number("SOTC_RENDER_SCALE", d.renderScale);
            const float strength = number("SOTC_BLOOM_SCALE", d.bloomStrength);
            d.bloom = strength > 0.0f;
            if (d.bloom)
                d.bloomStrength = strength;
            d.bloomRounds = number("SOTC_BLOOM_ROUNDS", d.bloomRounds);
            d.bloomSpread = number("SOTC_BLOOM_SPREAD", 1.0f);
            d.vertexFog = !on("SOTC_NATIVE_NOVFOG");
            d.vertexFogAmount = number("SOTC_FOG_AMOUNT", d.vertexFogAmount);
            d.zFog = !on("SOTC_NATIVE_ZFOG") || std::atoi(std::getenv("SOTC_NATIVE_ZFOG")) != 0;
            d.glow = !on("SOTC_NATIVE_NOGLOW");
            d.depthOfField = !on("SOTC_NATIVE_NODOF");
            d.shadows = !on("SOTC_NATIVE_NOSHADOWS");
            d.particles = !on("SOTC_NATIVE_NOPTCL");
            d.screenBlur = !on("SOTC_NATIVE_NOSBLUR");
            d.feedback = !on("SOTC_NATIVE_NOFEEDBACK");
            d.reflections = !on("SOTC_NATIVE_NOREFLECT");
            d.refraction = !on("SOTC_NATIVE_NOREFRACT");
            d.mipmaps = !on("SOTC_NATIVE_NOMIPS");
            d.textureScroll = !on("SOTC_NATIVE_NOROW");
            if (const char *h = std::getenv("SOTC_HIDE_LAYERS")) // n,n,n
                for (const char *c = h; *c;)
                {
                    d.hiddenLayers |= 1u << (std::atoi(c) & 31);
                    while (*c && *c != ',')
                        ++c;
                    if (*c)
                        ++c;
                }
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
            add("SOTC_RENDER_SCALE=" + number(t.renderScale));
        if (!t.bloom)
            add("SOTC_BLOOM_SCALE=0");
        else if (t.bloomStrength != d.bloomStrength)
            add("SOTC_BLOOM_SCALE=" + number(t.bloomStrength));
        if (t.bloomRounds != d.bloomRounds)
            add("SOTC_BLOOM_ROUNDS=" + number(t.bloomRounds));
        if (t.bloomSpread != d.bloomSpread)
            add("SOTC_BLOOM_SPREAD=" + number(t.bloomSpread));
        if (!t.vertexFog)
            add("SOTC_NATIVE_NOVFOG=1");
        if (t.vertexFogAmount != d.vertexFogAmount)
            add("SOTC_FOG_AMOUNT=" + number(t.vertexFogAmount));
        if (!t.zFog)
            add("SOTC_NATIVE_ZFOG=0");
        const std::pair<bool, const char *> offs[] = {{t.glow, "SOTC_NATIVE_NOGLOW=1"}, {t.depthOfField, "SOTC_NATIVE_NODOF=1"}, {t.shadows, "SOTC_NATIVE_NOSHADOWS=1"},
                                                      {t.particles, "SOTC_NATIVE_NOPTCL=1"}, {t.screenBlur, "SOTC_NATIVE_NOSBLUR=1"}, {t.feedback, "SOTC_NATIVE_NOFEEDBACK=1"}, {t.reflections, "SOTC_NATIVE_NOREFLECT=1"},
                                                      {t.refraction, "SOTC_NATIVE_NOREFRACT=1"}};
        for (const auto &[value, env] : offs)
            if (!value)
                add(env);
        if (t.hiddenLayers)
        {
            std::string layers;
            for (int i = 0; i < 32; ++i)
                if ((t.hiddenLayers >> i) & 1u)
                    layers += (layers.empty() ? "" : ",") + std::to_string(i);
            add("SOTC_HIDE_LAYERS=" + layers);
        }
        return out.empty() ? "(the defaults)" : out;
    }

    const std::vector<TuningItem> &tuningItems()
    {
        static const std::vector<TuningItem> items = []
        {
        std::vector<TuningItem> v = {
            {"render_scale", "Resolution", "Resolução",
             "The picture is drawn at 512 x 448 times this: 0.5 is faster, 1 is the PS2's, 2 to 4 are sharper.",
             "A imagem é desenhada em 512 x 448 vezes este valor: 0,5 é mais rápido, 1 é a do PS2, 2 a 4 ficam mais nítidas.",
             nullptr, &Tuning::renderScale, 0.5f, 4.0f, 0.5f, "SOTC_RENDER_SCALE"},
            {"bloom", "Bloom", "Bloom", "The glow around bright light and sky.", "O brilho em volta da luz forte e do céu.",
             &Tuning::bloom, nullptr, 0, 1, 1, "SOTC_BLOOM_SCALE"},
            {"bloom_strength", "Bloom strength", "Força do bloom",
             "Times the game's own strength, which it keeps raising and lowering by scene and camera.",
             "Vezes a força do próprio jogo, que ele aumenta e diminui conforme a cena e a câmera.",
             nullptr, &Tuning::bloomStrength, 0.0f, 2.0f, 0.02f, "SOTC_BLOOM_SCALE"},
            {"bloom_rounds", "Bloom blur", "Borrão do bloom", "Blur rounds: more spreads the glow wider.",
             "Rodadas de borrão: mais espalha o brilho.", nullptr, &Tuning::bloomRounds, 1.0f, 6.0f, 1.0f, "SOTC_BLOOM_ROUNDS"},
            {"bloom_spread", "Bloom spread", "Espalhamento", "How much each further blur round grows (from the second round).",
             "Quanto cada rodada de borrão a mais cresce (a partir da segunda).", nullptr, &Tuning::bloomSpread, 0.0f, 4.0f, 0.1f,
             "SOTC_BLOOM_SPREAD"},
            {"vertex_fog", "Distance fog", "Névoa por distância", "The PS2's fog that fades far things into the air colour.",
             "A névoa do PS2 que apaga o que está longe na cor do ar.", &Tuning::vertexFog, nullptr, 0, 1, 1, "SOTC_NATIVE_NOVFOG"},
            {"fog_amount", "Fog amount", "Quantidade de névoa", "Times the game's fog: 1 is the game's own.",
             "Vezes a névoa do jogo: 1 é a do próprio jogo.", nullptr, &Tuning::vertexFogAmount, 0.0f, 2.0f, 0.05f, "SOTC_FOG_AMOUNT"},
            {"zfog", "Depth fog", "Névoa de profundidade", "Far scenery fades into the sky.", "O cenário distante some no céu.",
             &Tuning::zFog, nullptr, 0, 1, 1, "SOTC_NATIVE_ZFOG"},
            {"glow", "Glow", "Brilho (glow)", "The cutscenes' glowing layer.", "A camada brilhante das cenas.", &Tuning::glow, nullptr, 0, 1, 1,
             "SOTC_NATIVE_NOGLOW"},
            {"dof", "Depth of field", "Profundidade de campo", "Far things blurred in some scenes.", "O fundo desfocado em algumas cenas.",
             &Tuning::depthOfField, nullptr, 0, 1, 1, "SOTC_NATIVE_NODOF"},
            {"shadows", "Shadows", "Sombras", "Characters' and colossi's shadows.", "As sombras dos personagens e colossos.", &Tuning::shadows,
             nullptr, 0, 1, 1, "SOTC_NATIVE_NOSHADOWS"},
            {"particles", "Particles", "Partículas", "Dust, sparks, birds, grass.", "Poeira, faíscas, pássaros, grama.", &Tuning::particles,
             nullptr, 0, 1, 1, "SOTC_NATIVE_NOPTCL"},
            {"screen_blur", "Screen blur", "Borrão de tela", "The motion and impact blurs.", "Os borrões de movimento e de impacto.",
             &Tuning::screenBlur, nullptr, 0, 1, 1, "SOTC_NATIVE_NOSBLUR"},
            {"feedback", "Motion trail", "Rastro", "The last frame laid over the new one (the eagle's flight, the title).",
             "O quadro anterior por cima do novo (o voo da águia, a tela-título).", &Tuning::feedback, nullptr, 0, 1, 1, "SOTC_NATIVE_NOFEEDBACK"},
            {"reflections", "Reflections", "Reflexos", "Mirror reflections on water and floors.", "Reflexos na água e no chão.",
             &Tuning::reflections, nullptr, 0, 1, 1, "SOTC_NATIVE_NOREFLECT"},
            {"refraction", "Water refraction", "Refração da água", "The scene seen through water.", "O cenário visto através da água.",
             &Tuning::refraction, nullptr, 0, 1, 1, "SOTC_NATIVE_NOREFRACT"},
        };
        auto debugToggle = [&](const char *key, const char *label, const char *labelPt, const char *help, const char *helpPt, bool Tuning::*flag,
                               const char *env = nullptr)
        {
            TuningItem item{key, label, labelPt, help, helpPt, flag, nullptr, 0, 1, 1, env};
            item.debug = true;
            v.push_back(item);
        };
        debugToggle("dbg_2d", "Draw 2D", "Desenhar 2D", "Sprites, fades, menus, subtitles.", "Sprites, fades, menus, legendas.", &Tuning::draw2D);
        debugToggle("dbg_world", "Draw world", "Desenhar mundo", "Everything that is not 2D.", "Tudo que não é 2D.", &Tuning::drawWorld);
        debugToggle("dbg_textures", "Textures", "Texturas", "Off: every draw at its vertex colour, no texture.",
                    "Desligado: tudo na cor do vértice, sem textura.", &Tuning::textures);
        debugToggle("dbg_lighting", "Lighting", "Iluminação", "Off: lit models at their vertex colour.", "Desligado: modelos iluminados na cor do vértice.",
                    &Tuning::lighting);
        debugToggle("dbg_additive", "Additive draws", "Desenhos aditivos", "Draws that add light (glows, light shafts).",
                    "Desenhos que somam luz (brilhos, feixes).", &Tuning::additive);
        debugToggle("dbg_subtract", "Subtract draws", "Desenhos subtrativos", "Draws that darken by subtracting.", "Desenhos que escurecem subtraindo.",
                    &Tuning::subtract);
        debugToggle("dbg_overlays", "Second passes", "Segundas passadas", "A second pass drawn over the same geometry.",
                    "Uma segunda passada desenhada sobre a mesma geometria.", &Tuning::overlays);
        debugToggle("dbg_sky", "Sky image", "Imagem do céu", "The sky image laid behind the scene.", "A imagem do céu posta atrás da cena.", &Tuning::skyImage);
        debugToggle("dbg_mips", "Mipmaps", "Mipmaps", "Smaller textures far away.", "Texturas menores ao longe.", &Tuning::mipmaps, "SOTC_NATIVE_NOMIPS");
        debugToggle("dbg_scroll", "Texture scrolling", "Rolagem de textura", "Water and cloth textures that move.", "Texturas que se movem (água, tecido).",
                    &Tuning::textureScroll, "SOTC_NATIVE_NOROW");
        static char labels[32][2][24];
        for (int i = 0; i < 32; ++i)
        {
            std::snprintf(labels[i][0], sizeof(labels[i][0]), "Layer %d", i);
            std::snprintf(labels[i][1], sizeof(labels[i][1]), "Camada %d", i);
            TuningItem item{nullptr, labels[i][0], labels[i][1], "Draws of this display list layer (1 sky, 2-3 world, 8 glow, 12-15 mirror, 19 refraction, 29 trail).",
                            "Desenhos desta camada da lista de exibição (1 céu, 2-3 mundo, 8 glow, 12-15 espelho, 19 refração, 29 rastro).",
                            nullptr, nullptr, 0, 1, 1, "SOTC_HIDE_LAYERS"};
            static char keys[32][16];
            std::snprintf(keys[i], sizeof(keys[i]), "dbg_hide_layer%d", i); // saved 1 = hidden
            item.key = keys[i];
            item.mask = &Tuning::hiddenLayers;
            item.bit = i;
            item.debug = true;
            v.push_back(item);
        }
        return v;
        }();
        return items;
    }

    std::string tuningValueText(const TuningItem &item)
    {
        const Tuning &t = tuning();
        if (item.toggle)
            return t.*item.toggle ? "On" : "Off";
        if (item.mask)
            return ((t.*item.mask >> item.bit) & 1u) ? "Off" : "On";
        char b[32];
        const float v = t.*item.number;
        if (item.number == &Tuning::renderScale)
            std::snprintf(b, sizeof(b), "%gx (%dx%d)", v, static_cast<int>(512 * v + 0.5f), static_cast<int>(448 * v + 0.5f));
        else
            std::snprintf(b, sizeof(b), item.step >= 1.0f ? "%.0f" : "%.2f", v);
        return b;
    }

    void tuningStep(const TuningItem &item, int dir)
    {
        Tuning &t = tuning();
        if (item.toggle)
        {
            t.*item.toggle = !(t.*item.toggle);
            return;
        }
        if (item.mask)
        {
            t.*item.mask ^= 1u << item.bit;
            return;
        }
        float &v = t.*item.number;
        const float n = std::round((v + item.step * static_cast<float>(dir > 0 ? 1 : -1)) / item.step) * item.step;
        v = std::clamp(n, item.min, item.max);
    }

    bool tuningRestore(const char *key, float value)
    {
        for (const TuningItem &item : tuningItems())
        {
            if (std::strcmp(item.key, key) != 0)
                continue;
            if (item.env && std::getenv(item.env))
                return false;
            if (item.toggle)
                tuning().*item.toggle = value != 0.0f;
            else if (item.mask)
                tuning().*item.mask = (tuning().*item.mask & ~(1u << item.bit)) | (value != 0.0f ? 1u << item.bit : 0u);
            else
                tuning().*item.number = std::clamp(value, item.min, item.max);
            return true;
        }
        return false;
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
        // The menus' Debug group: what is drawn at all.
        const Tuning &t = tuning();
        if (((t.hiddenLayers >> (state.order & 31u)) & 1u) || (state.screen ? !t.draw2D : !t.drawWorld) || (state.overlay && !t.overlays) ||
            (state.blend == Blend::Additive && !t.additive) || (state.blend == Blend::Subtract && !t.subtract))
            return;
        if (!t.textures || !t.lighting)
        {
            DrawState s = state;
            if (!t.textures)
                s.texture = 0;
            if (!t.lighting)
                s.lit = false;
            if (backend())
                g_backend->draw(mesh, clip, s);
            return;
        }
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
