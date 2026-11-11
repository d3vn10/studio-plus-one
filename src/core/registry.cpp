#include "core/registry.h"
#include "core/settings.h"
#include <algorithm>
#include <cmath>

namespace studio
{

    std::string_view to_string(ParamType type)
    {
        switch (type)
        {
        case ParamType::String:
            return "string";
        case ParamType::Path:
            return "path";
        case ParamType::Integer:
            return "integer";
        case ParamType::Number:
            return "number";
        case ParamType::Boolean:
            return "boolean";
        case ParamType::Enum:
            return "enum";
        case ParamType::List:
            return "list";
        }
        return "string";
    }

    std::filesystem::path Context::game_root(const Json &args) const
    {
        std::filesystem::path root;
        if (auto it = args.find("game-root"); it != args.end() && it->is_string() && !it->get<std::string>().empty())
            root = game_folder_from(
                std::filesystem::path(utf8_to_wide(it->get<std::string>()))); // also takes ReSkateLauncher.exe
        else if (!settings.game_root.empty())
            root = settings.game_root;
        else
            throw Error("game_root_missing",
                        "No ReSkate folder is set. Pass --game-root <folder> or run: studio-plus studio set game-root "
                        "<folder>");
        std::error_code ec;
        if (!std::filesystem::exists(root / L"Skate.exe", ec))
            throw Error("not_a_game_root",
                        "No Skate.exe in " + path_utf8(root) +
                            ". Point Studio+ at your ReSkate folder (where ReSkateLauncher.exe and Skate.exe are).",
                        {{"path", path_utf8(root)}});
        return root;
    }

    std::string Command::tool_name() const
    {
        std::string out = group + "_" + name;
        std::ranges::replace(out, '-', '_');
        return out;
    }

    Json Command::input_schema() const
    {
        Json schema = {{"type", "object"}, {"properties", Json::object()}};
        Json required = Json::array();
        for (const auto &p : params)
        {
            Json prop;
            switch (p.type)
            {
            case ParamType::String:
            case ParamType::Path:
                prop["type"] = "string";
                break;
            case ParamType::Integer:
                prop["type"] = "integer";
                break;
            case ParamType::Number:
                prop["type"] = "number";
                break;
            case ParamType::Boolean:
                prop["type"] = "boolean";
                break;
            case ParamType::Enum:
                prop["type"] = "string";
                prop["enum"] = p.choices;
                break;
            case ParamType::List:
                prop["type"] = "array";
                prop["items"] = {{"type", "string"}};
                break;
            }
            std::string help = p.help;
            if (p.type == ParamType::Path)
                help += help.empty() ? "Windows path." : " (Windows path)";
            prop["description"] = help;
            if (p.default_value)
                prop["default"] = *p.default_value;
            if (p.minimum && (p.type == ParamType::Integer || p.type == ParamType::Number))
                prop["minimum"] = *p.minimum;
            schema["properties"][p.name] = prop;
            if (p.required)
                required.push_back(p.name);
        }
        if (!required.empty())
            schema["required"] = required;
        schema["additionalProperties"] = false;
        return schema;
    }

    Json Command::describe() const
    {
        Json params_json = Json::array();
        for (const auto &p : params)
        {
            Json j = {{"name", p.name},
                      {"type", to_string(p.type)},
                      {"required", p.required},
                      {"positional", p.positional},
                      {"help", p.help}};
            if (!p.choices.empty())
                j["choices"] = p.choices;
            if (p.default_value)
                j["default"] = *p.default_value;
            if (p.minimum)
                j["minimum"] = *p.minimum;
            params_json.push_back(j);
        }
        return {{"id", id()},
                {"group", group},
                {"name", name},
                {"tool", tool_name()},
                {"summary", summary},
                {"description", description},
                {"params", params_json},
                {"examples", examples},
                {"writes_game", writes_game},
                {"writes_files", writes_files},
                {"read_only", read_only()},
                {"long_running", long_running}};
    }

    Registry &Registry::instance()
    {
        static Registry registry;
        return registry;
    }

    void Registry::add(Command command)
    {
        if (find(command.group, command.name))
            throw std::logic_error("duplicate command: " + command.id());
        for (const auto &p : command.params)
            if (p.type == ParamType::Path &&
                (p.name == "output" || p.name == "staging-root" || p.name == "staging-dir"))
                command.writes_files = true;
        commands_.push_back(std::move(command));
    }

    const Command *Registry::find(std::string_view group, std::string_view name) const
    {
        for (const auto &c : commands_)
            if (c.group == group && c.name == name)
                return &c;
        return nullptr;
    }

    const Command *Registry::find_tool(std::string_view tool_name) const
    {
        for (const auto &c : commands_)
            if (c.tool_name() == tool_name)
                return &c;
        return nullptr;
    }

    std::vector<std::string> Registry::groups() const
    {
        std::vector<std::string> out;
        for (const auto &c : commands_)
            if (std::find(out.begin(), out.end(), c.group) == out.end())
                out.push_back(c.group);
        return out;
    }

    Json Registry::describe() const
    {
        Json out = Json::array();
        for (const auto &c : commands_)
            out.push_back(c.describe());
        return out;
    }

