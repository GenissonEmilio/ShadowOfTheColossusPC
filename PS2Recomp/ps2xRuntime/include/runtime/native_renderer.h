#pragma once

// Native renderer: a game override hands it meshes once and, every frame, a
// list of draws (mesh + matrix). It draws them on the host GPU (Metal on
// macOS), with no PS2 hardware in between. The picture can replace the
// emulated one on screen (native_renderer::setShown).

#include <cstdint>
#include <vector>

namespace native_renderer
{
    struct Vertex
    {
        float x, y, z;    // model space
        float nx, ny, nz; // normal (lit draws), model space
        float u, v;       // texture coordinates (repeat)
        uint8_t r, g, b, a; // 0x80 = 1.0, as the PS2's
    };

    bool available();

    bool hasMesh(uint64_t id);
    // A triangle list, kept on the GPU under `id` until the renderer goes.
    void uploadMesh(uint64_t id, const Vertex *vertices, uint32_t count);

    bool hasTexture(uint64_t id);
    // RGBA8 (alpha 0x80 = opaque, as the PS2's), kept on the GPU under `id`.
    void uploadTexture(uint64_t id, uint32_t width, uint32_t height, const uint8_t *rgba);

    // Draws of one frame: `clip` (column-major) takes model space to clip
    // space with x, y and w as the game's projection gives them (x/w, y/w in
    // -1..1, y up); depth comes from w.
    void beginFrame();
    enum class Blend : uint8_t
    {
        Normal,   // Cs x As + Cd x (1 - As), alpha = texture x vertex (0x80 = 1)
        Additive, // Cs x As + Cd
        Opaque,   // no blending (alpha still cuts out)
        Subtract, // Cd - Cs x As
    };
    struct DrawState
    {
        uint64_t texture = 0; // 0: none
        Blend blend = Blend::Normal;
        uint32_t order = 0;   // draws go in this order (stable), like the game's display list layers
        // Lit draws (the VU1's lit microcode): d = max(0, L x (n, 1)), with
        // L the columns light[0..15]; colour = C x (d.xyz, 1), C the columns
        // light[16..31] (three light colours, then ambient); it scales the
        // vertex colour.
        float opacity = 1.0f; // scales the alpha
        bool background = false; // also drawn into the background image (what distance fades to)
        bool fogged = false;     // fades to the background image with distance (setFog)
        bool writeDepth = false; // write depth even when additive (a second pass follows)
        bool overlay = false; // a second pass over the same geometry: depth equal passes, no depth write
        bool screen = false; // 2D: positions already in clip space, no depth test or write
        uint32_t debugColour = 0; // non-zero: drawn flat in this RGB (who drew what)
        bool lit = false;
        float light[32] = {};
    };
    void draw(uint64_t mesh, const float clip[16], const DrawState &state);
    // Distance fog of this frame: fogged draws blend towards the background
    // image by clamp((w - start) / width, 0, 1), in `steps` steps (w: depth).
    struct Fog
    {
        bool enabled = false;
        float start = 0.0f, width = 1.0f;
        int steps = 0; // 0: smooth
        // The GS's per-vertex fog on 3D draws: F = clamp(base + scale / w,
        // 0, 255); colour = mix(fogColour, colour, F / 255).
        bool vertexFog = false;
        float base = 255.0f, scale = 0.0f;
        float colour[3] = {0, 0, 0}; // 0..1
    };
    void setFog(const Fog &fog);

    // SotC's bloom: a mask of what lies beyond `threshold` (GS Z: zA + zB / w;
    // empty pixels are 0), in `maskColor`, blurred at a quarter of the
    // resolution, lerped with the last frame's by `temporal` and added over
    // the 3D (draws before `order`) in `color` (0x80 = 1, alpha the amount).
    struct Bloom
    {
        bool enabled = false;
        uint32_t order = 20;
        float threshold = 0.0f, zA = 0.0f, zB = 0.0f;
        float maskColor[3] = {1, 1, 1};
        float color[4] = {0, 0, 0, 0};
        float temporal = 1.0f;
    };
    void setBloom(const Bloom &bloom);
    void endFrame(uint32_t width, uint32_t height);

    // Shown instead of the emulated picture (the emulator's present path asks).
    void setShown(bool shown);
    bool shown();
    // The last finished frame (RGBA8); false before the first.
    bool latestFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height);
}
