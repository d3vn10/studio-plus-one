#include "native/mesh_set.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace studio::native
{
    namespace
    {
        constexpr std::size_t section_size = 0x180;
        constexpr double pi = 3.14159265358979323846;

        template <class T> T get(const std::vector<std::byte> &b, std::size_t at)
        {
            if (at + sizeof(T) > b.size())
                throw std::runtime_error("MeshSet field out of range");
            T v;
            std::memcpy(&v, b.data() + at, sizeof(T));
            return v;
        }
        template <class T> void put(std::vector<std::byte> &b, std::size_t at, T v)
        {
            if (at + sizeof(T) > b.size())
                throw std::runtime_error("MeshSet field out of range");
            std::memcpy(b.data() + at, &v, sizeof(T));
        }

        std::size_t pointer(const std::vector<std::byte> &b, std::size_t at)
        {
            const auto p = get<std::uint64_t>(b, at) + 0x10;
            if (p >= b.size())
                throw std::runtime_error("MeshSet pointer out of range");
            return static_cast<std::size_t>(p);
        }

        std::uint8_t format_size(std::uint8_t format)
        {
            switch (format)
            {
            case 1:
                return 4;
            case 2:
                return 8;
            case 3:
                return 12;
            case 4:
                return 16;
            case 5:
                return 2;
            case 6:
                return 4;
            case 7:
                return 6;
            case 8:
                return 8;
            case 0x0C:
            case 0x0D:
            case 0x0B:
            case 0x0A:
                return 4;
            case 0x17:
                return 8;
            default:
                return 0;
            }
        }

        bool is_uv(std::uint8_t usage)
        {
            return usage >= 0x21 && usage <= 0x28;
        }

        // Byte offset of `stream` inside a section's vertex data.
        std::size_t stream_base(const MeshSection &s, std::size_t vertices, std::uint8_t stream)
        {
            std::size_t at = 0;
            for (std::uint8_t k = 0; k < stream && k < s.stream_strides.size(); ++k)
                at += vertices * s.stream_strides[k];
            return at;
        }

        Vec3 read_position(const std::vector<std::byte> &chunk, std::size_t at, std::uint8_t format)
        {
            Vec3 p{0, 0, 0};
            if (format == 8 || format == 7)
            {
                for (int k = 0; k < 3; ++k)
                    p[static_cast<size_t>(k)] = from_half(get<std::uint16_t>(chunk, at + static_cast<size_t>(k) * 2));
            }
            else if (format == 3 || format == 4)
            {
                for (int k = 0; k < 3; ++k)
                    p[static_cast<size_t>(k)] = get<float>(chunk, at + static_cast<size_t>(k) * 4);
            }
            return p;
        }

        void write_floats(std::byte *out, std::uint8_t format, const float *v, int n, float w_default)
        {
            float all[4] = {0, 0, 0, w_default};
            for (int i = 0; i < n && i < 4; ++i)
                all[i] = v[i];
            switch (format)
            {
            case 1:
            case 2:
            case 3:
            case 4:
            {
                const int count = format;
                std::memcpy(out, all, static_cast<size_t>(count) * 4);
                break;
            }
            case 5:
            case 6:
            case 7:
            case 8:
            {
                const int count = format - 4;
                for (int i = 0; i < count; ++i)
                {
                    const std::uint16_t h = to_half(all[i]);
                    std::memcpy(out + i * 2, &h, 2);
                }
                break;
            }
            default:
                break;
            }
        }

        // Uniform grid over a point set, for nearest-vertex lookups.
        class Grid
        {
          public:
            explicit Grid(const std::vector<Vec3> &points) : points_(points)
            {
                if (points.empty())
                    return;
                lo_ = hi_ = points[0];
                for (const auto &p : points)
                    for (int k = 0; k < 3; ++k)
                    {
                        lo_[static_cast<size_t>(k)] = std::min(lo_[static_cast<size_t>(k)], p[static_cast<size_t>(k)]);
                        hi_[static_cast<size_t>(k)] = std::max(hi_[static_cast<size_t>(k)], p[static_cast<size_t>(k)]);
                    }
                const float extent = std::max({hi_[0] - lo_[0], hi_[1] - lo_[1], hi_[2] - lo_[2], 1e-4f});
                cell_ = extent / std::max(1.0f, std::cbrt(static_cast<float>(points.size()) / 2.0f));
                for (std::size_t i = 0; i < points.size(); ++i)
                    cells_[key(cell_of(points[i]))].push_back(i);
            }
            std::size_t nearest(const Vec3 &p) const
            {
                if (points_.empty())
                    return 0;
                const auto c = cell_of(p);
                std::size_t best = 0;
                float best_d = std::numeric_limits<float>::max();
                for (int r = 0; r < 64; ++r)
                {
                    for (int dx = -r; dx <= r; ++dx)
                        for (int dy = -r; dy <= r; ++dy)
                            for (int dz = -r; dz <= r; ++dz)
                            {
                                if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != r)
                                    continue;
                                const auto it = cells_.find(key({c[0] + dx, c[1] + dy, c[2] + dz}));
                                if (it == cells_.end())
                                    continue;
                                for (auto i : it->second)
                                {
                                    const auto &q = points_[i];
                                    const float d = (q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) +
                                                    (q[2] - p[2]) * (q[2] - p[2]);
                                    if (d < best_d)
                                    {
                                        best_d = d;
                                        best = i;
                                    }
                                }
                            }
                    // Anything found within r cells is the nearest once the next ring cannot be closer.
                    if (best_d < std::numeric_limits<float>::max() &&
                        std::sqrt(best_d) <= static_cast<float>(r) * cell_)
                        break;
                }
                if (best_d == std::numeric_limits<float>::max())
                { // far outside the grid: brute force
                    for (std::size_t i = 0; i < points_.size(); ++i)
                    {
                        const auto &q = points_[i];
                        const float d = (q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) +
                                        (q[2] - p[2]) * (q[2] - p[2]);
                        if (d < best_d)
                        {
                            best_d = d;
                            best = i;
                        }
                    }
                }
                return best;
            }

          private:
            std::array<int, 3> cell_of(const Vec3 &p) const
            {
                return {static_cast<int>(std::floor((p[0] - lo_[0]) / cell_)),
                        static_cast<int>(std::floor((p[1] - lo_[1]) / cell_)),
                        static_cast<int>(std::floor((p[2] - lo_[2]) / cell_))};
            }
            static std::int64_t key(const std::array<int, 3> &c)
            {
                return (static_cast<std::int64_t>(c[0] + 1048576) << 42) |
                       (static_cast<std::int64_t>(c[1] + 1048576) << 21) | static_cast<std::int64_t>(c[2] + 1048576);
            }
            const std::vector<Vec3> &points_;
            Vec3 lo_{}, hi_{};
            float cell_ = 1;
            std::unordered_map<std::int64_t, std::vector<std::size_t>> cells_;
        };

        Vec3 sub(const Vec3 &a, const Vec3 &b)
        {
            return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        }
        Vec3 cross(const Vec3 &a, const Vec3 &b)
        {
            return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        }
        float dot(const Vec3 &a, const Vec3 &b)
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }
        Vec3 normalized(Vec3 v, const Vec3 &fallback)
        {
            const float l = std::sqrt(dot(v, v));
            if (l < 1e-12f)
                return fallback;
            return {v[0] / l, v[1] / l, v[2] / l};
        }
        Vec3 any_perpendicular(const Vec3 &n)
        {
            const Vec3 axis = std::abs(n[0]) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
            return normalized(cross(cross(n, axis), n), Vec3{1, 0, 0});
        }
    } // namespace

    std::uint16_t to_half(float value)
    {
        std::uint32_t f;
        std::memcpy(&f, &value, 4);
        const std::uint32_t sign = (f >> 16) & 0x8000;
        const int exponent = static_cast<int>((f >> 23) & 0xFF) - 127 + 15;
        std::uint32_t mantissa = f & 0x7FFFFF;
        if (((f >> 23) & 0xFF) == 0xFF)
            return static_cast<std::uint16_t>(sign | 0x7C00 | (mantissa ? 0x200 : 0));
        if (exponent >= 31)
            return static_cast<std::uint16_t>(sign | 0x7C00);
        if (exponent <= 0)
        {
            if (exponent < -10)
                return static_cast<std::uint16_t>(sign);
            mantissa |= 0x800000;
            const int shift = 14 - exponent;
            std::uint32_t half = mantissa >> shift;
            if ((mantissa >> (shift - 1)) & 1)
                ++half;
            return static_cast<std::uint16_t>(sign | half);
        }
        std::uint32_t half = sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
        if (mantissa & 0x1000)
            ++half; // round to nearest (carries into the exponent correctly)
        return static_cast<std::uint16_t>(half);
    }

    float from_half(std::uint16_t h)
    {
        const std::uint32_t sign = (h & 0x8000u) << 16;
        std::uint32_t exponent = (h >> 10) & 0x1F;
        std::uint32_t mantissa = h & 0x3FF;
        std::uint32_t f;
        if (exponent == 0)
        {
            if (mantissa == 0)
            {
                f = sign;
            }
            else
            {
                exponent = 127 - 15 + 1;
                while (!(mantissa & 0x400))
                {
                    mantissa <<= 1;
                    --exponent;
                }
                mantissa &= 0x3FF;
                f = sign | (exponent << 23) | (mantissa << 13);
            }
        }
        else if (exponent == 31)
        {
            f = sign | 0x7F800000 | (mantissa << 13);
        }
        else
        {
            f = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
        }
        float v;
        std::memcpy(&v, &f, 4);
        return v;
    }

    std::uint32_t encode_tangent_frame(const Vec3 &n, const Vec3 &t, bool flipped)
    {
        // Face: the normal's largest axis and its sign. The two other components, scaled by sqrt(2) x 255,
        // are 9-bit values around 256; the tangent is a 10-bit angle from a per-face reference axis.
        static const int uv_axes[6][2] = {{2, 1}, {2, 1}, {0, 2}, {0, 2}, {0, 1}, {0, 1}};
        static const Vec3 refs[6] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}, {1, 0, 0}};
        int axis = 0;
        for (int k = 1; k < 3; ++k)
            if (std::abs(n[static_cast<size_t>(k)]) > std::abs(n[static_cast<size_t>(axis)]))
                axis = k;
        const int face = axis * 2 + (n[static_cast<size_t>(axis)] < 0 ? 1 : 0);
        auto quant = [](float c)
        {
            const long q = std::lround(static_cast<double>(c) * std::sqrt(2.0) * 255.0);
            return static_cast<std::uint32_t>(std::clamp(q, -256L, 255L) + 256);
        };
        const std::uint32_t u = quant(n[static_cast<size_t>(uv_axes[face][0])]);
        const std::uint32_t v = quant(n[static_cast<size_t>(uv_axes[face][1])]);
        const Vec3 &r = refs[face];
        const float nr = dot(n, r);
        const Vec3 rr = normalized({r[0] - n[0] * nr, r[1] - n[1] * nr, r[2] - n[2] * nr}, any_perpendicular(n));
        const Vec3 c = cross(n, rr);
        const double angle = std::atan2(static_cast<double>(dot(t, c)), static_cast<double>(dot(t, rr)));
        const std::uint32_t a =
            static_cast<std::uint32_t>(((std::lround(angle * 1024.0 / (2.0 * pi)) % 1024) + 1024) % 1024);
        return (u & 0xFF) | ((v & 0xFF) << 8) | ((a & 0xFF) << 16) | (static_cast<std::uint32_t>(face) << 24) |
               (flipped ? 1u << 27 : 0u) | ((u >> 8) << 28) | ((v >> 8) << 29) | ((a >> 8) << 30);
    }

    MeshSet parse_mesh_set(std::vector<std::byte> res)
    {
        MeshSet m;
        m.res = std::move(res);
        const auto &b = m.res;
        if (b.size() < 0x200)
            throw std::runtime_error("resource is too small to be a MeshSet");
        for (int k = 0; k < 3; ++k)
        {
            m.box_min[static_cast<size_t>(k)] = get<float>(b, 0x10 + static_cast<size_t>(k) * 4);
            m.box_max[static_cast<size_t>(k)] = get<float>(b, 0x20 + static_cast<size_t>(k) * 4);
        }
        const auto lod_count = get<std::uint16_t>(b, 0xB4);
        if (lod_count == 0 || lod_count > 8)
            throw std::runtime_error("MeshSet LOD count " + std::to_string(lod_count) + " is not supported");
        for (std::size_t i = 0; i < lod_count; ++i)
        {
            MeshLod lod;
            lod.at = pointer(b, 0x30 + i * 8);
            const auto count = get<std::uint32_t>(b, lod.at + 0x08);
            const std::size_t first = get<std::uint32_t>(b, lod.at + 0x0C) + 0x10;
            if (count == 0 || count > 64 || first + count * section_size > b.size())
                throw std::runtime_error("MeshSet LOD " + std::to_string(i) + " has an unreadable section table");
            lod.index_bytes = get<std::uint32_t>(b, lod.at + 0x58);
            lod.vertex_bytes = get<std::uint32_t>(b, lod.at + 0x5C);
            std::memcpy(lod.chunk.bytes.data(), b.data() + lod.at + 0x74, 16);
            std::uint64_t indices = 0;
            for (std::size_t s = 0; s < count; ++s)
            {
                MeshSection sec;
                sec.at = first + s * section_size;
                const std::size_t name_at = pointer(b, sec.at + 0x08);
                for (std::size_t k = name_at; k < b.size() && b[k] != std::byte{0}; ++k)
                    sec.name += static_cast<char>(b[k]);
                sec.bones = get<std::uint16_t>(b, sec.at + 0x18);
                sec.vertex_stride = static_cast<std::uint32_t>(get<std::uint8_t>(b, sec.at + 0x1E));
                sec.triangles = get<std::uint32_t>(b, sec.at + 0x20);
                sec.start_index = get<std::uint32_t>(b, sec.at + 0x24);
                sec.vertex_offset = get<std::uint32_t>(b, sec.at + 0x28);
                sec.vertices = get<std::uint32_t>(b, sec.at + 0x2C);
                const std::size_t decl = sec.at + 0x70;
                const auto elements = get<std::uint8_t>(b, decl + 0x60);
                const auto streams = get<std::uint8_t>(b, decl + 0x61);
                if (elements > 16 || streams > 16)
                    throw std::runtime_error("MeshSet section " + sec.name + " has an unreadable vertex layout");
                for (std::uint8_t k = 0; k < streams; ++k)
                    sec.stream_strides.push_back(get<std::uint8_t>(b, decl + 0x40 + k * 2u));
                std::uint32_t total = 0;
                for (auto st : sec.stream_strides)
                    total += st;
                if (total != sec.vertex_stride)
                    throw std::runtime_error("MeshSet section " + sec.name + ": stream strides do not add up");
                for (std::uint8_t k = 0; k < elements; ++k)
                {
                    VertexElement e;
                    e.usage = get<std::uint8_t>(b, decl + k * 4u);
                    e.format = get<std::uint8_t>(b, decl + k * 4u + 1);
                    e.offset = get<std::uint8_t>(b, decl + k * 4u + 2);
                    e.stream = get<std::uint8_t>(b, decl + k * 4u + 3);
                    if (e.stream >= sec.stream_strides.size())
                        throw std::runtime_error("MeshSet section " + sec.name + ": element in a missing stream");
                    e.size = format_size(e.format);
                    if (!e.size)
                        e.size = static_cast<std::uint8_t>(sec.stream_strides[e.stream] - e.offset);
                    if (e.offset + e.size > sec.stream_strides[e.stream])
                        throw std::runtime_error("MeshSet section " + sec.name + ": element does not fit its stream");
                    sec.elements.push_back(e);
                }
                indices += static_cast<std::uint64_t>(sec.triangles) * 3;
                lod.sections.push_back(std::move(sec));
            }
            if (indices)
                lod.index_size = static_cast<std::uint32_t>(lod.index_bytes / indices);
            if (lod.index_size != 2 && lod.index_size != 4)
                throw std::runtime_error("MeshSet LOD " + std::to_string(i) + " uses " +
                                         std::to_string(lod.index_size) + "-byte indices");
            m.lods.push_back(std::move(lod));
        }
        return m;
    }

    std::vector<SectionGeometry> read_lod_geometry(const MeshSet &mesh,
                                                   std::size_t lod_index,
                                                   const std::vector<std::byte> &chunk)
    {
        const auto &lod = mesh.lods.at(lod_index);
        if (lod.vertex_bytes > chunk.size() || lod.index_bytes > chunk.size() - lod.vertex_bytes)
            throw std::runtime_error("MeshSet geometry chunk is shorter than its LOD sizes");
        const std::size_t indices_start = lod.vertex_bytes;
        std::vector<SectionGeometry> result(lod.sections.size());
        for (std::size_t section_index = 0; section_index < lod.sections.size(); ++section_index)
        {
            const auto &section = lod.sections[section_index];
            auto &geometry = result[section_index];
            const auto *position = static_cast<const VertexElement *>(nullptr);
            const auto *uv = static_cast<const VertexElement *>(nullptr);
            for (const auto &element : section.elements)
            {
                if (element.usage == 1 && !position)
                    position = &element;
                else if (element.usage == 0x21 && !uv)
                    uv = &element;
            }
            if (!position)
                throw std::runtime_error("MeshSet section " + section.name + " has no position element");
            if (section.vertices == 0 || section.triangles == 0)
                continue;
            const auto vertex_bytes =
                static_cast<std::uint64_t>(section.vertices) * section.vertex_stride;
            if (section.vertex_offset > lod.vertex_bytes || vertex_bytes > lod.vertex_bytes - section.vertex_offset)
                throw std::runtime_error("MeshSet section " + section.name + " vertices run past the LOD buffer");
            const auto index_count = static_cast<std::uint64_t>(section.triangles) * 3;
            const auto index_offset = static_cast<std::uint64_t>(section.start_index) * lod.index_size;
            const auto index_bytes = index_count * lod.index_size;
            if (index_offset > lod.index_bytes || index_bytes > lod.index_bytes - index_offset)
                throw std::runtime_error("MeshSet section " + section.name + " indices run past the LOD buffer");

            geometry.positions.reserve(section.vertices);
            if (uv && (uv->format == 2 || uv->format == 6))
                geometry.uv0.reserve(section.vertices);
            for (std::size_t vertex = 0; vertex < section.vertices; ++vertex)
            {
                const auto vertex_base = static_cast<std::size_t>(section.vertex_offset) +
                                         stream_base(section, section.vertices, position->stream) +
                                         vertex * section.stream_strides[position->stream] + position->offset;
                if (position->format == 3 || position->format == 4)
                    geometry.positions.push_back({get<float>(chunk, vertex_base),
                                                  get<float>(chunk, vertex_base + 4),
                                                  get<float>(chunk, vertex_base + 8)});
                else if (position->format == 7 || position->format == 8)
                    geometry.positions.push_back({from_half(get<std::uint16_t>(chunk, vertex_base)),
                                                  from_half(get<std::uint16_t>(chunk, vertex_base + 2)),
                                                  from_half(get<std::uint16_t>(chunk, vertex_base + 4))});
                else
                    throw std::runtime_error("MeshSet section " + section.name + " uses an unsupported position format");

                if (!uv || (uv->format != 2 && uv->format != 6))
                    continue;
                const auto uv_base = static_cast<std::size_t>(section.vertex_offset) +
                                     stream_base(section, section.vertices, uv->stream) +
                                     vertex * section.stream_strides[uv->stream] + uv->offset;
                if (uv->format == 2)
                    geometry.uv0.push_back({get<float>(chunk, uv_base), get<float>(chunk, uv_base + 4)});
                else if (uv->format == 6)
                    geometry.uv0.push_back({from_half(get<std::uint16_t>(chunk, uv_base)),
                                            from_half(get<std::uint16_t>(chunk, uv_base + 2))});
            }

            geometry.indices.reserve(static_cast<std::size_t>(index_count));
            const auto index_data = indices_start + static_cast<std::size_t>(index_offset);
            for (std::size_t index = 0; index < index_count; ++index)
            {
                const auto at = index_data + index * lod.index_size;
                const auto value = lod.index_size == 2 ? get<std::uint16_t>(chunk, at) : get<std::uint32_t>(chunk, at);
                if (value >= section.vertices)
                    throw std::runtime_error("MeshSet section " + section.name + " has an invalid vertex index");
                geometry.indices.push_back(value);
            }

            geometry.normals.assign(section.vertices, Vec3{0, 0, 0});
            for (std::size_t index = 0; index < geometry.indices.size(); index += 3)
            {
                const auto a = geometry.indices[index], b = geometry.indices[index + 1], c = geometry.indices[index + 2];
                const auto edge1 = sub(geometry.positions[b], geometry.positions[a]);
                const auto edge2 = sub(geometry.positions[c], geometry.positions[a]);
                const auto normal = cross(edge1, edge2);
                for (const auto vertex : {a, b, c})
                    for (std::size_t axis = 0; axis < 3; ++axis)
                        geometry.normals[vertex][axis] += normal[axis];
            }
            for (auto &normal : geometry.normals)
                normal = normalized(normal, Vec3{0, 1, 0});
        }
        return result;
    }

    LodBuild build_lod(MeshSet &mesh,
                       std::size_t lod_index,
                       const std::vector<std::byte> &original,
                       const std::vector<SectionGeometry> &geometry,
                       const BuildOptions &options)
    {
        LodBuild out;
        MeshLod &lod = mesh.lods.at(lod_index);
        if (geometry.size() != lod.sections.size())
            throw std::runtime_error("one geometry entry per section is needed");
        std::vector<std::byte> vertex_data, index_data;
        std::uint32_t start_index = 0;
        for (std::size_t si = 0; si < lod.sections.size(); ++si)
        {
            MeshSection &sec = lod.sections[si];
            const SectionGeometry &g = geometry[si];
            const std::size_t n = g.empty() ? 0 : g.positions.size();
            if (n > (lod.index_size == 2 ? 65535u : 0xFFFFFFFFu))
                throw std::runtime_error("section " + sec.name + " would get " + std::to_string(n) +
                                         " vertices; at most 65535 fit one section of this mesh");

            // The original section's vertices, for skinning and anything not generated here.
            const std::size_t on = sec.vertices;
            std::vector<Vec3> original_positions;
            const VertexElement *pos_element = nullptr;
            for (const auto &e : sec.elements)
                if (e.usage == 1)
                    pos_element = &e;
            if (!pos_element)
                throw std::runtime_error("section " + sec.name + " has no positions");
            if (on && static_cast<std::size_t>(sec.vertex_offset) + on * sec.vertex_stride > original.size())
                throw std::runtime_error("the original geometry chunk is shorter than the mesh says");
            for (std::size_t i = 0; i < on; ++i)
                original_positions.push_back(
                    read_position(original,
                                  sec.vertex_offset + stream_base(sec, on, pos_element->stream) +
                                      i * sec.stream_strides[pos_element->stream] + pos_element->offset,
                                  pos_element->format));
            Grid grid(original_positions);
            // For rigid skinning: each copied element's most common value in the original section.
            std::vector<std::vector<std::byte>> common(sec.elements.size());
            for (std::size_t ei = 0; ei < sec.elements.size(); ++ei)
            {
                const auto &e = sec.elements[ei];
                std::map<std::vector<std::byte>, std::size_t> counts;
                for (std::size_t i = 0; i < on; ++i)
                {
                    const std::size_t at = sec.vertex_offset + stream_base(sec, on, e.stream) +
                                           i * sec.stream_strides[e.stream] + e.offset;
                    counts[std::vector<std::byte>(original.begin() + static_cast<std::ptrdiff_t>(at),
                                                  original.begin() + static_cast<std::ptrdiff_t>(at + e.size))]++;
                }
                std::size_t best = 0;
                for (const auto &[value, count] : counts)
                    if (count > best)
                    {
                        best = count;
                        common[ei] = value;
                    }
                if (common[ei].empty())
                {
                    common[ei].assign(e.size, std::byte{0});
                    if (e.usage == 4 && e.format == 0x0D)
                        common[ei][0] = std::byte{255}; // all weight on the first bone
                }
            }
            if (n && !on && !options.rigid)
                out.warnings.push_back("section " + sec.name +
                                       " was empty in the game mesh, so its vertices get default skinning");

            // Tangents from UV0, per vertex: T along +u, handedness from +v.
            std::vector<Vec3> tangents(n, Vec3{0, 0, 0}), bitangents(n, Vec3{0, 0, 0});
            for (std::size_t t = 0; t + 2 < g.indices.size(); t += 3)
            {
                const auto i0 = g.indices[t], i1 = g.indices[t + 1], i2 = g.indices[t + 2];
                const Vec3 e1 = sub(g.positions[i1], g.positions[i0]), e2 = sub(g.positions[i2], g.positions[i0]);
                const float du1 = g.uv0[i1][0] - g.uv0[i0][0], dv1 = g.uv0[i1][1] - g.uv0[i0][1];
                const float du2 = g.uv0[i2][0] - g.uv0[i0][0], dv2 = g.uv0[i2][1] - g.uv0[i0][1];
                const float det = du1 * dv2 - du2 * dv1;
                if (std::abs(det) < 1e-20f)
                    continue;
                const float r = 1.0f / det;
                const Vec3 tu = {
                    (e1[0] * dv2 - e2[0] * dv1) * r, (e1[1] * dv2 - e2[1] * dv1) * r, (e1[2] * dv2 - e2[2] * dv1) * r};
                const Vec3 tv = {
                    (e2[0] * du1 - e1[0] * du2) * r, (e2[1] * du1 - e1[1] * du2) * r, (e2[2] * du1 - e1[2] * du2) * r};
                for (auto i : {i0, i1, i2})
                    for (int k = 0; k < 3; ++k)
                    {
                        tangents[i][static_cast<size_t>(k)] += tu[static_cast<size_t>(k)];
                        bitangents[i][static_cast<size_t>(k)] += tv[static_cast<size_t>(k)];
                    }
            }

            const std::size_t section_start = vertex_data.size();
            vertex_data.resize(section_start + n * sec.vertex_stride);
            std::array<float, 3> lo{0, 0, 0}, hi{0, 0, 0};
            for (std::size_t i = 0; i < n; ++i)
            {
                const Vec3 &p = g.positions[i];
                if (i == 0)
                    lo = hi = p;
                for (int k = 0; k < 3; ++k)
                {
                    lo[static_cast<size_t>(k)] = std::min(lo[static_cast<size_t>(k)], p[static_cast<size_t>(k)]);
                    hi[static_cast<size_t>(k)] = std::max(hi[static_cast<size_t>(k)], p[static_cast<size_t>(k)]);
                }
                const Vec3 normal = normalized(g.normals[i], Vec3{0, 1, 0});
                const float tn = dot(tangents[i], normal);
                const Vec3 tangent = normalized(
                    {tangents[i][0] - normal[0] * tn, tangents[i][1] - normal[1] * tn, tangents[i][2] - normal[2] * tn},
                    any_perpendicular(normal));
                const bool flipped = dot(cross(normal, tangent), bitangents[i]) < 0;
                const std::size_t near = on ? grid.nearest(p) : 0;
                for (std::size_t ei = 0; ei < sec.elements.size(); ++ei)
                {
                    const auto &e = sec.elements[ei];
                    std::byte *dst = vertex_data.data() + section_start + stream_base(sec, n, e.stream) +
                                     i * sec.stream_strides[e.stream] + e.offset;
                    if (e.usage == 1 && format_size(e.format))
                    {
                        write_floats(dst, e.format, p.data(), 3, 1.0f);
                    }
                    else if (is_uv(e.usage) && (e.format == 6 || e.format == 2))
                    {
                        const bool second = e.usage != 0x21 && !g.uv1.empty();
                        const Vec2 &uv = second ? g.uv1[i] : g.uv0[i];
                        write_floats(dst, e.format, uv.data(), 2, 0.0f);
                    }
                    else if (e.usage == 0x34 && e.size == 4)
                    {
                        const std::uint32_t word = encode_tangent_frame(normal, tangent, flipped);
                        std::memcpy(dst, &word, 4);
                    }
                    else if (on && !options.rigid)
                    {
                        const std::size_t at = sec.vertex_offset + stream_base(sec, on, e.stream) +
                                               near * sec.stream_strides[e.stream] + e.offset;
                        std::memcpy(dst, original.data() + at, e.size);
                    }
                    else
                    {
                        std::memcpy(dst, common[ei].data(), e.size);
                    }
                }
            }
            for (auto idx : g.indices)
            {
                if (idx >= n)
                    throw std::runtime_error("section " + sec.name + ": an index points past its vertices");
                if (lod.index_size == 2)
                {
                    const auto v = static_cast<std::uint16_t>(idx);
                    const auto *bytes = reinterpret_cast<const std::byte *>(&v);
                    index_data.insert(index_data.end(), bytes, bytes + 2);
                }
                else
                {
                    const auto *bytes = reinterpret_cast<const std::byte *>(&idx);
                    index_data.insert(index_data.end(), bytes, bytes + 4);
                }
            }

            const auto triangles = static_cast<std::uint32_t>(g.indices.size() / 3);
            sec.triangles = triangles;
            sec.vertices = static_cast<std::uint32_t>(n);
            sec.vertex_offset = n ? static_cast<std::uint32_t>(section_start) : 0;
            sec.start_index = n ? start_index : 0;
            put<std::uint32_t>(mesh.res, sec.at + 0x20, sec.triangles);
            put<std::uint32_t>(mesh.res, sec.at + 0x24, sec.start_index);
            put<std::uint32_t>(mesh.res, sec.at + 0x28, sec.vertex_offset);
            put<std::uint32_t>(mesh.res, sec.at + 0x2C, sec.vertices);
            put<std::uint32_t>(mesh.res, sec.at + 0x54, sec.vertex_offset);
            put<std::uint32_t>(mesh.res, sec.at + 0x140, sec.start_index);
            for (int k = 0; k < 3; ++k)
            {
                put<float>(
                    mesh.res, sec.at + 0x150 + static_cast<size_t>(k) * 4, n ? lo[static_cast<size_t>(k)] : 0.0f);
                put<float>(
                    mesh.res, sec.at + 0x160 + static_cast<size_t>(k) * 4, n ? hi[static_cast<size_t>(k)] : 0.0f);
            }
            put<float>(mesh.res, sec.at + 0x15C, 0.0f);
            put<float>(mesh.res, sec.at + 0x16C, 0.0f);
            start_index += triangles * 3;
        }
        lod.vertex_bytes = static_cast<std::uint32_t>(vertex_data.size());
        lod.index_bytes = static_cast<std::uint32_t>(index_data.size());
        put<std::uint32_t>(mesh.res, lod.at + 0x58, lod.index_bytes);
        put<std::uint32_t>(mesh.res, lod.at + 0x5C, lod.vertex_bytes);
        out.chunk = std::move(vertex_data);
        out.chunk.insert(out.chunk.end(), index_data.begin(), index_data.end());
        out.chunk.resize((out.chunk.size() + 15) / 16 * 16, std::byte{0});
        return out;
    }

    void write_mesh_bounds(MeshSet &mesh, const std::array<float, 3> &lo, const std::array<float, 3> &hi)
    {
        auto box = [&](std::size_t at)
        {
            for (int k = 0; k < 3; ++k)
            {
                put<float>(mesh.res, at + static_cast<size_t>(k) * 4, lo[static_cast<size_t>(k)]);
                put<float>(mesh.res, at + 16 + static_cast<size_t>(k) * 4, hi[static_cast<size_t>(k)]);
            }
        };
        box(0x10);
        mesh.box_min = lo;
        mesh.box_max = hi;
        // Part boxes (one per skinned part): the old Studio sets each to the whole mesh's box, and so does this.
        const auto parts = get<std::uint16_t>(mesh.res, 0xC6);
        if (parts && parts < 1024)
        {
            const std::size_t first = pointer(mesh.res, 0xD0);
            if (first + parts * 32u <= mesh.res.size())
                for (std::size_t i = 0; i < parts; ++i)
                    box(first + i * 32);
        }
    }

} // namespace studio::native
