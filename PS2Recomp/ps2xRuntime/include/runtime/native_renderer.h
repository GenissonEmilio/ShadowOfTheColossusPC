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
        uint8_t r, g, b, a;
    };

    bool available();

    bool hasMesh(uint64_t id);
    // A triangle list, kept on the GPU under `id` until the renderer goes.
    void uploadMesh(uint64_t id, const Vertex *vertices, uint32_t count);

    // Draws of one frame: `clip` (column-major) takes model space to clip
    // space with x, y and w as the game's projection gives them (x/w, y/w in
    // -1..1, y up); depth comes from w.
    void beginFrame();
    void draw(uint64_t mesh, const float clip[16]);
    void endFrame(uint32_t width, uint32_t height);

    // Shown instead of the emulated picture (the emulator's present path asks).
    void setShown(bool shown);
    bool shown();
    // The last finished frame (RGBA8); false before the first.
    bool latestFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height);
}
