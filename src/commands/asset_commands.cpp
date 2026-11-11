// Game asset search and EBX inspection (reskate_cli: assets, read, ebx, find-name, find-type,
// ebx-values, ebx-roundtrip, ebx-author, schema, cas, toc-stock, donors).
#include "commands/commands.h"
#include "core/engine.h"
#include "core/settings.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;

namespace studio
{

    namespace
    {

        // ---------------------------------------------------------------- small helpers

        bool starts_with(std::string_view s, std::string_view prefix)
        {
            return s.substr(0, prefix.size()) == prefix;
        }
        bool ends_with(std::string_view s, std::string_view suffix)
        {
            return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
        }

        std::vector<std::string> split_spaces(std::string_view text)
        {
            std::vector<std::string> out;
            std::string word;
            for (char ch : text)
            {
                if (ch == ' ')
                {
                    if (!word.empty())
                        out.push_back(word);
                    word.clear();
                }
                else
                    word.push_back(ch);
            }
            if (!word.empty())
                out.push_back(word);
            return out;
        }

        // "key=value" tokens of one line become an object; tokens without '=' are ignored.
        Json token_values(std::string_view text)
        {
            Json out = Json::object();
            for (const auto &token : split_spaces(text))
            {
                auto eq = token.find('=');
                if (eq == std::string::npos || eq == 0)
                    continue;
                out[token.substr(0, eq)] = typed_value(std::string_view(token).substr(eq + 1));
            }
            return out;
        }

        // "0x20" -> 32. Returns the text unchanged when it is not hex.
        Json hex_number(const std::string &text)
        {
            if (!starts_with(text, "0x"))
                return typed_value(text);
            try
            {
                size_t used = 0;
                long long v = std::stoll(text.substr(2), &used, 16);
                if (used == text.size() - 2)
                    return v;
            }
            catch (...)
            {
            }
            return text;
        }

        // "name[12]" -> 12, or -1.
        long long bracket_index(std::string_view token)
        {
            auto open = token.find('['), close = token.find(']');
            if (open == std::string_view::npos || close == std::string_view::npos || close <= open + 1)
                return -1;
            try
            {
                return std::stoll(std::string(token.substr(open + 1, close - open - 1)));
            }
            catch (...)
            {
                return -1;
            }
        }

        std::string lowercase(std::string text)
        {
            for (auto &ch : text)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return text;
        }

        // The game root, made absolute (the engine runs with the engine folder as its working directory,
        // so a relative path would point somewhere else for it) and checked before the engine spends
        // ~3 s loading the index only to fail with a cryptic message.
        fs::path checked_game_root(Context &c, const Json &a)
        {
            std::error_code ec;
            fs::path root = fs::absolute(c.game_root(a), ec);
            if (ec)
                root = c.game_root(a);
            if (!fs::is_directory(root, ec))
                throw Error("game_root_not_found",
                            "ReSkate folder does not exist: " + path_utf8(root),
                            {{"path", path_utf8(root)}});
            if (!fs::exists(root / L"Skate.exe", ec) || !fs::exists(root / L"Data" / L"layout.toc", ec))
                throw Error("not_a_game_root",
                            "Not a Skate install (needs Skate.exe and Data\\layout.toc): " + path_utf8(root) +
                                ". Pass the Skate install folder with --game-root.",
                            {{"path", path_utf8(root)}});
            return root;
        }

        long long positive(const Json &a, const std::string &param)
        {
            long long v = a[param];
            if (v < 1)
                throw Error("invalid_arguments", "--" + param + " must be at least 1", {{"param", param}});
            return v;
        }

        bool contains(std::string_view text, std::string_view part)
        {
            return text.find(part) != std::string_view::npos;
        }

        // Runs the engine and turns its known failure messages into specific error codes.
        // `missing` describes the looked-up asset (e.g. "EBX asset 'a/b'") for a clearer asset_not_found message.
        EngineRun run_checked(Context &c,
                              const std::vector<std::string> &args,
                              const std::string &code = "engine_failed",
                              const std::string &missing = "")
        {
            EngineRun run = run_engine(c, args);
            try
            {
                require_success(run, code);
            }
            catch (Error &e)
            {
                std::string message = e.what();
                if (contains(message, "asset was not found"))
                { // "asset was not found" / "EBX asset was not found"
                    if (missing.empty())
                        throw Error("asset_not_found", message, e.details);
                    std::string hint = starts_with(missing, "chunk")
                                           ? " Chunk GUIDs come from ChunkId fields, e.g. in 'ebx values' output."
                                           : " Names are lowercase paths; search with 'asset find <part of the name>'.";
                    throw Error("asset_not_found",
                                "No " + missing + " in the game (engine: " + message + ")." + hint,
                                e.details);
                }
                if (contains(message, "requires a valid Skate installation") ||
                    contains(message, "Oodle codec was not found") || contains(message, "for SHA-256"))
                    throw Error(
                        "not_a_game_root", message + ". Pass the Skate install folder with --game-root.", e.details);
                if (contains(message, "TOC envelope is invalid"))
                    throw Error("invalid_toc", message + ". The file is not a superbundle .toc.", e.details);
                if (contains(message, "could not open binary input"))
                {
                    if (!args.empty() && args[0] == "toc-stock")
                        throw Error("invalid_toc", message, e.details);
                    throw Error(
                        "game_data_unreadable",
                        message + ". The engine could not open a game data file; check the Skate install is complete.",
                        e.details);
                }
                if (contains(message, "not enough EBX assets"))
                    throw Error("count_too_large",
                                message + ". The game has fewer EBX assets than --count (see: asset summary).",
                                e.details);
                throw;
            }
            return run;
        }

