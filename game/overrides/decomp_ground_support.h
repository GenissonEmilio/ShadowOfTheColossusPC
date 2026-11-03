#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

class PS2Runtime;

namespace sotc_ground_support {

struct Vec3 { float x, y, z; };
struct Triangle {
    std::array<uint32_t, 3> vertex;
    // Edge e joins vertex[e] and vertex[(e+1)%3]. Negative means unsafe:
    // -1 is an open edge, -2 is a nonmanifold edge.
    std::array<int32_t, 3> neighbor;
    // Equality key for the complete canonical material record, not its table
    // index. UINT32_MAX marks a conditional/unknown collision classifier.
    uint32_t attribute;
};
struct MeshView {
    const Vec3* vertices;
    size_t vertexCount;
    const Triangle* triangles;
    size_t triangleCount;
};

// Orientation only. No height, vertex, collision reference, or mesh is changed.
// The input adapter must establish that this is a static rigid layout mesh.
bool resolveSupportPlane(const MeshView& mesh, uint32_t face, Vec3 contact,
                         float radius, std::array<float, 4>& plane);

// Install after the optional decomp/native registry has selected its callees.
void installGroundSupport(PS2Runtime& runtime);

} // namespace sotc_ground_support
