// studio-plus: the command line front end. Every command comes from the registry, so the CLI,
// the GUI and the MCP server always offer the same actions.
#include "core/registry.h"
#include "core/settings.h"
#include "mcp/server.h"
#include "version.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <mutex>

using namespace studio;

namespace
{
    enum Exit
    {
        exit_ok = 0,
        exit_failed = 1,
        exit_usage = 2
    };

    std::mutex g_output;

    void write(FILE *f, std::string_view text)
    {
        std::lock_guard lock(g_output);
        fwrite(text.data(), 1, text.size(), f);
        fflush(f);
    }

    std::string pad(std::string s, size_t width)
    {
        if (s.size() < width)
            s.append(width - s.size(), ' ');
        return s;
    }

    void print_overview()
    {
        const auto &reg = Registry::instance();
        std::string out = "ReSkate Studio+ " STUDIO_PLUS_VERSION "\n\nUsage:\n"
                          "  studio-plus <group> <command> [arguments] [--json]\n"
                          "  studio-plus <group> <command> --help     help for one command\n"
                          "  studio-plus commands [--json]            every command, machine-readable with --json\n"
                          "  studio-plus mcp                          run as an MCP server on stdin/stdout\n\n";
        for (const auto &group : reg.groups())
        {
            out += group + "\n";
            for (const auto &c : reg.all())
                if (c.group == group)
                    out += "  " + pad(c.id(), 28) + c.summary + "\n";
            out += "\n";
        }
        out += "Global options: --json (JSON result on stdout, JSON-lines progress on stderr), --quiet (no progress)\n";
        write(stdout, out);
    }

    std::string usage_line(const Command &c)
    {
        std::string line = "studio-plus " + c.id();
        for (const auto &p : c.params)
        {
            std::string token;
            if (p.positional)
                token = "<" + p.name + (p.type == ParamType::List ? "...>" : ">");
            else if (p.type == ParamType::Boolean)
                token = p.default_value && p.default_value->is_boolean() && p.default_value->get<bool>()
                            ? "--no-" + p.name
                            : "--" + p.name;
            else
                token = "--" + p.name + " <" + std::string(to_string(p.type)) + ">";
            line += " " + (p.required ? token : "[" + token + "]");
        }
        return line;
    }

    void print_command_help(const Command &c)
    {
        std::string out = usage_line(c) + "\n\n" + c.description + "\n";
        if (!c.params.empty())
        {
            out += "\nArguments:\n";
            for (const auto &p : c.params)
            {
                std::string name = p.positional ? "<" + p.name + ">" : "--" + p.name;
                std::string extra;
                if (!p.choices.empty())
                {
                    extra += " Choices:";
                    for (const auto &ch : p.choices)
                        extra += " " + ch;
                    extra += ".";
                }
                if (p.default_value)
                    extra += " Default: " + p.default_value->dump() + ".";
                if (p.required)
                    extra += " Required.";
                std::string help = p.help;
                if (!help.empty() && help.back() != '.' && !extra.empty())
                    help += ".";
                out += "  " + pad(name, 26) + help + extra + "\n";
            }
        }
        if (c.writes_game)
            out += "\nThis command changes files inside the ReSkate folder.\n";
        if (!c.examples.empty())
        {
            out += "\nExamples:\n";
            for (const auto &e : c.examples)
                out += "  studio-plus " + e + "\n";
        }
        write(stdout, out);
    }

    void print_human_value(std::string &out, const Json &value, int indent)
    {
        std::string pre(static_cast<size_t>(indent), ' ');
        if (value.is_object())
        {
            for (auto it = value.begin(); it != value.end(); ++it)
            {
                if (it->is_structured() && !it->empty())
                {
                    out += pre + it.key() + ":\n";
                    print_human_value(out, *it, indent + 2);
                }
                else
                {
                    out += pre + it.key() + ": " + (it->is_string() ? it->get<std::string>() : it->dump()) + "\n";
                }
            }
        }
        else if (value.is_array())
        {
            for (const auto &item : value)
            {
                bool flat = item.is_object();
                if (flat)
                    for (const auto &field : item)
                        flat = flat && !(field.is_structured() && !field.empty());
                if (flat && item.size() <= 3)
                {
                    // Small flat records stay on one line: "- kind=ebx  name=items/..."
                    std::string line;
                    for (auto it = item.begin(); it != item.end(); ++it)
                        line += (line.empty() ? "" : "  ") + it.key() + "=" +
                                (it->is_string() ? it->get<std::string>() : it->dump());
                    out += pre + "- " + line + "\n";
                }
                else if (item.is_structured())
                {
                    // Anything bigger becomes an indented block, so nested fields keep their names.
                    std::string block;
                    print_human_value(block, item, indent + 2);
                    if (block.size() >= static_cast<size_t>(indent + 2))
                        block.replace(static_cast<size_t>(indent), 2, "- ");
                    out += block;
                }
                else
                {
                    out += pre + "- " + (item.is_string() ? item.get<std::string>() : item.dump()) + "\n";
                }
            }
        }
        else
        {
            out += pre + (value.is_string() ? value.get<std::string>() : value.dump()) + "\n";
        }
    }

