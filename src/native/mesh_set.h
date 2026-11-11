#pragma once
// Frostbite MeshSet resources as Skate stores them, read as a template and rewritten in place with new
// geometry. The resource keeps its size and layout (pointers and the relocation table never move);
// only counts, offsets and bounds change, and each LOD gets a new geometry chunk. Layout notes are in
// docs/discovery/mesh-replace.md.
#include "Engine/Resource/binary_io.h"
#include "native/gltf.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace studio::native
{

    struct VertexElement
    {
        std::uint8_t usage = 0;  // 1 position, 2/3 bone indices, 4/5 bone weights, 0x21.. UV0.., 0x34 tangent frame
        std::uint8_t format = 0; // 3 Float3, 8 Half4, 6 Half2, 0x0D UByte4N, 0x17 UShort4, ...
        std::uint8_t offset = 0; // within the stream's vertex
        std::uint8_t stream = 0;
        std::uint8_t size = 0; // bytes
    };

    struct MeshSection
    {
        std::size_t at = 0; // offset of the section record in the resource
        std::string name;   // material section name, e.g. Truck_Mat
        std::uint32_t triangles = 0, start_index = 0, vertex_offset = 0, vertices = 0;
        std::uint32_t vertex_stride = 0;
        std::uint16_t bones = 0;
        std::vector<VertexElement> elements;
        std::vector<std::uint8_t> stream_strides; // each stream's vertices are stored one after another
    };

    struct MeshLod
    {
        std::size_t at = 0;
        std::uint32_t index_bytes = 0, vertex_bytes = 0;
        std::uint32_t index_size = 2; // bytes per index
        dingosdk::frostbite::Guid chunk;
        std::vector<MeshSection> sections;
    };

    struct MeshSet
    {
        std::vector<std::byte> res;
        std::vector<MeshLod> lods;
        std::array<float, 3> box_min{}, box_max{};
    };

    // Throws std::runtime_error when the bytes do not look like a MeshSet this code understands.
    MeshSet parse_mesh_set(std::vector<std::byte> res);

    // Geometry for one section, in game space.
    struct SectionGeometry
    {
        std::vector<Vec3> positions, normals;
        std::vector<Vec2> uv0, uv1;
        std::vector<std::uint32_t> indices; // relative to this section's first vertex
        bool empty() const
        {
            return indices.empty();
        }
    };

    // Decodes renderable geometry from a MeshSet LOD chunk into one glTF-style section per material section.
    // Bone weights are intentionally omitted; positions and UV0 are preserved, with normals reconstructed.
    std::vector<SectionGeometry> read_lod_geometry(const MeshSet &mesh,
                                                  std::size_t lod,
                                                  const std::vector<std::byte> &chunk);

    struct LodBuild
    {
        std::vector<std::byte> chunk; // the LOD's new geometry chunk
        std::vector<std::string> warnings;
    };

    struct BuildOptions
    {
        bool rigid = false; // every vertex takes its section's most common skinning instead of the nearest vertex's
    };

    // Encodes `geometry` (one entry per section of the LOD, empty entries make empty sections) into a new
    // chunk for LOD `lod`, writing counts, offsets and section bounds into mesh.res. `original_chunk` is
    // the LOD's current chunk: skinning and any vertex data this code does not generate are copied from
    // the nearest original vertex of the same section.
    LodBuild build_lod(MeshSet &mesh,
                       std::size_t lod,
                       const std::vector<std::byte> &original_chunk,
                       const std::vector<SectionGeometry> &geometry,
                       const BuildOptions &options);

    // Writes the mesh bounds (and every part box) from the box of all geometry written.
    void write_mesh_bounds(MeshSet &mesh, const std::array<float, 3> &lo, const std::array<float, 3> &hi);

    // The packed tangent frame (usage 0x34) for a unit normal, unit tangent and handedness.
    std::uint32_t encode_tangent_frame(const Vec3 &normal, const Vec3 &tangent, bool flipped_bitangent);

    // IEEE half from float (round to nearest).
    std::uint16_t to_half(float value);
    float from_half(std::uint16_t value);

} // namespace studio::native
