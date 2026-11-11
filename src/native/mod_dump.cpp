#include "native/mod_dump.h"

#include "native/asset_views.h"
#include "native/mesh_set.h"
#include "core/process.h"
#include "core/settings.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"
#include "Engine/Resource/texture.h"
#include "Engine/Resource/toc.h"
#include "Engine/Vfs/game_archives.h"
#include "Engine/Vfs/game_bundles.h"

#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <map>
#include <optional>
#include <memory>
#include <limits>
#include <set>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <nlohmann/json.hpp>

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;
namespace fb = dingosdk::frostbite;

namespace studio::native
{
    namespace
    {
        std::string lower(std::string value)
        {
            for (auto &ch : value)
                if (ch >= 'A' && ch <= 'Z')
                    ch = static_cast<char>(ch + ('a' - 'A'));
            return value;
        }

        std::string_view kind_name(fb::AssetKind kind)
        {
            switch (kind)
            {
            case fb::AssetKind::ebx:
                return "ebx";
            case fb::AssetKind::resource:
                return "res";
            default:
                return "chunk";
            }
        }

        fs::path safe_relative(std::string_view value)
        {
            if (value.empty() || value.find('\\') != std::string_view::npos ||
                value.find('\0') != std::string_view::npos)
                throw std::runtime_error("Mod contains an unsafe asset path");
            fs::path result(utf8_to_wide(value));
            if (result.is_absolute() || result.has_root_name() || result.has_root_directory())
                throw std::runtime_error("Mod contains an absolute asset path");
            for (const auto &part : result)
            {
                const auto text = path_utf8(part);
                if (text.empty() || text == "." || text == ".." || text.find(':') != std::string::npos)
                    throw std::runtime_error("Mod contains an unsafe asset path");
            }
            return result;
        }

        std::string bundle_namespace(std::string_view toc, std::string_view bundle)
        {
            std::uint64_t hash = 14695981039346656037ull;
            auto add = [&](std::string_view text)
            {
                for (const char raw : text)
                {
                    hash ^= static_cast<unsigned char>(raw);
                    hash *= 1099511628211ull;
                }
                hash ^= 0xFF;
                hash *= 1099511628211ull;
            };
            add(toc);
            add(bundle);
            std::ostringstream result;
            result << std::hex << hash;
            return result.str();
        }

        std::string bytes_hex(std::span<const std::byte> bytes)
        {
            constexpr char digits[] = "0123456789abcdef";
            std::string result;
            result.reserve(bytes.size() * 2);
            for (const auto byte : bytes)
            {
                const auto value = std::to_integer<unsigned char>(byte);
                result.push_back(digits[value >> 4]);
                result.push_back(digits[value & 0x0F]);
            }
            return result;
        }

        std::string sha1_hex(const fb::Sha1 &sha1)
        {
            return bytes_hex(sha1.bytes);
        }