    struct Parsed
    {
        Json args = Json::object();
        bool json = false, quiet = false, help = false;
    };

    // Turns CLI tokens into the same arguments object the MCP server receives.
    Parsed parse(const Command &c, const std::vector<std::string> &tokens)
    {
        Parsed p;
        std::vector<std::string> positionals;
        auto find_param = [&](std::string_view name) -> const Param *
        {
            for (const auto &param : c.params)
                if (param.name == name)
                    return &param;
            return nullptr;
        };
        for (size_t i = 0; i < tokens.size(); ++i)
        {
            const std::string &t = tokens[i];
            if (t == "--")
            {
                for (++i; i < tokens.size(); ++i)
                    positionals.push_back(tokens[i]);
                break;
            }
            if (t == "--json")
            {
                p.json = true;
                continue;
            }
            if (t == "--quiet")
            {
                p.quiet = true;
                continue;
            }
            if (t == "--help" || t == "-h")
            {
                p.help = true;
                continue;
            }
            if (t.rfind("--", 0) == 0 && t.size() > 2)
            {
                std::string name = t.substr(2), value;
                bool has_inline = false;
                if (auto eq = name.find('='); eq != std::string::npos)
                {
                    value = name.substr(eq + 1);
                    name = name.substr(0, eq);
                    has_inline = true;
                }
                const Param *param = find_param(name);
                if (!param && name.rfind("no-", 0) == 0)
                {
                    if (const Param *negated = find_param(name.substr(3));
                        negated && negated->type == ParamType::Boolean)
                    {
                        p.args[negated->name] = false;
                        continue;
                    }
                }
                if (!param)
                    throw Error("invalid_arguments",
                                "unknown option --" + name + " for " + c.id() + " (see --help)",
                                {{"param", name}});
                if (param->type == ParamType::Boolean && !has_inline)
                {
                    p.args[name] = true;
                    continue;
                }
                if (!has_inline)
                {
                    if (i + 1 >= tokens.size())
                        throw Error("invalid_arguments", "--" + name + " needs a value", {{"param", name}});
                    value = tokens[++i];
                }
                if (param->type == ParamType::List)
                {
                    if (!p.args.contains(name))
                        p.args[name] = Json::array();
                    p.args[name].push_back(value);
                }
                else
                {
                    p.args[name] = value;
                }
                continue;
            }
            positionals.push_back(t);
        }
        size_t next = 0;
        for (const auto &param : c.params)
        {
            if (!param.positional || next >= positionals.size())
                continue;
            if (p.args.contains(param.name))
                continue;
            if (param.type == ParamType::List)
            {
                Json list = Json::array();
                while (next < positionals.size())
                    list.push_back(positionals[next++]);
                p.args[param.name] = list;
            }
            else
            {
                p.args[param.name] = positionals[next++];
            }
        }
        if (next < positionals.size())
            throw Error("invalid_arguments",
                        "unexpected argument '" + positionals[next] + "' for " + c.id() + " (see --help)");
        // Paths are made absolute here so results and errors always show full paths.
        for (const auto &param : c.params)
            if (param.type == ParamType::Path && p.args.contains(param.name) && p.args[param.name].is_string())
            {
                std::string v = p.args[param.name];
                if (!v.empty())
                {
                    std::error_code ec;
                    auto abs = std::filesystem::absolute(utf8_to_wide(v), ec);
                    if (!ec)
                        p.args[param.name] = path_utf8(abs);
                }
            }
        return p;
    }