    Param game_root_param()
    {
        return {"game-root",
                ParamType::Path,
                "ReSkate folder (where ReSkateLauncher.exe and Skate.exe are). Defaults to the saved setting "
                "(studio-plus studio set game-root <folder>).",
                false,
                false,
                {},
                std::nullopt};
    }

    namespace
    {
        [[noreturn]] void bad(const std::string &param, const std::string &message)
        {
            throw Error("invalid_arguments", "--" + param + ": " + message, {{"param", param}});
        }
    } // namespace

    Json normalise_args(const Command &command, const Json &args)
    {
        if (!args.is_object())
            throw Error("invalid_arguments", "arguments must be a JSON object");
        Json out = Json::object();
        for (auto it = args.begin(); it != args.end(); ++it)
        {
            auto known = std::find_if(
                command.params.begin(), command.params.end(), [&](const Param &p) { return p.name == it.key(); });
            if (known == command.params.end())
                throw Error("invalid_arguments",
                            "unknown argument --" + it.key() + " for " + command.id(),
                            {{"param", it.key()}});
        }
        for (const auto &p : command.params)
        {
            auto it = args.find(p.name);
            if (it == args.end() || it->is_null())
            {
                if (p.default_value)
                    out[p.name] = *p.default_value;
                else if (p.required)
                    bad(p.name, "is required");
                continue;
            }
            Json v = *it;
            switch (p.type)
            {
            case ParamType::String:
            case ParamType::Path:
                if (!v.is_string())
                    bad(p.name, "must be a string");
                if (p.required && v.get<std::string>().empty())
                    bad(p.name, "must not be empty");
                // The engine runs in another working directory, so relative paths are resolved here,
                // for CLI and MCP callers alike.
                if (p.type == ParamType::Path && !v.get<std::string>().empty())
                {
                    std::error_code ec;
                    auto abs = std::filesystem::absolute(utf8_to_wide(v.get<std::string>()), ec);
                    if (!ec)
                        v = path_utf8(abs.lexically_normal());
                }
                break;
            case ParamType::Integer:
                if (v.is_string())
                {
                    try
                    {
                        size_t used = 0;
                        long long n = std::stoll(v.get<std::string>(), &used);
                        if (used != v.get<std::string>().size())
                            throw 0;
                        v = n;
                    }
                    catch (...)
                    {
                        bad(p.name, "must be a whole number");
                    }
                }
                else if (v.is_number_float())
                {
                    double d = v.get<double>();
                    if (std::floor(d) != d)
                        bad(p.name, "must be a whole number");
                    v = static_cast<long long>(d);
                }
                else if (!v.is_number_integer())
                    bad(p.name, "must be a whole number");
                break;
            case ParamType::Number:
                if (v.is_string())
                {
                    try
                    {
                        size_t used = 0;
                        double n = std::stod(v.get<std::string>(), &used);
                        if (used != v.get<std::string>().size())
                            throw 0;
                        v = n;
                    }
                    catch (...)
                    {
                        bad(p.name, "must be a number");
                    }
                }
                else if (!v.is_number())
                    bad(p.name, "must be a number");
                break;
            case ParamType::Boolean:
                if (v.is_string())
                {
                    auto s = v.get<std::string>();
                    if (s == "true" || s == "1" || s == "yes")
                        v = true;
                    else if (s == "false" || s == "0" || s == "no")
                        v = false;
                    else
                        bad(p.name, "must be true or false");
                }
                else if (!v.is_boolean())
                    bad(p.name, "must be true or false");
                break;
            case ParamType::Enum:
            {
                if (!v.is_string())
                    bad(p.name, "must be one of the listed choices");
                auto s = v.get<std::string>();
                if (std::find(p.choices.begin(), p.choices.end(), s) == p.choices.end())
                {
                    std::string list;
                    for (const auto &c : p.choices)
                        list += (list.empty() ? "" : ", ") + c;
                    bad(p.name, "must be one of: " + list);
                }
                break;
            }
            case ParamType::List:
                if (v.is_string())
                    v = Json::array({v});
                if (!v.is_array())
                    bad(p.name, "must be a list of strings");
                for (const auto &item : v)
                    if (!item.is_string())
                        bad(p.name, "must be a list of strings");
                //if (p.required && v.empty())
                //    bad(p.name, "needs at least one value");
                break;
            }
            if (p.minimum && v.is_number() && v.get<double>() < *p.minimum)
            {
                Json shown =
                    std::floor(*p.minimum) == *p.minimum ? Json(static_cast<long long>(*p.minimum)) : Json(*p.minimum);
                bad(p.name, "must be at least " + shown.dump());
            }
            out[p.name] = v;
        }
        return out;
    }

    Json execute(Context &context, const Command &command, const Json &args)
    {
        try
        {
            Json normalised = normalise_args(command, args);
            Json result = command.run(context, normalised);
            return {{"ok", true}, {"command", command.id()}, {"result", result}};
        }
        catch (const Error &e)
        {
            return {{"ok", false},
                    {"command", command.id()},
                    {"error", {{"code", e.code}, {"message", e.what()}, {"details", e.details}}}};
        }
        catch (const std::exception &e)
        {
            return {{"ok", false},
                    {"command", command.id()},
                    {"error", {{"code", "internal_error"}, {"message", e.what()}, {"details", Json::object()}}}};
        }
    }
} // namespace studio
