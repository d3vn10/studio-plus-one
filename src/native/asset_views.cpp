#include "native/asset_views.h"

#include "core/settings.h"

#include <Engine/Resource/ebx_document.h>
#include <Engine/Resource/ebx_writer.h>

#pragma warning(push, 0)
#include <bcdec.h>
#include <miniz.h>
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;

namespace studio::native
{
    namespace ebx = fb::ebx;

    void write_file(const fs::path &path, const void *data, std::size_t size)
    {
        std::error_code ec;
        if (path.has_parent_path())
            fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            throw Error("output_failed", "Cannot write " + path_utf8(path), {{"path", path_utf8(path)}});
        out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
        if (!out)
            throw Error("output_failed", "Cannot write " + path_utf8(path), {{"path", path_utf8(path)}});
    }

    namespace
    {

        constexpr std::uint32_t lua_resource_type = 0xEC383B87;

        bool is_texture_resource(const Entry &e)
        {
            return e.kind == Kind::res && e.resource_type == fb::texture_resource_type;
        }

        const Entry *texture_resource_for(const AssetIndex &index, const Entry &e)
        {
            if (is_texture_resource(e))
                return &e;
            const Entry *res = index.find(Kind::res, index.name(e));
            return res && is_texture_resource(*res) ? res : nullptr;
        }

        const Entry *lua_resource_for(const AssetIndex &index, const Entry &e)
        {
            if (e.kind == Kind::res && e.resource_type == lua_resource_type)
                return &e;
            const Entry *res = index.find(Kind::res, index.name(e));
            return res && res->resource_type == lua_resource_type ? res : nullptr;
        }

        std::string guid_text(const std::array<std::byte, 16> &bytes)
        {
            fb::Guid g;
            g.bytes = bytes;
            return g.string();
        }

    } // namespace

    Json describe_entry(const AssetIndex &index, const Entry &e, const GameFiles &files)
    {
        Json out = {{"name", index.name(e)},
                    {"kind", kind_name(e.kind)},
                    {"type", index.type_name(e)},
                    {"category", category_name(index.category(e))},
                    {"size", e.size},
                    {"stored", e.stored}};
        if (e.bundle < index.bundles.size())
        {
            out["bundle"] = index.bundles[e.bundle].name;
            out["superbundle"] = index.bundles[e.bundle].superbundle;
        }
        out["bundle_count"] = e.bundle_count;
        if (e.stored)
            out["location"] = files.describe(e.location());
        static const std::array<std::byte, 16> zero{};
        if (e.kind != Kind::res && e.guid != zero)
            out["guid"] = guid_text(e.guid);
        if (e.kind == Kind::res)
        {
            out["resource_type"] = hex32(e.resource_type);
            out["resource_id"] = hex64(e.resource_id);
            out["resource_meta"] = hex_bytes(e.meta.data(), e.meta.size());
        }
        if (e.kind == Kind::chunk && e.logical_offset)
            out["logical_offset"] = e.logical_offset;
        if (e.flags & entry_type_failed)
            out["type_error"] = "The root type could not be read from this EBX";
        Json related = Json::array();
        for (Kind k : {Kind::ebx, Kind::res})
        {
            if (k == e.kind || e.kind == Kind::chunk)
                continue;
            if (const Entry *other = index.find(k, index.name(e)))
                related.push_back({{"kind", kind_name(k)}, {"type", index.type_name(*other)}, {"size", other->size}});
        }
        out["related"] = related;
        Json views = Json::array();
        if (e.kind == Kind::ebx)
            views.push_back("properties");
        if (texture_resource_for(index, e))
            views.push_back("texture");
        if (lua_resource_for(index, e))
            views.push_back("lua");
        views.push_back("raw");
        out["views"] = views;
        return out;
    }

    // ---------------------------------------------------------------- EBX

    namespace
    {

        class EbxJson
        {
          public:
            EbxJson(const AssetIndex &index, const ebx::Document &doc, const EbxOptions &options)
                : index_(index), doc_(doc), options_(options)
            {
            }

            Json instance(std::size_t i) const
            {
                const auto &inst = doc_.instances[i];
                Json out = {{"index", i}, {"type", type_name(inst.descriptor)}};
                if (inst.exported)
                    out["guid"] = inst.instanceGuid.string();
                out["exported"] = inst.exported;
                out["fields"] = inst.object ? fields(*inst.object) : Json::object();
                return out;
            }

            Json imports() const
            {
                Json out = Json::array();
                for (std::size_t i = 0; i < doc_.imports.size(); ++i)
                {
                    const auto &imp = doc_.imports[i];
                    Json item = {
                        {"index", i}, {"file_guid", imp.fileGuid.string()}, {"class_guid", imp.classGuid.string()}};
                    if (const Entry *target = index_.find_ebx_guid(imp.fileGuid.bytes))
                    {
                        item["asset"] = index_.name(*target);
                        item["type"] = index_.type_name(*target);
                    }
                    out.push_back(std::move(item));
                }
                return out;
            }