        // ---------------------------------------------------------------- ebx-values parser
        //
        // reskate_cli ebx-values prints an indentation tree (two spaces per level, no closing braces):
        //   root=<Type> file=<partition guid>
        //   import[i] partition=<guid> instance=<guid>
        //   instance[i] <Type> [exported] guid=<guid> raw=<hex>
        //     Field = value | Field { (struct) | Field [n] (array, elements "[k] = v" / "[k] {")
        //   type[i] <Type> encoded=0x30 size=88 fields=10
        //   import[i] <partition>/<instance> <resolved asset name>

        struct TreeLine
        {
            size_t indent;
            std::string text;
        };

        Json parse_scalar(const std::string &v)
        {
            if (v.empty())
                return nullptr;
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
                return v.substr(1, v.size() - 2);
            if (starts_with(v, "pointer(") && v.back() == ')')
            {
                std::string inner = v.substr(8, v.size() - 9);
                auto comma = inner.find(',');
                if (comma != std::string::npos)
                {
                    Json kind = typed_value(inner.substr(0, comma)), index = typed_value(inner.substr(comma + 1));
                    if (kind.is_number_integer() && index.is_number_integer())
                        return {{"$pointer", Json::array({kind, index})}, {"$raw", v}};
                }
                return v;
            }
            if (starts_with(v, "resource(") && v.back() == ')')
                return {{"resource", v.substr(9, v.size() - 10)}};
            if (starts_with(v, "boxed(") && v.back() == ')')
            {
                Json boxed = Json::object();
                std::string inner = v.substr(6, v.size() - 7);
                for (auto &part : split_spaces(inner))
                {
                    if (!part.empty() && part.back() == ',')
                        part.pop_back();
                    auto eq = part.find('=');
                    if (eq == std::string::npos)
                        continue;
                    std::string key = part.substr(0, eq), value = part.substr(eq + 1);
                    boxed[key] = key == "type" ? Json(value) : typed_value(value);
                }
                return {{"boxed", boxed}};
            }
            Json value = typed_value(v);
            // JSON has no NaN or infinity (nlohmann would print null): keep those as the engine's text, e.g. "-nan".
            if (value.is_number_float() && !std::isfinite(value.get<double>()))
                return v;
            return value;
        }

        // Parses the children of one struct or array: every following line at exactly `indent`.
        Json parse_children(const std::vector<TreeLine> &lines, size_t &i, size_t indent, bool is_array, Json &unparsed)
        {
            Json out = is_array ? Json::array() : Json::object();
            while (i < lines.size() && lines[i].indent >= indent)
            {
                if (lines[i].indent > indent)
                {
                    unparsed.push_back(lines[i].text);
                    ++i;
                    continue;
                }
                const std::string &text = lines[i].text;
                ++i;
                std::string key;
                Json value;
                if (auto eq = text.find(" = "); eq != std::string::npos)
                {
                    key = text.substr(0, eq);
                    value = parse_scalar(text.substr(eq + 3));
                }
                else if (ends_with(text, " {"))
                {
                    key = text.substr(0, text.size() - 2);
                    value = parse_children(lines, i, indent + 2, false, unparsed);
                }
                else if (auto open = text.rfind(" ["); open != std::string::npos && text.back() == ']')
                {
                    key = text.substr(0, open);
                    value = parse_children(lines, i, indent + 2, true, unparsed);
                }
                else
                {
                    key = text; // a field the engine prints without a value
                    value = nullptr;
                }
                if (is_array)
                {
                    out.push_back(std::move(value));
                }
                else if (out.contains(key))
                {
                    // Not expected in EBX, but never drop data: later duplicates get a "#n" suffix.
                    int n = 2;
                    while (out.contains(key + "#" + std::to_string(n)))
                        ++n;
                    out[key + "#" + std::to_string(n)] = std::move(value);
                }
                else
                {
                    out[key] = std::move(value);
                }
            }
            return out;
        }

        // Replaces {"$pointer":[kind,index]} with what it points at:
        // kind 0 = null, kind 1 = an instance in this asset, kind 2 = an entry in the import table.
        // Boxed values get the matching entry of the boxed table (by offset) merged in.
        void resolve_pointers(Json &node, const Json &instances, const Json &imports, const Json &boxed)
        {
            if (node.is_array())
            {
                for (auto &child : node)
                    resolve_pointers(child, instances, imports, boxed);
                return;
            }
            if (!node.is_object())
                return;
            if (node.contains("boxed") && node["boxed"].is_object() && node["boxed"].contains("offset"))
            {
                for (const auto &entry : boxed)
                {
                    if (entry.value("offset", Json()) != node["boxed"]["offset"])
                        continue;
                    for (const char *key : {"index", "count", "bytes", "data"})
                        if (entry.contains(key))
                            node["boxed"][key] = entry[key];
                    break;
                }
                return;
            }
            if (node.contains("$pointer"))
            {
                long long kind = node["$pointer"][0], index = node["$pointer"][1];
                std::string raw = node["$raw"];
                if (kind == 0)
                {
                    node = nullptr;
                }
                else if (kind == 1 && index >= 0 && static_cast<size_t>(index) < instances.size())
                {
                    node = {{"ref", "instance"},
                            {"instance", index},
                            {"type", instances[static_cast<size_t>(index)]["type"]}};
                }
                else if (kind == 2 && index >= 0 && static_cast<size_t>(index) < imports.size())
                {
                    const Json &imp = imports[static_cast<size_t>(index)];
                    Json asset = imp.contains("asset") ? imp["asset"] : Json(nullptr);
                    node = {{"ref", "import"}, {"import", index}, {"asset", asset}};
                    if (!asset.is_string())
                    { // the engine could not name the imported asset: give its GUIDs instead
                        node["partition"] = imp.value("partition", Json());
                        node["instance_guid"] = imp.value("instance", Json());
                    }
                }
                else
                {
                    node = {{"ref", "unknown"}, {"raw", raw}};
                }
                return;
            }
            for (auto it = node.begin(); it != node.end(); ++it)
                resolve_pointers(*it, instances, imports, boxed);
        }

