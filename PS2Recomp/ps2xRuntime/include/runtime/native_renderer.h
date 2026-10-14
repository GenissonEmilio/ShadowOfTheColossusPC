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
        float nx, ny, nz; // face normal, model space
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
    };
    struct DrawState
    {
        uint64_t texture = 0; // 0: none
        Blend blend = Blend::Normal;
        uint32_t order = 0;   // draws go in this order (stable), like the game's display list layers
    };
    void draw(uint64_t mesh, const float clip[16], const DrawState &state);
    void endFrame(uint32_t width, uint32_t height);

    // Shown instead of the emulated picture (the emulator's present path asks).
    void setShown(bool shown);
    bool shown();
    // The last finished frame (RGBA8); false before the first.
    bool latestFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height);
}