            std::string type_name(std::int32_t descriptor) const
            {
                if (descriptor < 0 || static_cast<std::size_t>(descriptor) >= doc_.types.size())
                    return "";
                return doc_.types[static_cast<std::size_t>(descriptor)].name;
            }

          private:
            Json fields(const ebx::Object &object) const
            {
                Json out = Json::object();
                for (const auto &f : object.fields)
                {
                    const auto &desc = doc_.fields[f.descriptor];
                    out[f.name] = value(f.value, desc);
                }
                return out;
            }

            // An enum member's name, from the enum type's own field list (each member's value is its offset).
            Json enum_value(std::int64_t v, std::uint16_t class_ref) const
            {
                if (class_ref < doc_.types.size())
                {
                    const auto &type = doc_.types[class_ref];
                    for (std::size_t k = 0; k < type.fieldCount; ++k)
                    {
                        const auto at = static_cast<std::size_t>(type.fieldIndex) + k;
                        if (at < doc_.fields.size() && static_cast<std::int64_t>(doc_.fields[at].dataOffset) == v)
                            return doc_.fields[at].name;
                    }
                }
                return v;
            }

            Json value(const ebx::Value &v, const ebx::FieldDescriptor &desc) const
            {
                return std::visit(
                    [&](const auto &x) -> Json
                    {
                        using T = std::decay_t<decltype(x)>;
                        if constexpr (std::is_same_v<T, std::monostate>)
                            return nullptr;
                        else if constexpr (std::is_same_v<T, bool>)
                            return x;
                        else if constexpr (std::is_same_v<T, std::int64_t>)
                        {
                            if (desc.type() == ebx::FieldType::enumeration)
                                return enum_value(x, desc.classRef);
                            return x;
                        }
                        else if constexpr (std::is_same_v<T, std::uint64_t>)
                            return x;
                        else if constexpr (std::is_same_v<T, double>)
                        {
                            if (!std::isfinite(x))
                                return std::isnan(x) ? "nan" : (x > 0 ? "inf" : "-inf");
                            // Floats read as float32: print them as such, not as 0.10000000149011612.
                            if (desc.type() == ebx::FieldType::float32)
                                return static_cast<double>(std::stod(float_text(x)));
                            return x;
                        }
                        else if constexpr (std::is_same_v<T, std::string>)
                            return x;
                        else if constexpr (std::is_same_v<T, fb::Guid>)
                            return x.string();
                        else if constexpr (std::is_same_v<T, fb::Sha1>)
                            return hex_bytes(x.bytes.data(), x.bytes.size());
                        else if constexpr (std::is_same_v<T, ebx::ResourceReference>)
                        {
                            if (!x.id)
                                return nullptr;
                            Json out = {{"$resource", hex64(x.id)}};
                            if (const Entry *res = index_.find_resource_id(x.id))
                                out["name"] = index_.name(*res);
                            return out;
                        }
                        else if constexpr (std::is_same_v<T, ebx::PointerReference>)
                        {
                            if (x.kind == ebx::PointerKind::null)
                                return nullptr;
                            if (x.kind == ebx::PointerKind::internal)
                            {
                                const auto i = static_cast<std::size_t>(x.index);
                                return {
                                    {"$ref", x.index},
                                    {"type", i < doc_.instances.size() ? type_name(doc_.instances[i].descriptor) : ""}};
                            }
                            Json out = {{"$import", x.index}};
                            const auto i = static_cast<std::size_t>(x.index);
                            if (i < doc_.imports.size())
                            {
                                if (const Entry *target = index_.find_ebx_guid(doc_.imports[i].fileGuid.bytes))
                                    out["asset"] = index_.name(*target);
                                else
                                    out["file_guid"] = doc_.imports[i].fileGuid.string();
                            }
                            return out;
                        }
                        else if constexpr (std::is_same_v<T, ebx::TypeReference>)
                        {
                            if (!x.encoded)
                                return nullptr;
                            if (x.primitive)
                                return {{"$type", "primitive " + std::to_string(static_cast<int>(x.primitiveType))}};
                            return {{"$type", type_name(x.descriptor)}};
                        }
                        else if constexpr (std::is_same_v<T, ebx::BoxedReference>)
                        {
                            return {{"$boxed", x.encodedType}};
                        }
                        else if constexpr (std::is_same_v<T, std::shared_ptr<ebx::Object>>)
                        {
                            if (!x)
                                return nullptr;
                            Json out = {{"$type", type_name(x->descriptor)}};
                            Json object_fields = fields(*x);
                            for (auto &[k, val] : object_fields.items())
                                out[k] = val;
                            return out;
                        }
                        else
                        {
                            Json out = Json::array();
                            const auto shown = std::min<std::size_t>(
                                x.size(), static_cast<std::size_t>(std::max(0ll, options_.max_array)));
                            for (std::size_t i = 0; i < shown; ++i)
                                out.push_back(value(x[i], desc));
                            if (shown < x.size())
                                out.push_back({{"$more", x.size() - shown}});
                            return out;
                        }
                    },
                    v.data);
            }