        Json parse_ebx_values(const std::string &asset, const std::vector<std::string> &out_lines)
        {
            std::vector<TreeLine> lines;
            for (const auto &l : out_lines)
            {
                size_t indent = l.find_first_not_of(' ');
                if (indent == std::string::npos)
                    continue;
                lines.push_back({indent, l.substr(indent)});
            }
            Json result = {{"asset", asset}, {"root_type", nullptr}, {"partition_guid", nullptr}};
            Json instances = Json::array(), imports = Json::array(), types = Json::array(), unparsed = Json::array();
            Json boxed = Json::array();
            auto import_at = [&](long long index) -> Json &
            {
                while (imports.size() <= static_cast<size_t>(index))
                    imports.push_back({{"index", static_cast<long long>(imports.size())}});
                return imports[static_cast<size_t>(index)];
            };
            size_t i = 0;
            while (i < lines.size())
            {
                if (lines[i].indent != 0)
                {
                    unparsed.push_back(lines[i].text);
                    ++i;
                    continue;
                }
                const std::string text = lines[i].text;
                ++i;
                auto words = split_spaces(text);
                if (starts_with(text, "root="))
                {
                    Json kv = token_values(text);
                    result["root_type"] = kv.value("root", Json());
                    result["partition_guid"] = kv.value("file", Json());
                }
                else if (starts_with(text, "import[") && words.size() >= 2)
                {
                    long long index = bracket_index(words[0]);
                    if (index < 0)
                    {
                        unparsed.push_back(text);
                        continue;
                    }
                    Json &imp = import_at(index);
                    if (starts_with(words[1], "partition="))
                    {
                        Json kv = token_values(text);
                        imp["partition"] = kv.value("partition", Json());
                        imp["instance"] = kv.value("instance", Json());
                    }
                    else
                    {
                        auto slash = words[1].find('/');
                        if (slash != std::string::npos)
                        {
                            imp["partition"] = words[1].substr(0, slash);
                            imp["instance"] = words[1].substr(slash + 1);
                        }
                        auto name_at = text.find(' ', text.find(words[1]));
                        imp["asset"] = name_at == std::string::npos ? Json(nullptr) : Json(text.substr(name_at + 1));
                    }
                }
                else if (starts_with(text, "instance[") && words.size() >= 2)
                {
                    Json inst = {{"index", bracket_index(words[0])}, {"type", words[1]}, {"exported", false}};
                    for (size_t w = 2; w < words.size(); ++w)
                    {
                        if (words[w] == "exported")
                            inst["exported"] = true;
                        else if (starts_with(words[w], "guid="))
                            inst["guid"] = words[w].substr(5);
                        else if (starts_with(words[w], "raw="))
                            inst["raw"] = words[w].substr(4);
                    }
                    inst["fields"] = parse_children(lines, i, 2, false, unparsed);
                    instances.push_back(std::move(inst));
                }
                else if (starts_with(text, "type[") && words.size() >= 2)
                {
                    Json kv = token_values(text);
                    types.push_back({{"index", bracket_index(words[0])},
                                     {"name", words[1]},
                                     {"encoded", kv.value("encoded", Json())},
                                     {"size", kv.value("size", Json())},
                                     {"field_count", kv.value("fields", Json())}});
                }
                else if (starts_with(text, "boxed[") && words.size() >= 2)
                {
                    // boxed[i] offset=1072 count=1 type=0x60 class=0xffff bytes=8 data=0500000000000000
                    Json entry = {{"index", bracket_index(words[0])}};
                    for (size_t w = 1; w < words.size(); ++w)
                    {
                        auto eq = words[w].find('=');
                        if (eq == std::string::npos)
                            continue;
                        std::string key = words[w].substr(0, eq), value = words[w].substr(eq + 1);
                        bool numeric = key == "offset" || key == "count" || key == "bytes";
                        entry[key] = numeric ? typed_value(value) : Json(value); // data stays hex text
                    }
                    boxed.push_back(std::move(entry));
                }
                else
                {
                    unparsed.push_back(text);
                }
            }
            for (auto &inst : instances)
                resolve_pointers(inst["fields"], instances, imports, boxed);
            result["instance_count"] = instances.size();
            result["instances"] = std::move(instances);
            result["imports"] = std::move(imports);
            result["types"] = std::move(types);
            if (!boxed.empty())
                result["boxed"] = std::move(boxed);
            if (!unparsed.empty())
                result["unparsed_lines"] = std::move(unparsed);
            return result;
        }

        // ---------------------------------------------------------------- schema parser

        // Frostbite EBX field type codes as printed by `schema` (type=N).
        constexpr std::array<const char *, 29> k_field_types = {
            "Inherited", "DbObject", "Struct",        "Pointer",   "Array",   "FixedArray", "String", "CString",
            "Enum",      "FileRef",  "Boolean",       "Int8",      "UInt8",   "Int16",      "UInt16", "Int32",
            "UInt32",    "Int64",    "UInt64",        "Float32",   "Float64", "Guid",       "Sha1",   "ResourceRef",
            "Function",  "TypeRef",  "BoxedValueRef", "Interface", "Delegate"};