        std::string raw_sha1_hex(std::span<const std::byte> bytes)
        {
            struct AlgorithmHandle
            {
                BCRYPT_ALG_HANDLE value{};
                ~AlgorithmHandle()
                {
                    if (value)
                        BCryptCloseAlgorithmProvider(value, 0);
                }
            } algorithm;
            struct HashHandle
            {
                BCRYPT_HASH_HANDLE value{};
                ~HashHandle()
                {
                    if (value)
                        BCryptDestroyHash(value);
                }
            } hash;
            auto require_success = [](NTSTATUS status, std::string_view operation)
            {
                if (!BCRYPT_SUCCESS(status))
                    throw std::runtime_error(std::string(operation) + " failed with NTSTATUS " +
                                             std::to_string(static_cast<long>(status)));
            };
            require_success(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA1_ALGORITHM, nullptr, 0),
                            "BCryptOpenAlgorithmProvider");
            ULONG object_length{}, hash_length{}, written{};
            require_success(BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH,
                                              reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length),
                                              &written, 0),
                            "BCryptGetProperty(BCRYPT_OBJECT_LENGTH)");
            require_success(BCryptGetProperty(algorithm.value, BCRYPT_HASH_LENGTH,
                                              reinterpret_cast<PUCHAR>(&hash_length), sizeof(hash_length),
                                              &written, 0),
                            "BCryptGetProperty(BCRYPT_HASH_LENGTH)");
            std::vector<UCHAR> object(object_length);
            std::vector<std::byte> digest(hash_length);
            require_success(BCryptCreateHash(algorithm.value, &hash.value, object.data(), object_length,
                                             nullptr, 0, 0),
                            "BCryptCreateHash");
            for (std::size_t offset = 0; offset < bytes.size();)
            {
                const auto length = static_cast<ULONG>(
                    std::min(bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<ULONG>::max())));
                auto *data = const_cast<PUCHAR>(
                    reinterpret_cast<const UCHAR *>(bytes.data() + static_cast<std::ptrdiff_t>(offset)));
                require_success(BCryptHashData(hash.value, data, length, 0), "BCryptHashData");
                offset += length;
            }
            require_success(BCryptFinishHash(hash.value, reinterpret_cast<PUCHAR>(digest.data()), hash_length, 0),
                            "BCryptFinishHash");
            return bytes_hex(digest);
        }

        std::vector<std::byte> read_file(const fs::path &path)
        {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input)
                throw std::runtime_error("Cannot open " + path_utf8(path));
            const auto length = input.tellg();
            if (length < 0)
                throw std::runtime_error("Cannot read " + path_utf8(path));
            std::vector<std::byte> bytes(static_cast<std::size_t>(length));
            input.seekg(0);
            if (!bytes.empty() &&
                !input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
                throw std::runtime_error("Cannot read " + path_utf8(path));
            return bytes;
        }

        void write_file(const fs::path &path, const std::byte *data, std::size_t size)
        {
            std::error_code error;
            fs::create_directories(path.parent_path(), error);
            if (error)
                throw std::runtime_error("Cannot create " + path_utf8(path.parent_path()) + ": " +
                                         error.message());
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output || (size && !output.write(reinterpret_cast<const char *>(data),
                                                   static_cast<std::streamsize>(size))) ||
                !output.flush())
                throw std::runtime_error("Cannot write " + path_utf8(path));
        }

        void write_file(const fs::path &path, const std::vector<std::byte> &bytes)
        {
            write_file(path, bytes.data(), bytes.size());
        }

        void write_file(const fs::path &path, const std::vector<unsigned char> &bytes)
        {
            write_file(path, reinterpret_cast<const std::byte *>(bytes.data()), bytes.size());
        }

        void collect_asset_references(const fb::ebx::Value &value,
                                      std::set<std::uint64_t> &resource_ids,
                                      std::set<fb::Guid> &chunk_ids)
        {
            if (const auto *reference = std::get_if<fb::ebx::ResourceReference>(&value.data))
            {
                if (reference->id)
                    resource_ids.insert(reference->id);
            }
            else if (const auto *guid = std::get_if<fb::Guid>(&value.data))
            {
                if (*guid != fb::Guid{})
                    chunk_ids.insert(*guid);
            }
            else if (const auto *object = std::get_if<std::shared_ptr<fb::ebx::Object>>(&value.data);
                     object && *object)
            {
                for (const auto &field : (*object)->fields)
                    collect_asset_references(field.value, resource_ids, chunk_ids);
            }
            else if (const auto *array = std::get_if<fb::ebx::Value::Array>(&value.data))
            {
                for (const auto &element : *array)
                    collect_asset_references(element, resource_ids, chunk_ids);
            }
        }

        void collect_document_references(const std::vector<std::byte> &bytes,
                                         std::set<std::uint64_t> &resource_ids,
                                         std::set<fb::Guid> &chunk_ids)
        {
            const auto document = fb::ebx::read_document(bytes);
            for (const auto &instance : document.instances)
                if (instance.object)
                    for (const auto &field : instance.object->fields)
                        collect_asset_references(field.value, resource_ids, chunk_ids);
            for (const auto offset : document.resourceRefOffsets)
            {
                const auto position = document.dataStart + offset;
                if (position > bytes.size() || bytes.size() - position < sizeof(std::uint64_t))
                    throw std::runtime_error("EBX resource reference points outside its data");
                fb::BinaryReader reader(bytes);
                reader.seek(position);
                const auto id = reader.u64();
                if (id)
                    resource_ids.insert(id);
            }
        }

        std::vector<std::byte> read_payload(const fb::BundleFileInfo &payload,
                                           const fs::path &mod_root,
                                           const fs::path &game_root,
                                           const dingosdk::vfs::GameArchives &mod_archives,
                                           const dingosdk::vfs::GameArchives *game_archives)
        {
            if (!payload.location.patch && !game_archives)
                throw std::runtime_error("Payload belongs to the base game, but no game folder is available");
            const auto &archives = payload.location.patch ? mod_archives : *game_archives;
            const auto &root = payload.location.patch ? mod_root : game_root / L"Data";
            const auto stored = archives.read(root, payload.location, payload.offset, payload.size);
            fb::CasDecodeOptions options;
            options.gameRoot = game_root;
            options.maximumOutputSize = 1024ull * 1024ull * 1024ull;
            return fb::decode_cas(stored, options);
        }

        void progress(const ModDumpProgress &callback, const std::string &message)
        {
            if (callback)
                callback(message);
        }

        void append_u32_le(std::vector<std::byte> &bytes, std::uint32_t value)
        {
            for (int shift = 0; shift <= 24; shift += 8)
                bytes.push_back(static_cast<std::byte>(value >> shift));
        }

        void append_float_le(std::vector<std::byte> &bytes, float value)
        {
            append_u32_le(bytes, std::bit_cast<std::uint32_t>(value));
        }

        void write_mesh_glb(const fs::path &path,
                            const std::vector<SectionGeometry> &sections,
                            const MeshLod &lod,
                            std::string_view mesh_name)
        {
            using GlbJson = nlohmann::json;
            GlbJson accessors = GlbJson::array(), buffer_views = GlbJson::array(), materials = GlbJson::array();
            GlbJson primitives = GlbJson::array();
            std::vector<std::byte> binary;
            for (std::size_t section_index = 0; section_index < sections.size(); ++section_index)
            {
                const auto &geometry = sections[section_index];
                if (geometry.empty())
                    continue;
                if (geometry.positions.empty() || geometry.normals.size() != geometry.positions.size() ||
                    (!geometry.uv0.empty() && geometry.uv0.size() != geometry.positions.size()))
                    throw std::runtime_error("MeshSet section geometry is incomplete");

                GlbJson attributes = GlbJson::object();
                auto add_view = [&](const std::vector<std::byte> &data, std::uint32_t target,
                                    std::uint32_t component_type, std::size_t count, const char *type,
                                    const GlbJson &minimum, const GlbJson &maximum)
                {
                    while (binary.size() % 4)
                        binary.push_back(std::byte{});
                    const auto offset = binary.size();
                    binary.insert(binary.end(), data.begin(), data.end());
                    const auto view_index = buffer_views.size();
                    buffer_views.push_back({{"buffer", 0},
                                            {"byteLength", data.size()},
                                            {"byteOffset", offset},
                                            {"target", target}});
                    GlbJson accessor = {{"bufferView", view_index},
                                     {"componentType", component_type},
                                     {"count", count},
                                     {"type", type}};
                    if (!minimum.is_null())
                        accessor["min"] = minimum;
                    if (!maximum.is_null())
                        accessor["max"] = maximum;
                    accessors.push_back(std::move(accessor));
                    return accessors.size() - 1;
                };
                auto vectors = [&](const auto &values, std::size_t width, bool positions, bool uv,
                                   const char *label)
                {
                    std::vector<std::byte> data;
                    data.reserve(values.size() * width * sizeof(float));
                    std::array<float, 3> low{std::numeric_limits<float>::max(),
                                             std::numeric_limits<float>::max(),
                                             std::numeric_limits<float>::max()};
                    std::array<float, 3> high{std::numeric_limits<float>::lowest(),
                                              std::numeric_limits<float>::lowest(),
                                              std::numeric_limits<float>::lowest()};
                    for (const auto &value : values)
                    {
                        std::array<float, 3> converted{};
                        if (uv)
                            converted = {value[0], 1.0f - value[1], 0.0f};
                        else if (positions)
                            converted = {value[0], value[2], -value[1]};
                        else
                            converted = {value[0], value[2], -value[1]};
                        for (std::size_t axis = 0; axis < width; ++axis)
                        {
                            if (!std::isfinite(converted[axis]))
                                throw std::runtime_error(std::string(label) + " contains a non-finite value");
                            if (positions)
                            {
                                low[axis] = std::min(low[axis], converted[axis]);
                                high[axis] = std::max(high[axis], converted[axis]);
                            }
                            append_float_le(data, converted[axis]);
                        }
                    }
                    GlbJson minimum = nullptr, maximum = nullptr;
                    if (positions)
                    {
                        minimum = GlbJson::array();
                        maximum = GlbJson::array();
                        for (std::size_t axis = 0; axis < width; ++axis)
                        {
                            minimum.push_back(low[axis]);
                            maximum.push_back(high[axis]);
                        }
                    }
                    return add_view(data, 34962, 5126, values.size(), width == 3 ? "VEC3" : "VEC2",
                                    minimum, maximum);
                };

                attributes["POSITION"] = vectors(geometry.positions, 3, true, false, "positions");
                attributes["NORMAL"] = vectors(geometry.normals, 3, false, false, "normals");
                if (!geometry.uv0.empty())
                    attributes["TEXCOORD_0"] = vectors(geometry.uv0, 2, false, true, "UVs");
                std::vector<std::byte> index_data;
                index_data.reserve(geometry.indices.size() * sizeof(std::uint32_t));
                for (const auto index : geometry.indices)
                    append_u32_le(index_data, index);
                const auto [low_index, high_index] =
                    std::minmax_element(geometry.indices.begin(), geometry.indices.end());
                const auto index_accessor = add_view(index_data, 34963, 5125, geometry.indices.size(), "SCALAR",
                                                     GlbJson::array({*low_index}), GlbJson::array({*high_index}));
                const auto material_index = materials.size();
                const auto section_name = lod.sections[section_index].name.empty()
                                              ? "section_" + std::to_string(section_index)
                                              : lod.sections[section_index].name;
                materials.push_back({{"name", section_name}});
                primitives.push_back({{"attributes", std::move(attributes)},
                                      {"indices", index_accessor},
                                      {"material", material_index},
                                      {"mode", 4}});
            }
            if (primitives.empty())
                throw std::runtime_error("MeshSet LOD has no triangle geometry");

            const auto json = GlbJson{{"accessors", std::move(accessors)},
                                   {"asset", {{"generator", "ReSkate Studio+ mod dump"}, {"version", "2.0"}}},
                                   {"bufferViews", std::move(buffer_views)},
                                   {"buffers", GlbJson::array({GlbJson{{"byteLength", binary.size()}}})},
                                   {"materials", std::move(materials)},
                                   {"meshes", GlbJson::array({GlbJson{{"name", mesh_name}, {"primitives", std::move(primitives)}}})},
                                   {"nodes", GlbJson::array({GlbJson{{"mesh", 0}, {"name", mesh_name}}})},
                                   {"scene", 0},
                                   {"scenes", GlbJson::array({GlbJson{{"nodes", GlbJson::array({0})}}})}}
                              .dump();
            std::vector<std::byte> json_bytes;
            json_bytes.reserve(json.size() + 3);
            for (const auto ch : json)
                json_bytes.push_back(static_cast<std::byte>(ch));
            while (json_bytes.size() % 4)
                json_bytes.push_back(std::byte{' '});
            while (binary.size() % 4)
                binary.push_back(std::byte{});
            std::vector<std::byte> glb;
            glb.reserve(28 + json_bytes.size() + binary.size());
            glb.insert(glb.end(), {std::byte{'g'}, std::byte{'l'}, std::byte{'T'}, std::byte{'F'}});
            append_u32_le(glb, 2);
            append_u32_le(glb, static_cast<std::uint32_t>(12 + 8 + json_bytes.size() + 8 + binary.size()));
            append_u32_le(glb, static_cast<std::uint32_t>(json_bytes.size()));
            append_u32_le(glb, 0x4E4F534A);
            glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
            append_u32_le(glb, static_cast<std::uint32_t>(binary.size()));
            append_u32_le(glb, 0x004E4942);
            glb.insert(glb.end(), binary.begin(), binary.end());
            write_file(path, glb);
        }

        void export_static_meshes_to_fbx(const std::vector<std::pair<fs::path, fs::path>> &jobs,
                                         fs::path blender,
                                         ModDumpReport &report,
                                         const ModDumpProgress &on_progress,
                                         std::atomic<bool> *cancel)
        {
            if (jobs.empty())
            {
                report.fbx_status = "No MeshSet LODs were available to convert.";
                return;
            }
            if (blender.empty())
                blender = detect_blender();
            std::error_code error;
            if (blender.empty() || !fs::is_regular_file(blender, error))
            {
                report.fbx_status = "Blender is unavailable; static GLBs were kept and FBX files were not created.";
                report.warnings.push_back(report.fbx_status +
                                          " Set Blender with: studio-plus studio set blender <blender.exe>.");
                return;
            }

            static std::atomic<std::uint64_t> next_work_id{0};
            const auto temp_root = fs::temp_directory_path(error);
            if (error)
                throw std::runtime_error("Cannot find a temporary directory for Blender: " + error.message());
            const auto work = temp_root /
                              (L"studio-plus-mod-dump-" +
                               std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"-" +
                               std::to_wstring(next_work_id.fetch_add(1)));
            if (!fs::create_directory(work, error) || error)
                throw std::runtime_error("Cannot create Blender work directory " + path_utf8(work) + ": " +
                                         error.message());
            struct WorkDirectory
            {
                fs::path path;
                ~WorkDirectory()
                {
                    std::error_code ignored;
                    fs::remove_all(path, ignored);
                }
            } cleanup{work};

            nlohmann::json manifest = nlohmann::json::array();
            for (const auto &[glb, fbx] : jobs)
            {
                fs::create_directories(fbx.parent_path(), error);
                if (error)
                    throw std::runtime_error("Cannot create FBX output folder: " + error.message());
                fs::remove(fbx, error);
                if (error)
                    throw std::runtime_error("Cannot replace " + path_utf8(fbx) + ": " + error.message());
                manifest.push_back({{"source", path_utf8(glb)}, {"destination", path_utf8(fbx)}});
            }

            const std::string script =
                "import bpy, json, os, sys, traceback\n"
                "manifest = sys.argv[sys.argv.index('--') + 1]\n"
                "with open(manifest, encoding='utf-8') as f:\n"
                "    jobs = json.load(f)\n"
                "failed = False\n"
                "for job in jobs:\n"
                "    try:\n"
                "        bpy.ops.wm.read_factory_settings(use_empty=True)\n"
                "        bpy.ops.import_scene.gltf(filepath=job['source'])\n"
                "        meshes = [obj for obj in bpy.context.scene.objects if obj.type == 'MESH']\n"
                "        if not meshes:\n"
                "            raise RuntimeError('GLB import produced no mesh objects')\n"
                "        bpy.ops.object.select_all(action='DESELECT')\n"
                "        for obj in meshes:\n"
                "            obj.select_set(True)\n"
                "        bpy.context.view_layer.objects.active = meshes[0]\n"
                "        result = bpy.ops.export_scene.fbx(filepath=job['destination'], use_selection=True, "
                "object_types={'MESH'}, axis_forward='-Z', axis_up='Y', add_leaf_bones=False, bake_anim=False)\n"
                "        if 'FINISHED' not in result or not os.path.isfile(job['destination']):\n"
                "            raise RuntimeError('FBX exporter did not create an output file')\n"
                "        print('RESKATE_FBX_OK:' + job['destination'])\n"
                "    except Exception as exc:\n"
                "        failed = True\n"
                "        print('RESKATE_FBX_ERROR:' + job['destination'] + ': ' + str(exc))\n"
                "        traceback.print_exc()\n"
                "sys.exit(1 if failed else 0)\n";
            const auto script_bytes = std::vector<std::byte>(
                reinterpret_cast<const std::byte *>(script.data()),
                reinterpret_cast<const std::byte *>(script.data() + script.size()));
            const auto manifest_text = manifest.dump();
            const auto manifest_bytes = std::vector<std::byte>(
                reinterpret_cast<const std::byte *>(manifest_text.data()),
                reinterpret_cast<const std::byte *>(manifest_text.data() + manifest_text.size()));
            const auto script_path = work / L"export_static_fbx.py";
            const auto manifest_path = work / L"jobs.json";
            write_file(script_path, script_bytes);
            write_file(manifest_path, manifest_bytes);

            progress(on_progress, "Exporting static MeshSet LODs to FBX with Blender");
            ProcessOptions options;
            options.executable = blender;
            options.args = {"-b", "--factory-startup", "--python", path_utf8(script_path), "--",
                            path_utf8(manifest_path)};
            options.working_dir = work;
            options.cancel = cancel;
            ProcessResult result;
            try
            {
                result = run_process(options);
            }
            catch (const std::exception &failure)
            {
                report.fbx_status = "Blender could not be started; static GLBs were kept.";
                for (const auto &[glb, fbx] : jobs)
                    report.warnings.push_back("FBX " + path_utf8(fbx.filename()) + ": " + failure.what());
                return;
            }
            if (result.cancelled)
                throw Error("cancelled", "Cancelled");

            for (const auto &[glb, fbx] : jobs)
            {
                const auto output_size = fs::file_size(fbx, error);
                if (!error && output_size > 0)
                {
                    ++report.fbxs;
                    continue;
                }
                error.clear();
                std::string detail = "Blender exited with code " + std::to_string(result.exit_code);
                const auto marker = result.stdout_text.find("RESKATE_FBX_ERROR:" + path_utf8(fbx));
                if (marker != std::string::npos)
                {
                    const auto end = result.stdout_text.find_first_of("\r\n", marker);
                    detail = result.stdout_text.substr(marker, end == std::string::npos ? std::string::npos
                                                                                        : end - marker);
                }
                else if (!result.stderr_text.empty())
                {
                    detail += "; " + result.stderr_text.substr(
                                          result.stderr_text.size() > 1024 ? result.stderr_text.size() - 1024 : 0);
                }
                report.warnings.push_back("FBX " + path_utf8(fbx.filename()) + " was not exported: " + detail);
            }
            report.fbx_status =
                "FBX files contain static mesh geometry only; no armature, skin weights, or donor skeleton is included.";
            if (report.fbxs < jobs.size())
                report.fbx_status += " Some conversions failed; see warnings.";
        }

        bool is_inside(const fs::path &candidate, const fs::path &root)
        {
            const auto relative = candidate.lexically_relative(root);
            return !relative.empty() && *relative.begin() != "..";
        }
    } // namespace

    ModDumpReport dump_mod_folder(const fs::path &mod_directory,
                                  const fs::path &game_root,
                                  const fs::path &blender,
                                  const ModDumpProgress &on_progress,
                                  std::atomic<bool> *cancel)
    {
        std::error_code error;
        const auto selected_root = fs::weakly_canonical(mod_directory, error);
        if (error || !fs::is_directory(selected_root))
            throw std::runtime_error("Mod folder does not exist: " + path_utf8(mod_directory));
        auto mod_root = selected_root;
        auto layout_path = mod_root / L"layout.toc";
        if (!fs::is_regular_file(layout_path))
            throw std::runtime_error("Selected mod folder must contain layout.toc at its root");
        mod_root = fs::weakly_canonical(mod_root, error);
        if (error || !is_inside(mod_root, selected_root))
            throw std::runtime_error("Mod folder resolves outside the selected folder");
        layout_path = mod_root / L"layout.toc";
        const auto resolved_layout = fs::weakly_canonical(layout_path, error);
        if (error || !is_inside(resolved_layout, mod_root))
            throw std::runtime_error("layout.toc escapes the selected mod folder");
        layout_path = resolved_layout;
        progress(on_progress, "Reading mod layout.toc");
        const auto layout = dingosdk::vfs::read_layout(layout_path);
        const auto *super_bundles = layout.root.field("superBundles");
        if (!super_bundles || super_bundles->type != 1)
            throw std::runtime_error("layout.toc has no superBundles list");

        const auto output = selected_root / L".dump";
        if (fs::exists(output, error) && fs::is_symlink(fs::symlink_status(output, error)))
            throw std::runtime_error("Refusing to write through a symbolic link at " + path_utf8(output));
        if (error)
            throw std::runtime_error("Cannot inspect dump folder: " + error.message());
        if (fs::is_directory(output, error))
        {
            for (fs::recursive_directory_iterator it(output, error), end; it != end && !error; it.increment(error))
                if (it->is_symlink(error))
                    throw std::runtime_error("Dump folder contains a symbolic link: " + path_utf8(it->path()));
            if (error)
                throw std::runtime_error("Cannot inspect dump folder: " + error.message());
        }
        fs::create_directories(output, error);
        if (error)
            throw std::runtime_error("Cannot create dump folder " + path_utf8(output) + ": " +
                                     error.message());

        ModDumpReport report;
        report.directory = output;
        std::vector<std::pair<fs::path, fs::path>> mesh_fbx_jobs;
        progress(on_progress, "Preserving loose mod files");
        for (fs::recursive_directory_iterator it(selected_root, error), end; it != end && !error; it.increment(error))
        {
            const auto relative = it->path().lexically_relative(selected_root);
            if (!relative.empty() && *relative.begin() == L".dump")
            {
                if (it->is_directory(error))
                    it.disable_recursion_pending();
                continue;
            }
            if (it->is_symlink(error))
                throw std::runtime_error("Mod folder contains a symbolic link: " + path_utf8(relative));
            if (!it->is_regular_file(error) || error)
            {
                error.clear();
                continue;
            }
            auto extension = it->path().extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
            if (extension == L".cas")
                continue;
            try
            {
                write_file(output / L"source" / L"loose" / relative, read_file(it->path()));
                ++report.loose_files;
            }
            catch (const std::exception &failure)
            {
                report.warnings.push_back("Loose file " + path_utf8(relative) + ": " + failure.what());
            }
        }
        if (error)
            throw std::runtime_error("Cannot enumerate mod folder: " + error.message());

        dingosdk::vfs::GameArchives mod_archives(mod_root, layout.root);

        std::unique_ptr<dingosdk::vfs::GameArchives> game_archives;
        std::unique_ptr<dingosdk::vfs::GameData> game_data;
        if (!game_root.empty() && fs::is_regular_file(game_root / L"Data" / L"layout.toc"))
        {
            const auto game_layout = dingosdk::vfs::read_layout(game_root / L"Data" / L"layout.toc");
            game_archives = std::make_unique<dingosdk::vfs::GameArchives>(game_root / L"Data", game_layout.root);
            game_data = std::make_unique<dingosdk::vfs::GameData>(game_root);
        }

        write_file(output / L"source" / L"layout.toc", read_file(layout_path));

        std::set<std::string, std::less<>> seen_tocs;
        std::unordered_set<std::string> written_cas;
        std::map<std::uint64_t, std::string> texture_labels;
        std::map<std::string, nlohmann::json> dumped_resources;
        std::map<std::string, nlohmann::json> dumped_chunks;
        nlohmann::json dumped_assets = nlohmann::json::array();
        nlohmann::json dumped_ebx = nlohmann::json::array();

        for (const auto &row : super_bundles->children)
        {
            const auto *name = row.field("name");
            if (!name || name->type != 7 || name->text.empty())
                continue;
            auto toc_relative = safe_relative(name->text);
            if (lower(toc_relative.extension().string()) != ".toc")
                toc_relative += ".toc";
            const auto toc_key = lower(path_utf8(toc_relative));
            if (!seen_tocs.emplace(toc_key).second)
                continue;
            const auto toc_path = mod_root / toc_relative;
            if (!fs::is_regular_file(toc_path, error))
            {
                error.clear();
                if (game_data)
                {
                    try
                    {
                        (void)game_data->read_toc(path_utf8(toc_relative));
                    }
                    catch (const std::exception &failure)
                    {
                        report.warnings.push_back("TOC is missing from the mod and base game: " +
                                                  path_utf8(toc_relative) + ": " + failure.what());
                    }
                }
                else
                {
                    report.warnings.push_back("TOC is missing from the mod; no base-game root is available: " +
                                              path_utf8(toc_relative));
                }
                continue;
            }
            const auto resolved_toc = fs::weakly_canonical(toc_path, error);
            if (error || !is_inside(resolved_toc, mod_root))
                throw std::runtime_error("TOC path escapes the selected mod folder: " + path_utf8(toc_relative));

            progress(on_progress, "Reading " + path_utf8(toc_relative));
            const auto toc_bytes = read_file(toc_path);
            write_file(output / L"source" / L"tocs" / toc_relative, toc_bytes);
            const auto toc = fb::read_toc(toc_bytes);
            std::optional<fb::TocDocument> base_toc;
            if (game_data)
            {
                try
                {
                    base_toc = game_data->read_toc(path_utf8(toc_relative));
                }
                catch (const std::exception &)
                {
                    // This mod may add a new superbundle.
                }
            }

            for (const auto &toc_bundle : toc.bundles)
            {
                try
                {
                    const auto bundle_name = safe_relative(toc_bundle.name);
                    const auto bundle_key = lower(path_utf8(bundle_name));
                    const auto output_namespace = bundle_namespace(toc_key, bundle_key);
                    progress(on_progress, "Reading bundle " + path_utf8(bundle_name));
                    const auto region = fb::read_bundle_region(toc_bundle.region);
                    dingosdk::vfs::GameBundle bundle;
                    if (!region.inlineManifest.empty())
                    {
                        bundle.manifest = fb::read_binary_bundle(region.inlineManifest);
                        bundle.payloads = region.files;
                        auto manifest_relative = bundle_name;
                        manifest_relative += L".bundle";
                        write_file(output / L"source" / L"manifests" / toc_relative / manifest_relative,
                                   region.inlineManifest);
                    }
                    else
                    {
                        if (region.files.empty())
                            throw std::runtime_error("Bundle has no manifest payload");
                        std::vector<std::byte> manifest_bytes;
                        const auto &manifest_record = region.files.front();
                        try
                        {
                            if (!manifest_record.location.patch && !game_archives)
                                throw std::runtime_error("Bundle manifest belongs to the base game but no game folder is available");
                            const auto &archives = manifest_record.location.patch ? mod_archives : *game_archives;
                            const auto &root = manifest_record.location.patch ? mod_root : game_root / L"Data";
                            manifest_bytes = archives.read(root, manifest_record.location, manifest_record.offset,
                                                           manifest_record.size);
                            if (manifest_record.location.patch)
                            {
                                write_file(output / L"source" / L"cas" / toc_relative / bundle_name / L"manifest.cas",
                                           manifest_bytes);
                                ++report.cas_blocks;
                            }
                            try
                            {
                                bundle.manifest = fb::read_binary_bundle(manifest_bytes);
                            }
                            catch (const std::exception &)
                            {
                                fb::CasDecodeOptions options;
                                options.gameRoot = game_root;
                                manifest_bytes = fb::decode_cas(manifest_bytes, options);
                                bundle.manifest = fb::read_binary_bundle(manifest_bytes);
                            }
                        }
                        catch (const std::exception &failure)
                        {
                            throw std::runtime_error("Cannot read bundle manifest: " + std::string(failure.what()));
                        }
                        auto manifest_relative = bundle_name;
                        manifest_relative += L".bundle";
                        write_file(output / L"source" / L"manifests" / toc_relative / manifest_relative,
                                   manifest_bytes);
                        bundle.payloads.assign(region.files.begin() + 1, region.files.end());
                    }
                    ++report.bundles;

                    std::optional<dingosdk::vfs::GameBundle> base_bundle;
                    if (game_data && base_toc)
                    {
                        try
                        {
                            base_bundle = game_data->read_bundle(*base_toc, toc_bundle.name);
                        }
                        catch (const std::exception &failure)
                        {
                            report.warnings.push_back("Base bundle " + toc_bundle.name + ": " + failure.what());
                        }
                    }

                    auto export_assets = [&](const std::vector<fb::BundleAsset> &assets,
                                             fb::AssetKind kind,
                                             std::string_view extension,
                                             std::size_t &count)
                    {
                        for (std::size_t index = 0; index < assets.size(); ++index)
                        {
                            const auto &asset = assets[index];
                            const auto *payload = bundle.payload(kind, index);
                            if (!payload)
                            {
                                report.warnings.push_back("Missing payload for " + asset.name);
                                continue;
                            }
                            if (!payload->location.patch)
                                continue;
                            try
                            {
                                const auto stored = mod_archives.read(mod_root, payload->location, payload->offset,
                                                                     payload->size);
                                const auto cas_key = toc_key + "/" + bundle_key + "/" +
                                                     std::string(kind_name(kind)) + "/" +
                                                     std::to_string(index);
                                const auto cas_path =
                                    output / L"source" / L"cas" / toc_relative / bundle_name /
                                    (std::string(kind_name(kind)) + "-" + std::to_string(index) + ".cas");
                                if (written_cas.emplace(cas_key).second)
                                {
                                    write_file(cas_path, stored);
                                    ++report.cas_blocks;
                                }
                                fb::CasDecodeOptions options;
                                options.gameRoot = game_root;
                                options.maximumOutputSize = 1024ull * 1024ull * 1024ull;
                                const auto decoded = fb::decode_cas(stored, options);
                                const auto relative = safe_relative(asset.name);
                                auto target =
                                    output / L"assets" /
                                    fs::path(kind == fb::AssetKind::ebx
                                                 ? L"ebx"
                                                 : kind == fb::AssetKind::resource ? L"res" : L"chunks") /
                                    utf8_to_wide(output_namespace) / relative;
                                if (lower(target.extension().string()) != extension)
                                    target += extension;
                                write_file(target, decoded);
                                ++count;
                                nlohmann::json asset_entry = {
                                    {"kind", std::string(kind_name(kind))},
                                    {"name", asset.name},
                                    {"file", path_utf8(target.lexically_relative(output))},
                                    {"cas",
                                     {{"file", path_utf8(cas_path.lexically_relative(output))},
                                      {"patch", payload->location.patch},
                                      {"install_chunk", payload->location.installChunk},
                                      {"archive", payload->location.archive},
                                      {"offset", payload->offset},
                                      {"size", payload->size}}},
                                    {"toc", path_utf8(toc_relative)},
                                    {"bundle", path_utf8(bundle_name)}};
                                if (kind == fb::AssetKind::ebx)
                                {
                                    asset_entry["sha1"] = raw_sha1_hex(decoded);
                                    asset_entry["manifest_sha1"] = sha1_hex(asset.sha1);
                                }
                                if (kind == fb::AssetKind::resource && asset.resourceId)
                                {
                                    asset_entry["resource_id"] = std::to_string(asset.resourceId);
                                    dumped_resources[std::to_string(asset.resourceId)] = asset_entry;
                                }
                                else if (kind == fb::AssetKind::chunk)
                                {
                                    asset_entry["guid"] = asset.guid.string();
                                    dumped_chunks[asset.guid.string()] = asset_entry;
                                }
                                if (kind == fb::AssetKind::ebx)
                                {
                                    nlohmann::json references = nlohmann::json::array();
                                    try
                                    {
                                        std::set<std::uint64_t> refs;
                                        std::set<fb::Guid> chunk_refs;
                                        collect_document_references(decoded, refs, chunk_refs);
                                        for (const auto id : refs)
                                        {
                                            texture_labels.try_emplace(id, asset.name);
                                            references.push_back({{"kind", "resource"},
                                                                  {"id", std::to_string(id)}});
                                        }
                                        for (const auto &id : chunk_refs)
                                            references.push_back({{"kind", "chunk"}, {"id", id.string()}});
                                    }
                                    catch (const std::exception &failure)
                                    {
                                        report.warnings.push_back("EBX references unavailable for " + asset.name +
                                                                  ": " + failure.what());
                                    }
                                    asset_entry["references"] = std::move(references);
                                    dumped_ebx.push_back(asset_entry);
                                }
                                else
                                    dumped_assets.push_back(std::move(asset_entry));
                            }
                            catch (const std::exception &failure)
                            {
                                report.warnings.push_back("Could not export " + asset.name + ": " + failure.what());
                            }
                        }
                    };

                    export_assets(bundle.manifest.ebx, fb::AssetKind::ebx, ".ebx", report.ebx);
                    export_assets(bundle.manifest.resources, fb::AssetKind::resource, ".res", report.resources);
                    export_assets(bundle.manifest.chunks, fb::AssetKind::chunk, ".chunk", report.chunks);

                    for (const auto &resource : bundle.manifest.resources)
                    {
                        if (!resource.resourceId || resource.resourceType != fb::texture_resource_type)
                            continue;
                        try
                        {
                            const auto resource_it = std::find_if(bundle.manifest.resources.begin(),
                                                                  bundle.manifest.resources.end(),
                                                                  [&](const fb::BundleAsset &entry)
                                                                  { return entry.resourceId == resource.resourceId; });
                            const auto resource_index =
                                static_cast<std::size_t>(std::distance(bundle.manifest.resources.begin(), resource_it));
                            const auto *resource_payload = bundle.payload(fb::AssetKind::resource, resource_index);
                            if (!resource_payload)
                                throw std::runtime_error("Texture resource payload is missing");
                            const auto header_bytes = read_payload(*resource_payload, mod_root, game_root, mod_archives,
                                                                  game_archives.get());
                            const auto header = fb::read_texture_header(header_bytes, resource.resourceMeta);

                            std::vector<std::byte> pixels;
                            std::size_t pixels_start{};
                            std::size_t chunk_index{};
                            const auto toc_chunk = std::find_if(
                                toc.chunks.begin(), toc.chunks.end(), [&](const fb::TocChunk &chunk)
                                { return chunk.guid == header.chunk && !chunk.removed && chunk.location.patch; });
                            if (toc_chunk != toc.chunks.end())
                            {
                                fb::BundleFileInfo info{toc_chunk->location, toc_chunk->offset, toc_chunk->size};
                                pixels = read_payload(info, mod_root, game_root, mod_archives, game_archives.get());
                            }
                            if (pixels.empty() && bundle.find_chunk(header.chunk, &chunk_index))
                            {
                                if (const auto *chunk_payload = bundle.payload(fb::AssetKind::chunk, chunk_index))
                                {
                                    pixels = read_payload(*chunk_payload, mod_root, game_root, mod_archives,
                                                          game_archives.get());
                                    pixels_start =
                                        bundle.manifest.chunks[chunk_index].logicalOffset & 0xFFFF0000u;
                                }
                            }
                            if (pixels.empty() && base_bundle)
                            {
                                if (base_bundle->find_chunk(header.chunk, &chunk_index))
                                    if (const auto *payload =
                                            base_bundle->payload(fb::AssetKind::chunk, chunk_index))
                                    {
                                        pixels = game_data->read(*payload);
                                        pixels_start =
                                            base_bundle->manifest.chunks[chunk_index].logicalOffset & 0xFFFF0000u;
                                    }
                            }
                            if (pixels.empty())
                                throw std::runtime_error("Texture pixel chunk is not available in the mod or base bundle");
                            Texture texture;
                            texture.header = header;
                            texture.pixels = std::move(pixels);
                            texture.pixels_start = pixels_start;
                            const auto dds = encode_dds(texture);
                            const auto label = texture_labels.contains(resource.resourceId)
                                                   ? texture_labels.at(resource.resourceId)
                                                   : resource.name;
                            auto ebx_relative = safe_relative(label);
                            ebx_relative.replace_extension();
                            auto resource_relative = safe_relative(resource.name).filename().stem();
                            resource_relative += L"__" + std::to_wstring(resource.resourceId) + L".dds";
                            write_file(output / L"assets" / L"textures" / utf8_to_wide(output_namespace) /
                                           ebx_relative.filename() / resource_relative,
                                       dds);
                            ++report.textures;
                        }
                        catch (const std::exception &failure)
                        {
                            report.warnings.push_back("Texture " + resource.name + " was not converted: " +
                                                      failure.what());
                        }
                    }

                    constexpr std::uint32_t mesh_set_type = 0x49B156D4;
                    for (std::size_t resource_index = 0;
                         resource_index < bundle.manifest.resources.size(); ++resource_index)
                    {
                        const auto &resource = bundle.manifest.resources[resource_index];
                        if (resource.resourceType != mesh_set_type)
                            continue;
                        const auto *resource_payload = bundle.payload(fb::AssetKind::resource, resource_index);
                        if (!resource_payload || !resource_payload->location.patch)
                            continue;
                        try
                        {
                            auto mesh = parse_mesh_set(read_payload(*resource_payload, mod_root, game_root,
                                                                    mod_archives, game_archives.get()));
                            for (std::size_t lod_index = 0; lod_index < mesh.lods.size(); ++lod_index)
                            {
                                const auto &lod = mesh.lods[lod_index];
                                std::vector<std::byte> geometry_chunk;
                                std::size_t chunk_index{};
                                const auto toc_chunk = std::find_if(
                                    toc.chunks.begin(), toc.chunks.end(), [&](const fb::TocChunk &chunk)
                                    { return chunk.guid == lod.chunk && !chunk.removed && chunk.location.patch; });
                                if (toc_chunk != toc.chunks.end())
                                {
                                    fb::BundleFileInfo payload{toc_chunk->location, toc_chunk->offset, toc_chunk->size};
                                    geometry_chunk =
                                        read_payload(payload, mod_root, game_root, mod_archives, game_archives.get());
                                }
                                else if (bundle.find_chunk(lod.chunk, &chunk_index))
                                {
                                    if (const auto *payload = bundle.payload(fb::AssetKind::chunk, chunk_index))
                                        geometry_chunk =
                                            read_payload(*payload, mod_root, game_root, mod_archives, game_archives.get());
                                }
                                if (geometry_chunk.empty() && base_bundle &&
                                    base_bundle->find_chunk(lod.chunk, &chunk_index))
                                {
                                    if (const auto *payload =
                                            base_bundle->payload(fb::AssetKind::chunk, chunk_index))
                                        geometry_chunk = game_data->read(*payload);
                                }
                                if (geometry_chunk.empty())
                                    throw std::runtime_error("LOD " + std::to_string(lod_index) +
                                                             " geometry chunk is unavailable");
                                const auto sections = read_lod_geometry(mesh, lod_index, geometry_chunk);
                                auto relative = safe_relative(resource.name);
                                relative += L"_lod" + std::to_wstring(lod_index) + L".glb";
                                const auto glb_path =
                                    output / L"assets" / L"meshes" / utf8_to_wide(output_namespace) / relative;
                                write_mesh_glb(glb_path, sections, lod, resource.name);
                                auto fbx_path = glb_path;
                                fbx_path.replace_extension(L".fbx");
                                mesh_fbx_jobs.emplace_back(glb_path, fbx_path);
                                ++report.meshes;
                            }
                        }
                        catch (const std::exception &failure)
                        {
                            report.warnings.push_back("Mesh " + resource.name + " was not converted: " +
                                                      failure.what());
                        }
                    }
                }
                catch (const std::exception &failure)
                {
                    report.warnings.push_back("Bundle " + toc_bundle.name + ": " + failure.what());
                }
            }

            for (std::size_t index = 0; index < toc.chunks.size(); ++index)
            {
                const auto &chunk = toc.chunks[index];
                if (chunk.removed || !chunk.location.patch)
                    continue;
                try
                {
                    const auto stored = mod_archives.read(mod_root, chunk.location, chunk.offset, chunk.size);
                    const auto key = toc_key + "/toc-chunk/" + std::to_string(index);
                    const auto cas_path = output / L"source" / L"cas" / toc_relative / L"toc-chunk" /
                                          (std::to_string(index) + ".cas");
                    if (written_cas.emplace(key).second)
                    {
                        write_file(cas_path, stored);
                        ++report.cas_blocks;
                    }
                    fb::CasDecodeOptions options;
                    options.gameRoot = game_root;
                    options.maximumOutputSize = 1024ull * 1024ull * 1024ull;
                    const auto decoded = fb::decode_cas(stored, options);
                    const auto target = output / L"assets" / L"chunks" / (chunk.guid.string() + ".chunk");
                    write_file(target, decoded);
                    ++report.chunks;
                    const auto guid = chunk.guid.string();
                    auto asset_entry = nlohmann::json{
                        {"kind", "chunk"},
                        {"name", guid},
                        {"guid", guid},
                        {"file", path_utf8(target.lexically_relative(output))},
                        {"cas",
                         {{"file", path_utf8(cas_path.lexically_relative(output))},
                          {"patch", chunk.location.patch},
                          {"install_chunk", chunk.location.installChunk},
                          {"archive", chunk.location.archive},
                          {"offset", chunk.offset},
                          {"size", chunk.size}}},
                        {"toc", path_utf8(toc_relative)}};
                    dumped_assets.push_back(asset_entry);
                    dumped_chunks[guid] = std::move(asset_entry);
                }
                catch (const std::exception &failure)
                {
                    report.warnings.push_back("TOC chunk " + chunk.guid.string() + ": " + failure.what());
                }
            }
        }

        export_static_meshes_to_fbx(mesh_fbx_jobs, blender, report, on_progress, cancel);
        report.ebx_asset_references = 0;
        report.resolved_ebx_asset_references = 0;
        for (auto &ebx : dumped_ebx)
            for (auto &reference : ebx["references"])
            {
                ++report.ebx_asset_references;
                const auto kind = reference["kind"].get<std::string>();
                const auto id = reference["id"].get<std::string>();
                const auto &targets = kind == "resource" ? dumped_resources : dumped_chunks;
                const auto found = targets.find(id);
                reference["resolved"] = found != targets.end();
                if (found != targets.end())
                {
                    reference["target"] = found->second;
                    ++report.resolved_ebx_asset_references;
                }
            }
        const nlohmann::json reference_index = {
            {"version", 1},
            {"ebx", dumped_ebx},
            {"assets", dumped_assets}};
        const auto reference_text = reference_index.dump(2) + "\n";
        const auto reference_bytes = std::vector<std::byte>(
            reinterpret_cast<const std::byte *>(reference_text.data()),
            reinterpret_cast<const std::byte *>(reference_text.data() + reference_text.size()));
        write_file(output / L"asset-references.json", reference_bytes);
        const std::string capabilities =
            "Supported conversions: MeshSet LODs to static .glb files (positions, reconstructed normals, UV0 and "
            "material sections; skin weights are not exported), optional static .fbx files generated from those GLBs "
            "with Blender, and Texture resources to native-format .dds files (all available mips and supported "
            "slices).\n"
            "Meshes with unsupported layouts, audio, levels, unsupported texture formats and other resources are "
            "retained as decoded raw .ebx/.res/.chunk files when available. FBX files contain static geometry only; "
            "the current mod reader does not resolve donor assets or skeleton hierarchy/bind poses, so no armature "
            "or skin weights are included. Audio transcoding is not implemented.\n"
            "Decoded assets are grouped under stable short per-bundle folders. asset-references.json maps each raw "
            "EBX SHA-1 and resource/chunk reference to any matching dumped asset and its stored CAS range, including "
            "the source archive, offset and size. Stored CAS entry ranges (not whole archives) are in source/cas; "
            "bundle payload filenames use their kind and zero-based manifest index. Source TOCs and manifests are "
            "in source/tocs and source/manifests; other loose mod files are copied to source/loose.\n";
        const auto capability_bytes = std::vector<std::byte>(
            reinterpret_cast<const std::byte *>(capabilities.data()),
            reinterpret_cast<const std::byte *>(capabilities.data() + capabilities.size()));
        write_file(output / L"README.txt", capability_bytes);
        std::ostringstream summary;
        summary << "Mod dump: " << path_utf8(selected_root) << "\n"
                << "Bundles: " << report.bundles << "\n"
                << "EBX files: " << report.ebx << "\n"
                << "RES files: " << report.resources << "\n"
                << "Chunks: " << report.chunks << "\n"
                << "EBX non-EBX asset references: " << report.ebx_asset_references << "\n"
                << "Resolved EBX non-EBX asset references: " << report.resolved_ebx_asset_references << "\n"
                << "Stored CAS payload ranges: " << report.cas_blocks << "\n"
                << "DDS textures: " << report.textures << "\n"
                << "Static mesh LODs exported as GLB: " << report.meshes << "\n"
                << "Static mesh LODs exported as FBX: " << report.fbxs << "\n"
                << "FBX status: " << report.fbx_status << "\n"
                << "Loose files: " << report.loose_files << "\n"
                << "Warnings: " << report.warnings.size() << "\n";
        for (const auto &warning : report.warnings)
            summary << "- " << warning << "\n";
        const auto summary_text = summary.str();
        const auto summary_bytes = std::vector<std::byte>(
            reinterpret_cast<const std::byte *>(summary_text.data()),
            reinterpret_cast<const std::byte *>(summary_text.data() + summary_text.size()));
        write_file(output / L"dump-report.txt", summary_bytes);
        progress(on_progress, "Mod dump complete");
        return report;
    }
} // namespace studio::native