            static std::string float_text(double v)
            {
                char text[32];
                std::snprintf(text, sizeof(text), "%.9g", v);
                // The shortest text that reads back as the same float.
                for (int digits = 6; digits <= 9; ++digits)
                {
                    char shorter[32];
                    std::snprintf(shorter, sizeof(shorter), "%.*g", digits, v);
                    if (static_cast<float>(std::strtod(shorter, nullptr)) == static_cast<float>(v))
                        return shorter;
                }
                return text;
            }

            const AssetIndex &index_;
            const ebx::Document &doc_;
            const EbxOptions &options_;
        };

    } // namespace

    Json ebx_json(const AssetIndex &index, const Entry &e, const GameFiles &files, const EbxOptions &options)
    {
        const auto bytes = files.read(e.location());
        ebx::Document doc;
        try
        {
            doc = ebx::read_document(bytes);
        }
        catch (const std::exception &error)
        {
            throw Error("ebx_unreadable",
                        "Cannot read the EBX of " + std::string(index.name(e)) + ": " + error.what(),
                        {{"name", index.name(e)}});
        }
        const EbxJson json(index, doc, options);
        Json out = {{"name", index.name(e)},
                    {"guid", doc.fileGuid.string()},
                    {"root_type", doc.rootType},
                    {"bytes", bytes.size()},
                    {"instance_count", doc.instances.size()}};
        Json instances = Json::array();
        if (options.instance)
        {
            const auto i = *options.instance;
            if (i < 0 || static_cast<std::size_t>(i) >= doc.instances.size())
                throw Error("invalid_arguments",
                            "--instance must be below " + std::to_string(doc.instances.size()),
                            {{"param", "instance"}});
            instances.push_back(json.instance(static_cast<std::size_t>(i)));
        }
        else
        {
            const auto shown =
                std::min<std::size_t>(doc.instances.size(), static_cast<std::size_t>(std::max(1ll, options.limit)));
            for (std::size_t i = 0; i < shown; ++i)
                instances.push_back(json.instance(i));
            if (shown < doc.instances.size())
                out["instances_truncated"] = doc.instances.size() - shown;
        }
        out["imports"] = json.imports();
        out["instances"] = std::move(instances);
        return out;
    }

    Json ebx_imports(const AssetIndex &index, const Entry &e, const GameFiles &files)
    {
        const auto bytes = files.read(e.location());
        ebx::Document doc;
        try
        {
            doc = ebx::read_document(bytes);
        }
        catch (const std::exception &error)
        {
            throw Error("ebx_unreadable",
                        "Cannot read the EBX of " + std::string(index.name(e)) + ": " + error.what(),
                        {{"name", index.name(e)}});
        }
        return EbxJson(index, doc, {}).imports();
    }

    // ---------------------------------------------------------------- EBX edits

    namespace
    {

        struct Target
        {
            ebx::Value *value = nullptr;
            const ebx::FieldDescriptor *field = nullptr;
        };

