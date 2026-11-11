#pragma once
// A small glTF 2.0 binary (.glb) reader: triangle primitives with positions, normals and UVs, in
// world space (node transforms applied), grouped by material name. Enough for mesh replace.
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace studio::native
{

    using Vec2 = std::array<float, 2>;
    using Vec3 = std::array<float, 3>;

    struct GltfPrimitive
    {
        std::string material; // material name, "" when the primitive has none
        std::vector<Vec3> positions;
        std::vector<Vec3> normals;          // filled from the faces when the file has none
        std::vector<Vec2> uv0, uv1;         // uv1 empty when the file has no TEXCOORD_1
        std::vector<std::uint32_t> indices; // triangle list
    };

    struct GltfModel
    {
        std::vector<GltfPrimitive> primitives;
        std::size_t vertices() const;
        std::size_t triangles() const;
    };

    // Throws std::runtime_error with a readable message on anything it cannot read.
    GltfModel load_glb(const std::filesystem::path &path);

    // Copies the .glb at `from` to `to` with one material called `name` on every primitive, and every mesh and mesh
    // node named `name` too, so an importer that routes by material name puts all of it in one section. The geometry
    // is not touched (textures are dropped with the materials). Returns the material names it replaced. Throws
    // std::runtime_error like load_glb.
    std::vector<std::string> write_glb_one_material(const std::filesystem::path &from,
                                                    const std::filesystem::path &to,
                                                    const std::string &name);

} // namespace studio::native
