// Cosmetics, costumes, textures and meshes (reskate_cli: cosmetics, cosmetic-audit, cosmetic-mesh-import,
// native-costume-info, native-costume-donors, native-costume-build, textures, meshes, mesh-author, tangents).
#include "commands/commands.h"
#include "core/engine.h"
#include "core/settings.h"
#include "native/gltf.h"
#include "native/asset_views.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <regex>
#include <utility>
#include <unordered_map>

namespace fs = std::filesystem;

namespace studio
{

    namespace
    {
        std::string lower(std::string s)
        {
            for (auto &ch : s)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return s;
        }

        std::string snake(std::string s)
        {
            std::replace(s.begin(), s.end(), '-', '_');
            return s;
        }

        std::vector<std::string> split(const std::string &s, char sep)
        {
            std::vector<std::string> out;
            size_t start = 0;
            for (size_t i = 0; i <= s.size(); ++i)
                if (i == s.size() || s[i] == sep)
                {
                    out.push_back(s.substr(start, i - start));
                    start = i + 1;
                }
            return out;
        }

        bool starts_with(std::string_view s, std::string_view prefix)
        {
            return s.substr(0, prefix.size()) == prefix;
        }

        // "key=value" -> {key, value}; key empty when there is no '='.
        std::pair<std::string, std::string> split_kv(const std::string &s)
        {
            auto eq = s.find('=');
            if (eq == std::string::npos)
                return {"", s};
            return {s.substr(0, eq), s.substr(eq + 1)};
        }

        // Adds a value under `key`, turning the field into an array when the key repeats.
        void put(Json &obj, const std::string &key, Json value)
        {
            if (!obj.contains(key))
            {
                obj[key] = std::move(value);
                return;
            }
            if (!obj[key].is_array())
                obj[key] = Json::array({obj[key]});
            obj[key].push_back(std::move(value));
        }

        // key=value stdout lines as an object with snake_case keys and typed values.
        Json key_values(const std::vector<std::string> &lines)
        {
            Json raw = parse_key_values(lines, true);
            Json out = Json::object();
            for (auto it = raw.begin(); it != raw.end(); ++it)
                out[snake(it.key())] = *it;
            return out;
        }

        // Path argument made absolute (MCP callers may pass relative paths; the engine runs in its own folder).
        fs::path path_arg(const Json &a, const std::string &param)
        {
            fs::path p = utf8_to_wide(arg_string(a, param));
            std::error_code ec;
            fs::path abs = fs::absolute(p, ec);
            return ec ? p : abs;
        }

        std::string ext_of(const fs::path &p)
        {
            return lower(path_utf8(p.extension()));
        }

        void require_file(const fs::path &p, const std::string &param)
        {
            std::error_code ec;
            if (!fs::is_regular_file(p, ec))
                throw Error("file_not_found",
                            "--" + param + ": file not found: " + path_utf8(p),
                            {{"param", param}, {"path", path_utf8(p)}});
        }

        void require_extension(const fs::path &p, const std::string &param, const std::vector<std::string> &allowed)
        {
            std::string ext = ext_of(p);
            if (std::find(allowed.begin(), allowed.end(), ext) != allowed.end())
                return;
            std::string list;
            for (const auto &e : allowed)
                list += (list.empty() ? "" : " or ") + e;
            throw Error("invalid_arguments",
                        "--" + param + ": must be a " + list + " file, got '" + path_utf8(p.filename()) + "'",
                        {{"param", param}, {"path", path_utf8(p)}});
        }

        void require_output_dir(const fs::path &p, const std::string &param)
        {
            std::error_code ec;
            if (fs::is_directory(p, ec))
                throw Error("invalid_arguments",
                            "--" + param + ": is a folder, give a file name: " + path_utf8(p),
                            {{"param", param}});
            fs::path dir = p.parent_path();
            if (!dir.empty() && !fs::is_directory(dir, ec))
                throw Error("output_dir_missing",
                            "--" + param + ": folder does not exist: " + path_utf8(dir),
                            {{"param", param}, {"path", path_utf8(dir)}});
        }

        void require_positive(const Json &a, const std::string &param)
        {
            if (a[param].get<long long>() < 1)
                throw Error("invalid_arguments", "--" + param + " must be at least 1", {{"param", param}});
        }

        // The engine only notices a bad ReSkate folder when it hashes Skate.exe; check it up front.
        fs::path checked_game_root(const Context &c, const Json &a)
        {
            fs::path root = c.game_root(a);
            std::error_code ec;
            if (!fs::is_regular_file(root / L"Skate.exe", ec))
                throw Error("not_a_game_root",
                            "No Skate.exe in " + path_utf8(root) + ". Pass --game-root <ReSkate folder>.",
                            {{"path", path_utf8(root)}});
            return root;
        }

        long long file_size_or(const fs::path &p, long long fallback)
        {
            std::error_code ec;
            auto n = fs::file_size(p, ec);
            return ec ? fallback : static_cast<long long>(n);
        }

        struct KnownError
        {
            const char *text; // substring of the engine's error line
            const char *code;
            const char *hint; // appended to the message, may be empty
        };

        // Turns the engine's known failures into stable codes; anything else goes through require_success.
        void check_run(const EngineRun &run,
                       std::initializer_list<KnownError> known,
                       const std::string &fallback = "engine_failed")
        {
            if (run.exit_code == 0)
                return;
            std::vector<KnownError> all(known);
            all.push_back({"for SHA-256", "not_a_game_root", ""});
            auto scan = [&](const std::vector<std::string> &lines)
            {
                for (auto it = lines.rbegin(); it != lines.rend(); ++it)
                    for (const auto &k : all)
                        if (it->find(k.text) != std::string::npos)
                        {
                            std::string message = *it;
                            if (*k.hint)
                                message += ". " + std::string(k.hint);
                            throw Error(k.code, message, {{"exit_code", run.exit_code}, {"engine_message", *it}});
                        }
            };
            scan(run.err_lines);
            scan(run.out_lines);
            require_success(run, fallback);
        }

        const KnownError bad_number{"invalid stoull", "invalid_arguments", "The count must be a whole number"};

        // stderr handler for this group: warnings go to the log and other lines become progress, except the
        // engine's known error lines, which the command reports once as its error instead of also as progress.
        void engine_stderr(Context &c, std::string_view line)
        {
            static const char *const errors[] = {
                "for SHA-256",
                "invalid stoull",
                "not enough texture resources",
                "no usable tangent frames",
                "not in the cosmetic catalog",
                "no item-specific geometry slot",
                "to a native material section",
                "Select an FBX or GLB mesh",
                "cosmetic name supports",
                "bind pose differs",
                "not a binary FBMOD",
                "no native costume group",
                "invalid native costume",
                "invalid JSON",
                "native costume package: ",
                "native costume build failed",
                "native costume output must use",
            };
            if (lower(std::string(line.substr(0, 7))) == "warning")
            {
                c.log("warning", line);
                return;
            }
            for (const char *e : errors)
                if (line.find(e) != std::string_view::npos)
                    return;
            c.progress(-1, line);
        }

        std::string trim(std::string s)
        {
            auto space = [](unsigned char ch) { return std::isspace(ch) != 0; };
            while (!s.empty() && space(static_cast<unsigned char>(s.back())))
                s.pop_back();
            size_t i = 0;
            while (i < s.size() && space(static_cast<unsigned char>(s[i])))
                ++i;
            return s.substr(i);
        }