    int run_command(const Command &c, const std::vector<std::string> &tokens)
    {
        Parsed parsed;
        bool json_requested = std::find(tokens.begin(), tokens.end(), "--json") != tokens.end();
        try
        {
            parsed = parse(c, tokens);
        }
        catch (const Error &e)
        {
            if (json_requested)
            {
                Json j = {{"ok", false},
                          {"command", c.id()},
                          {"error", {{"code", e.code}, {"message", e.what()}, {"details", e.details}}}};
                write(stdout, j.dump() + "\n");
            }
            else
            {
                write(stderr, std::string("error: ") + e.what() + "\nusage: " + usage_line(c) + "\n");
            }
            return exit_usage;
        }
        if (parsed.help)
        {
            print_command_help(c);
            return exit_ok;
        }

        Settings settings = Settings::load();
        Context context(settings);
        if (!parsed.quiet)
        {
            context.on_progress = [&](double fraction, std::string_view message)
            {
                if (parsed.json)
                {
                    Json j = {{"event", "progress"}, {"message", message}};
                    if (fraction >= 0)
                        j["fraction"] = fraction;
                    write(stderr, j.dump() + "\n");
                }
                else
                {
                    std::string line =
                        fraction >= 0 ? "[" + std::to_string(static_cast<int>(fraction * 100)) + "%] " : "  ";
                    write(stderr, line + std::string(message) + "\n");
                }
            };
        }
        context.on_log = [&](std::string_view level, std::string_view message)
        {
            if (parsed.json)
                write(stderr, Json({{"event", "log"}, {"level", level}, {"message", message}}).dump() + "\n");
            else
                write(stderr, std::string(level) + ": " + std::string(message) + "\n");
        };
        static std::atomic<bool> cancel{false};
        context.cancel = &cancel;
        SetConsoleCtrlHandler(
            [](DWORD type) -> BOOL
            {
                if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT)
                {
                    cancel = true;
                    return TRUE;
                }
                return FALSE;
            },
            TRUE);

        Json outcome = execute(context, c, parsed.args);
        bool ok = outcome["ok"].get<bool>();
        if (parsed.json)
        {
            write(stdout, outcome.dump() + "\n");
        }
        else if (ok)
        {
            std::string out;
            print_human_value(out, outcome["result"], 0);
            write(stdout, out);
        }
        else
        {
            const Json &err = outcome["error"];
            std::string out = "error: " + err["message"].get<std::string>() + "\n";
            if (err["code"] == "invalid_arguments")
                out += "usage: " + usage_line(c) + "\n";
            write(stderr, out);
        }
        if (ok)
            return exit_ok;
        return outcome["error"]["code"] == "invalid_arguments" ? exit_usage : exit_failed;
    }
} // namespace

int wmain(int argc, wchar_t **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    register_all_commands();
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(wide_to_utf8(argv[i]));

    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h")
    {
        print_overview();
        return exit_ok;
    }
    if (args[0] == "--version" || args[0] == "version")
    {
        write(stdout, "ReSkate Studio+ " STUDIO_PLUS_VERSION "\n");
        return exit_ok;
    }
    if (args[0] == "mcp")
        return run_mcp_server();
    if (args[0] == "commands")
    {
        bool json = std::find(args.begin(), args.end(), "--json") != args.end();
        if (json)
            write(stdout,
                  Json({{"version", STUDIO_PLUS_VERSION}, {"commands", Registry::instance().describe()}}).dump(2) +
                      "\n");
        else
            print_overview();
        return exit_ok;
    }

    const auto &reg = Registry::instance();
    const std::string &group = args[0];
    auto groups = reg.groups();
    if (std::find(groups.begin(), groups.end(), group) == groups.end())
    {
        write(stderr, "error: unknown command group '" + group + "'. Run studio-plus help for the list.\n");
        return exit_usage;
    }
    if (args.size() < 2 || args[1] == "--help" || args[1] == "-h")
    {
        std::string out = "Commands in '" + group + "':\n";
        for (const auto &c : reg.all())
            if (c.group == group)
                out += "  " + pad(c.id(), 28) + c.summary + "\n";
        write(stdout, out);
        return args.size() < 2 ? exit_usage : exit_ok;
    }
    const Command *c = reg.find(group, args[1]);
    if (!c)
    {
        write(stderr,
              "error: unknown command '" + group + " " + args[1] + "'. Run studio-plus " + group + " --help.\n");
        return exit_usage;
    }
    return run_command(*c, std::vector<std::string>(args.begin() + 2, args.end()));
}