        std::string lower_text(std::string text)
        {
            for (auto &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        std::size_t index_number(const std::string &text, const std::string &path)
        {
            std::size_t used = 0;
            std::size_t value = 0;
            try
            {
                value = static_cast<std::size_t>(std::stoul(text, &used));
            }
            catch (const std::exception &)
            {
                used = 0;
            }
            if (text.empty() || used != text.size())
                throw Error(
                    "invalid_path", "Cannot use '" + path + "': [" + text + "] is not an index", {{"path", path}});
            return value;
        }

        // "[2].Transform.trans.x" is instance 2, then Transform, trans, x. Without a leading [n] it is instance 0.
        Target find_target(ebx::Document &doc, const std::string &path)
        {
            auto bad = [&](const std::string &why)
            { return Error("invalid_path", "Cannot use '" + path + "': " + why, {{"path", path}}); };
            std::size_t at = 0, instance = 0;
            if (path.starts_with("["))
            {
                const auto close = path.find(']');
                if (close == std::string::npos)
                    throw bad("no ] after the instance number");
                instance = index_number(path.substr(1, close - 1), path);
                at = close + 1;
            }
            if (instance >= doc.instances.size() || !doc.instances[instance].object)
                throw bad("the asset has " + std::to_string(doc.instances.size()) + " instances");
            ebx::Object *object = doc.instances[instance].object.get();
            Target t;
            while (at < path.size())
            {
                if (path[at] == '[')
                {
                    const auto close = path.find(']', at);
                    if (close == std::string::npos || !t.value)
                        throw bad("an index must follow a field");
                    const auto i = index_number(path.substr(at + 1, close - at - 1), path);
                    auto *array = std::get_if<ebx::Value::Array>(&t.value->data);
                    if (!array)
                        throw bad("the field before [" + std::to_string(i) + "] is not an array");
                    if (i >= array->size())
                        throw bad("the array has " + std::to_string(array->size()) + " items");
                    t.value = &(*array)[i];
                    at = close + 1;
                }
                else
                {
                    if (path[at] == '.')
                        ++at;
                    const auto end = path.find_first_of(".[", at);
                    const std::string name = path.substr(at, end == std::string::npos ? std::string::npos : end - at);
                    if (!object)
                        throw bad("'" + name + "' is inside something that is not a struct");
                    ebx::FieldValue *found = nullptr;
                    for (auto &f : object->fields)
                        if (lower_text(f.name) == lower_text(name))
                        {
                            found = &f;
                            break;
                        }
                    if (!found)
                        throw bad("no field named '" + name + "'");
                    t.value = &found->value;
                    t.field = &doc.fields[found->descriptor];
                    at = end == std::string::npos ? path.size() : end;
                }
                object = nullptr;
                if (auto *inner = std::get_if<std::shared_ptr<ebx::Object>>(&t.value->data))
                    object = inner->get();
            }
            if (!t.value)
                throw bad("no field given");
            return t;
        }

        Json scalar_json(const ebx::Value &v)
        {
            return std::visit(
                [](const auto &x) -> Json
                {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> ||
                                  std::is_same_v<T, std::uint64_t> || std::is_same_v<T, double> ||
                                  std::is_same_v<T, std::string>)
                        return x;
                    else
                        return nullptr;
                },
                v.data);
        }

        void assign(const ebx::Document &doc, Target &t, const std::string &text, const std::string &path)
        {
            auto bad = [&](const std::string &why)
            {
                return Error("invalid_value",
                             "Cannot set " + path + " to '" + text + "': " + why,
                             {{"path", path}, {"value", text}});
            };
            using FT = ebx::FieldType;
            const auto type = t.field ? t.field->type() : FT::inherited;
            auto &data = t.value->data;
            try
            {
                if (std::holds_alternative<bool>(data))
                {
                    const auto low = lower_text(text);
                    if (low != "true" && low != "false" && low != "1" && low != "0")
                        throw bad("use true or false");
                    data = low == "true" || low == "1";
                }
                else if (std::holds_alternative<std::int64_t>(data))
                {
                    if (type == FT::enumeration && t.field && t.field->classRef < doc.types.size() && !text.empty() &&
                        !std::isdigit(static_cast<unsigned char>(text[0])) && text[0] != '-')
                    {
                        const auto &et = doc.types[t.field->classRef];
                        for (std::size_t k = 0; k < et.fieldCount; ++k)
                        {
                            const auto &member = doc.fields[static_cast<std::size_t>(et.fieldIndex) + k];
                            if (lower_text(member.name) == lower_text(text))
                            {
                                data = static_cast<std::int64_t>(member.dataOffset);
                                return;
                            }
                        }
                        throw bad("not a member of " + et.name);
                    }
                    std::size_t used = 0;
                    const long long v = std::stoll(text, &used);
                    if (used != text.size())
                        throw bad("not a whole number");
                    const long long lo = type == FT::int8    ? INT8_MIN
                                         : type == FT::int16 ? INT16_MIN
                                         : type == FT::int64 ? INT64_MIN
                                                             : INT32_MIN;
                    const long long hi = type == FT::int8    ? INT8_MAX
                                         : type == FT::int16 ? INT16_MAX
                                         : type == FT::int64 ? INT64_MAX
                                                             : INT32_MAX;
                    if (v < lo || v > hi)
                        throw bad("out of range for this field");
                    data = static_cast<std::int64_t>(v);
                }
                else if (std::holds_alternative<std::uint64_t>(data))
                {
                    if (text.starts_with("-"))
                        throw bad("this field cannot be negative");
                    std::size_t used = 0;
                    const unsigned long long v = std::stoull(text, &used, 0);
                    if (used != text.size())
                        throw bad("not a whole number");
                    const unsigned long long hi = type == FT::uint8    ? UINT8_MAX
                                                  : type == FT::uint16 ? UINT16_MAX
                                                  : type == FT::uint32 ? UINT32_MAX
                                                                       : UINT64_MAX;
                    if (v > hi)
                        throw bad("out of range for this field");
                    data = static_cast<std::uint64_t>(v);
                }
                else if (std::holds_alternative<double>(data))
                {
                    std::size_t used = 0;
                    const double v = std::stod(text, &used);
                    if (used != text.size())
                        throw bad("not a number");
                    data = v;
                }
                else if (std::holds_alternative<std::string>(data))
                {
                    if (type == FT::string && text.size() >= 32)
                        throw bad("this field holds at most 31 characters");
                    data = text;
                }
                else
                {
                    throw bad("only booleans, numbers, enums and strings can be set so far");
                }
            }
            catch (const std::invalid_argument &)
            {
                throw bad("not a number");
            }
            catch (const std::out_of_range &)
            {
                throw bad("out of range");
            }
        }

    } // namespace