        bool is_guid(std::string_view s)
        {
            if (s.size() == 38 && s.front() == '{' && s.back() == '}')
                s = s.substr(1, 36);
            if (s.size() != 36)
                return false;
            for (size_t i = 0; i < s.size(); ++i)
            {
                bool dash = i == 8 || i == 13 || i == 18 || i == 23;
                if (dash ? s[i] != '-' : !std::isxdigit(static_cast<unsigned char>(s[i])))
                    return false;
            }
            return true;
        }

        // Progress every 100 rows for commands that stream one row per item to stdout.
        LineHandler row_progress(const char *verb)
        {
            return [verb, rows = 0LL](Context &c, std::string_view line) mutable
            {
                if (!(starts_with(line, "OK\t") || starts_with(line, "FAIL\t")))
                    return;
                if (++rows % 100 == 0)
                    c.progress(-1, std::string(verb) + " " + std::to_string(rows) + " cosmetics");
            };
        }

        // ---- cosmetic list ---------------------------------------------------------------------------
        Json run_cosmetic_list(Context &c, const Json &a)
        {
            EngineRun run = run_engine(c, {"cosmetics", path_utf8(checked_game_root(c, a))}, engine_stderr);
            check_run(run, {});
            Json sample = Json::array();
            Json counts = Json::object();
            Json board = Json::object();
            Json character = Json::object();
            Json materials = Json::array();
            Json other = Json::object();
            Json lines = Json::array();
            for (const auto &line : run.out_lines)
            {
                auto eq = line.find('=');
                auto space = line.find(' ');
                if (eq != std::string::npos && (space == std::string::npos || space > eq))
                {
                    auto [key, value] = split_kv(line);
                    if (key == "cosmetics")
                        counts["cosmetics"] = typed_value(value);
                    else if (key == "board-cosmetics")
                        counts["board_cosmetics"] = typed_value(value);
                    else if (key == "warnings")
                        counts["warnings"] = typed_value(value);
                    else if (starts_with(key, "preview-"))
                        board[snake(key.substr(8))] = value;
                    else if (key == "character-material")
                    {
                        auto f = split(value, '\t');
                        Json m = {{"section", f.size() > 0 ? f[0] : ""}};
                        if (f.size() > 1)
                            m["count"] = typed_value(f[1]);
                        if (f.size() > 2)
                            m["texture"] = f[2];
                        m["udim"] = f.size() > 3 && f[3] == "UDIM";
                        materials.push_back(m);
                    }
                    else if (starts_with(key, "character-preview-"))
                    {
                        std::string k = snake(key.substr(18));
                        character[k] =
                            (k == "roles" || k == "parameters" || k == "materials") ? typed_value(value) : Json(value);
                    }
                    else
                    {
                        put(other, snake(key), typed_value(value));
                    }
                    continue;
                }
                auto f = split(line, ' ');
                if (f.size() == 3)
                    sample.push_back({{"slot", f[0]}, {"item", f[1]}, {"appearance", f[2]}});
                else
                    lines.push_back(line);
            }
            character["material_list"] = materials;
            Json out = {
                {"counts", counts}, {"sample", sample}, {"board_preview", board}, {"character_preview", character}};
            if (!other.empty())
                out["other"] = other;
            if (!lines.empty())
                out["lines"] = lines;
            return out;
        }

        // ---- native cosmetic catalog ----------------------------------------------------------------
        // These are the item-name prefixes used by the in-game Cosmetics categories. Match against the
        // native asset index (which has already read the TOCs, bundle manifests and EBX file GUIDs).
        // Own_CustHair is shared by the eyebrow and facial-hair entries, so keep it as one combined group.
        Json run_native_cosmetic_catalog(Context &c, const Json &a)
        {
            struct PrefixRule
            {
                const char *category;
                const char *prefix;
            };
            static constexpr PrefixRule rules[] = {
                {"Body", "own_custbody"}, {"Bottom (Deck Graphic)", "own_deckgraphic"},
                {"Bottoms (Pants)", "own_bottom"}, {"Eyes", "own_custeye"},
                {"Eyebrows / Facial Hair", "own_custhair"}, {"Eyewear", "own_eyewearglasses"},
                {"Facial Hair Color", "own_facialhaircolor"}, {"Costume", "own_costume"},
                {"Griptape", "own_deckgripcolor"}, {"Griptape", "dev_sb_deck_gripcolor_anim"},
                {"Grip Cutout", "own_deckgripcutout"}, {"Hair Color", "own_custscalp"},
                {"Hair Color", "dev_own_custhair"}, {"Hats", "own_headwear"}, {"Hats", "own_mask"},
                {"Heads", "own_custhead"}, {"Left Hand", "own_handwearglove"},
                {"Lipstick", "own_lipcolor"}, {"Lipstick Style", "own_lipstyle"},
                {"Makeup", "own_makeup"}, {"Moles", "own_moles"}, {"Mouths", "own_custmouth"},
                {"Noses", "own_custnose"}, {"Outfits", "own_outfit"}, {"Outfits", "own_topdress"},
                {"Playercard Backgrounds", "own_pcard_bg"}, {"Playercard Icons", "own_pcard_icon"},
                {"Shoes", "own_shoe"}, {"Skins", "own_custskintype"}, {"Socks", "own_sock"},
                {"Sticker", "own_sticker"}, {"Stubble", "own_stubble"}, {"Tattoos", "own_tattooflash"},
                {"Tops", "own_top"}, {"Trucks", "own_truck"}, {"Voices", "own_voicetype"},
                {"Wheels", "own_wheel"}, {"Wheels", "own_sb_deck_wheel"}, {"Scalps", "own_custhairscalp"},
            };

            const fs::path root = checked_game_root(c, a);
            bool from_cache = false;
            const bool refresh_index = a.value("refresh", false);
            const auto index = native::build_index(c, root, refresh_index, &from_cache);
            const auto files = native::game_files(root);

            auto prefix_for = [&](const native::Entry &entry) -> const PrefixRule *
            {
                const auto name = index->lower_name(entry);
                const auto slash = name.find_last_of('/');
                const auto leaf = slash == std::string_view::npos ? name : name.substr(slash + 1);
                const PrefixRule *best = nullptr;
                for (const auto &rule : rules)
                    if (leaf.starts_with(rule.prefix) &&
                        (!best || std::char_traits<char>::length(rule.prefix) >
                                      std::char_traits<char>::length(best->prefix)))
                        best = &rule;
                return best;
            };
            auto is_base_item = [&](const native::Entry &entry)
            {
                return lower(index->type_name(entry)).find("delmarbaseitemasset") != std::string::npos;
            };
            auto is_build_kit = [&](const native::Entry &entry)
            {
                const std::string name = lower(std::string(index->name(entry)));
                const std::string type = lower(index->type_name(entry));
                return name.find("buildkit") != std::string::npos ||
                       name.find("build_kit") != std::string::npos ||
                       name.find("build-kit") != std::string::npos ||
                       type.find("buildkit") != std::string::npos ||
                       type.find("build_kit") != std::string::npos ||
                       type.find("build-kit") != std::string::npos;
            };
            auto name_key = [](std::string_view name)
            {
                std::string key(name);
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch)
                               { return static_cast<char>(std::tolower(ch)); });
                return key;
            };

