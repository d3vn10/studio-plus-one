// The asset browser's commands, on Studio+'s own reader of the game's data (src/native, built on
// ReSkate's engine code) instead of reskate_cli: asset index / search / tree / info / types,
// ebx get, texture export, asset export-raw and asset lua-source.
#include "commands/commands.h"
#include "core/settings.h"
#include "native/asset_index.h"
#include "native/asset_views.h"
#include "native/game_assets.h"
#include "native/mod_files.h"
#include "Engine/Resource/ebx_document.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace studio
{
    namespace
    {

        using native::AssetIndex;
        using native::Entry;
        using native::Kind;

        constexpr const char *k_index_note = " Uses the asset index (build it once with 'asset index'; it is cached in "
                                             "the data folder for this game build).";

        std::string lower(std::string text)
        {
            for (auto &c : text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c + ('a' - 'A'));
            return text;
        }

        double seconds_since(std::chrono::steady_clock::time_point start)
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }

        std::shared_ptr<const AssetIndex> index_for(Context &c, const Json &a)
        {
            return native::load_index(c, c.game_root(a));
        }

        Param kind_param(const char *fallback, bool with_all)
        {
            std::vector<std::string> choices{"ebx", "res", "chunk"};
            if (with_all)
                choices.insert(choices.begin(), "all");
            return {"kind",
                    ParamType::Enum,
                    "Asset kind: ebx (EBX asset), res (resource) or chunk" + std::string(with_all ? ", or all" : ""),
                    false,
                    false,
                    choices,
                    Json(fallback)};
        }

        Param category_param()
        {
            std::vector<std::string> choices;
            for (auto name : native::category_names())
                choices.emplace_back(name);
            return {"category",
                    ParamType::Enum,
                    "Only assets of this group of types: texture, mesh, lua, level, shader, animation, "
                    "audio, video, blueprint or other",
                    false,
                    false,
                    choices};
        }

        Kind entry_kind(const Json &a)
        {
            const auto kind = native::kind_from(a.value("kind", "ebx"));
            return kind ? *kind : Kind::ebx;
        }

        // One entry as the commands return it.
        Json entry_json(const AssetIndex &index, const Entry &e)
        {
            Json out = {{"name", index.name(e)},
                        {"kind", native::kind_name(e.kind)},
                        {"type", index.type_name(e)},
                        {"category", native::category_name(index.category(e))},
                        {"size", e.size}};
            if (e.bundle_count > 1)
                out["bundles"] = e.bundle_count;
            return out;
        }

        const Entry &require_entry(const AssetIndex &index, Kind kind, const std::string &name)
        {
            const Entry *e = index.find(kind, name);
            if (!e)
                throw Error("asset_not_found",
                            "No " + std::string(native::kind_name(kind)) + " asset named '" + name +
                                "'. Find names with: studio-plus asset search <text>",
                            {{"name", name}, {"kind", native::kind_name(kind)}});
            return *e;
        }

        // What a filter asks for, checked once per entry.
        struct Filter
        {
            std::optional<Kind> kind;
            std::optional<native::Category> category;
            std::string type;                    // exact, case-insensitive
            std::vector<std::string> words;      // all in the name
            std::vector<std::string> type_words; // "*word": all in the type name
            std::string folder;                  // the name starts with this

            static Filter from(const Json &a, bool search)
            {
                Filter f;
                const std::string kind = a.value("kind", "all");
                if (kind != "all")
                    f.kind = native::kind_from(kind);
                if (a.contains("category") && a["category"].is_string())
                    f.category = native::category_from(a["category"].get<std::string>());
                if (a.contains("type") && a["type"].is_string())
                    f.type = lower(a["type"].get<std::string>());
                if (search)
                {
                    if (a.contains("folder") && a["folder"].is_string())
                        f.folder = lower(a["folder"].get<std::string>());
                    std::string word;
                    const std::string text = lower(a.value("text", "")) + " ";
                    for (char ch : text)
                    {
                        if (ch != ' ')
                        {
                            word.push_back(ch);
                            continue;
                        }
                        if (word.size() > 1 && word[0] == '*')
                            f.type_words.push_back(word.substr(1));
                        else if (!word.empty() && word != "*")
                            f.words.push_back(word);
                        word.clear();
                    }
                }
                return f;
            }

            bool type_ok(const AssetIndex &index, const Entry &e) const
            {
                if (category && index.category(e) != *category)
                    return false;
                if (!type.empty() || !type_words.empty())
                {
                    const std::string t = lower(index.type_name(e));
                    if (!type.empty() && t != type)
                        return false;
                    for (const auto &w : type_words)
                        if (t.find(w) == std::string::npos)
                            return false;
                }
                return true;
            }

            bool matches(const AssetIndex &index, const Entry &e) const
            {
                if (kind && e.kind != *kind)
                    return false;
                const auto name = index.lower_name(e);
                if (!folder.empty() && !name.starts_with(folder))
                    return false;
                for (const auto &w : words)
                    if (name.find(w) == std::string_view::npos)
                        return false;
                return type_ok(index, e);
            }
        };

        // The run of entries whose names start with `prefix`.
        std::pair<std::size_t, std::size_t> prefix_range(const AssetIndex &index, std::string_view prefix)
        {
            const auto &entries = index.entries;
            auto first = std::lower_bound(entries.begin(),
                                          entries.end(),
                                          prefix,
                                          [&](const Entry &e, std::string_view p) { return index.name(e) < p; });
            auto last = first;
            if (prefix.empty())
                last = entries.end();
            else
                last = std::find_if(
                    first, entries.end(), [&](const Entry &e) { return !index.name(e).starts_with(prefix); });
            return {static_cast<std::size_t>(first - entries.begin()),
                    static_cast<std::size_t>(last - entries.begin())};
        }

        Json index_summary(const AssetIndex &index, const native::CacheKey &key)
        {
            std::error_code ec;
            return {{"ebx", index.count(Kind::ebx)},
                    {"res", index.count(Kind::res)},
                    {"chunks", index.count(Kind::chunk)},
                    {"assets", index.entries.size()},
                    {"bundles", index.bundles.size()},
                    {"superbundles", index.superbundles},
                    {"types", index.types.size() - 1},
                    {"type_failures", index.type_failures},
                    {"build_seconds", std::round(index.build_seconds * 10) / 10},
                    {"built_at", index.built_at},
                    {"skate_sha256", key.skate_sha256},
                    {"cache", path_utf8(key.file)},
                    {"cache_bytes", static_cast<std::uint64_t>(fs::file_size(key.file, ec))}};
        }

    } // namespace

    void register_browse_commands(Registry &r)
    {
        r.add({
            .group = "asset",
            .name = "index",
            .summary = "Build or load the asset index used by asset search, tree and the Assets page",
            .description = "Reads every superbundle TOC and bundle manifest in the game's Data folder and the root "
                           "type of every EBX "
                           "asset, and saves the result in the data folder (asset-index\\assets-<build>.bin), keyed by "
                           "the Skate.exe "
                           "SHA-256 and the TOC files. A valid cache is loaded instead of rebuilt (well under a "
                           "second); --refresh "
                           "rebuilds it anyway. A full build reads every EBX payload once and takes a minute or two, "
                           "with progress. "
                           "Reads the game only; the Patch layer is not indexed.",
            .params =
                {
                    {"refresh",
                     ParamType::Boolean,
                     "Rebuild even when the cache is valid",
                     false,
                     false,
                     {},
                     Json(false)},
                    game_root_param(),
                },
            .examples = {"asset index", "asset index --refresh --json"},
            .long_running = true,
            .run = [](Context &c, const Json &a) -> Json
            {
                const auto root = c.game_root(a);
                bool from_cache = false;
                const auto index = native::build_index(c, root, a.value("refresh", false), &from_cache);
                Json out = index_summary(*index, native::cache_key(root));
                out["from_cache"] = from_cache;
                return out;
            },
        });

        r.add({
            .group = "asset",
            .name = "search",
            .summary = "Search the asset index by name, type, kind and folder",
            .description =
                std::string(
                    "Every word of the text must be in the asset name (case-insensitive); a word starting with * must "
                    "be in "
                    "the type name instead (e.g. 'popsicle *texture'). Filters: --kind, --category, --type (exact type "
                    "name) and --folder (the name starts with it). Results are in name order, paged with --offset and "
                    "--limit; total is the full match count. Empty text lists everything the filters allow.") +
                k_index_note,
            .params =
                {
                    {"text", ParamType::String, "Words in the name; *word for the type", false, true, {}, Json("")},
                    kind_param("all", true),
                    category_param(),
                    {"type", ParamType::String, "Exact type name, e.g. TextureAsset"},
                    {"folder", ParamType::String, "Only names starting with this, e.g. characters/"},
                    {"offset", ParamType::Integer, "Matches to skip (paging)", false, false, {}, Json(0), 0},
                    {"limit", ParamType::Integer, "Most matches to return", false, false, {}, Json(100), 1},
                    game_root_param(),
                },
            .examples = {"asset search popsicle",
                         "asset search \"deckgraphic *texture\" --limit 20",
                         "asset search --category level --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                const auto start = std::chrono::steady_clock::now();
                const auto index = index_for(c, a);
                const auto filter = Filter::from(a, true);
                const auto offset = a.value("offset", 0ll), limit = a.value("limit", 100ll);
                auto [first, last] = prefix_range(*index, filter.folder);
                long long total = 0;
                Json matches = Json::array();
                for (std::size_t i = first; i < last; ++i)
                {
                    const auto &e = index->entries[i];
                    if (!filter.matches(*index, e))
                        continue;
                    if (total >= offset && total < offset + limit)
                        matches.push_back(entry_json(*index, e));
                    ++total;
                }
                return {{"total", total},
                        {"offset", offset},
                        {"count", matches.size()},
                        {"matches", matches},
                        {"seconds", std::round(seconds_since(start) * 1000) / 1000}};
            },
        });

        r.add({
            .group = "asset",
            .name = "tree",
            .summary = "The folders and assets directly inside one folder of the asset index",
            .description =
                std::string(
                    "Asset names are paths (characters/lua/skaterlualoaderasset). This lists the sub-folders of --path "
                    "with how "
                    "many assets each holds, and the assets directly in it (paged with --offset and --limit). An empty "
                    "path is "
                    "the top level. --kind (default ebx), --category and --type filter what is counted and listed.") +
                k_index_note,
            .params =
                {
                    {"path",
                     ParamType::String,
                     "Folder, e.g. characters/lua (no trailing slash needed); empty for the top",
                     false,
                     true,
                     {},
                     Json("")},
                    kind_param("ebx", true),
                    category_param(),
                    {"type", ParamType::String, "Exact type name, e.g. TextureAsset"},
                    {"offset", ParamType::Integer, "Assets to skip (paging)", false, false, {}, Json(0), 0},
                    {"limit", ParamType::Integer, "Most assets to return", false, false, {}, Json(500), 1},
                    game_root_param(),
                },
            .examples = {"asset tree", "asset tree characters/lua", "asset tree items --kind all --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                const auto start = std::chrono::steady_clock::now();
                const auto index = index_for(c, a);
                auto filter = Filter::from(a, false);
                std::string path = a.value("path", "");
                while (!path.empty() && path.back() == '/')
                    path.pop_back();
                const std::string prefix = path.empty() ? "" : path + "/";
                const auto [first, last] = prefix_range(*index, prefix);
                const auto offset = a.value("offset", 0ll), limit = a.value("limit", 500ll);
                Json folders = Json::array(), assets = Json::array();
                long long asset_total = 0, total = 0;
                std::string folder;
                long long folder_count = 0;
                auto flush = [&]
                {
                    if (folder_count)
                        folders.push_back({{"name", folder}, {"path", prefix + folder}, {"count", folder_count}});
                    folder_count = 0;
                };
                for (std::size_t i = first; i < last; ++i)
                {
                    const auto &e = index->entries[i];
                    if (!filter.matches(*index, e))
                        continue;
                    ++total;
                    const auto rest = index->name(e).substr(prefix.size());
                    const auto slash = rest.find('/');
                    if (slash == std::string_view::npos)
                    {
                        if (asset_total >= offset && asset_total < offset + limit)
                        {
                            Json item = entry_json(*index, e);
                            item["label"] = rest;
                            assets.push_back(std::move(item));
                        }
                        ++asset_total;
                        continue;
                    }
                    const auto name = rest.substr(0, slash);
                    if (name != folder)
                    {
                        flush();
                        folder = std::string(name);
                    }
                    ++folder_count;
                }
                flush();
                return {{"path", path},
                        {"total", total},
                        {"folders", folders},
                        {"asset_total", asset_total},
                        {"offset", offset},
                        {"assets", assets},
                        {"seconds", std::round(seconds_since(start) * 1000) / 1000}};
            },
        });

        r.add({
            .group = "asset",
            .name = "info",
            .summary = "One asset from the index: type, sizes, bundle, ids and what can be shown for it",
            .description =
                std::string(
                    "Everything the index knows about one asset: type, decoded and stored size, the first bundle and "
                    "superbundle carrying it and how many bundles do, where its payload is, its guid or resource id, "
                    "assets of "
                    "other kinds with the same name (a TextureAsset and its Texture resource), and views: which of "
                    "properties (ebx get), texture (texture export) and lua (asset lua-source) apply.") +
                k_index_note,
            .params =
                {
                    {"name", ParamType::String, "Asset name, e.g. characters/lua/skaterlualoaderasset", true, true},
                    kind_param("ebx", false),
                    game_root_param(),
                },
            .examples = {"asset info characters/lua/skaterlualoaderasset",
                         "asset info systems/render/textures/common_white --kind res"},
            .run = [](Context &c, const Json &a) -> Json
            {
                const auto index = index_for(c, a);
                const auto &e = require_entry(*index, entry_kind(a), a["name"].get<std::string>());
                return native::describe_entry(*index, e, *native::game_files(c.game_root(a)));
            },
        });

        r.add({
            .group = "asset",
            .name = "types",
            .summary = "Every asset type in the index with how many assets have it",
            .description = std::string("The type names in the index (EBX root types, resource types and Chunk), most "
                                       "common first, each with "
                                       "its count, kind and category. --text keeps the types whose name contains it.") +
                           k_index_note,
            .params =
                {
                    {"text", ParamType::String, "Part of the type name", false, true, {}, Json("")},
                    kind_param("all", true),
                    category_param(),
                    game_root_param(),
                },
            .examples = {"asset types", "asset types mesh --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                const auto index = index_for(c, a);
                const auto filter = Filter::from(a, false);
                const std::string text = lower(a.value("text", ""));
                std::vector<long long> counts(index->types.size());
                std::vector<int> kinds(index->types.size(), -1);
                for (const auto &e : index->entries)
                {
                    if (filter.kind && e.kind != *filter.kind)
                        continue;
                    ++counts[e.type];
                    kinds[e.type] = static_cast<int>(e.kind);
                }
                std::vector<std::size_t> order;
                for (std::size_t t = 1; t < index->types.size(); ++t)
                {
                    if (!counts[t])
                        continue;
                    if (filter.category && index->types[t].category != *filter.category)
                        continue;
                    if (!text.empty() && lower(index->types[t].name).find(text) == std::string::npos)
                        continue;
                    order.push_back(t);
                }
                std::sort(order.begin(),
                          order.end(),
                          [&](std::size_t x, std::size_t y)
                          {
                              return counts[x] != counts[y] ? counts[x] > counts[y]
                                                            : index->types[x].name < index->types[y].name;
                          });
                Json types = Json::array();
                for (auto t : order)
                    types.push_back({{"type", index->types[t].name},
                                     {"count", counts[t]},
                                     {"kind", native::kind_name(static_cast<Kind>(kinds[t]))},
                                     {"category", native::category_name(index->types[t].category)}});
                return {{"count", types.size()}, {"types", types}};
            },
        });

        r.add({
            .group = "ebx",
            .name = "get",
            .summary = "One EBX asset as structured JSON, from Studio+'s own decoder",
            .description =
                std::string(
                    "Decodes the EBX asset and returns its root type, guid, imports (with the asset each one names) "
                    "and its "
                    "instances: type, guid, exported, and fields by name. Structs carry \"$type\"; pointers are "
                    "{\"$ref\": instance} or {\"$import\": import, \"asset\": name}; resource references are "
                    "{\"$resource\": id, \"name\": resource}; enums are their member name. --instance returns one "
                    "instance; "
                    "otherwise the first --limit. Arrays longer than --max-array end with {\"$more\": n}. Reads "
                    "only.") +
                k_index_note,
            .params =
                {
                    {"name", ParamType::String, "EBX asset name, e.g. characters/lua/luafacecompositor", true, true},
                    {"instance",
                     ParamType::Integer,
                     "Only this instance (0 is the root)",
                     false,
                     false,
                     {},
                     std::nullopt,
                     0},
                    {"limit", ParamType::Integer, "Most instances to return", false, false, {}, Json(200), 1},
                    {"max-array", ParamType::Integer, "Most elements shown per array", false, false, {}, Json(512), 0},
                    game_root_param(),
                },
            .examples = {"ebx get characters/lua/luafacecompositor",
                         "ebx get common/textures/default/neutral/neutral_c --json",
                         "ebx get gameplay/somelevel --instance 0 --max-array 20 --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                const auto index = index_for(c, a);
                const auto &e = require_entry(*index, Kind::ebx, a["name"].get<std::string>());
                native::EbxOptions options;
                if (a.contains("instance") && a["instance"].is_number_integer())
                    options.instance = a["instance"].get<long long>();
                options.limit = a.value("limit", 200ll);
                options.max_array = a.value("max-array", 512ll);
                return native::ebx_json(*index, e, *native::game_files(c.game_root(a)), options);
            },
        });

        r.add({
            .group = "ebx",
            .name = "set",
            .summary = "Edit fields of one EBX asset into a .fbproject, .fbmod or loose .ebx",
            .description = std::string("Reads the EBX asset, changes the fields given with --set Path=Value (give it "
                                       "once per field), writes the "
                                       "result with ReSkate's EBX writer and reads it back to check every change "
                                       "landed; nothing is saved if one "
                                       "did not. Paths are field names joined with dots, array items as [n], and an "
                                       "optional leading [n] for an "
                                       "instance other than the root: Name, [3].Enabled, Transform.trans.x, Items[0]. "
                                       "Booleans, numbers, enum "
                                       "member names and strings can be set.\n\n"
                                       "--output decides what is written: a .fbproject gets the edited asset as a "
                                       "change to the game (an existing "
                                       "project keeps everything else it holds, and an earlier edit of the same asset "
                                       "is replaced, so several "
                                       "edits collect in one project); an .fbmod is written new with just this asset, "
                                       "ready for 'mod compile'; "
                                       "a .ebx is the loose file. The game is not touched. Edits build on the game's "
                                       "asset, not on an earlier "
                                       "edit in the project: give every field for one asset in the same run. "
                                       "roundtrip_identical says whether "
                                       "the untouched asset writes back byte for byte.") +
                           k_index_note,
            .params =
                {
                    {"name", ParamType::String, "EBX asset name", true, true},
                    {"set", ParamType::List, "Path=Value, e.g. Name=MyAsset or [2].Enabled=false", true},
                    {"output",
                     ParamType::Path,
                     "File to write: .fbproject (added to if it exists), .fbmod or .ebx",
                     true},
                    game_root_param(),
                },
            .examples = {"ebx set characters/lua/luafacecompositor --set Name=Characters/Lua/Mine --output mine.ebx",
                         "ebx set <asset> --set [2].Enabled=false --output C:\\mods\\tweaks.fbproject"},
            .run = [](Context &c, const Json &a) -> Json
            {
                const auto index = index_for(c, a);
                const auto &e = require_entry(*index, Kind::ebx, a["name"].get<std::string>());
                std::vector<native::EbxEdit> edits;
                for (const auto &item : a["set"])
                {
                    const std::string text = item.get<std::string>();
                    const auto eq = text.find('=');
                    if (eq == std::string::npos || eq == 0)
                        throw Error(
                            "invalid_arguments", "--set takes Path=Value, got '" + text + "'", {{"param", "set"}});
                    edits.push_back({text.substr(0, eq), text.substr(eq + 1)});
                }
                const fs::path output = fs::absolute(fs::path(utf8_to_wide(a["output"].get<std::string>())));
                std::wstring ext = output.extension().native();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
                if (ext != L".fbproject" && ext != L".fbmod" && ext != L".ebx")
                    throw Error("bad_output",
                                "--output must end in .fbproject, .fbmod or .ebx: " + path_utf8(output),
                                {{"param", "output"}});
                const fs::path root = c.game_root(a);
                refuse_game_folder(c, root, output, "output");
                if (ext == L".ebx")
                    return native::ebx_set(*index, e, *native::game_files(root), edits, output);

                std::vector<std::byte> written;
                Json out = native::ebx_set(*index, e, *native::game_files(root), edits, output, &written);
                const std::string name(index->name(e));
                native::ModResource res;
                res.kind = native::ModResource::Kind::ebx;
                res.name = name;
                res.data = written;
                res.id = dingosdk::frostbite::ebx::read_root_info(written).fileGuid;
                c.progress(-1, "Finding the bundles " + name + " is in");
                native::GameAssets game(root);
                game.on_progress = [&c](std::string_view message) { c.progress(-1, message); };
                res.bundles = game.bundles_with(dingosdk::frostbite::AssetKind::ebx, name);

                native::Project project;
                std::error_code ec;
                const bool add_to = ext == L".fbproject" && fs::exists(output, ec);
                if (add_to)
                {
                    try
                    {
                        project = native::read_fbproject(output);
                    }
                    catch (const std::exception &failure)
                    {
                        throw Error("not_a_project",
                                    path_utf8(output) + " exists but is not a project it can add to: " + failure.what(),
                                    {{"param", "output"}});
                    }
                }
                else
                {
                    project.info.title = "EBX edits";
                    project.info.description = "Edited with ReSkate Studio+.";
                }
                auto &list = project.resources;
                const auto same = std::find_if(list.begin(),
                                               list.end(),
                                               [&](const native::ModResource &held)
                                               { return held.kind == res.kind && held.name == res.name; });
                out["replaced_earlier_edit"] = same != list.end();
                if (same != list.end())
                    *same = std::move(res);
                else
                    list.push_back(std::move(res));
                try
                {
                    if (ext == L".fbproject")
                        native::write_fbproject(output, project.info, list);
                    else
                        native::write_fbmod(output, project.info, list, root);
                }
                catch (const std::exception &failure)
                {
                    throw Error(
                        "write_failed", std::string(failure.what()) + ": " + path_utf8(output), {{"param", "output"}});
                }
                out["format"] = ext == L".fbproject" ? "fbproject" : "fbmod";
                out["added_to_existing"] = add_to;
                out["project_resources"] = list.size();
                return out;
            },
        });

        r.add({
            .group = "texture",
            .name = "export",
            .summary = "Export one texture to PNG (one mip) or DDS (every mip, native format)",
            .description =
                std::string("Reads a TextureAsset (or its Texture resource with --kind res) and its pixel chunk and "
                            "writes --output. "
                            "PNG holds one mip of one slice decoded to RGBA (BC1-7 and 8-bit RGBA/BGRA; BC5 normal "
                            "maps get their z "
                            "rebuilt, BC6H is tone-mapped). DDS holds every mip and slice in the native format with a "
                            "DX10 header. "
                            "--format defaults to the output's extension. --mip picks the mip (0 is the largest); "
                            "without it the largest "
                            "mip in the game's data is used (streamed textures may lack their biggest mips), or with "
                            "--max-size the "
                            "largest that fits in that many pixels. Returns the size, format and mip written.") +
                k_index_note,
            .params =
                {
                    {"name",
                     ParamType::String,
                     "Texture name, e.g. common/textures/default/neutral/neutral_c",
                     true,
                     true},
                    {"output", ParamType::Path, "File to write (.png or .dds)", true, true},
                    {"format",
                     ParamType::Enum,
                     "png or dds (default: from --output's extension)",
                     false,
                     false,
                     {"png", "dds"}},
                    kind_param("ebx", false),
                    {"mip",
                     ParamType::Integer,
                     "PNG: which mip, 0 is the largest (default: the largest in the game's data)",
                     false,
                     false,
                     {},
                     std::nullopt,
                     0},
                    {"max-size",
                     ParamType::Integer,
                     "PNG: the largest mip no wider or taller than this",
                     false,
                     false,
                     {},
                     std::nullopt,
                     1},
                    {"slice", ParamType::Integer, "PNG: which slice or cube face", false, false, {}, Json(0), 0},
                    game_root_param(),
                },
            .examples = {"texture export common/textures/default/neutral/neutral_c neutral.png",
                         "texture export common/textures/default/neutral/neutral_c neutral.dds",
                         "texture export systems/render/textures/common_white white.png --max-size 256 --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                const auto index = index_for(c, a);
                const auto &e = require_entry(*index, entry_kind(a), a["name"].get<std::string>());
                const fs::path output(utf8_to_wide(a["output"].get<std::string>()));
                std::string format = a.value("format", "");
                if (format.empty())
                    format = lower(path_utf8(output.extension())) == ".dds" ? "dds" : "png";
                c.progress(-1, "Reading the texture");
                const auto texture = native::read_texture(*index, e, *native::game_files(c.game_root(a)));
                Json out = native::texture_info(texture);
                out["name"] = index->name(e);
                if (format == "dds")
                {
                    const auto dds = native::encode_dds(texture);
                    native::write_file(output, dds.data(), dds.size());
                    out["bytes"] = dds.size();
                }
                else
                {
                    std::uint32_t mip = 0;
                    if (a.contains("mip") && a["mip"].is_number_integer())
                    {
                        mip = static_cast<std::uint32_t>(a["mip"].get<long long>());
                    }
                    else
                    {
                        // The largest mip that is in the game's data and, with --max-size, fits.
                        const auto most = a.contains("max-size") && a["max-size"].is_number_integer()
                                              ? a["max-size"].get<long long>()
                                              : 1ll << 30;
                        while (mip + 1u < texture.header.mip_count &&
                               (!native::mip_present(texture, mip) ||
                                std::max(texture.header.width >> mip, texture.header.height >> mip) > most))
                            ++mip;
                    }
                    c.progress(-1, "Decoding mip " + std::to_string(mip));
                    const auto image =
                        native::decode_mip(texture, mip, static_cast<std::uint32_t>(a.value("slice", 0ll)));
                    const auto png = native::encode_png(image);
                    native::write_file(output, png.data(), png.size());
                    out["mip"] = mip;
                    out["mip_width"] = image.width;
                    out["mip_height"] = image.height;
                    out["bytes"] = png.size();
                }
                out["output"] = path_utf8(output);
                out["output_format"] = format;
                return out;
            },
        });

        r.add({
            .group = "asset",
            .name = "export-raw",
            .summary = "Write one asset's payload to a file (.ebx, .res or .chunk) as the game has it",
            .description =
                std::string("Writes the decoded payload of an EBX asset, resource or chunk to --output, byte for byte "
                            "as the game "
                            "reads it. --stored writes it as stored in the cas archive instead (usually compressed). A "
                            "resource's "
                            "16-byte metadata, which its payload does not include, is returned as resource_meta.") +
                k_index_note,
            .params =
                {
                    {"name", ParamType::String, "Asset name, or a chunk's guid", true, true},
                    {"output", ParamType::Path, "File to write", true, true},
                    kind_param("ebx", false),
                    {"stored",
                     ParamType::Boolean,
                     "Write the bytes as stored in the archive, undecoded",
                     false,
                     false,
                     {},
                     Json(false)},
                    game_root_param(),
                },
            .examples = {"asset export-raw characters/lua/luafacecompositor luafacecompositor.ebx",
                         "asset export-raw characters/lua/luafacecompositor luafacecompositor.res --kind res --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                const auto index = index_for(c, a);
                const auto &e = require_entry(*index, entry_kind(a), a["name"].get<std::string>());
                const auto files = native::game_files(c.game_root(a));
                const auto bytes =
                    a.value("stored", false) ? files->read_stored(e.location()) : files->read(e.location());
                const fs::path output(utf8_to_wide(a["output"].get<std::string>()));
                native::write_file(output, bytes.data(), bytes.size());
                Json out = {{"name", index->name(e)},
                            {"kind", native::kind_name(e.kind)},
                            {"type", index->type_name(e)},
                            {"output", path_utf8(output)},
                            {"bytes", bytes.size()},
                            {"stored", a.value("stored", false)}};
                if (e.kind == Kind::res)
                    out["resource_meta"] = native::hex_bytes(e.meta.data(), e.meta.size());
                return out;
            },
        });

        r.add({
            .group = "asset",
            .name = "lua-source",
            .summary = "The Lua source of a LuaAsset or LuaScript resource",
            .description =
                std::string(
                    "Returns the Lua source text stored in a LuaScript resource (give the LuaAsset EBX name or the "
                    "resource "
                    "with --kind res), with its line and byte counts. --output also writes it to a .lua file. Unlike "
                    "'asset lua' this returns the text itself and needs no engine.") +
                k_index_note,
            .params =
                {
                    {"name", ParamType::String, "LuaAsset name, e.g. characters/lua/luafacecompositor", true, true},
                    kind_param("ebx", false),
                    {"output", ParamType::Path, "Also write the source to this file"},
                    game_root_param(),
                },
            .examples = {"asset lua-source characters/lua/luafacecompositor",
                         "asset lua-source characters/lua/luafacecompositor --output luafacecompositor.lua"},
            .run = [](Context &c, const Json &a) -> Json
            {
                const auto index = index_for(c, a);
                const auto &e = require_entry(*index, entry_kind(a), a["name"].get<std::string>());
                const auto lua = native::read_lua(*index, e, *native::game_files(c.game_root(a)));
                Json out = {{"name", index->name(e)},
                            {"resource", lua.name},
                            {"bytes", lua.source.size()},
                            {"lines",
                             std::count(lua.source.begin(), lua.source.end(), '\n') +
                                 (lua.source.empty() || lua.source.back() == '\n' ? 0 : 1)}};
                if (!lua.file.empty())
                    out["file"] = lua.file;
                if (a.contains("output") && a["output"].is_string() && !a["output"].get<std::string>().empty())
                {
                    const fs::path output(utf8_to_wide(a["output"].get<std::string>()));
                    native::write_file(output, lua.source.data(), lua.source.size());
                    out["output"] = path_utf8(output);
                }
                out["source"] = lua.source;
                return out;
            },
        });
    }

} // namespace studio
