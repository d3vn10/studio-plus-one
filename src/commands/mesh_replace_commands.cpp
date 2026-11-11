// Native mesh commands that do not go through reskate_cli: mesh find, mesh info, mesh export-raw and mesh
// replace. They read the game with the ReSkate readers in src/native and write ReSkate Studio .fbproject /
// Frosty .fbmod files themselves. Registered from cosmetic_commands.cpp, which also uses board_part_mesh and
// model_as_glb for cosmetic new-board-part.
#include "commands/commands.h"
#include "core/engine.h"
#include "core/settings.h"
#include "core/process.h"
#include "native/game_assets.h"
#include "native/gltf.h"
#include "native/mesh_set.h"
#include "native/mod_files.h"
#include "Engine/Resource/ebx_document.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <filesystem>
#include <fstream>
#include <cwctype>
#include <optional>
#include <variant>

namespace fs = std::filesystem;

namespace studio
{
    namespace
    {
        namespace fb = dingosdk::frostbite;

        fs::path path_param(const Json &a, const std::string &name)
        {
            fs::path p = utf8_to_wide(arg_string(a, name));
            std::error_code ec;
            fs::path abs = fs::absolute(p, ec);
            return ec ? p : abs;
        }

        void write_file(const fs::path &path, std::span<const std::byte> bytes)
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out)
                throw Error("write_failed", "Cannot write " + path_utf8(path), {{"path", path_utf8(path)}});
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }

        std::string hex(std::span<const std::byte> bytes)
        {
            static const char digits[] = "0123456789abcdef";
            std::string out;
            for (auto b : bytes)
            {
                const auto v = std::to_integer<unsigned>(b);
                out += digits[v >> 4];
                out += digits[v & 15];
            }
            return out;
        }

        Json run_export_raw(Context &c, const Json &a)
        {
            const std::string name = arg_string(a, "mesh");
            const fs::path dir = path_param(a, "output-dir");
            std::error_code ec;
            fs::create_directories(dir, ec);
            native::GameAssets game(c.game_root(a));
            game.on_progress = [&c](std::string_view message) { c.progress(-1, message); };
            auto res = game.find(fb::AssetKind::resource, name);
            if (!res)
                throw Error("mesh_not_found", "No resource named " + name + " in the game", {{"mesh", name}});
            const std::string stem = path_utf8(fs::path(utf8_to_wide(name)).filename());
            write_file(dir / utf8_to_wide(stem + ".res"), res->bytes);
            Json out = {{"mesh", res->name},
                        {"bundle", res->bundle},
                        {"toc", res->toc},
                        {"resource_type", res->meta.resourceType},
                        {"resource_id", res->meta.resourceId},
                        {"resource_meta", hex(res->meta.resourceMeta)},
                        {"bytes", res->bytes.size()},
                        {"bundles", game.bundles_with(fb::AssetKind::resource, name)}};
            if (auto ebx = game.find(fb::AssetKind::ebx, name))
            {
                write_file(dir / utf8_to_wide(stem + ".ebx"), ebx->bytes);
                out["ebx_bytes"] = ebx->bytes.size();
            }
            // Every 16-byte run in the resource that names a chunk the game has is written out as <id>.chunk.
            Json chunks = Json::array();
            for (std::size_t at = 0; at + 16 <= res->bytes.size(); at += 4)
            {
                fb::Guid id;
                std::copy_n(res->bytes.begin() + static_cast<std::ptrdiff_t>(at), 16, id.bytes.begin());
                bool zero =
                    std::all_of(id.bytes.begin(), id.bytes.end(), [](std::byte b) { return b == std::byte{0}; });
                if (zero)
                    continue;
                if (auto chunk = game.find_chunk(id))
                {
                    write_file(dir / utf8_to_wide(chunk->name + ".chunk"), chunk->bytes);
                    chunks.push_back({{"id", chunk->name},
                                      {"offset_in_res", at},
                                      {"bytes", chunk->bytes.size()},
                                      {"logical_offset", chunk->meta.logicalOffset},
                                      {"logical_size", chunk->meta.logicalSize},
                                      {"bundle", chunk->bundle}});
                }
            }
            out["chunks"] = chunks;
            out["output_dir"] = path_utf8(dir);
            return out;
        }

        constexpr std::uint32_t mesh_set_type = 0x49B156D4; // the MeshSet resource type

        std::string lower(std::string s)
        {
            for (auto &ch : s)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return s;
        }

        std::string usage_name(std::uint8_t usage)
        {
            switch (usage)
            {
            case 0x01:
                return "position";
            case 0x02:
                return "bone_indices";
            case 0x03:
                return "bone_indices2";
            case 0x04:
                return "bone_weights";
            case 0x05:
                return "bone_weights2";
            case 0x34:
                return "tangent_frame";
            default:
                if (usage >= 0x21 && usage <= 0x28)
                    return "uv" + std::to_string(usage - 0x21);
                return "usage_" + std::to_string(usage);
            }
        }

        native::GameAssets open_game(Context &c, const Json &a)
        {
            native::GameAssets game(c.game_root(a));
            game.on_progress = [&c](std::string_view message) { c.progress(-1, message); };
            return game;
        }

        // A game mesh read for replacing: its MeshSet, every LOD's chunk, and where they live.
        struct GameMesh
        {
            std::string name;
            native::GameAsset res;
            native::MeshSet mesh;
            std::vector<native::GameAsset> chunks; // one per LOD
        };

        GameMesh read_mesh(native::GameAssets &game, const std::string &name)
        {
            GameMesh m;
            auto res = game.find(fb::AssetKind::resource, name);
            if (!res && name.find('/') == std::string::npos)
            {
                // A short name such as deck_gen_popsicle_mesh: use the one mesh whose path ends with it.
                std::vector<std::string> matches;
                for (const auto &[full, type] : game.find_resources(name, mesh_set_type, 0))
                    if (full.size() > name.size() && full.ends_with("/" + name))
                        matches.push_back(full);
                if (matches.size() > 1)
                    throw Error("mesh_ambiguous",
                                "More than one mesh is named " + name + "; give the full path",
                                {{"mesh", name}, {"matches", matches}});
                if (matches.size() == 1)
                    res = game.find(fb::AssetKind::resource, matches[0]);
            }
            if (!res)
                throw Error("mesh_not_found",
                            "No mesh named " + name + " in the game. Find names with: studio-plus mesh find <text>",
                            {{"mesh", name}});
            if (res->meta.resourceType != mesh_set_type)
                throw Error("not_a_mesh", name + " is a resource but not a mesh (MeshSet)", {{"mesh", name}});
            m.name = res->name;
            try
            {
                m.mesh = native::parse_mesh_set(res->bytes);
            }
            catch (const std::exception &e)
            {
                throw Error("mesh_unsupported", name + ": " + e.what(), {{"mesh", name}});
            }
            for (std::size_t i = 0; i < m.mesh.lods.size(); ++i)
            {
                for (std::size_t j = 0; j < i; ++j)
                    if (m.mesh.lods[j].chunk == m.mesh.lods[i].chunk)
                        throw Error(
                            "mesh_unsupported",
                            name + ": several LODs share one geometry chunk, which mesh replace does not handle yet",
                            {{"mesh", name}});
                auto chunk = game.find_chunk(m.mesh.lods[i].chunk);
                if (!chunk)
                    throw Error("chunk_not_found",
                                name + ": geometry chunk " + native::guid_text(m.mesh.lods[i].chunk) +
                                    " is not in the game",
                                {{"mesh", name}});
                m.chunks.push_back(std::move(*chunk));
            }
            m.res = std::move(*res);
            return m;
        }

        Json section_json(const native::MeshSection &s)
        {
            Json layout = Json::array();
            for (const auto &e : s.elements)
                layout.push_back(usage_name(e.usage));
            return {{"section", s.name},
                    {"triangles", s.triangles},
                    {"vertices", s.vertices},
                    {"bones", s.bones},
                    {"vertex_stride", s.vertex_stride},
                    {"layout", layout}};
        }

        Json run_mesh_info(Context &c, const Json &a)
        {
            auto game = open_game(c, a);
            GameMesh m = read_mesh(game, arg_string(a, "mesh"));
            Json lods = Json::array();
            for (std::size_t i = 0; i < m.mesh.lods.size(); ++i)
            {
                const auto &lod = m.mesh.lods[i];
                Json sections = Json::array();
                for (const auto &s : lod.sections)
                    sections.push_back(section_json(s));
                lods.push_back({{"lod", i},
                                {"chunk", native::guid_text(lod.chunk)},
                                {"chunk_bytes", m.chunks[i].bytes.size()},
                                {"sections", sections}});
            }
            const auto bundles = game.bundles_with(fb::AssetKind::resource, m.name);
            return {{"mesh", m.name},
                    {"res_bytes", m.res.bytes.size()},
                    {"bounds", {{"min", m.mesh.box_min}, {"max", m.mesh.box_max}}},
                    {"bundles", bundles},
                    {"shared", bundles.size() > 1},
                    {"lods", lods}};
        }

        // ---- mesh replace ----------------------------------------------------------------------------

        // Converts an .fbx to a temporary .glb with Blender (the Studio+ Blender setting).
        fs::path fbx_to_glb(Context &c, const fs::path &fbx)
        {
            fs::path blender = c.settings.blender;
            std::error_code ec;
            if (blender.empty() || !fs::is_regular_file(blender, ec))
                throw Error("blender_missing",
                            "An .fbx model needs Blender to convert it. Set it with: studio-plus studio set blender "
                            "<blender.exe>, "
                            "or export a .glb instead");
            const fs::path work = Settings::data_dir() / L"work";
            fs::create_directories(work, ec);
            const fs::path script = work / L"fbx_to_glb.py";
            const fs::path out = work / (fbx.stem().wstring() + L".mesh-replace.glb");
            {
                std::ofstream f(script, std::ios::trunc);
                f << "import bpy, sys\n"
                     "src, dst = sys.argv[sys.argv.index('--') + 1:][:2]\n"
                     "bpy.ops.wm.read_factory_settings(use_empty=True)\n"
                     "bpy.ops.import_scene.fbx(filepath=src)\n"
                     "bpy.ops.export_scene.gltf(filepath=dst, export_format='GLB', export_yup=True, "
                     "export_texcoords=True, export_normals=True)\n";
            }
            fs::remove(out, ec);
            c.progress(-1, "converting " + path_utf8(fbx.filename()) + " to .glb in Blender");
            ProcessOptions po;
            po.executable = blender;
            po.args = {"-b", "--factory-startup", "--python", path_utf8(script), "--", path_utf8(fbx), path_utf8(out)};
            po.working_dir = work;
            po.cancel = c.cancel;
            const ProcessResult r = run_process(po);
            if (r.cancelled)
                throw Error("cancelled", "Cancelled");
            if (!fs::is_regular_file(out, ec))
                throw Error("fbx_convert_failed",
                            "Blender could not convert " + path_utf8(fbx.filename()) + " to .glb",
                            {{"exit_code", r.exit_code}});
            return out;
        }

        // glTF primitives sorted into one geometry list per section of a LOD.
        struct Routing
        {
            std::vector<native::SectionGeometry> geometry;
            Json routes = Json::array(); // material -> section
        };

        bool is_shadow(const native::MeshSection &s)
        {
            // Shadow-caster sections (e.g. Truck_Mat_Shadow) have no UVs; in the game's meshes each holds all the
            // geometry of the visible sections.
            return std::none_of(
                s.elements.begin(), s.elements.end(), [](const native::VertexElement &e) { return e.usage == 0x21; });
        }

        void append(native::SectionGeometry &g, const native::GltfPrimitive &p, float scale)
        {
            const auto base = static_cast<std::uint32_t>(g.positions.size());
            for (const auto &v : p.positions)
                g.positions.push_back({v[0] * scale, v[1] * scale, v[2] * scale});
            g.normals.insert(g.normals.end(), p.normals.begin(), p.normals.end());
            g.uv0.insert(g.uv0.end(), p.uv0.begin(), p.uv0.end());
            const auto &uv1 = p.uv1.empty() ? p.uv0 : p.uv1;
            g.uv1.insert(g.uv1.end(), uv1.begin(), uv1.end());
            for (auto i : p.indices)
                g.indices.push_back(base + i);
        }

        Routing route(const native::GltfModel &model,
                      const native::MeshLod &lod,
                      const std::map<std::string, std::string> &explicit_routes,
                      const std::string &default_section,
                      float scale,
                      bool shadows)
        {
            Routing r;
            r.geometry.resize(lod.sections.size());
            auto find_section = [&](const std::string &name) -> int
            {
                for (std::size_t i = 0; i < lod.sections.size(); ++i)
                    if (lower(lod.sections[i].name) == lower(name))
                        return static_cast<int>(i);
                return -1;
            };
            int fallback = find_section(default_section);
            if (fallback < 0)
                for (std::size_t i = 0; i < lod.sections.size() && fallback < 0; ++i)
                    if (!is_shadow(lod.sections[i]))
                        fallback = static_cast<int>(i);
            if (fallback < 0)
                fallback = 0;
            for (const auto &p : model.primitives)
            {
                int target = -1;
                std::string how;
                if (auto it = explicit_routes.find(lower(p.material)); it != explicit_routes.end())
                {
                    target = find_section(it->second);
                    how = "route";
                }
                if (target < 0 && (target = find_section(p.material)) >= 0)
                    how = "same name";
                if (target < 0)
                {
                    target = fallback;
                    how = "default section";
                }
                append(r.geometry[static_cast<std::size_t>(target)], p, scale);
                r.routes.push_back({{"material", p.material},
                                    {"section", lod.sections[static_cast<std::size_t>(target)].name},
                                    {"how", how},
                                    {"triangles", p.indices.size() / 3}});
                if (shadows)
                    for (std::size_t i = 0; i < lod.sections.size(); ++i)
                        if (static_cast<int>(i) != target && is_shadow(lod.sections[i]))
                            append(r.geometry[i], p, scale);
            }
            return r;
        }

        // The largest visible (non-shadow) section of LOD 0: where geometry with no matching material goes.
        std::string main_section(const native::MeshSet &mesh)
        {
            const auto &sections = mesh.lods.front().sections;
            std::size_t best = sections.size();
            for (std::size_t i = 0; i < sections.size(); ++i)
                if (!is_shadow(sections[i]) &&
                    (best == sections.size() || sections[i].vertices > sections[best].vertices))
                    best = i;
            return sections[best == sections.size() ? 0 : best].name;
        }

        // One 1 mm triangle in the main section and nothing else: hides the mesh (the stand-in the old Studio's
        // users made by hand). Kept non-empty so no LOD is left without geometry.
        native::GltfModel hidden_model(const std::string &section)
        {
            native::GltfModel m;
            native::GltfPrimitive p;
            p.material = section;
            p.positions = {{0, 0, 0}, {0.001f, 0, 0}, {0, 0.001f, 0}};
            p.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
            p.uv0 = {{0, 0}, {1, 0}, {0, 1}};
            p.indices = {0, 1, 2};
            m.primitives.push_back(p);
            return m;
        }

        // Rewrites `m` with `model` and adds its chunks and resource to `out`.
        Json replace_into(GameMesh &m,
                          const native::GltfModel &model,
                          const std::map<std::string, std::string> &routes,
                          float scale,
                          bool rigid,
                          bool shadows,
                          native::GameAssets &game,
                          std::vector<native::ModResource> &out)
        {
            const std::string fallback = main_section(m.mesh);
            const std::string tag = "reskate-cosmetic-original-mesh:v1|donor=" + m.name;
            Json lods = Json::array(), warnings = Json::array(), routing;
            std::array<float, 3> lo{}, hi{};
            bool any = false;
            std::vector<native::ModResource> chunks;
            for (std::size_t i = 0; i < m.mesh.lods.size(); ++i)
            {
                Json before = Json::array();
                for (const auto &s : m.mesh.lods[i].sections)
                    before.push_back({{"section", s.name}, {"triangles", s.triangles}, {"vertices", s.vertices}});
                Routing r = route(model, m.mesh.lods[i], routes, fallback, scale, shadows);
                if (i == 0)
                    routing = r.routes;
                native::LodBuild built;
                try
                {
                    built = native::build_lod(m.mesh, i, m.chunks[i].bytes, r.geometry, {.rigid = rigid});
                }
                catch (const std::exception &e)
                {
                    throw Error("mesh_build_failed",
                                m.name + " LOD " + std::to_string(i) + ": " + e.what(),
                                {{"mesh", m.name}});
                }
                for (auto &w : built.warnings)
                    if (std::find(warnings.begin(), warnings.end(), w) == warnings.end())
                        warnings.push_back(w);
                for (const auto &g : r.geometry)
                    for (const auto &p : g.positions)
                    {
                        if (!any)
                        {
                            lo = hi = p;
                            any = true;
                        }
                        for (std::size_t k = 0; k < 3; ++k)
                        {
                            lo[k] = std::min(lo[k], p[k]);
                            hi[k] = std::max(hi[k], p[k]);
                        }
                    }
                Json after = Json::array();
                for (const auto &s : m.mesh.lods[i].sections)
                    after.push_back({{"section", s.name}, {"triangles", s.triangles}, {"vertices", s.vertices}});
                native::ModResource chunk;
                chunk.kind = native::ModResource::Kind::chunk;
                chunk.name = native::guid_text(m.mesh.lods[i].chunk);
                chunk.user_data = tag;
                chunk.id = m.mesh.lods[i].chunk;
                chunk.bundles = game.bundles_with(fb::AssetKind::chunk, chunk.name);
                chunk.data = std::move(built.chunk);
                lods.push_back({{"lod", i},
                                {"chunk", chunk.name},
                                {"chunk_bytes_before", m.chunks[i].bytes.size()},
                                {"chunk_bytes", chunk.data.size()},
                                {"before", before},
                                {"after", after}});
                chunks.push_back(std::move(chunk));
            }
            native::write_mesh_bounds(m.mesh, lo, hi);
            native::ModResource res;
            res.kind = native::ModResource::Kind::res;
            res.name = m.name;
            res.user_data = tag;
            res.res_type = m.res.meta.resourceType;
            res.res_rid = m.res.meta.resourceId;
            res.res_meta = m.res.meta.resourceMeta;
            res.bundles = game.bundles_with(fb::AssetKind::resource, m.name);
            for (const auto &ch : chunks)
                res.links.push_back(ch.name);
            res.data = m.mesh.res;
            for (auto &ch : chunks)
                out.push_back(std::move(ch));
            out.push_back(std::move(res));
            return {{"mesh", m.name},
                    {"bundles", game.bundles_with(fb::AssetKind::resource, m.name)},
                    {"bounds", {{"min", lo}, {"max", hi}}},
                    {"routing", routing},
                    {"lods", lods},
                    {"warnings", warnings}};
        }

        Json run_mesh_replace(Context &c, const Json &a)
        {
            const std::string mesh_name = arg_string(a, "mesh");
            const fs::path output = path_param(a, "output");
            const std::string ext = lower(path_utf8(output.extension()));
            if (ext != ".fbproject" && ext != ".fbmod")
                throw Error(
                    "invalid_arguments", "--output: must be a .fbproject or .fbmod file", {{"param", "output"}});
            std::error_code ec;
            if (!output.parent_path().empty() && !fs::is_directory(output.parent_path(), ec))
                throw Error("output_dir_missing",
                            "--output: folder does not exist: " + path_utf8(output.parent_path()),
                            {{"param", "output"}});
            const bool hide_main = a.value("hide-mesh", false);
            const std::string model_arg = arg_string(a, "model");
            if (model_arg.empty() && !hide_main)
                throw Error("invalid_arguments",
                            "Give --model <file.glb|file.fbx>, or --hide-mesh to hide the mesh",
                            {{"param", "model"}});
            const double scale = a.value("scale", 1.0);
            if (!(scale > 0))
                throw Error("invalid_arguments", "--scale must be more than 0", {{"param", "scale"}});
            std::map<std::string, std::string> routes;
            for (const auto &item : a.value("route", Json::array()))
            {
                const std::string text = item.get<std::string>();
                const auto eq = text.find('=');
                if (eq == std::string::npos || eq == 0 || eq + 1 == text.size())
                    throw Error("invalid_arguments",
                                "--route takes material=section, e.g. Chrome=Truck_Mat; got '" + text + "'",
                                {{"param", "route"}});
                routes[lower(text.substr(0, eq))] = text.substr(eq + 1);
            }

            native::GltfModel model;
            fs::path model_path;
            if (!hide_main)
            {
                model_path = path_param(a, "model");
                if (!fs::is_regular_file(model_path, ec))
                    throw Error(
                        "file_not_found", "--model: file not found: " + path_utf8(model_path), {{"param", "model"}});
                const std::string mext = lower(path_utf8(model_path.extension()));
                if (mext != ".glb" && mext != ".fbx")
                    throw Error("invalid_arguments", "--model: must be a .glb or .fbx file", {{"param", "model"}});
                const fs::path glb = mext == ".fbx" ? fbx_to_glb(c, model_path) : model_path;
                try
                {
                    model = native::load_glb(glb);
                }
                catch (const std::exception &e)
                {
                    throw Error("model_unreadable",
                                "--model: " + path_utf8(model_path.filename()) + ": " + e.what(),
                                {{"param", "model"}});
                }
            }

            auto game = open_game(c, a);
            std::vector<native::ModResource> resources;
            Json replaced = Json::array();
            c.progress(-1, "reading " + mesh_name);
            GameMesh main = read_mesh(game, mesh_name);
            c.progress(-1, "encoding " + mesh_name);
            replaced.push_back(replace_into(main,
                                            hide_main ? hidden_model(main_section(main.mesh)) : model,
                                            routes,
                                            static_cast<float>(scale),
                                            a.value("rigid", false),
                                            !hide_main,
                                            game,
                                            resources));
            replaced.back()["hidden"] = hide_main;
            for (const auto &item : a.value("hide", Json::array()))
            {
                const std::string name = item.get<std::string>();
                if (lower(name) == lower(main.name))
                    continue;
                c.progress(-1, "hiding " + name);
                GameMesh m = read_mesh(game, name);
                replaced.push_back(
                    replace_into(m, hidden_model(main_section(m.mesh)), {}, 1.0f, false, false, game, resources));
                replaced.back()["hidden"] = true;
            }

            native::ModInfo info;
            info.title = arg_string(a, "title");
            if (info.title.empty())
                info.title = hide_main ? "Hidden mesh" : path_utf8(model_path.stem());
            info.author = arg_string(a, "author");
            info.description = "Replaces " + main.name +
                               (a.value("hide", Json::array()).empty() ? "" : " and hides other meshes") +
                               ". Made with ReSkate Studio+.";
            c.progress(-1, "writing " + path_utf8(output.filename()));
            try
            {
                if (ext == ".fbmod")
                    native::write_fbmod(output, info, resources, c.game_root(a));
                else
                    native::write_fbproject(output, info, resources);
            }
            catch (const std::exception &e)
            {
                throw Error("write_failed",
                            "--output: " + std::string(e.what()) + ": " + path_utf8(output),
                            {{"param", "output"}});
            }
            Json res_list = Json::array();
            for (const auto &r : resources)
                res_list.push_back({{"kind", r.kind == native::ModResource::Kind::res ? "res" : "chunk"},
                                    {"name", r.name},
                                    {"bytes", r.data.size()}});
            Json out = {{"output", path_utf8(output)},
                        {"format", ext.substr(1)},
                        {"output_bytes", fs::file_size(output, ec)},
                        {"title", info.title},
                        {"replaced", replaced},
                        {"resources", res_list}};
            if (!hide_main)
                out["model"] = {{"path", path_utf8(model_path)},
                                {"primitives", model.primitives.size()},
                                {"vertices", model.vertices()},
                                {"triangles", model.triangles()}};
            return out;
        }
    } // namespace

    namespace
    {
        namespace ebx = dingosdk::frostbite::ebx;

        std::string lower_ascii(std::string text)
        {
            for (auto &ch : text)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return text;
        }

        std::optional<long long> integer_of(const ebx::Value &v)
        {
            if (const auto *i = std::get_if<std::int64_t>(&v.data))
                return *i;
            if (const auto *u = std::get_if<std::uint64_t>(&v.data))
                return static_cast<long long>(*u);
            return std::nullopt;
        }

        // Every AssetPaths entry with AssetTypeId 2 (the item's own geometry, e.g. Truck_Royal_TheRoyal), anywhere in
        // the object tree: the entry's string field is the geometry name.
        void geometry_names(const ebx::Value &v, bool in_asset_paths, std::vector<std::string> &out)
        {
            if (const auto *list = std::get_if<ebx::Value::Array>(&v.data))
            {
                for (const auto &item : *list)
                    geometry_names(item, in_asset_paths, out);
                return;
            }
            const auto *object = std::get_if<std::shared_ptr<ebx::Object>>(&v.data);
            if (!object || !*object)
                return;
            if (in_asset_paths)
            {
                std::optional<long long> type;
                std::string name;
                for (const auto &f : (*object)->fields)
                {
                    if (lower_ascii(f.name) == "assettypeid")
                        type = integer_of(f.value);
                    else if (const auto *text = std::get_if<std::string>(&f.value.data);
                             text && name.empty() && !text->empty())
                        name = *text;
                }
                if (type == 2 && !name.empty() && std::find(out.begin(), out.end(), name) == out.end())
                    out.push_back(name);
            }
            for (const auto &f : (*object)->fields)
                geometry_names(f.value, lower_ascii(f.name) == "assetpaths", out);
        }
    } // namespace

    // The mesh a board-part item draws: its geometry names from the item's EBX, the MeshSet named after the
    // first, and that mesh's sections. Throws Error("donor_not_found" / "donor_has_no_geometry" /
    // "donor_mesh_not_found").
    Json board_part_mesh(Context &c, const Json &a, const std::string &item)
    {
        auto game = open_game(c, a);
        c.progress(-1, "reading " + item);
        const auto found = game.find(fb::AssetKind::ebx, item);
        if (!found)
            throw Error(
                "donor_not_found", "No item called " + item + " in the game; see 'cosmetic audit'", {{"donor", item}});
        std::vector<std::string> geometry;
        try
        {
            const auto doc = ebx::read_document(found->bytes);
            for (const auto &instance : doc.instances)
                if (instance.object)
                    geometry_names(ebx::Value{instance.object}, false, geometry);
        }
        catch (const std::exception &e)
        {
            throw Error("donor_unreadable", "Cannot read the EBX of " + item + ": " + e.what(), {{"donor", item}});
        }
        if (geometry.empty())
            throw Error(
                "donor_has_no_geometry",
                item +
                    " has no geometry of its own (no AssetPaths entry with AssetTypeId 2). Deck, grip and wheel items "
                    "share one mesh per part; pick a truck item.",
                {{"donor", item}});
        const std::string want = lower_ascii(geometry.front());
        std::string mesh;
        for (const auto &[name, type] : game.find_resources(want, mesh_set_type, 0))
        {
            const std::string leaf = lower_ascii(name.substr(name.find_last_of('/') + 1));
            if (leaf == want + "_mesh")
            {
                mesh = name;
                break;
            }
            if (mesh.empty() && leaf.starts_with(want))
                mesh = name;
        }
        if (mesh.empty())
            throw Error("donor_mesh_not_found",
                        "No mesh named after " + geometry.front() + " (the geometry of " + item + ")",
                        {{"donor", item}, {"geometry", geometry}});
        c.progress(-1, "reading " + mesh);
        const GameMesh m = read_mesh(game, mesh);
        Json sections = Json::array();
        for (const auto &sec : m.mesh.lods.front().sections)
            sections.push_back({{"section", sec.name},
                                {"vertices", sec.vertices},
                                {"triangles", sec.triangles},
                                {"shadow", is_shadow(sec)}});
        return {{"donor", item},
                {"geometry", geometry},
                {"mesh", m.name},
                {"sections", sections},
                {"main_section", main_section(m.mesh)}};
    }

    // `model` as a .glb: a .glb as it is, an .fbx converted with Blender into the data folder.
    std::filesystem::path model_as_glb(Context &c, const std::filesystem::path &model)
    {
        std::wstring ext = model.extension().native();
        for (auto &ch : ext)
            ch = static_cast<wchar_t>(std::towlower(ch));
        return ext == L".fbx" ? fbx_to_glb(c, model) : model;
    }

    static Json run_mesh_find(Context &c, const Json &a)
    {
        const std::string text = arg_string(a, "text");
        const long long limit = a["limit"];
        native::GameAssets game(c.game_root(a));
        game.on_progress = [&c](std::string_view message) { c.progress(-1, message); };
        const auto found = game.find_resources(text, mesh_set_type, 0);
        Json meshes = Json::array();
        for (const auto &[name, type] : found)
        {
            if (limit > 0 && static_cast<long long>(meshes.size()) >= limit)
                break;
            meshes.push_back({{"mesh", name}, {"bundles", game.bundles_with(fb::AssetKind::resource, name).size()}});
        }
        return {{"text", text}, {"count", found.size()}, {"returned", meshes.size()}, {"meshes", meshes}};
    }

    void register_mesh_replace_commands(Registry &r)
    {
        r.add({
            .group = "mesh",
            .name = "find",
            .summary = "Find game meshes (MeshSet resources) by name",
            .description =
                "Lists the game's MeshSet resources whose name contains --text, with how many bundles load "
                "each one. A mesh in one level bundle only shows in that level; a mesh in many bundles is "
                "shared. Use the name with 'mesh info' and 'mesh replace'. Reads the game only, a few seconds.",
            .params =
                {
                    {"text", ParamType::String, "Part of the mesh name, e.g. skateboard or truck", true, true},
                    {"limit", ParamType::Integer, "Most meshes to return; 0 returns all", false, false, {}, Json(200)},
                    game_root_param(),
                },
            .examples = {"mesh find skateboard", "mesh find truck --json"},
            .long_running = true,
            .run = run_mesh_find,
        });

        r.add({
            .group = "mesh",
            .name = "export-raw",
            .summary = "Write a game mesh's MeshSet resource, EBX and geometry chunks to a folder",
            .description = "Reads the installed game directly (no reskate_cli) and writes <name>.res (the MeshSet "
                           "resource), <name>.ebx and every geometry chunk the resource points at as <id>.chunk into "
                           "--output-dir. Returns the resource type, id, meta, its bundles and the chunk sizes. For "
                           "research and for checking a replacement against the original. Reads the game only.",
            .params =
                {
                    {"mesh",
                     ParamType::String,
                     "Mesh asset name, e.g. characters/skateboard/static/static_skateboard_mesh",
                     true,
                     true},
                    {"output-dir", ParamType::Path, "Folder to write into (created if missing)", true, true},
                    game_root_param(),
                },
            .examples = {"mesh export-raw characters/skateboard/static/static_skateboard_mesh C:\\work\\deck_raw"},
            .long_running = true,
            .run = run_export_raw,
        });

        r.add({
            .group = "mesh",
            .name = "info",
            .summary = "Show a game mesh's LODs, material sections, vertex layout and bundles",
            .description =
                "Reads one MeshSet from the installed game (no reskate_cli) and returns its bounds, the bundles "
                "that load it (shared is true when more than one does), and per LOD the geometry chunk and each "
                "material section with its triangles, vertices, bone count and vertex layout. The section names "
                "are what 'mesh replace --route' and glTF material names map onto. Reads only, a few seconds.",
            .params =
                {
                    {"mesh",
                     ParamType::String,
                     "Mesh asset name, e.g. "
                     "characters/skateboard/unlicensed/deck/generic/popsicle/2022/deck_gen_popsicle_mesh",
                     true,
                     true},
                    game_root_param(),
                },
            .examples = {"mesh info characters/skateboard/unlicensed/truck/generic/default/2022/truck_gen_default_mesh",
                         "mesh info characters/skateboard/static/static_skateboard_mesh --json"},
            .long_running = true,
            .run = run_mesh_info,
        });

        r.add({
            .group = "mesh",
            .name = "replace",
            .summary = "Replace a game mesh with your .glb/.fbx (the old Studio's \"replace original\")",
            .description =
                "Replaces a game mesh asset in place with your model and writes the result as --output (.fbproject or "
                ".fbmod; build an .fbmod into a mod with 'mod compile'). Nothing in the ReSkate folder is changed. The "
                "mesh "
                "keeps its name, so it changes everywhere the game uses it, for every item that uses it and for every "
                "player "
                "with the mod: replacing the deck mesh changes every deck. The model must be in the game's space: Y "
                "up, "
                "metres, for board parts the board length along Z with the origin on the ground under the board "
                "centre. "
                "Each glTF material goes to the mesh section with the same name (see 'mesh info'), else to --route, "
                "else to "
                "the mesh's largest section; the other sections are emptied, and shadow sections (no UVs) get the "
                "whole model. Every LOD gets the full model. Normals and "
                "UV0/UV1 come from the model (UV1 falls back to UV0); skinning comes from the nearest original vertex "
                "of the "
                "same section (--rigid: the section's most common bone instead, so nothing moves with the wheels). "
                "--hide lists more meshes to hide in the same output (one 1 mm triangle each), e.g. the trucks and "
                "wheels under a scooter deck; --hide-mesh hides --mesh itself. At most 65535 vertices per section. "
                "Board "
                "meshes: deck .../deck/generic/popsicle/2022/deck_gen_popsicle_mesh, trucks "
                ".../truck/generic/default/2022/truck_gen_default_mesh, wheels "
                ".../wheel/generic/classic/2022/wheel_gen_classic_mesh "
                "(all under characters/skateboard/unlicensed). characters/skateboard/static/static_skateboard_mesh is "
                "only "
                "the cosmetic preview board. An .fbx is converted with Blender first. Takes a few seconds.",
            .params =
                {
                    {"mesh",
                     ParamType::String,
                     "Game mesh to replace, e.g. "
                     "characters/skateboard/unlicensed/deck/generic/popsicle/2022/deck_gen_popsicle_mesh",
                     true,
                     true},
                    {"model", ParamType::Path, "Your model: .glb, or .fbx (needs Blender)", false, true},
                    {"output", ParamType::Path, "File to write: .fbproject or .fbmod", true, true},
                    {"route",
                     ParamType::List,
                     "material=section pairs, e.g. Chrome=Skateboard_base_mat (material names are not case sensitive)"},
                    {"hide", ParamType::List, "More game meshes to hide in the same output"},
                    {"hide-mesh",
                     ParamType::Boolean,
                     "Hide --mesh instead of replacing it (no --model needed)",
                     false,
                     false,
                     {},
                     Json(false)},
                    {"rigid",
                     ParamType::Boolean,
                     "Skin each section to its most common bone instead of the nearest vertex's bones",
                     false,
                     false,
                     {},
                     Json(false)},
                    {"scale", ParamType::Number, "Scale the model by this factor first", false, false, {}, Json(1.0)},
                    {"title", ParamType::String, "Mod title (default: the model file name)"},
                    {"author", ParamType::String, "Mod author"},
                    game_root_param(),
                },
            .examples =
                {"mesh replace characters/skateboard/unlicensed/deck/generic/popsicle/2022/deck_gen_popsicle_mesh "
                 "scooter.glb scooter.fbmod --hide "
                 "characters/skateboard/unlicensed/truck/generic/default/2022/truck_gen_default_mesh "
                 "--hide characters/skateboard/unlicensed/wheel/generic/classic/2022/wheel_gen_classic_mesh --rigid",
                 "mesh replace characters/skateboard/static/static_skateboard_mesh deck.glb deck.fbproject --route "
                 "Blue=Skateboard_base_mat"},
            .long_running = true,
            .run = run_mesh_replace,
        });
    }
} // namespace studio