            struct Decoded
            {
                const native::Entry *entry = nullptr;
                std::vector<std::string> imports;
                std::string error;
                const PrefixRule *rule = nullptr;
                bool base = false;
            };
            std::vector<Decoded> decoded_assets;
            std::unordered_map<std::string, std::size_t> by_name;
            std::size_t matched_ebx = 0, decoded_count = 0, failed_ebx = 0;
            std::string first_read_error;
            c.progress(-1, "Finding base items and cosmetic assets in the native index");

            for (const auto &entry : index->entries)
            {
                if (entry.kind != native::Kind::ebx)
                    continue;
                const auto *rule = prefix_for(entry);
                const bool base = is_base_item(entry);
                if (!rule && !base)
                    continue;
                if (is_build_kit(entry))
                    continue;
                ++matched_ebx;
                if ((matched_ebx % 100) == 0)
                    c.progress(-1, "Reading cosmetic EBX data: " + std::to_string(matched_ebx));

                Decoded item;
                item.entry = &entry;
                item.rule = rule;
                item.base = base;
                try
                {
                    // The catalog only needs the import table. Do not serialize up to 200
                    // instances and their fields for every cosmetic EBX.
                    const Json imports = native::ebx_imports(*index, entry, *files);
                    std::set<std::string> seen;
                    for (const auto &import : imports)
                    {
                        const auto target = import.value("asset", "");
                        if (!target.empty() && seen.insert(target).second)
                            item.imports.push_back(target);
                    }
                    ++decoded_count;
                }
                catch (const std::exception &error)
                {
                    item.error = error.what();
                    ++failed_ebx;
                    if (first_read_error.empty())
                        first_read_error = item.error;
                }
                by_name.emplace(name_key(index->name(entry)), decoded_assets.size());
                decoded_assets.push_back(std::move(item));
            }

            auto category_for = [&](const Decoded &item)
            {
                if (item.rule)
                    return std::string(item.rule->category);
                // Base-item names are not guaranteed to use the cosmetic prefix. Infer from directly
                // referenced category-prefixed EBXs when possible; otherwise preserve the unknown value.
                for (const auto &name : item.imports)
                    if (const auto found = by_name.find(name_key(name)); found != by_name.end())
                        if (const auto *rule = decoded_assets[found->second].rule)
                            return std::string(rule->category);
                return std::string("Uncategorized");
            };

            Json items = Json::array();
            Json counts = Json::object();
            std::set<std::string> assigned;
            std::size_t cosmetic_count = 0;

            // A base item is the cosmetic identity. Its direct EBX imports are attached to the same
            // record; category-prefixed assets that import the base item are attached as well.
            for (const auto &base : decoded_assets)
            {
                if (!base.base)
                    continue;
                const std::string base_name(index->name(*base.entry));
                const std::string base_key = name_key(base_name);
                Json assets = Json::array();
                std::set<std::string> asset_names;
                asset_names.insert(base_name);

                // Keep the base item's direct imports and one additional EBX-reference level. Some
                // item records point to a wrapper EBX which owns the thumbnail and large thumbnail.
                auto add_linked_assets = [&](const std::string &start)
                {
                    const auto found = by_name.find(name_key(start));
                    if (found == by_name.end())
                        return;
                    const auto &linked = decoded_assets[found->second];
                    const std::string linked_name(index->name(*linked.entry));
                    asset_names.insert(linked_name);
                    for (const auto &child_name : linked.imports)
                        if (by_name.contains(name_key(child_name)))
                            asset_names.insert(std::string(index->name(*decoded_assets[by_name.at(name_key(child_name))].entry)));
                };
                for (const auto &name : base.imports)
                    add_linked_assets(name);
                for (const auto &candidate : decoded_assets)
                    if (candidate.rule &&
                        std::any_of(candidate.imports.begin(), candidate.imports.end(),
                                    [&](const std::string &name) { return name_key(name) == base_key; }))
                        add_linked_assets(std::string(index->name(*candidate.entry)));

                for (const auto &name : asset_names)
                {
                    assigned.insert(name_key(name));
                    const auto found = by_name.find(name_key(name));
                    if (found == by_name.end())
                        continue;
                    const auto &asset = decoded_assets[found->second];
                    native::fb::Guid guid;
                    guid.bytes = asset.entry->guid;
                    Json record = {{"name", name},
                                   {"kind", "ebx"},
                                   {"guid", guid.string()},
                                   {"type", index->type_name(*asset.entry)},
                                   {"status", asset.error.empty() ? "ok" : "fail"}};
                    if (!asset.error.empty())
                        record["error"] = asset.error;
                    assets.push_back(std::move(record));
                }

                native::fb::Guid guid;
                guid.bytes = base.entry->guid;
                const std::string category = category_for(base);
                Json cosmetic = {{"item", base_name},
                                 {"slot", category},
                                 {"status", base.error.empty() ? "ok" : "fail"},
                                 {"type", index->type_name(*base.entry)},
                                 {"guid", guid.string()},
                                 {"base_item", base_name},
                                 {"assets", std::move(assets)},
                                 {"references", base.imports}};
                if (!base.error.empty())
                    cosmetic["reference_error"] = base.error;
                items.push_back(std::move(cosmetic));
                ++cosmetic_count;
                counts[category] = counts.value(category, 0) + 1;
            }

            // Keep prefix-discovered assets that are not linked to a discovered base item as
            // standalone fallback records. This makes incomplete or older asset sets visible.
            for (const auto &asset : decoded_assets)
            {
                if (!asset.rule || assigned.contains(name_key(index->name(*asset.entry))))
                    continue;
                native::fb::Guid guid;
                guid.bytes = asset.entry->guid;
                const std::string name(index->name(*asset.entry));
                Json assets = Json::array({{{"name", name},
                                            {"kind", "ebx"},
                                            {"guid", guid.string()},
                                            {"type", index->type_name(*asset.entry)},
                                            {"status", asset.error.empty() ? "ok" : "fail"}}});
                Json cosmetic = {{"item", name},
                                 {"slot", asset.rule->category},
                                 {"status", asset.error.empty() ? "ok" : "fail"},
                                 {"type", index->type_name(*asset.entry)},
                                 {"guid", guid.string()},
                                 {"prefix", asset.rule->prefix},
                                 {"assets", std::move(assets)},
                                 {"references", asset.imports},
                                 {"fallback", true}};
                if (!asset.error.empty())
                    cosmetic["reference_error"] = asset.error;
                items.push_back(std::move(cosmetic));
                ++cosmetic_count;
                counts[asset.rule->category] = counts.value(asset.rule->category, 0) + 1;
            }

