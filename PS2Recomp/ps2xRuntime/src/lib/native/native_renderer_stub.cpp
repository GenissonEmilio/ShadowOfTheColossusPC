// Platforms without a native renderer yet: nothing is drawn, the emulated
// picture stays on screen.
#if !defined(PS2X_HAVE_METAL)

#include "runtime/native_renderer.h"

namespace native_renderer
{
    bool available() { return false; }
    bool hasMesh(uint64_t) { return false; }
    void uploadMesh(uint64_t, const Vertex *, uint32_t) {}
    void beginFrame() {}
    bool hasTexture(uint64_t) { return false; }
    void uploadTexture(uint64_t, uint32_t, uint32_t, const uint8_t *) {}
    void draw(uint64_t, const float[16], const DrawState &) {}
    void setFog(const Fog &) {}
    void endFrame(uint32_t, uint32_t) {}
    void setShown(bool) {}
    bool shown() { return false; }
    bool latestFrame(std::vector<uint8_t> &, uint32_t &, uint32_t &) { return false; }
}

#endif