        Json parse_schema(const std::string &asset, const std::vector<std::string> &out_lines)
        {
            Json result = {{"asset", asset}, {"root_type", nullptr}};
            Json instances = Json::array(), types = Json::array(), unparsed = Json::array();
            for (const auto &line : out_lines)
            {
                if (starts_with(line, "root="))
                {
                    auto words = split_spaces(line);
                    result["root_type"] = words[0].substr(5);
                    for (const auto &w : words)
                    {
                        if (!starts_with(w, "data="))
                            continue;
                        std::string range = w.substr(5);
                        auto dots = range.find("..");
                        if (dots == std::string::npos)
                            continue;
                        result["data_start"] = hex_number(range.substr(0, dots));
                        result["data_end"] = hex_number(range.substr(dots + 2));
                    }
                }
                else if (starts_with(line, "instance["))
                {
                    Json kv = token_values(line);
                    Json inst = {{"index", bracket_index(line)}};
                    for (const char *key : {"fixup", "descriptor"})
                        inst[key] = kv.value(key, Json());
                    inst["offset"] = kv.contains("offset") && kv["offset"].is_string() ? hex_number(kv["offset"])
                                                                                       : kv.value("offset", Json());
                    inst["exported"] = kv.value("exported", 0) != 0;
                    instances.push_back(std::move(inst));
                }
                else if (starts_with(line, "type["))
                {
                    auto words = split_spaces(line);
                    Json kv = token_values(line);
                    types.push_back({{"index", bracket_index(words[0])},
                                     {"name", words.size() > 1 ? words[1] : ""},
                                     {"size", kv.value("size", Json())},
                                     {"align", kv.value("align", Json())},
                                     {"field_count", kv.value("fields", Json())},
                                     {"fields", Json::array()}});
                }
                else if (starts_with(line, "data-prefix="))
                {
                    result["data_prefix"] = line.substr(12);
                }
                else if (starts_with(line, " ") && !types.empty())
                {
                    std::string body = line.substr(line.find_first_not_of(' '));
                    std::string name;
                    if (!starts_with(body, "offset="))
                        name = body.substr(0, body.find(' '));
                    Json kv = token_values(body);
                    Json field = {{"name", name},
                                  {"offset", kv.value("offset", Json())},
                                  {"type_code", kv.value("type", Json())},
                                  {"category", kv.value("category", Json())},
                                  {"class_index", kv.value("class", Json())}};
                    types.back()["fields"].push_back(std::move(field));
                }
                else
                {
                    unparsed.push_back(line);
                }
            }
            // Second pass: names for type codes and class indices, base classes, and enums.
            Json shaped = Json::array();
            for (const auto &type : types)
            {
                Json t = {{"index", type["index"]}, {"name", type["name"]}};
                bool is_enum = type["size"] == 0 && type["align"] == 0 && !type["fields"].empty();
                t["kind"] = is_enum ? "enum" : "type";
                if (is_enum)
                {
                    Json members = Json::array();
                    for (const auto &f : type["fields"])
                        members.push_back({{"name", f["name"]}, {"value", f["offset"]}});
                    t["member_count"] = members.size();
                    t["members"] = std::move(members);
                    shaped.push_back(std::move(t));
                    continue;
                }
                t["base"] = nullptr;
                t["size"] = type["size"];
                t["align"] = type["align"];
                t["field_count"] = type["field_count"];
                Json fields = Json::array();
                for (const auto &f : type["fields"])
                {
                    long long code = f["type_code"].is_number_integer() ? f["type_code"].get<long long>() : -1;
                    long long cls = f["class_index"].is_number_integer() ? f["class_index"].get<long long>() : 65535;
                    std::string class_name;
                    if (cls >= 0 && cls != 65535 && static_cast<size_t>(cls) < types.size())
                        class_name = types[static_cast<size_t>(cls)]["name"].get<std::string>();
                    if (f["name"] == "" && code == 0)
                    { // unnamed inherited entry = base class
                        if (!class_name.empty())
                            t["base"] = class_name;
                        continue;
                    }
                    Json out = {{"name", f["name"]}, {"offset", f["offset"]}};
                    out["type"] = code >= 0 && static_cast<size_t>(code) < k_field_types.size()
                                      ? Json(k_field_types[static_cast<size_t>(code)])
                                      : Json(nullptr);
                    out["type_code"] = f["type_code"];
                    out["array"] = f["category"] == 4;
                    out["category"] = f["category"];
                    if (!class_name.empty())
                        out["class"] = class_name;
                    if (cls != 65535)
                        out["class_index"] = cls;
                    fields.push_back(std::move(out));
                }
                t["fields"] = std::move(fields);
                shaped.push_back(std::move(t));
            }
            types = std::move(shaped);
            result["instances"] = std::move(instances);
            result["types"] = std::move(types);
            if (!unparsed.empty())
                result["unparsed_lines"] = std::move(unparsed);
            return result;
        }

        const char *k_game_root_note =
            " Takes about 3 s, most of it loading the game's asset index (about 185,000 EBX).";

    } // namespace