            return {{"total", cosmetic_count},
                    {"matched_ebx", matched_ebx},
                    {"decoded_ebx", decoded_count},
                    {"failed_ebx", failed_ebx},
                    {"first_read_error", first_read_error},
                    {"index_cached", from_cache},
                    {"counts", std::move(counts)},
                    {"items", std::move(items)}};
        }

        // ---- cosmetic audit --------------------------------------------------------------------------
        Json run_cosmetic_audit(Context &c, const Json &a)
        {
            std::string slot = lower(arg_string(a, "slot"));
            std::string text = lower(arg_string(a, "text"));
            std::string status = a["status"];
            long long limit = a["limit"];
            if (limit < 0)
                throw Error("invalid_arguments", "--limit must be 0 (no limit) or more", {{"param", "limit"}});

            c.progress(-1, "auditing every cosmetic (about a minute)");
            EngineRun run = run_engine(
                c, {"cosmetic-audit", path_utf8(checked_game_root(c, a))}, engine_stderr, row_progress("audited"));
            check_run(run, {});

            Json items = Json::array();
            Json summary = Json::object();
            Json by_slot = {{"failures", Json::object()},
                            {"uncovered", Json::object()},
                            {"unmatched", Json::object()},
                            {"degraded", Json::object()}};
            Json lines = Json::array();
            long long matched = 0, ok_rows = 0, fail_rows = 0;
            for (const auto &line : run.out_lines)
            {
                auto f = split(line, '\t');
                const std::string &tag = f[0];
                if ((tag == "OK" || tag == "FAIL") && f.size() >= 4)
                {
                    bool ok = tag == "OK";
                    (ok ? ok_rows : fail_rows)++;
                    if (status != "all" && (status == "ok") != ok)
                        continue;
                    if (!slot.empty() && lower(f[2]) != slot)
                        continue;
                    if (!text.empty() && f[3].find(text) == std::string::npos)
                        continue;
                    ++matched;
                    if (limit > 0 && static_cast<long long>(items.size()) >= limit)
                        continue;
                    Json item = {
                        {"status", ok ? "ok" : "fail"}, {"index", typed_value(f[1])}, {"slot", f[2]}, {"item", f[3]}};
                    if (ok)
                    {
                        for (size_t i = 4; i < f.size(); ++i)
                        {
                            auto [key, value] = split_kv(f[i]);
                            if (key.empty())
                            {
                                put(item, "notes", value);
                                continue;
                            }
                            // "warnings=N" is a count; "warning=<text>" columns are the messages.
                            if (key == "warning")
                                put(item, "warning_messages", value);
                            else
                                put(item, snake(key), key == "texture" ? Json(value) : typed_value(value));
                        }
                        if (item.contains("warning_messages") && !item["warning_messages"].is_array())
                            item["warning_messages"] = Json::array({item["warning_messages"]});
                    }
                    else
                    {
                        std::string reason;
                        for (size_t i = 4; i < f.size(); ++i)
                            reason += (reason.empty() ? "" : "\t") + f[i];
                        item["reason"] = reason;
                    }
                    items.push_back(item);
                }
                else if (tag == "SUMMARY")
                {
                    for (size_t i = 1; i < f.size(); ++i)
                    {
                        auto [key, value] = split_kv(f[i]);
                        if (!key.empty())
                            summary[snake(key)] = typed_value(value);
                    }
                }
                else if (f.size() == 3 && tag.size() > 8 && tag.substr(tag.size() - 8) == "_BY_SLOT")
                {
                    std::string group = lower(tag.substr(0, tag.size() - 8));
                    by_slot[group][f[1]] = typed_value(f[2]);
                }
                else
                {
                    lines.push_back(line);
                }
            }
            Json out = {{"summary", summary},
                        {"by_slot", by_slot},
                        {"filter", {{"status", status}, {"slot", slot}, {"text", text}, {"limit", limit}}},
                        {"rows", {{"ok", ok_rows}, {"fail", fail_rows}}},
                        {"matched", matched},
                        {"returned", items.size()},
                        {"items", items}};
            if (!lines.empty())
                out["lines"] = lines;
            return out;
        }

        // ---- cosmetic import-mesh --------------------------------------------------------------------
        Json run_import_mesh(Context &c, const Json &a)
        {
            fs::path root = checked_game_root(c, a);
            std::string donor = lower(trim(arg_string(a, "donor")));
            if (!starts_with(donor, "items/"))
                throw Error("invalid_arguments",
                            "--donor must be a catalog item path starting with items/, e.g. "
                            "items/cust_fullbodycostume/own_costume_gen_isaacclarke_00001 (see cosmetic audit)",
                            {{"param", "donor"}});
            fs::path model = path_arg(a, "model");
            fs::path output = path_arg(a, "output");
            //require_file(model, "model");
            //require_extension(model, "model", {".glb", ".fbx"});
            require_extension(output, "output", {".fbproject", ".fbmod"});
            require_output_dir(output, "output");
            std::string format = ext_of(output) == ".fbmod" ? "fbmod" : "fbproject";
            // The engine names the new cosmetic after the model file and only checks the name after loading
            // the catalog, with a message that does not mention the file.
            std::string stem = path_utf8(model.stem());
            bool name_ok = !trim(stem).empty() &&
                           std::all_of(stem.begin(),
                                       stem.end(),
                                       [](char ch)
                                       {
                                           auto u = static_cast<unsigned char>(ch);
                                           return u < 128 && (std::isalnum(u) || ch == ' ' || ch == '_' || ch == '-');
                                       });
            if (!name_ok)
                throw Error("invalid_model_name",
                            "--model: the new cosmetic is named after the model file, and '" + stem +
                                "' may only use ASCII "
                                "letters, numbers, spaces, _ and -. Rename or copy the file, e.g. my_deck.glb",
                            {{"param", "model"}, {"path", path_utf8(model)}, {"name", stem}});

            //c.progress(-1, "importing " + path_utf8(model.filename()) + " onto " + donor);
            EngineRun run =
                run_engine(c,
                           {"cosmetic-mesh-import", path_utf8(root), donor, "", path_utf8(output)},
                           engine_stderr);
            check_run(run,
                      {
                          {"cosmetic name supports", "invalid_model_name", "The name comes from the model file name"},
                          {"bind pose differs", "bind_pose_mismatch", ""},
                          {"not in the cosmetic catalog",
                           "donor_not_found",
                           "Find a valid item path with: studio-plus cosmetic audit --text <name>"},
                          {"no item-specific geometry slot",
                           "donor_has_no_geometry_slot",
                           "Deck, wheel and other board items share one mesh and cannot be donors; use a clothing or "
                           "costume item"},
                          {"to a native material section",
                           "mesh_material_unroutable",
                           "Name the mesh materials after the donor's material sections, or pick another donor"},
                          {"Select an FBX or GLB mesh", "unsupported_model_format", ""},
                      },
                      "import_failed");

            Json ebx = Json::array();
            Json resources = Json::array();
            Json chunks = Json::array();
            Json lookups = Json::array();
            Json readback = Json::object();
            Json other = Json::array();
            std::string item, written;
            long long chunk_bytes = 0;
            static const std::regex ebx_re(R"(^ebx (\S+) \(([^)]*)\) file=(\S+) bundle=(\S+)$)");
            static const std::regex import_re(R"(^\s+import (\S+) -> (\S+)$)");
            static const std::regex path_re(R"(^\s+path type=(\d+) name=(\S+)$)");
            static const std::regex res_re(R"(^res (\S+) bytes=(\d+)$)");
            static const std::regex chunk_re(R"(^chunk (\S+) bytes=(\d+)$)");
            static const std::regex lookup_re(R"(^lookup (\S+) -> (\S+)$)");
            static const std::regex readback_re(R"(^readback=(.*) bones=(\d+)$)");
            std::smatch m;
            for (const auto &line : run.out_lines)
            {
                if (std::regex_match(line, m, readback_re))
                {
                    readback = {{"path", m[1].str()}, {"bones", std::stoll(m[2].str())}};
                }
                else if (std::regex_match(line, m, ebx_re))
                {
                    ebx.push_back({{"name", m[1].str()},
                                   {"type", m[2].str()},
                                   {"file", m[3].str()},
                                   {"bundle", m[4].str()},
                                   {"imports", Json::array()},
                                   {"paths", Json::array()}});
                }
                else if (std::regex_match(line, m, import_re) && !ebx.empty())
                {
                    ebx.back()["imports"].push_back({{"guid", m[1].str()}, {"asset", m[2].str()}});
                }
                else if (std::regex_match(line, m, path_re) && !ebx.empty())
                {
                    ebx.back()["paths"].push_back({{"type", std::stoll(m[1].str())}, {"name", m[2].str()}});
                }
                else if (std::regex_match(line, m, res_re))
                {
                    resources.push_back({{"name", m[1].str()}, {"bytes", std::stoll(m[2].str())}});
                }
                else if (std::regex_match(line, m, chunk_re))
                {
                    long long bytes = std::stoll(m[2].str());
                    chunk_bytes += bytes;
                    chunks.push_back({{"guid", m[1].str()}, {"bytes", bytes}});
                }
                else if (std::regex_match(line, m, lookup_re))
                {
                    Json entry = {{"preset", m[1].str()}, {"bundle_ref_table", m[2].str()}};
                    if (std::find(lookups.begin(), lookups.end(), entry) == lookups.end())
                        lookups.push_back(entry);
                }
                else if (starts_with(line, "item="))
                {
                    item = line.substr(5);
                }
                else if (starts_with(line, "output="))
                {
                    written = line.substr(7);
                }
                else
                {
                    other.push_back(line);
                }
            }
            // Drop the per-entry arrays that stayed empty so the result reads cleanly.
            Json presets = Json::array();
            for (auto &e : ebx)
            {
                if (e["imports"].empty())
                    e.erase("imports");
                if (e["paths"].empty())
                    e.erase("paths");
                if (e["type"] == "AppearanceShaderExpressionPreset")
                    presets.push_back(e["name"]);
            }
            if (written.empty())
                written = path_utf8(output);
            if (readback.contains("path"))
                readback["bytes"] = file_size_or(fs::path(utf8_to_wide(readback["path"].get<std::string>())), 0);
            Json out = {{"item", item},
                        {"output", written},
                        {"format", format},
                        {"output_bytes", file_size_or(fs::path(utf8_to_wide(written)), 0)},
                        {"donor", donor},
                        {"model", path_utf8(model)},
                        {"readback", readback},
                        {"appearance_presets", presets},
                        {"counts",
                         {{"ebx", ebx.size()},
                          {"resources", resources.size()},
                          {"chunks", chunks.size()},
                          {"chunk_bytes", chunk_bytes}}},
                        {"ebx", ebx},
                        {"resources", resources},
                        {"chunks", chunks},
                        {"lookups", lookups}};
            if (!other.empty())
                out["lines"] = other;
            return out;
        }

        // ---- costume build ---------------------------------------------------------------------------
        void check_costume_package(const fs::path &dir)
        {
            std::error_code ec;
            std::string d = path_utf8(dir);
            if (fs::is_regular_file(dir, ec))
                throw Error("invalid_arguments",
                            "--package: is a file, give the package folder that holds recipe.json: " + d,
                            {{"param", "package"}, {"path", d}});
            if (!fs::is_directory(dir, ec))
                throw Error(
                    "package_not_found", "--package: folder not found: " + d, {{"param", "package"}, {"path", d}});
            Json missing = Json::array();
            for (const wchar_t *name : {L"recipe.json", L"mesh.res"})
                if (!fs::is_regular_file(dir / name, ec))
                    missing.push_back(path_utf8(dir / name));
            long long chunk_files = 0;
            if (fs::is_directory(dir / L"chunks", ec))
            {
                for (const auto &e : fs::directory_iterator(dir / L"chunks", ec))
                {
                    if (!e.is_regular_file() || ext_of(e.path()) != ".chunk")
                        continue;
                    ++chunk_files;
                    if (!is_guid(path_utf8(e.path().stem())))
                        throw Error("invalid_package",
                                    "Chunk file name is not a GUID (<GUID>.chunk): " + path_utf8(e.path()),
                                    {{"path", path_utf8(e.path())}});
                }
            }
            else
            {
                missing.push_back(path_utf8(dir / L"chunks"));
            }
            if (!missing.empty())
                throw Error("invalid_package",
                            "Native costume package is missing: " + missing[0].get<std::string>() +
                                (missing.size() > 1 ? " (and " + std::to_string(missing.size() - 1) + " more)" : ""),
                            {{"missing", missing}});
            if (chunk_files != 8)
                throw Error("invalid_package",
                            "Native costume package needs exactly 8 files in chunks\\ (<GUID>.chunk), found " +
                                std::to_string(chunk_files),
                            {{"chunk_files", chunk_files}});
        }
    } // namespace

    // mesh find / info / export-raw / replace, in mesh_replace_commands.cpp, and two helpers it shares.
    void register_mesh_replace_commands(Registry &r);
    Json board_part_mesh(Context &c, const Json &a, const std::string &item);
    fs::path model_as_glb(Context &c, const fs::path &model);

    namespace
    {

        // A truck item to clone when none is given: from the audit, a generic one if there is one.
        std::string pick_truck_donor(Context &c, const fs::path &root)
        {
            c.progress(-1, "looking for a truck item to clone (cosmetic audit, about a minute)");
            EngineRun run = run_engine(c, {"cosmetic-audit", path_utf8(root)}, engine_stderr, row_progress("audited"));
            check_run(run, {});
            std::string first, generic;
            for (const auto &line : run.out_lines)
            {
                const auto f = split(line, '\t');
                if (f.size() < 4 || f[0] != "OK" || lower(f[2]) != "truck")
                    continue;
                const std::string item = lower(f[3]);
                if (first.empty())
                    first = item;
                if (generic.empty() && item.find("gen") != std::string::npos &&
                    item.find("default") != std::string::npos)
                    generic = item;
            }
            if (first.empty())
                throw Error(
                    "donor_not_found",
                    "No truck item in the cosmetic catalog to clone; pass --donor (see: cosmetic audit --slot truck)");
            return generic.empty() ? first : generic;
        }

        // cosmetic new-board-part: the model as a NEW truck item with its own geometry, through the engine's
        // cosmetic-mesh-import. The engine routes each material to the donor mesh's section of the same name and
        // refuses anything else, so a copy of the model with every material named after the donor's main section
        // goes in. Research: docs/discovery/own-board.md.
        Json run_new_board_part(Context &c, const Json &a)
        {
            const fs::path root = checked_game_root(c, a);
            const fs::path model = path_arg(a, "model");
            const fs::path output = path_arg(a, "output");
            require_file(model, "model");
            require_extension(model, "model", {".glb", ".fbx"});
            require_extension(output, "output", {".fbproject", ".fbmod"});
            require_output_dir(output, "output");
            refuse_game_folder(c, root, output, "output");
            // The engine names the item after the model file: items/<slot>/own_<name>.
            std::string name = trim(arg_string(a, "name"));
            if (name.empty())
                name = path_utf8(model.stem());
            const bool name_ok =
                !name.empty() && name.size() <= 48 &&
                std::all_of(name.begin(),
                            name.end(),
                            [](char ch)
                            {
                                const auto u = static_cast<unsigned char>(ch);
                                return u < 128 && (std::isalnum(u) || ch == ' ' || ch == '_' || ch == '-');
                            });
            if (!name_ok)
                throw Error("invalid_name",
                            "--name '" + name + "': use up to 48 ASCII letters, numbers, spaces, _ and -",
                            {{"param", "name"}});

            std::string donor = lower(trim(arg_string(a, "donor")));
            if (donor.empty())
                donor = pick_truck_donor(c, root);
            if (!starts_with(donor, "items/"))
                throw Error(
                    "invalid_arguments",
                    "--donor must be a catalog item path starting with items/ (see: cosmetic audit --slot truck)",
                    {{"param", "donor"}});
            Json part = board_part_mesh(c, a, donor);
            std::string section = trim(arg_string(a, "section"));
            if (section.empty())
                section = part.value("main_section", "");

            const fs::path work = Settings::data_dir() / L"work" / L"new-board-part";
            std::error_code ec;
            fs::create_directories(work, ec);
            const fs::path glb = model_as_glb(c, model);
            const fs::path prepared = work / (utf8_to_wide(name) + L".glb");
            std::vector<std::string> renamed;
            try
            {
                renamed = native::write_glb_one_material(glb, prepared, section);
            }
            catch (const std::exception &e)
            {
                throw Error("model_unreadable",
                            "--model: " + std::string(e.what()) + ": " + path_utf8(glb),
                            {{"param", "model"}});
            }

            Json args = {{"donor", donor}, {"model", path_utf8(prepared)}, {"output", path_utf8(output)}};
            if (a.contains("game-root"))
                args["game-root"] = a["game-root"];
            Json out = run_import_mesh(c, args);
            part["section_used"] = section;
            part["model_materials"] = renamed;
            part["prepared_model"] = path_utf8(prepared);
            out["board_part"] = part;
            out["slot"] = "truck";
            out["notes"] = Json::array({"In game, pick the new item in the trucks list: it draws your model where the "
                                        "trucks go. The deck and wheels "
                                        "you pick still draw too; only the trucks are replaced.",
                                        "Everyone else's boards are unchanged. In multiplayer, players who have this "
                                        "mod too see the scooter on you; "
                                        "players without it cannot (mods are on each player's own PC), and what they "
                                        "see instead depends on how the "
                                        "game handles an item it does not know: test it."});
            return out;
        }

    } // namespace

    void register_cosmetic_commands(Registry &r)
    {
        r.add({
            .group = "cosmetic",
            .name = "list",
            .summary = "Cosmetic catalog counts, a 10-item sample and the preview targets",
            .description =
                "Loads the cosmetic catalog from the installed game and returns: counts (cosmetics, "
                "board_cosmetics, warnings), the first 10 catalog rows as a sample (slot, item path, "
                "appearance preset), and the default preview targets Studio uses (board mesh/texture/section "
                "and the character preview item with its material sections). It is a summary, not a full "
                "dump: the engine prints only 10 rows. For every cosmetic with its slot use 'cosmetic audit'. "
                "Reads only, takes about 3 seconds.",
            .params = {game_root_param()},
            .examples = {"cosmetic list", "cosmetic list --json"},
            .run = run_cosmetic_list,
        });

        r.add({
            .group = "cosmetic",
            .name = "catalog",
            .summary = "Find Cosmetics-tab items through the native TOC and EBX GUID index",
            .description =
                "Loads the shared native game asset index, matches the known Cosmetics-tab item-name prefixes, "
                "and follows EBX imports to related asset names. Pass --refresh to rebuild the shared index first. "
                "Unlike 'cosmetic audit', this does not call reskate_cli or test previewability. Reads only.",
            .params = {
                {"refresh", ParamType::Boolean, "Rebuild the shared game asset index before scanning cosmetics", false, false, {}, Json(false)},
                game_root_param(),
            },
            .examples = {"cosmetic catalog", "cosmetic catalog --json"},
            .long_running = true,
            .run = run_native_cosmetic_catalog,
        });

        r.add({
            .group = "cosmetic",
            .name = "audit",
            .summary = "Every cosmetic with its slot, previewability and texture (about a minute)",
            .description =
                "Walks every cosmetic in the catalog (about 2500 on the current build), resolves its preview "
                "geometry, material sections and textures, and returns one entry per item: status ok/fail, "
                "index, slot (cust_tops, cust_fullBodyCostume, board_bottomart, truck, ...), item path, and for "
                "ok items the section/material counts, main texture and any warning messages; fail items carry "
                "the reason. Also returns catalog totals and per-slot failure/unmatched/degraded counts. This is "
                "the place to find item paths for 'cosmetic import-mesh --donor'. Filters (--slot, --status, "
                "--text, --limit) are applied here after the scan, so totals always cover the whole catalog. "
                "FAIL rows are normal (faces, bodies, colours have no previewable mesh). Reads only; takes "
                "about 60-65 seconds and the unfiltered JSON is about 1.6 MB, so filter when you can.",
            .params =
                {
                    {"slot",
                     ParamType::String,
                     "Only this slot, e.g. cust_fullBodyCostume or truck (case-insensitive)"},
                    {"status",
                     ParamType::Enum,
                     "Only ok or only fail items",
                     false,
                     false,
                     {"all", "ok", "fail"},
                     Json("all")},
                    {"text", ParamType::String, "Only items whose path contains this text, e.g. isaacclarke"},
                    {"limit",
                     ParamType::Integer,
                     "Most items to return; 0 returns all matches",
                     false,
                     false,
                     {},
                     Json(0)},
                    game_root_param(),
                },
            .examples = {"cosmetic audit --slot cust_fullBodyCostume --status ok",
                         "cosmetic audit --text isaacclarke --json",
                         "cosmetic audit --status fail --limit 20"},
            .long_running = true,
            .run = run_cosmetic_audit,
        });

        r.add({
            .group = "cosmetic",
            .name = "import-mesh",
            .summary = "Make a new cosmetic from a donor item and a GLB/FBX mesh",
            .description =
                "Clones a donor cosmetic from the catalog into a new item and replaces its geometry with your "
                ".glb or .fbx mesh, skinned to the donor's skeleton. Writes --output as an FBProject (.fbproject) "
                "or a binary Frosty mod (.fbmod), plus <output file name without extension>.readback.glb next "
                "to it (the mesh as it was encoded, to check in Blender). Existing files are overwritten. "
                "Nothing in the ReSkate folder is changed. The new item is named after the model file, lowercased "
                "with spaces as _: 'My Deck.glb' gives items/<slot>/own_my_deck with appearance presets "
                "my_deck_ap, my_deck_role21_ap, ... so rename the model to name the item. The model file name "
                "may only use ASCII letters, numbers, spaces, _ and - (invalid_model_name). "
                "Gotchas: deck and wheel items cannot be donors (they share one mesh: "
                "donor_has_no_geometry_slot), so this cannot replace the shared skateboard deck mesh; each mesh "
                "object's material must map to one of the donor's "
                "material sections or the import fails (mesh_material_unroutable). Known working donor: "
                "items/cust_fullbodycostume/own_costume_gen_isaacclarke_00001. Find others with 'cosmetic audit'. "
                "Takes about 15 seconds.",
            .params =
                {
                    {"donor",
                     ParamType::String,
                     "Catalog item path of the donor cosmetic, e.g. "
                     "items/cust_fullbodycostume/own_costume_gen_isaacclarke_00001",
                     true,
                     true},
                    {"model", ParamType::Path, "Mesh to import (.glb or .fbx)", false, false},
                    {"output", ParamType::Path, "File to write: .fbproject or .fbmod", true, true},
                    game_root_param(),
                },
            .examples = {"cosmetic import-mesh items/cust_fullbodycostume/own_costume_gen_isaacclarke_00001 "
                         "razor_scooter_deck.glb scooter.fbproject",
                         "cosmetic import-mesh items/cust_fullbodycostume/own_costume_gen_isaacclarke_00001 "
                         "C:\\mods\\suit.fbx C:\\mods\\suit.fbmod --json"},
            .long_running = true,
            .run = run_import_mesh,
        });

        r.add({
            .group = "cosmetic",
            .name = "new-board-part",
            .summary =
                "Make your model its own selectable truck item (e.g. a scooter), leaving every other board as it is",
            .description =
                "Clones a truck item into a NEW truck item whose geometry is your .glb/.fbx (the engine's "
                "cosmetic-mesh-import, "
                "which gives the new item its own geometry asset and bundle). Unlike 'mesh replace', which changes a "
                "shared mesh "
                "for every board, only players who pick the new item see it. Truck items are the board parts the game "
                "gives "
                "their own geometry; deck, grip and wheel items share one mesh, so they cannot carry a model of their "
                "own.\n\n"
                "The engine only takes a model whose materials are named after the donor mesh's material sections, so "
                "Studio+ "
                "reads the donor item's geometry (its AssetPaths entry with AssetTypeId 2), finds that mesh, and "
                "imports a copy "
                "of your model with every material named after its main section (or --section). The new item is called "
                "items/truck/own_<name> (--name, default the model's file name). Model space as for mesh replace: Y "
                "up, metres, "
                "board length along Z, origin on the ground under the board centre. In game, the deck and wheels the "
                "player "
                "picks still draw with it. Without --donor, a generic truck from the catalog is cloned (that runs the "
                "cosmetic "
                "audit, about a minute). Writes --output and a .readback.glb beside it; nothing in the ReSkate folder "
                "changes. "
                "Not yet verified in game: see docs/discovery/own-board.md.",
            .params =
                {
                    {"model", ParamType::Path, "Your model: .glb, or .fbx (needs Blender)", true, true},
                    {"output",
                     ParamType::Path,
                     "File to write: .fbmod (to build and install) or .fbproject",
                     true,
                     true},
                    {"name",
                     ParamType::String,
                     "Name of the new item (ASCII letters, numbers, spaces, _ and -). Default: the model's file name"},
                    {"donor",
                     ParamType::String,
                     "Truck item to clone, e.g. from 'cosmetic audit --slot truck'. Default: a generic truck"},
                    {"section",
                     ParamType::String,
                     "Material section to name every material after. Default: the donor mesh's main section"},
                    game_root_param(),
                },
            .examples =
                {"cosmetic new-board-part razor_scooter_deck.glb C:\\mods\\Razor_Scooter_Item.fbmod --name \"Razor "
                 "Scooter\"",
                 "cosmetic new-board-part scooter.glb scooter.fbmod --donor <truck item from cosmetic audit> --json"},
            .long_running = true,
            .run = run_new_board_part,
        });

        r.add({
            .group = "costume",
            .name = "info",
            .summary = "Validate and describe the native costume inside an FBMOD",
            .description =
                "Checks the native costume group in a binary .fbmod made by 'costume build' against the "
                "installed game and returns its preset, mesh resource id and counts of textures, geometry "
                "chunks, morph chunks and marked resources. Only 'costume build' output carries the native "
                "costume recipe: .fbmod files from 'cosmetic import-mesh' give no_native_costume, and "
                ".fbproject files are rejected (the engine needs a binary FBMOD). Reads only, about 3 seconds.",
            .params =
                {
                    {"fbmod", ParamType::Path, "Binary .fbmod produced by costume build", true, true},
                    game_root_param(),
                },
            .examples = {"costume info C:\\mods\\my_costume.fbmod", "costume info my_costume.fbmod --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                fs::path root = checked_game_root(c, a);
                fs::path file = path_arg(a, "fbmod");
                require_extension(file, "fbmod", {".fbmod"});
                require_file(file, "fbmod");
                EngineRun run = run_engine(c, {"native-costume-info", path_utf8(root), path_utf8(file)}, engine_stderr);
                check_run(
                    run,
                    {
                        {"not a binary FBMOD", "not_binary_fbmod", ""},
                        {"no native costume group", "no_native_costume", "Only costume build output contains one"},
                        {"invalid native costume group", "invalid_native_costume", ""},
                    });
                Json out = key_values(run.out_lines);
                out["file"] = path_utf8(file);
                return out;
            },
        });

        r.add({
            .group = "costume",
            .name = "donors",
            .summary = "Check the stock donor assets native costumes are built from",
            .description =
                "Measures the pinned Isaac Clarke full-body costume assets that 'costume build' clones and "
                "returns partitions, instances and bytes (5, 74 and 41442 on build 25414733). A quick health "
                "check that the installed game still has the donors native costumes need. Reads only, about "
                "3 seconds.",
            .params = {game_root_param()},
            .examples = {"costume donors", "costume donors --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                EngineRun run =
                    run_engine(c, {"native-costume-donors", path_utf8(checked_game_root(c, a))}, engine_stderr);
                check_run(run, {});
                return key_values(run.out_lines);
            },
        });

        r.add({
            .group = "costume",
            .name = "build",
            .summary = "Build a native full-body costume from a package folder",
            .description =
                "Builds a native full-body costume (cloned from the Isaac Clarke donor) from a prepared package "
                "folder and writes it to --output as .fbproject or .fbmod. Nothing in the ReSkate folder is "
                "changed. The package folder must hold recipe.json, mesh.res and chunks\\<GUID>.chunk (exactly 8 "
                "geometry and morph chunks); these are checked before the engine runs. Private textures, when "
                "the recipe uses them, sit in a folder per texture category holding texture.ebx, texture.res, "
                "texture.resmeta and texture.chunk (the engine checks these). recipe.json needs Assets (five roles), "
                "TextureNames, "
                "MaterialKeys (fourteen), MorphChunks (2), MeshResourceId and DiagnosticMode; the item must be "
                "Items/cust_fullBodyCostume/Own_..., the preset must end _dmPreset. No command here creates a "
                "package; ReSkate Studio's NATIVE COSTUME page uses the same format. Check the result with "
                "'costume info'.",
            .params =
                {
                    {"package", ParamType::Path, "Package folder with recipe.json, mesh.res and chunks\\", true, true},
                    {"output", ParamType::Path, "File to write: .fbproject or .fbmod", true, true},
                    game_root_param(),
                },
            .examples = {"costume build C:\\mods\\costume_pkg C:\\mods\\costume.fbmod",
                         "costume build costume_pkg costume.fbproject --json"},
            .long_running = true,
            .run = [](Context &c, const Json &a) -> Json
            {
                fs::path root = checked_game_root(c, a);
                fs::path package = path_arg(a, "package");
                fs::path output = path_arg(a, "output");
                require_extension(output, "output", {".fbproject", ".fbmod"});
                require_output_dir(output, "output");
                check_costume_package(package);
                EngineRun run = run_engine(
                    c, {"native-costume-build", path_utf8(root), path_utf8(package), path_utf8(output)}, engine_stderr);
                check_run(run,
                          {
                              {"invalid native costume recipe", "invalid_recipe", ""},
                              {"invalid JSON", "invalid_recipe", "recipe.json is not valid JSON"},
                              {"native costume package", "invalid_package", ""},
                              {"native costume build failed", "build_failed", ""},
                          },
                          "build_failed");
                Json out = key_values(run.out_lines);
                if (!out.contains("output"))
                    out["output"] = path_utf8(output);
                out["output_bytes"] =
                    file_size_or(fs::path(utf8_to_wide(out["output"].is_string() ? out["output"].get<std::string>()
                                                                                 : path_utf8(output))),
                                 0);
                out["format"] = ext_of(output) == ".fbmod" ? "fbmod" : "fbproject";
                return out;
            },
        });

        r.add({
            .group = "texture",
            .name = "list",
            .summary = "Decode test: decode the first N game textures and report size and format",
            .description =
                "Decoder self-test, not a texture browser: decodes the first --count texture resources in the "
                "game and returns how many decoded plus up to 10 samples (name, width, height, DXGI format such "
                "as BC1_UNORM or BC7_SRGB, bytes). A count larger than the game holds fails with "
                "not_enough_textures after decoding them all. Reads only; about 3 seconds for the default.",
            .params =
                {
                    {"count", ParamType::Integer, "How many textures to decode", false, true, {}, Json(100)},
                    game_root_param(),
                },
            .examples = {"texture list", "texture list 500 --json"},
            .long_running = true,
            .run = [](Context &c, const Json &a) -> Json
            {
                require_positive(a, "count");
                EngineRun run = run_engine(
                    c,
                    {"textures", path_utf8(checked_game_root(c, a)), std::to_string(a["count"].get<long long>())},
                    engine_stderr);
                check_run(
                    run,
                    {bad_number, {"not enough texture resources", "not_enough_textures", "Use a smaller --count"}});
                static const std::regex re(R"(^(.*) = (\d+)x(\d+) (\S+) \((\d+) bytes\)$)");
                Json samples = Json::array(), lines = Json::array(), out = Json::object();
                std::smatch m;
                for (const auto &line : run.out_lines)
                {
                    if (std::regex_match(line, m, re))
                        samples.push_back({{"name", m[1].str()},
                                           {"width", std::stoll(m[2].str())},
                                           {"height", std::stoll(m[3].str())},
                                           {"format", m[4].str()},
                                           {"bytes", std::stoll(m[5].str())}});
                    else if (starts_with(line, "decoded-textures="))
                        out["decoded"] = typed_value(line.substr(17));
                    else
                        lines.push_back(line);
                }
                out["samples"] = samples;
                if (!lines.empty())
                    out["lines"] = lines;
                return out;
            },
        });

        r.add({
            .group = "mesh",
            .name = "list",
            .summary = "Decode test: decode the first N game meshes and resolve their materials",
            .description =
                "Decoder self-test, not a mesh search: decodes the first --count MeshSet resources, resolves "
                "their materials and returns counts (decoded_meshes, resolved_materials, udim_materials, "
                "material_warnings), the warning reasons with counts (e.g. 'buildkit pattern paint requires UV1 "
                "baking'), and up to 10 samples (name, kind, lod, vertices, triangles). Reads only; about 3 "
                "seconds for the default.",
            .params =
                {
                    {"count", ParamType::Integer, "How many meshes to decode", false, true, {}, Json(25)},
                    game_root_param(),
                },
            .examples = {"mesh list", "mesh list 100 --json"},
            .long_running = true,
            .run = [](Context &c, const Json &a) -> Json
            {
                require_positive(a, "count");
                EngineRun run = run_engine(
                    c,
                    {"meshes", path_utf8(checked_game_root(c, a)), std::to_string(a["count"].get<long long>())},
                    engine_stderr);
                check_run(run, {bad_number});
                static const std::regex re(R"(^(.*) = (\S+) LOD (\d+) \((\d+) vertices, (\d+) triangles\)$)");
                Json samples = Json::array(), reasons = Json::array();
                std::vector<std::string> plain;
                std::smatch m;
                for (const auto &line : run.out_lines)
                {
                    if (std::regex_match(line, m, re))
                    {
                        samples.push_back({{"name", m[1].str()},
                                           {"kind", m[2].str()},
                                           {"lod", std::stoll(m[3].str())},
                                           {"vertices", std::stoll(m[4].str())},
                                           {"triangles", std::stoll(m[5].str())}});
                    }
                    else if (starts_with(line, "material-warning-reason="))
                    {
                        auto f = split(line.substr(24), '\t');
                        Json reason = {{"count", typed_value(f[0])}};
                        if (f.size() > 1)
                            reason["reason"] = f[1];
                        reasons.push_back(reason);
                    }
                    else
                    {
                        plain.push_back(line);
                    }
                }
                Json out = key_values(plain);
                out["warning_reasons"] = reasons;
                out["samples"] = samples;
                return out;
            },
        });

        r.add({
            .group = "mesh",
            .name = "author",
            .summary = "Self-test of the mesh encoder (author and decode a tiny test mesh)",
            .description =
                "Diagnostic only: takes a fixed buildkit donor mesh, authors a tiny test MeshSet (6 vertices, "
                "2 triangles) in memory and decodes it back. Returns donor, resource_bytes, chunk_bytes, "
                "decoded_vertices and decoded_triangles (6 and 2 when the encoder works). Takes no input mesh "
                "and writes nothing. To put your own mesh in the game use 'cosmetic import-mesh'.",
            .params = {game_root_param()},
            .examples = {"mesh author", "mesh author --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                EngineRun run = run_engine(c, {"mesh-author", path_utf8(checked_game_root(c, a))}, engine_stderr);
                check_run(run, {});
                Json out = key_values(run.out_lines);
                if (out.contains("decoded_vertices") && out.contains("decoded_triangles"))
                    out["passed"] = out["decoded_vertices"] == 6 && out["decoded_triangles"] == 2;
                return out;
            },
        });

        r.add({
            .group = "mesh",
            .name = "tangents",
            .summary = "Sample tangent frames from game meshes (encoding statistics)",
            .description =
                "Diagnostic only: samples tangent frames from game mesh data to check the tangent encoding "
                "and returns samples, frames (usable frames found) and up_face. Sampling is random, so frames "
                "differs between identical runs. Reads only, about 3 seconds.",
            .params =
                {
                    {"samples", ParamType::Integer, "How many tangent frames to sample", false, true, {}, Json(20000)},
                    game_root_param(),
                },
            .examples = {"mesh tangents", "mesh tangents 5000 --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                require_positive(a, "samples");
                EngineRun run = run_engine(
                    c,
                    {"tangents", path_utf8(checked_game_root(c, a)), std::to_string(a["samples"].get<long long>())},
                    engine_stderr);
                check_run(run, {bad_number, {"no usable tangent frames", "no_tangent_frames", "Try more --samples"}});
                return key_values(run.out_lines);
            },
        });

        register_mesh_replace_commands(r);
    }
} // namespace studio