    Json ebx_set(const AssetIndex &index,
                 const Entry &e,
                 const GameFiles &files,
                 const std::vector<EbxEdit> &edits,
                 const fs::path &output,
                 std::vector<std::byte> *written_out)
    {
        const auto bytes = files.read(e.location());
        ebx::Document doc;
        try
        {
            doc = ebx::read_document(bytes);
        }
        catch (const std::exception &error)
        {
            throw Error("ebx_unreadable", "Cannot read the EBX of " + std::string(index.name(e)) + ": " + error.what());
        }
        // Written back untouched first: whether the writer reproduces the game's bytes says how far to trust an edit.
        const auto same = ebx::write_document(doc);
        const bool identical = same.size() == bytes.size() && std::equal(same.begin(), same.end(), bytes.begin());
        Json changes = Json::array();
        for (const auto &edit : edits)
        {
            auto target = find_target(doc, edit.path);
            const Json before = scalar_json(*target.value);
            assign(doc, target, edit.value, edit.path);
            changes.push_back({{"path", edit.path}, {"old", before}, {"new", scalar_json(*target.value)}});
        }
        const auto written = ebx::write_document(doc);
        // Read the result back: every edit must be there before anything is saved.
        auto check = ebx::read_document(written);
        for (std::size_t i = 0; i < edits.size(); ++i)
        {
            const Json now = scalar_json(*find_target(check, edits[i].path).value);
            const Json &want = changes[i]["new"];
            bool ok = now == want;
            if (!ok && now.is_number() && want.is_number())
                ok = static_cast<float>(now.get<double>()) == static_cast<float>(want.get<double>());
            if (!ok)
                throw Error("ebx_write_failed",
                            "The change to " + edits[i].path + " did not survive writing; nothing was saved.",
                            {{"path", edits[i].path}});
            changes[i]["new"] = now;
        }
        Json out = {{"name", index.name(e)},
                    {"output", path_utf8(output)},
                    {"bytes", written.size()},
                    {"original_bytes", bytes.size()},
                    {"roundtrip_identical", identical},
                    {"changes", changes}};
        if (written_out)
            *written_out = written;
        else
            write_file(output, written.data(), written.size());
        return out;
    }

    // ---------------------------------------------------------------- textures

    namespace
    {

        enum class Codec
        {
            bc1,
            bc2,
            bc3,
            bc4,
            bc5,
            bc6u,
            bc6s,
            bc7,
            rgba8,
            bgra8,
            unknown
        };

        struct Format
        {
            Codec codec = Codec::unknown;
            std::uint32_t unit = 0; // bytes per block (block formats) or per pixel
            bool block = false;
            std::uint32_t dxgi = 0; // DXGI_FORMAT, 0 when not known
            const char *name = "unknown";
        };

        // Frostbite format ordinals (ReSkate Studio's texture_format table) for the colour and BC formats.
        Format format_of(std::uint32_t ordinal)
        {
            switch (ordinal)
            {
            case 18:
                return {Codec::rgba8, 4, false, 28, "R8G8B8A8_UNORM"};
            case 20:
                return {Codec::rgba8, 4, false, 29, "R8G8B8A8_SRGB"};
            case 23:
                return {Codec::bgra8, 4, false, 87, "B8G8R8A8_UNORM"};
            case 24:
                return {Codec::bgra8, 4, false, 91, "B8G8R8A8_SRGB"};
            case 54:
                return {Codec::bc1, 8, true, 71, "BC1_UNORM"};
            case 55:
                return {Codec::bc1, 8, true, 72, "BC1_SRGB"};
            case 56:
                return {Codec::bc1, 8, true, 71, "BC1A_UNORM"};
            case 57:
                return {Codec::bc1, 8, true, 72, "BC1A_SRGB"};
            case 58:
                return {Codec::bc2, 16, true, 74, "BC2_UNORM"};
            case 59:
                return {Codec::bc2, 16, true, 75, "BC2_SRGB"};
            case 60:
                return {Codec::bc3, 16, true, 77, "BC3_UNORM"};
            case 61:
                return {Codec::bc3, 16, true, 78, "BC3_SRGB"};
            case 62:
                return {Codec::bc4, 8, true, 80, "BC4_UNORM"};
            case 63:
                return {Codec::bc5, 16, true, 83, "BC5_UNORM"};
            case 64:
                return {Codec::bc6u, 16, true, 95, "BC6U_FLOAT"};
            case 65:
                return {Codec::bc6s, 16, true, 96, "BC6S_FLOAT"};
            case 66:
                return {Codec::bc7, 16, true, 98, "BC7_UNORM"};
            case 67:
                return {Codec::bc7, 16, true, 99, "BC7_SRGB"};
            default:
                return {};
            }
        }

