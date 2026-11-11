#include "native/gltf.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace studio::native
{
    namespace
    {
        using Json = nlohmann::json;
        using Mat4 = std::array<double, 16>; // column major, as glTF stores it

        Mat4 identity()
        {
            return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        }

        Mat4 multiply(const Mat4 &a, const Mat4 &b)
        {
            Mat4 r{};
            for (int c = 0; c < 4; ++c)
                for (int row = 0; row < 4; ++row)
                {
                    double s = 0;
                    for (int k = 0; k < 4; ++k)
                        s += a[k * 4 + row] * b[c * 4 + k];
                    r[c * 4 + row] = s;
                }
            return r;
        }

        Mat4 node_matrix(const Json &node)
        {
            if (node.contains("matrix"))
            {
                Mat4 m{};
                for (int i = 0; i < 16; ++i)
                    m[i] = node["matrix"][i].get<double>();
                return m;
            }
            double t[3] = {0, 0, 0}, s[3] = {1, 1, 1}, q[4] = {0, 0, 0, 1};
            if (node.contains("translation"))
                for (int i = 0; i < 3; ++i)
                    t[i] = node["translation"][i].get<double>();
            if (node.contains("scale"))
                for (int i = 0; i < 3; ++i)
                    s[i] = node["scale"][i].get<double>();
            if (node.contains("rotation"))
                for (int i = 0; i < 4; ++i)
                    q[i] = node["rotation"][i].get<double>();
            const double x = q[0], y = q[1], z = q[2], w = q[3];
            const double r[9] = {1 - 2 * (y * y + z * z),
                                 2 * (x * y + z * w),
                                 2 * (x * z - y * w),
                                 2 * (x * y - z * w),
                                 1 - 2 * (x * x + z * z),
                                 2 * (y * z + x * w),
                                 2 * (x * z + y * w),
                                 2 * (y * z - x * w),
                                 1 - 2 * (x * x + y * y)};
            Mat4 m{};
            for (int c = 0; c < 3; ++c)
                for (int row = 0; row < 3; ++row)
                    m[c * 4 + row] = r[c * 3 + row] * s[c];
            m[12] = t[0];
            m[13] = t[1];
            m[14] = t[2];
            m[15] = 1;
            return m;
        }

        struct Reader
        {
            const Json &doc;
            const std::vector<unsigned char> &bin;

            // Reads accessor `index` as `components` floats per element (normalised integers scaled to 0..1).
            std::vector<float> floats(int index, int components) const
            {
                const Json &a = doc["accessors"].at(static_cast<size_t>(index));
                const size_t count = a["count"].get<size_t>();
                const std::string type = a["type"];
                const int n = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : 0;
                if (n < components)
                    throw std::runtime_error("glTF accessor " + std::to_string(index) + " has too few components");
                std::vector<float> out(count * static_cast<size_t>(components), 0.0f);
                if (!a.contains("bufferView"))
                    return out; // all zero, as the spec says
                const Json &view = doc["bufferViews"].at(a["bufferView"].get<size_t>());
                if (view.value("buffer", 0) != 0)
                    throw std::runtime_error("glTF data outside the .glb buffer is not supported");
                const int ctype = a["componentType"];
                const size_t csize = ctype == 5126 || ctype == 5125 ? 4 : ctype == 5123 || ctype == 5122 ? 2 : 1;
                const size_t stride = view.value("byteStride", static_cast<size_t>(n) * csize);
                const size_t base = view.value("byteOffset", size_t{0}) + a.value("byteOffset", size_t{0});
                const bool normalized = a.value("normalized", false);
                if (count && base + (count - 1) * stride + static_cast<size_t>(n) * csize > bin.size())
                    throw std::runtime_error("glTF accessor " + std::to_string(index) +
                                             " runs past the end of the buffer");
                for (size_t i = 0; i < count; ++i)
                    for (int c = 0; c < components; ++c)
                    {
                        const unsigned char *p = bin.data() + base + i * stride + static_cast<size_t>(c) * csize;
                        float v = 0;
                        switch (ctype)
                        {
                        case 5126:
                            std::memcpy(&v, p, 4);
                            break;
                        case 5121:
                            v = normalized ? p[0] / 255.0f : p[0];
                            break;
                        case 5120:
                        {
                            auto s = static_cast<signed char>(p[0]);
                            v = normalized ? std::max(s / 127.0f, -1.0f) : s;
                            break;
                        }
                        case 5123:
                        {
                            std::uint16_t u;
                            std::memcpy(&u, p, 2);
                            v = normalized ? u / 65535.0f : u;
                            break;
                        }
                        case 5122:
                        {
                            std::int16_t s;
                            std::memcpy(&s, p, 2);
                            v = normalized ? std::max(s / 32767.0f, -1.0f) : s;
                            break;
                        }
                        case 5125:
                        {
                            std::uint32_t u;
                            std::memcpy(&u, p, 4);
                            v = static_cast<float>(u);
                            break;
                        }
                        default:
                            throw std::runtime_error("glTF accessor component type " + std::to_string(ctype) +
                                                     " is not supported");
                        }
                        out[i * static_cast<size_t>(components) + static_cast<size_t>(c)] = v;
                    }
                return out;
            }

            std::vector<std::uint32_t> indices(int index) const
            {
                const Json &a = doc["accessors"].at(static_cast<size_t>(index));
                const Json &view = doc["bufferViews"].at(a["bufferView"].get<size_t>());
                const size_t count = a["count"].get<size_t>();
                const int ctype = a["componentType"];
                const size_t csize = ctype == 5125 ? 4 : ctype == 5123 ? 2 : 1;
                const size_t stride = view.value("byteStride", csize);
                const size_t base = view.value("byteOffset", size_t{0}) + a.value("byteOffset", size_t{0});
                if (count && base + (count - 1) * stride + csize > bin.size())
                    throw std::runtime_error("glTF index accessor runs past the end of the buffer");
                std::vector<std::uint32_t> out(count);
                for (size_t i = 0; i < count; ++i)
                {
                    const unsigned char *p = bin.data() + base + i * stride;
                    std::uint32_t v = 0;
                    std::memcpy(&v, p, csize);
                    out[i] = v;
                }
                return out;
            }
        };

        void face_normals(GltfPrimitive &p)
        {
            p.normals.assign(p.positions.size(), Vec3{0, 0, 0});
            for (size_t t = 0; t + 2 < p.indices.size(); t += 3)
            {
                const auto &a = p.positions[p.indices[t]];
                const auto &b = p.positions[p.indices[t + 1]];
                const auto &c = p.positions[p.indices[t + 2]];
                const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                const float n[3] = {
                    e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                for (int k = 0; k < 3; ++k)
                    for (int j = 0; j < 3; ++j)
                        p.normals[p.indices[t + static_cast<size_t>(k)]][static_cast<size_t>(j)] += n[j];
            }
            for (auto &n : p.normals)
            {
                const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (l > 0)
                    for (auto &c : n)
                        c /= l;
                else
                    n = {0, 1, 0};
            }
        }
    } // namespace

    std::size_t GltfModel::vertices() const
    {
        std::size_t n = 0;
        for (const auto &p : primitives)
            n += p.positions.size();
        return n;
    }

    std::size_t GltfModel::triangles() const
    {
        std::size_t n = 0;
        for (const auto &p : primitives)
            n += p.indices.size() / 3;
        return n;
    }

    GltfModel load_glb(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw std::runtime_error("cannot open the model file");
        std::vector<unsigned char> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto u32 = [&](size_t at)
        {
            std::uint32_t v = 0;
            if (at + 4 <= file.size())
                std::memcpy(&v, file.data() + at, 4);
            return v;
        };
        if (file.size() < 20 || u32(0) != 0x46546C67)
            throw std::runtime_error("not a binary glTF (.glb) file");
        const std::uint32_t json_len = u32(12);
        if (u32(16) != 0x4E4F534A || 20ull + json_len > file.size())
            throw std::runtime_error("the .glb has no JSON chunk");
        const Json doc = Json::parse(file.begin() + 20, file.begin() + 20 + json_len);
        std::vector<unsigned char> bin;
        const size_t bin_at = 20 + json_len;
        if (bin_at + 8 <= file.size() && u32(bin_at + 4) == 0x004E4942)
        {
            const std::uint32_t len = u32(bin_at);
            if (bin_at + 8 + len > file.size())
                throw std::runtime_error("the .glb binary chunk is cut short");
            bin.assign(file.begin() + static_cast<std::ptrdiff_t>(bin_at + 8),
                       file.begin() + static_cast<std::ptrdiff_t>(bin_at + 8 + len));
        }
        if (doc.contains("extensionsRequired"))
        {
            const auto &extensions = doc["extensionsRequired"];

            if (!extensions.empty())
            {
                throw std::runtime_error("the .glb needs the extension " + extensions.front().get<std::string>() +
                                         " (export without Draco or mesh compression)");
            }
        }
        Reader reader{doc, bin};
        GltfModel model;

        // Walk the default scene (or every root node) with world matrices.
        std::vector<std::pair<int, Mat4>> stack;
        const Json &nodes = doc.value("nodes", Json::array());
        std::vector<int> roots;
        if (doc.contains("scenes") && !doc["scenes"].empty())
        {
            const size_t scene = doc.value("scene", 0);
            for (const auto &n : doc["scenes"].at(scene).value("nodes", Json::array()))
                roots.push_back(n.get<int>());
        }
        else
        {
            for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
                roots.push_back(i);
        }
        for (int r : roots)
            stack.emplace_back(r, identity());
        while (!stack.empty())
        {
            auto [index, parent] = stack.back();
            stack.pop_back();
            const Json &node = nodes.at(static_cast<size_t>(index));
            const Mat4 world = multiply(parent, node_matrix(node));
            for (const auto &child : node.value("children", Json::array()))
                stack.emplace_back(child.get<int>(), world);
            if (!node.contains("mesh"))
                continue;
            // Normals use the inverse transpose; for the rotation + scale matrices exporters write, the
            // cofactor matrix is proportional to it and is renormalised below.
            const double *m = world.data();
            const double cof[9] = {m[5] * m[10] - m[6] * m[9],
                                   m[6] * m[8] - m[4] * m[10],
                                   m[4] * m[9] - m[5] * m[8],
                                   m[2] * m[9] - m[1] * m[10],
                                   m[0] * m[10] - m[2] * m[8],
                                   m[1] * m[8] - m[0] * m[9],
                                   m[1] * m[6] - m[2] * m[5],
                                   m[2] * m[4] - m[0] * m[6],
                                   m[0] * m[5] - m[1] * m[4]};
            const double det = m[0] * cof[0] + m[4] * cof[1] + m[8] * cof[2];
            const Json &mesh = doc["meshes"].at(node["mesh"].get<size_t>());
            for (const auto &prim : mesh["primitives"])
            {
                if (prim.value("mode", 4) != 4)
                    continue; // triangles only
                const Json &attrs = prim["attributes"];
                if (!attrs.contains("POSITION"))
                    continue;
                GltfPrimitive p;
                if (prim.contains("material"))
                {
                    const Json &mat = doc["materials"].at(prim["material"].get<size_t>());
                    p.material = mat.value("name", "material_" + std::to_string(prim["material"].get<int>()));
                }
                const auto pos = reader.floats(attrs["POSITION"], 3);
                p.positions.resize(pos.size() / 3);
                for (size_t i = 0; i < p.positions.size(); ++i)
                {
                    const double x = pos[i * 3], y = pos[i * 3 + 1], z = pos[i * 3 + 2];
                    for (int r = 0; r < 3; ++r)
                        p.positions[i][static_cast<size_t>(r)] =
                            static_cast<float>(m[r] * x + m[4 + r] * y + m[8 + r] * z + m[12 + r]);
                }
                if (prim.contains("indices"))
                {
                    p.indices = reader.indices(prim["indices"]);
                }
                else
                {
                    p.indices.resize(p.positions.size() - p.positions.size() % 3);
                    for (size_t i = 0; i < p.indices.size(); ++i)
                        p.indices[i] = static_cast<std::uint32_t>(i);
                }
                for (auto i : p.indices)
                    if (i >= p.positions.size())
                        throw std::runtime_error("a glTF index points past the last vertex");
                if (det < 0) // a mirroring transform flips the winding
                    for (size_t t = 0; t + 2 < p.indices.size(); t += 3)
                        std::swap(p.indices[t + 1], p.indices[t + 2]);
                if (attrs.contains("NORMAL"))
                {
                    const auto nrm = reader.floats(attrs["NORMAL"], 3);
                    p.normals.resize(p.positions.size());
                    for (size_t i = 0; i < p.normals.size(); ++i)
                    {
                        double v[3];
                        for (int r = 0; r < 3; ++r)
                            v[r] = cof[r * 3] * nrm[i * 3] + cof[r * 3 + 1] * nrm[i * 3 + 1] +
                                   cof[r * 3 + 2] * nrm[i * 3 + 2];
                        if (det < 0)
                            for (double &c : v)
                                c = -c;
                        const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                        for (int r = 0; r < 3; ++r)
                            p.normals[i][static_cast<size_t>(r)] = static_cast<float>(l > 0 ? v[r] / l : (r == 1));
                    }
                }
                else
                {
                    face_normals(p);
                }
                auto uvs = [&](const char *name, std::vector<Vec2> &out)
                {
                    if (!attrs.contains(name))
                        return;
                    const auto uv = reader.floats(attrs[name], 2);
                    out.resize(uv.size() / 2);
                    for (size_t i = 0; i < out.size(); ++i)
                        out[i] = {uv[i * 2], uv[i * 2 + 1]};
                };
                uvs("TEXCOORD_0", p.uv0);
                uvs("TEXCOORD_1", p.uv1);
                if (p.uv0.empty())
                    p.uv0.assign(p.positions.size(), Vec2{0, 0});
                if (!p.indices.empty())
                    model.primitives.push_back(std::move(p));
            }
        }
        if (model.primitives.empty())
            throw std::runtime_error("the .glb has no triangle meshes");
        return model;
    }

    std::vector<std::string> write_glb_one_material(const std::filesystem::path &from,
                                                    const std::filesystem::path &to,
                                                    const std::string &name)
    {
        std::ifstream in(from, std::ios::binary);
        if (!in)
            throw std::runtime_error("cannot open the model file");
        std::vector<unsigned char> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto u32 = [&](size_t at)
        {
            std::uint32_t v = 0;
            if (at + 4 <= file.size())
                std::memcpy(&v, file.data() + at, 4);
            return v;
        };
        if (file.size() < 20 || u32(0) != 0x46546C67)
            throw std::runtime_error("not a binary glTF (.glb) file");
        const std::uint32_t json_len = u32(12);
        if (u32(16) != 0x4E4F534A || 20ull + json_len > file.size())
            throw std::runtime_error("the .glb has no JSON chunk");
        Json doc = Json::parse(file.begin() + 20, file.begin() + 20 + json_len);

        // One material for everything: several materials with the same name read back as Name, Name.001, ...
        std::vector<std::string> replaced;
        if (doc.contains("materials") && doc["materials"].is_array())
            for (const auto &m : doc["materials"])
                replaced.push_back(m.is_object() ? m.value("name", std::string()) : std::string());
        doc["materials"] = Json::array({{{"name", name}}});
        if (!doc["meshes"].is_array())
            doc["meshes"] = Json::array();
        for (auto &mesh : doc["meshes"])
        {
            mesh["name"] = name;
            for (auto &p : mesh["primitives"])
                p["material"] = 0;
        }
        if (doc.contains("nodes"))
            for (auto &node : doc["nodes"])
                if (node.contains("mesh"))
                    node["name"] = name;

        // The JSON chunk is padded with spaces to 4 bytes; the binary chunk after it is kept as it is.
        std::string text = doc.dump();
        while (text.size() % 4)
            text.push_back(' ');
        const size_t rest_at = 20 + json_len;
        std::vector<unsigned char> out;
        auto put = [&](std::uint32_t v)
        {
            unsigned char b[4];
            std::memcpy(b, &v, 4);
            out.insert(out.end(), b, b + 4);
        };
        put(0x46546C67);
        put(2);
        put(static_cast<std::uint32_t>(12 + 8 + text.size() + (file.size() - rest_at)));
        put(static_cast<std::uint32_t>(text.size()));
        put(0x4E4F534A);
        out.insert(out.end(), text.begin(), text.end());
        out.insert(out.end(), file.begin() + static_cast<std::ptrdiff_t>(rest_at), file.end());
        std::ofstream o(to, std::ios::binary | std::ios::trunc);
        if (!o)
            throw std::runtime_error("cannot write the model copy");
        o.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!o)
            throw std::runtime_error("writing the model copy failed");
        return replaced;
    }

} // namespace studio::native