    void register_asset_commands(Registry &r)
    {
        r.add({
            .group = "asset",
            .name = "find",
            .summary = "Search game assets by name",
            .description =
                std::string(
                    "Substring search over every EBX asset and resource name in the game. Names in the index are "
                    "lowercase, so the search text is lowercased for you. Returns count and each match with its kind "
                    "(ebx or res) and name, in index order (not sorted). No match is not an error: count is 0. "
                    "Use the names with 'asset read', 'ebx values' or 'ebx schema'. Reads only.") +
                k_game_root_note,
            .params =
                {
                    {"text", ParamType::String, "Part of the asset name, e.g. baker_popsicle", true, true},
                    {"limit", ParamType::Integer, "Most matches to return (at least 1)", false, false, {}, Json(100)},
                    game_root_param(),
                },
            .examples = {"asset find baker_popsicle", "asset find deckgraphic --limit 10 --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                std::string text = a["text"];
                for (auto &ch : text)
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                long long limit = positive(a, "limit");
                EngineRun run =
                    run_checked(c, {"find-name", path_utf8(checked_game_root(c, a)), text, std::to_string(limit)});
                Json matches = Json::array();
                long long count = 0;
                for (const auto &line : run.out_lines)
                {
                    if (line.rfind("matches=", 0) == 0)
                    {
                        count = std::stoll(line.substr(8));
                        continue;
                    }
                    auto space = line.find(' ');
                    if (space == std::string::npos)
                        continue;
                    matches.push_back({{"kind", line.substr(0, space)}, {"name", line.substr(space + 1)}});
                }
                return {{"count", count}, {"matches", matches}};
            },
        });

        r.add({
            .group = "asset",
            .name = "summary",
            .summary = "Count the game's superbundles, bundles, EBX, resources and chunks",
            .description =
                std::string("Loads the game's asset index and returns how many superbundles, bundles, EBX assets, "
                            "resources and "
                            "chunks it holds. A quick check that a ReSkate folder is valid and readable. Reads only, "
                            "writes nothing.") +
                k_game_root_note +
                " --patch is passed through to the engine; on the tested install the counts were identical with and "
                "without it.",
            .params =
                {
                    {"patch",
                     ParamType::Boolean,
                     "Pass --patch to the engine (include the Patch layer)",
                     false,
                     false,
                     {},
                     Json(false)},
                    game_root_param(),
                },
            .examples = {"asset summary", "asset summary --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                std::vector<std::string> args = {"assets", path_utf8(checked_game_root(c, a))};
                add_flag(args, a, "patch", "--patch");
                EngineRun run = run_checked(c, args);
                return parse_key_values(run.out_lines);
            },
        });

        r.add({
            .group = "asset",
            .name = "read",
            .summary = "Locate one asset: GUID, resource id, sizes and the bundle it is in",
            .description =
                std::string(
                    "Finds one asset by kind and name, decodes it from CAS and returns where it lives: GUID, resource "
                    "id and "
                    "type, encoded / decoded / expected sizes in bytes, and its bundle (and superbundle when the "
                    "engine "
                    "reports one). It does not return the payload; use 'ebx values' to see an EBX asset's contents. "
                    "Names are matched case-insensitively and come back lowercase. For kind chunk the name is a chunk "
                    "GUID in "
                    "the dashed form 8-4-4-4-12 (e.g. a ChunkId shown by 'ebx values'); chunks also report their "
                    "superbundle. "
                    "resource_id is returned as a decimal string because it is a 64-bit value. Reads only.") +
                k_game_root_note,
            .params =
                {
                    {"kind",
                     ParamType::Enum,
                     "Asset kind: ebx (EBX asset), res (resource; 'resource' is the same), chunk "
                     "(chunk by GUID)",
                     true,
                     true,
                     {"ebx", "res", "resource", "chunk"}},
                    {"name",
                     ParamType::String,
                     "Asset path for ebx/res (e.g. items/cust_board/own_deckgraphic_baker_popsicle_00001), "
                     "or for chunk a GUID like e6662b14-5db6-69d9-0e28-a3b5d04871ca",
                     true,
                     true},
                    game_root_param(),
                },
            .examples = {"asset read ebx items/cust_board/own_deckgraphic_baker_popsicle_00001",
                         "asset read res thumbnail/tool/own_deckgraphic_baker_popsicle_00001 --json",
                         "asset read chunk e6662b14-5db6-69d9-0e28-a3b5d04871ca --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                std::string kind = a["kind"], name = a["name"];
                if (kind == "resource")
                    kind = "res";
                if (kind == "chunk")
                {
                    // The engine only finds chunks by the dashed GUID; anything else costs ~3 s and says "not found".
                    if (name.size() == 38 && name.front() == '{' && name.back() == '}')
                        name = name.substr(1, 36);
                    bool guid = name.size() == 36;
                    for (size_t k = 0; guid && k < name.size(); ++k)
                        guid = (k == 8 || k == 13 || k == 18 || k == 23)
                                   ? name[k] == '-'
                                   : std::isxdigit(static_cast<unsigned char>(name[k])) != 0;
                    if (!guid)
                        throw Error("invalid_arguments",
                                    "--name: a chunk is named by its GUID in the form "
                                    "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx, got '" +
                                        name + "'",
                                    {{"param", "name"}});
                    name = lowercase(name);
                }
                std::string what = (kind == "ebx"   ? "EBX asset '"
                                    : kind == "res" ? "resource '"
                                                    : "chunk '") +
                                   name + "'";
                EngineRun run =
                    run_checked(c, {"read", path_utf8(checked_game_root(c, a)), kind, name}, "engine_failed", what);
                Json kv = Json::object();
                for (const auto &line : run.out_lines)
                {
                    auto eq = line.find('=');
                    if (eq != std::string::npos && eq > 0)
                        kv[line.substr(0, eq)] = line.substr(eq + 1);
                }
                auto number = [&](const char *key) -> Json
                { return kv.contains(key) ? typed_value(kv[key].get<std::string>()) : Json(nullptr); };
                Json out = {{"kind", kind},
                            {"name", kv.value("name", name)},
                            {"guid", kv.value("guid", Json())},
                            {"resource_id", kv.value("resource-id", Json())},
                            {"resource_type", number("resource-type")},
                            {"encoded_size", number("encoded")},
                            {"decoded_size", number("decoded")},
                            {"expected_size", number("expected")},
                            {"bundle", kv.value("bundle", Json())}};
                if (kv.contains("superbundle"))
                    out["superbundle"] = kv["superbundle"];
                for (auto &[key, value] : kv.items())
                {
                    static const std::vector<std::string> known = {"name",
                                                                   "guid",
                                                                   "resource-id",
                                                                   "resource-type",
                                                                   "encoded",
                                                                   "decoded",
                                                                   "expected",
                                                                   "bundle",
                                                                   "superbundle"};
                    if (std::find(known.begin(), known.end(), key) == known.end())
                        out[key] = typed_value(value.get<std::string>());
                }
                return out;
            },
        });

        r.add({
            .group = "asset",
            .name = "find-type",
            .summary = "List EBX assets whose root type is the given type",
            .description =
                std::string(
                    "Lists EBX assets whose root type equals the type name, e.g. TextureAsset or DelMarBaseItemAsset. "
                    "The match is exact but case-insensitive: a partial name such as Texture finds nothing, and a type "
                    "that "
                    "only appears inside assets (not as their root, e.g. SkateboardItemData) also finds nothing. Use "
                    "'ebx values' on an asset to see its root type. Reads only.") +
                " Stops early once --limit matches are found (about 3 s); a full scan with no or few matches takes 8 "
                "to 10 s.",
            .params =
                {
                    {"type", ParamType::String, "Exact root type name, e.g. TextureAsset", true, true},
                    {"limit", ParamType::Integer, "Most matches to return (at least 1)", false, false, {}, Json(20)},
                    game_root_param(),
                },
            .examples = {"asset find-type DelMarBaseItemAsset --limit 5", "asset find-type TextureAsset --json"},
            .long_running = true,
            .run = [](Context &c, const Json &a) -> Json
            {
                std::string type = a["type"];
                long long limit = positive(a, "limit");
                fs::path root = checked_game_root(c, a);
                c.progress(-1, "Scanning EBX root types for " + type);
                EngineRun run = run_checked(c, {"find-type", path_utf8(root), type, std::to_string(limit)});
                Json matches = Json::array();
                long long count = 0;
                for (const auto &line : run.out_lines)
                {
                    if (starts_with(line, "matches="))
                    {
                        count = std::stoll(line.substr(8));
                        continue;
                    }
                    matches.push_back(line);
                }
                return {{"type", type}, {"count", count}, {"limit_reached", count >= limit}, {"matches", matches}};
            },
        });

        r.add({
            .group = "asset",
            .name = "donors",
            .summary = "Show the stock assets native map authoring copies as templates",
            .description = std::string("Returns the stock donor asset the engine picks for each category when it "
                                       "authors native world and map "
                                       "data: RigidMeshAsset, TextureAsset, PhysicsAsset, ObjectBlueprint, "
                                       "ObjectBlueprint+Physics, LayerData, "
                                       "SubWorldData, DetachedSubWorldData and MeshVariationDatabase. 'count' is the "
                                       "engine's own donor total, "
                                       "which can be higher than the number of categories listed. Reads only.") +
                           k_game_root_note,
            .params = {game_root_param()},
            .examples = {"asset donors", "asset donors --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                EngineRun run = run_checked(c, {"donors", path_utf8(checked_game_root(c, a))});
                Json donors = Json::object();
                Json count = nullptr;
                Json other = Json::array();
                for (const auto &line : run.out_lines)
                {
                    auto eq = line.find('=');
                    if (eq == std::string::npos || eq == 0)
                    {
                        other.push_back(line);
                        continue;
                    }
                    std::string key = line.substr(0, eq), value = line.substr(eq + 1);
                    if (key == "donors")
                        count = typed_value(value);
                    else
                        donors[key] = value;
                }
                Json out = {{"count", count}, {"donors", donors}};
                if (!other.empty())
                    out["lines"] = other;
                return out;
            },
        });

        r.add({
            .group = "ebx",
            .name = "scan",
            .summary = "Decoder self-test: decode the first N EBX assets",
            .description =
                std::string(
                    "Validation pass, not a lister or filter. Decodes the first --count EBX assets in index order and "
                    "returns how many decoded, plus up to 10 samples with their root type and instance count. Fails "
                    "with "
                    "count_too_large if --count is more than the game has. Reads only.") +
                " About 3 s up to 20000 assets, longer for more.",
            .params =
                {
                    {"count",
                     ParamType::Integer,
                     "How many EBX assets to decode, from the start of the index (at least 1)",
                     false,
                     true,
                     {},
                     Json(100)},
                    game_root_param(),
                },
            .examples = {"ebx scan", "ebx scan 5000 --json"},
            .long_running = true,
            .run = [](Context &c, const Json &a) -> Json
            {
                long long count = positive(a, "count");
                fs::path root = checked_game_root(c, a);
                c.progress(-1, "Decoding " + std::to_string(count) + " EBX assets");
                EngineRun run = run_checked(c, {"ebx", path_utf8(root), std::to_string(count)}, "ebx_decode_failed");
                Json samples = Json::array();
                Json decoded = nullptr;
                for (const auto &line : run.out_lines)
                {
                    if (starts_with(line, "decoded-ebx="))
                    {
                        decoded = typed_value(line.substr(12));
                        continue;
                    }
                    auto sep = line.rfind(" = ");
                    if (sep == std::string::npos)
                        continue;
                    std::string rest = line.substr(sep + 3);
                    Json sample = {{"asset", line.substr(0, sep)}, {"root_type", rest.substr(0, rest.find(' '))}};
                    auto open = rest.find('(');
                    if (open != std::string::npos)
                        sample["instances"] = typed_value(rest.substr(open + 1, rest.find(' ', open) - open - 1));
                    samples.push_back(std::move(sample));
                }
                return {{"decoded", decoded}, {"samples", samples}};
            },
        });

        r.add({
            .group = "ebx",
            .name = "values",
            .summary = "Decode one EBX asset: every instance with its field values, imports and types",
            .description =
                std::string(
                    "The main way to look inside an EBX asset. Fully decodes it and returns:\n"
                    "  root_type, partition_guid\n"
                    "  instances: index, type, exported, guid, raw (header hex) and fields. Fields keep the engine's "
                    "order; "
                    "structs are objects, arrays are arrays, strings are strings, numbers are numbers.\n"
                    "  Pointers are resolved: null for a null pointer, {ref: instance, instance, type} for an instance "
                    "in "
                    "this asset, {ref: import, import, asset} for another asset (asset is the imported asset's name).\n"
                    "  resource(...) values become {resource: \"0x...\"}. Boxed values become {boxed: {type, offset, "
                    "index, "
                    "count, bytes, data}}, where data is the raw little-endian hex from the asset's boxed-value table "
                    "(also "
                    "returned whole as boxed).\n"
                    "  A field the engine prints without a value (e.g. DebugDataId) is null.\n"
                    "  imports: index, partition and instance GUIDs and the resolved asset name (null when the engine "
                    "cannot name it; pointers to such an import then carry partition and instance_guid instead).\n"
                    "  types: every type descriptor used (name, encoded flags, size, field_count).\n"
                    "Non-finite floats come back as the engine's text (e.g. \"-nan\"), since JSON has no NaN. "
                    "Enum fields come back as numbers; see 'ebx schema' for the enum member names. Any line the parser "
                    "did "
                    "not understand is kept under unparsed_lines. The asset name is case-insensitive. Reads only.") +
                k_game_root_note +
                " --patch is passed through to the engine; on the tested install the output was identical with it.",
            .params =
                {
                    {"asset",
                     ParamType::String,
                     "EBX asset path, e.g. items/cust_board/own_deckgraphic_baker_popsicle_00001",
                     true,
                     true},
                    {"patch", ParamType::Boolean, "Pass --patch to the engine", false, false, {}, Json(false)},
                    game_root_param(),
                },
            .examples = {"ebx values items/cust_board/own_deckgraphic_baker_popsicle_00001 --json",
                         "ebx values common/textures/default/neutral/neutral_c"},
            .run = [](Context &c, const Json &a) -> Json
            {
                std::string asset = a["asset"];
                std::vector<std::string> args = {"ebx-values", path_utf8(checked_game_root(c, a)), asset};
                add_flag(args, a, "patch", "--patch");
                EngineRun run = run_checked(c, args, "engine_failed", "EBX asset '" + lowercase(asset) + "'");
                return parse_ebx_values(lowercase(asset), run.out_lines);
            },
        });

        r.add({
            .group = "ebx",
            .name = "roundtrip",
            .summary = "Self-test: decode and re-encode the first N EBX assets",
            .description =
                std::string(
                    "Validation pass. Decodes and re-encodes the first --count EBX assets in index order and checks "
                    "that "
                    "each rebuilt graph is identical. Cannot be pointed at one asset. On failure the error is "
                    "roundtrip_failed and details name the asset and reason. Known: on game build 25414733 it fails "
                    "between 5000 and 10000 assets at animation/dingo/cdb_skatepedia_grindtricks with 'EBX "
                    "floating-point "
                    "field must be finite' (a stock asset, not your mod). Reads only, writes nothing.") +
                k_game_root_note,
            .params =
                {
                    {"count",
                     ParamType::Integer,
                     "How many EBX assets to round-trip, from the start of the index (at least 1)",
                     false,
                     true,
                     {},
                     Json(100)},
                    game_root_param(),
                },
            .examples = {"ebx roundtrip", "ebx roundtrip 2000 --json"},
            .long_running = true,
            .run = [](Context &c, const Json &a) -> Json
            {
                long long count = positive(a, "count");
                fs::path root = checked_game_root(c, a);
                c.progress(-1, "Round-tripping " + std::to_string(count) + " EBX assets");
                EngineRun run = run_engine(c, {"ebx-roundtrip", path_utf8(root), std::to_string(count)});
                if (run.exit_code != 0)
                {
                    try
                    {
                        require_success(run, "roundtrip_failed");
                    }
                    catch (Error &e)
                    {
                        std::string message = e.what();
                        if (message.find("not enough EBX assets") != std::string::npos)
                            throw Error("count_too_large",
                                        message + ". The game has fewer EBX assets than --count.",
                                        e.details);
                        Json details = e.details;
                        auto colon = message.find(": ");
                        if (colon != std::string::npos && message.find('/') < colon)
                        {
                            details["asset"] = message.substr(0, colon);
                            details["reason"] = message.substr(colon + 2);
                        }
                        throw Error("roundtrip_failed", message, details);
                    }
                }
                Json kv = parse_key_values(run.out_lines);
                return {{"rebuilt", kv.value("rebuilt-ebx", Json())}, {"identical", true}};
            },
        });

        r.add({
            .group = "ebx",
            .name = "author",
            .summary = "Self-test of native authoring: build a test world in memory",
            .description =
                std::string(
                    "Native authoring self-test. In memory only, builds a test world (texture, triangle mesh, physics, "
                    "blueprint, layer, subworld and MeshVariationDatabase under world/custom/native_test and "
                    "levels/game/native_test) from stock donors (see 'asset donors') and returns the size in bytes of "
                    "each authored piece and whether collision got wired. Writes nothing to disk or the game.") +
                k_game_root_note,
            .params = {game_root_param()},
            .examples = {"ebx author", "ebx author --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                EngineRun run = run_checked(c, {"ebx-author", path_utf8(checked_game_root(c, a))}, "authoring_failed");
                Json kv = parse_key_values(run.out_lines);
                Json bytes = Json::object();
                Json out = Json::object();
                for (auto &[key, value] : kv.items())
                {
                    if (key == "collision-wired")
                        out["collision_wired"] = value == 1 || value == true;
                    else if (key == "lines")
                        out["lines"] = value;
                    else
                    {
                        std::string k = key;
                        std::replace(k.begin(), k.end(), '-', '_');
                        bytes[k] = value;
                    }
                }
                Json result = {{"bytes", bytes}};
                for (auto &[key, value] : out.items())
                    result[key] = value;
                return result;
            },
        });

        r.add({
            .group = "ebx",
            .name = "schema",
            .summary = "Binary layout of one EBX asset: types, fields, offsets",
            .description =
                std::string(
                    "Returns the binary layout of one EBX asset: the data range (data_start, data_end as byte "
                    "offsets), "
                    "each instance's fixup and descriptor index, offset and exported flag, and every type with its "
                    "size, "
                    "alignment, base class and fields. Each field has name, offset, type (Frostbite EBX type name, "
                    "e.g. "
                    "CString, Pointer, Struct, Enum, Float32), type_code, array (true for array fields), category and, "
                    "for struct / enum / class-typed fields, the class it refers to. Enum types come back with kind "
                    "enum "
                    "and members (name, value). Ends with data_prefix, a hex dump of the first bytes. Use 'ebx values' "
                    "for "
                    "the actual field values. Large for complex assets. Reads only.") +
                k_game_root_note,
            .params =
                {
                    {"asset",
                     ParamType::String,
                     "EBX asset path, e.g. common/textures/default/neutral/neutral_c",
                     true,
                     true},
                    game_root_param(),
                },
            .examples = {"ebx schema common/textures/default/neutral/neutral_c",
                         "ebx schema items/cust_board/own_deckgraphic_baker_popsicle_00001 --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                std::string asset = a["asset"];
                EngineRun run = run_checked(c,
                                            {"schema", path_utf8(checked_game_root(c, a)), asset},
                                            "engine_failed",
                                            "EBX asset '" + lowercase(asset) + "'");
                return parse_schema(lowercase(asset), run.out_lines);
            },
        });

        r.add({
            .group = "game",
            .name = "cas",
            .summary = "Self-test of the game's Oodle CAS codec",
            .description =
                "Oodle codec self-test. Compresses and decompresses a synthetic 1 MiB buffer through the game's own "
                "Oodle DLL and returns both sizes. It does not read any .cas file; it only proves the engine can load "
                "Oodle from this ReSkate folder. Fails with not_a_game_root if the folder is not a valid install. "
                "Takes under a second. Reads only.",
            .params = {game_root_param()},
            .examples = {"game cas", "game cas --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                EngineRun run = run_checked(c, {"cas", path_utf8(checked_game_root(c, a))}, "cas_roundtrip_failed");
                Json kv = parse_key_values(run.out_lines);
                Json out = {{"ok", true},
                            {"decoded_bytes", kv.value("decoded-bytes", Json())},
                            {"encoded_bytes", kv.value("encoded-bytes", Json())}};
                if (kv.contains("lines"))
                    out["lines"] = kv["lines"];
                return out;
            },
        });

        r.add({
            .group = "game",
            .name = "toc-stock",
            .summary = "Read a superbundle .toc file: bundle count, chunk count, flags",
            .description =
                "Parses one superbundle .toc file and returns its bundle count, chunk count and flags. Takes a .toc "
                "file, not the game root. TOC files in an install: Data\\layout.toc, "
                "Data\\Win32\\{buildkititems,customization,globals,items,ui}.toc, levels\\game\\*\\*.toc, loc\\*.toc. "
                "Reads the file only; working on a copy is safest. Fails with invalid_toc if the file is not a TOC. "
                "Takes under a second.",
            .params =
                {
                    {"toc", ParamType::Path, "The .toc file to read", true, true},
                },
            .examples =
                {"game toc-stock \"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Skate\\Data\\Win32\\ui.toc\"",
                 "game toc-stock C:\\work\\copies\\items.toc --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                std::error_code ec;
                // Absolute: the engine runs in the engine folder, so a relative path (possible over MCP) would miss.
                fs::path path = fs::absolute(fs::path(utf8_to_wide(a["toc"].get<std::string>())), ec);
                if (ec)
                    path = utf8_to_wide(a["toc"].get<std::string>());
                std::string toc = path_utf8(path);
                if (!fs::exists(path, ec))
                    throw Error("path_not_found", "File does not exist: " + toc, {{"path", toc}});
                if (!fs::is_regular_file(path, ec))
                    throw Error(
                        "invalid_toc", "Not a file: " + toc + ". Pass a .toc file, not a folder.", {{"path", toc}});
                EngineRun run = run_checked(c, {"toc-stock", toc});
                Json kv = parse_key_values(run.out_lines);
                Json out = {{"toc", toc},
                            {"bundles", kv.value("bundles", Json())},
                            {"chunks", kv.value("chunks", Json())},
                            {"flags", kv.value("flags", Json())}};
                if (kv.contains("lines"))
                    out["lines"] = kv["lines"];
                return out;
            },
        });
    }
} // namespace studio