        std::uint32_t mip_dim(std::uint32_t size, std::uint32_t mip)
        {
            return std::max(1u, size >> mip);
        }

        std::size_t level_bytes(const Format &f, std::uint32_t width, std::uint32_t height)
        {
            if (!f.block)
                return std::size_t{width} * height * f.unit;
            return std::size_t{std::max(1u, (width + 3) / 4)} * std::max(1u, (height + 3) / 4) * f.unit;
        }

        const char *type_name(std::uint32_t type)
        {
            switch (type)
            {
            case 0:
                return "2D";
            case 1:
                return "Cube";
            case 2:
                return "3D";
            case 3:
                return "2DArray";
            default:
                return "other";
            }
        }

        // Where one mip of one slice starts in the chunk, and its size. The chunk holds the mips largest
        // first, each with all its slices.
        std::pair<std::size_t, std::size_t> mip_span(const Texture &t, std::uint32_t mip, std::uint32_t slice)
        {
            const auto f = format_of(t.header.format);
            const auto slices = texture_slices(t);
            std::size_t offset = 0;
            for (std::uint32_t m = 0; m < mip; ++m)
                offset += level_bytes(f, mip_dim(t.header.width, m), mip_dim(t.header.height, m)) * slices;
            const auto size = level_bytes(f, mip_dim(t.header.width, mip), mip_dim(t.header.height, mip));
            // Relative to the piece of the chunk that is in the game; a mip before it reads as out of range.
            offset += size * slice;
            return {offset >= t.pixels_start ? offset - t.pixels_start : SIZE_MAX, size};
        }

    } // namespace

    bool mip_present(const Texture &t, std::uint32_t mip)
    {
        if (mip >= t.header.mip_count)
            return false;
        const auto [offset, size] = mip_span(t, mip, 0);
        return offset <= t.pixels.size() && size <= t.pixels.size() - offset;
    }

    std::size_t texture_slices(const Texture &t)
    {
        if (t.header.type == 1)
            return std::max<std::size_t>(6, t.header.slices);
        if (t.header.type == 3)
            return std::max<std::size_t>(1, t.header.slices);
        return 1;
    }

    Texture read_texture(const AssetIndex &index, const Entry &e, const GameFiles &files)
    {
        Texture t;
        t.resource = texture_resource_for(index, e);
        if (!t.resource)
            throw Error("not_a_texture",
                        std::string(index.name(e)) + " is not a texture (no Texture resource of that name)",
                        {{"name", index.name(e)}});
        try
        {
            t.header = fb::read_texture_header(files.read(t.resource->location()), t.resource->meta);
        }
        catch (const std::exception &error)
        {
            throw Error("texture_unreadable",
                        "Cannot read the texture header of " + std::string(index.name(e)) + ": " + error.what());
        }
        const Entry *chunk = index.find_chunk(t.header.chunk);
        if (!chunk)
            throw Error("texture_chunk_missing",
                        "The pixel chunk " + t.header.chunk.string() + " of " + std::string(index.name(e)) +
                            " is not in the index");
        t.pixels = files.read(chunk->location());
        // A piece of a streamed chunk: its logical offset says where it starts (the low 16 bits are part of its size).
        t.pixels_start = chunk->logical_offset & 0xFFFF0000u;
        return t;
    }

    Json texture_info(const Texture &t)
    {
        const auto f = format_of(t.header.format);
        Json out = {{"width", t.header.width},
                    {"height", t.header.height},
                    {"depth", t.header.depth},
                    {"slices", texture_slices(t)},
                    {"mips", t.header.mip_count},
                    {"first_mip", t.header.first_mip},
                    {"texture_type", type_name(t.header.type)},
                    {"format", f.name},
                    {"format_ordinal", t.header.format},
                    {"chunk", t.header.chunk.string()},
                    {"chunk_bytes", t.pixels.size()},
                    {"chunk_start", t.pixels_start},
                    {"decodable", f.codec != Codec::unknown && t.header.type != 2}};
        return out;
    }

    fb::Image decode_mip(const Texture &t, std::uint32_t mip, std::uint32_t slice)
    {
        const auto f = format_of(t.header.format);
        if (f.codec == Codec::unknown || t.header.type == 2)
            throw Error("texture_format_unsupported",
                        "Studio+ cannot decode texture format " + std::to_string(t.header.format) + " (" +
                            type_name(t.header.type) + "). Export it as DDS or raw instead.",
                        {{"format", t.header.format}});
        if (mip >= t.header.mip_count)
            throw Error(
                "invalid_arguments", "--mip must be below " + std::to_string(t.header.mip_count), {{"param", "mip"}});
        if (slice >= texture_slices(t))
            throw Error("invalid_arguments",
                        "--slice must be below " + std::to_string(texture_slices(t)),
                        {{"param", "slice"}});
        const auto width = mip_dim(t.header.width, mip), height = mip_dim(t.header.height, mip);
        const auto [offset, size] = mip_span(t, mip, slice);
        if (offset > t.pixels.size() || size > t.pixels.size() - offset)
            throw Error("texture_mip_missing",
                        "Mip " + std::to_string(mip) + " of this texture is not in the game's data (only " +
                            std::to_string(t.pixels.size()) + " bytes of its pixel chunk are). Try a smaller mip.",
                        {{"mip", mip}});
        const auto *src = reinterpret_cast<const std::uint8_t *>(t.pixels.data() + offset);
        fb::Image image{width, height, std::vector<std::uint8_t>(std::size_t{width} * height * 4)};
        if (!f.block)
        {
            std::copy_n(src, image.rgba.size(), image.rgba.begin());
            if (f.codec == Codec::bgra8)
                for (std::size_t i = 0; i < image.rgba.size(); i += 4)
                    std::swap(image.rgba[i], image.rgba[i + 2]);
            return image;
        }
        const auto bw = (width + 3) / 4, bh = (height + 3) / 4;
        const auto pw = bw * 4;
        std::vector<std::uint8_t> padded(std::size_t{pw} * bh * 4 * 4);
        const int pitch = static_cast<int>(pw * 4);
        std::uint8_t one[16 * 2];
        float hdr[16 * 3];
        for (std::uint32_t by = 0; by < bh; ++by)
            for (std::uint32_t bx = 0; bx < bw; ++bx)
            {
                auto *out = padded.data() + (std::size_t{by} * 4 * pw + std::size_t{bx} * 4) * 4;
                switch (f.codec)
                {
                case Codec::bc1:
                    bcdec_bc1(src, out, pitch);
                    break;
                case Codec::bc2:
                    bcdec_bc2(src, out, pitch);
                    break;
                case Codec::bc3:
                    bcdec_bc3(src, out, pitch);
                    break;
                case Codec::bc7:
                    bcdec_bc7(src, out, pitch);
                    break;
                case Codec::bc4:
                    bcdec_bc4(src, one, 4);
                    for (int p = 0; p < 16; ++p)
                    {
                        auto *px = out + (p / 4) * pitch + (p % 4) * 4;
                        px[0] = px[1] = px[2] = one[p];
                        px[3] = 255;
                    }
                    break;
                case Codec::bc5:
                    bcdec_bc5(src, one, 8);
                    for (int p = 0; p < 16; ++p)
                    {
                        auto *px = out + (p / 4) * pitch + (p % 4) * 4;
                        // A two-channel normal map: rebuild z so it looks like one.
                        const float x = one[p * 2] / 127.5f - 1, y = one[p * 2 + 1] / 127.5f - 1;
                        const float z = std::sqrt(std::max(0.0f, 1 - x * x - y * y));
                        px[0] = one[p * 2];
                        px[1] = one[p * 2 + 1];
                        px[2] = static_cast<std::uint8_t>(std::clamp((z * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
                        px[3] = 255;
                    }
                    break;
                default:
                    bcdec_bc6h_float(src, hdr, 12, f.codec == Codec::bc6s ? 1 : 0);
                    for (int p = 0; p < 16; ++p)
                    {
                        auto *px = out + (p / 4) * pitch + (p % 4) * 4;
                        for (int ch = 0; ch < 3; ++ch)
                        {
                            // Reinhard, then gamma, so bright HDR values still show.
                            const float v = std::max(0.0f, hdr[p * 3 + ch]);
                            px[ch] = static_cast<std::uint8_t>(
                                std::clamp(std::pow(v / (1 + v), 1 / 2.2f) * 255.0f, 0.0f, 255.0f));
                        }
                        px[3] = 255;
                    }
                    break;
                }
                src += f.unit;
            }
        for (std::uint32_t y = 0; y < height; ++y)
            std::copy_n(padded.data() + std::size_t{y} * pw * 4,
                        std::size_t{width} * 4,
                        image.rgba.data() + std::size_t{y} * width * 4);
        return image;
    }

    std::vector<unsigned char> encode_png(const fb::Image &image)
    {
        size_t length = 0;
        void *png = tdefl_write_image_to_png_file_in_memory_ex(
            image.rgba.data(), static_cast<int>(image.width), static_cast<int>(image.height), 4, &length, 6, MZ_FALSE);
        if (!png)
            throw Error("png_failed", "Could not encode the PNG");
        std::vector<unsigned char> out(static_cast<unsigned char *>(png), static_cast<unsigned char *>(png) + length);
        mz_free(png);
        return out;
    }

    std::vector<unsigned char> encode_dds(const Texture &t)
    {
        const auto f = format_of(t.header.format);
        if (!f.dxgi || t.header.type == 2)
            throw Error("texture_format_unsupported",
                        "Studio+ does not know the DDS format for texture format " + std::to_string(t.header.format) +
                            ". Export it raw instead.",
                        {{"format", t.header.format}});
        const auto slices = texture_slices(t);
        const bool cube = t.header.type == 1;
        // The first mip whose every slice is in the data: a streamed texture may have only its small mips.
        std::uint32_t base = 0;
        auto present = [&](std::uint32_t m)
        {
            for (std::uint32_t s = 0; s < slices; ++s)
            {
                const auto [offset, size] = mip_span(t, m, s);
                if (offset > t.pixels.size() || size > t.pixels.size() - offset)
                    return false;
            }
            return true;
        };
        while (base < t.header.mip_count && !present(base))
            ++base;
        if (base == t.header.mip_count)
            throw Error("texture_mip_missing", "None of this texture's mips are in the game's data");
        const auto width = mip_dim(t.header.width, base), height = mip_dim(t.header.height, base);
        const auto mips = static_cast<std::uint32_t>(t.header.mip_count - base);
        std::vector<unsigned char> out;
        auto u32 = [&](std::uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                out.push_back(static_cast<unsigned char>(v >> (8 * i)));
        };
        u32(0x20534444); // "DDS "
        u32(124);
        u32(0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000); // caps, height, width, pixel format, mip count, linear size
        u32(height);
        u32(width);
        u32(static_cast<std::uint32_t>(level_bytes(f, width, height)));
        u32(0);
        u32(mips);
        for (int i = 0; i < 11; ++i)
            u32(0);
        u32(32);
        u32(0x4);
        u32(0x30315844); // pixel format: FOURCC "DX10"
        for (int i = 0; i < 5; ++i)
            u32(0);
        u32(0x1000 | (mips > 1 ? 0x400008u : 0u) | (cube ? 0x8u : 0u));
        u32(cube ? 0xFE00u : 0u);
        u32(0);
        u32(0);
        u32(0);
        u32(f.dxgi);
        u32(3); // TEXTURE2D
        u32(cube ? 0x4u : 0u);
        u32(static_cast<std::uint32_t>(cube ? slices / 6 : slices));
        u32(0);
        // DDS keeps each slice's mips together; the chunk keeps each mip's slices together.
        for (std::uint32_t s = 0; s < slices; ++s)
            for (std::uint32_t m = base; m < t.header.mip_count; ++m)
            {
                const auto [offset, size] = mip_span(t, m, s);
                const auto *p = reinterpret_cast<const unsigned char *>(t.pixels.data() + offset);
                out.insert(out.end(), p, p + size);
            }
        return out;
    }

    // ---------------------------------------------------------------- Lua

    LuaSource read_lua(const AssetIndex &index, const Entry &e, const GameFiles &files)
    {
        const Entry *res = lua_resource_for(index, e);
        if (!res)
            throw Error("not_a_lua_asset",
                        std::string(index.name(e)) + " has no LuaScript resource",
                        {{"name", index.name(e)}});
        const auto bytes = files.read(res->location());
        // The payload: u64 offset of the file name, u64 offset of the source, u64 offset of the bound
        // names, u32 bound name count, u32 source length; then the text, NUL-terminated.
        const auto *data = reinterpret_cast<const char *>(bytes.data());
        auto u64_at = [&](std::size_t at)
        {
            std::uint64_t v = 0;
            std::memcpy(&v, data + at, 8);
            return v;
        };
        auto u32_at = [&](std::size_t at)
        {
            std::uint32_t v = 0;
            std::memcpy(&v, data + at, 4);
            return v;
        };
        if (bytes.size() < 0x20)
            throw Error("lua_unreadable", "The LuaScript resource of " + std::string(index.name(e)) + " is too short");
        const auto name_at = u64_at(0), source_at = u64_at(8);
        auto length = std::uint64_t{u32_at(0x1C)};
        if (source_at >= bytes.size())
            throw Error("lua_unreadable", "The LuaScript resource of " + std::string(index.name(e)) + " has no source");
        if (length > bytes.size() - source_at)
            length = bytes.size() - source_at;
        LuaSource out;
        out.name = std::string(index.name(*res));
        out.source.assign(data + source_at, static_cast<std::size_t>(length));
        if (const auto nul = out.source.find('\0'); nul != std::string::npos)
            out.source.resize(nul);
        if (name_at < bytes.size())
            out.file = std::string(data + name_at, strnlen(data + name_at, bytes.size() - static_cast<size_t>(name_at)));
        return out;
    }

} // namespace studio::native
