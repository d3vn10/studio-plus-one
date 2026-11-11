#include "core/engine.h"
#include "core/settings.h"
#include <charconv>

namespace studio
{

    namespace
    {
        std::vector<std::string> split_lines(const std::string &text)
        {
            std::vector<std::string> out;
            std::string line;
            for (char c : text)
            {
                if (c == '\n' || c == '\r')
                {
                    if (!line.empty())
                        out.push_back(line);
                    line.clear();
                }
                else
                    line.push_back(c);
            }
            if (!line.empty())
                out.push_back(line);
            return out;
        }

        bool starts_with_ci(std::string_view s, std::string_view prefix)
        {
            if (s.size() < prefix.size())
                return false;
            for (size_t i = 0; i < prefix.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(s[i])) != prefix[i])
                    return false;
            return true;
        }
    } // namespace

    EngineRun run_engine(Context &context,
                         const std::vector<std::string> &args,
                         LineHandler on_err_line,
                         LineHandler on_out_line,
                         const std::filesystem::path &working_dir,
                         const std::vector<std::pair<std::wstring, std::wstring>> &extra_env)
    {
        ProcessOptions options;
        options.executable = context.settings.reskate_cli();
        if (!working_dir.empty())
        {
            options.working_dir = working_dir;
        }
        else
        {
            std::error_code ec;
            options.working_dir = Settings::data_dir() / L"work";
            std::filesystem::create_directories(options.working_dir, ec);
            if (ec)
                options.working_dir = std::filesystem::temp_directory_path(ec);
        }
        options.args = args;
        options.cancel = context.cancel;
        if (!context.settings.blender.empty())
            options.extra_env.emplace_back(L"RESKATE_BLENDER", context.settings.blender.native());
        options.extra_env.insert(options.extra_env.end(), extra_env.begin(), extra_env.end());
        std::string held;
        bool holding = false;
        auto forward = [&](std::string_view line)
        {
            if (starts_with_ci(line, "warning"))
                context.log("warning", line);
            else
                context.progress(-1, line);
        };
        options.on_stderr_line = [&](std::string_view line)
        {
            if (on_err_line)
            {
                on_err_line(context, line);
                return;
            }
            if (holding)
                forward(held);
            held.assign(line);
            holding = true;
        };
        if (on_out_line)
            options.on_stdout_line = [&](std::string_view line) { on_out_line(context, line); };

        ProcessResult r = run_process(options);
        if (r.cancelled)
            throw Error("cancelled", "Cancelled");
        if (holding && r.exit_code == 0)
            forward(held);
        EngineRun run;
        run.exit_code = r.exit_code;
        run.out = r.stdout_text;
        run.out_lines = split_lines(r.stdout_text);
        run.err_lines = split_lines(r.stderr_text);
        run.seconds = r.seconds;
        return run;
    }

    void require_success(const EngineRun &run, const std::string &code)
    {
        if (run.exit_code == 0)
            return;
        std::string message;
        // The engine prints its error as the last stderr line; skip trailing usage dumps.
        for (auto it = run.err_lines.rbegin(); it != run.err_lines.rend(); ++it)
        {
            if (it->rfind("usage:", 0) == 0 || it->rfind("  ", 0) == 0 || it->rfind("Animation commands", 0) == 0)
                continue;
            message = *it;
            break;
        }
        if (message.empty() && !run.err_lines.empty())
            message = run.err_lines.back();
        if (message.empty())
            message = "reskate_cli exited with code " + std::to_string(run.exit_code);
        Json tail = Json::array();
        size_t from = run.err_lines.size() > 20 ? run.err_lines.size() - 20 : 0;
        for (size_t i = from; i < run.err_lines.size(); ++i)
            tail.push_back(run.err_lines[i]);
        throw Error(code, message, {{"exit_code", run.exit_code}, {"stderr_tail", tail}});
    }

    Json typed_value(std::string_view text)
    {
        if (text == "true")
            return true;
        if (text == "false")
            return false;
        if (!text.empty())
        {
            long long i = 0;
            auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), i);
            if (ec == std::errc() && p == text.data() + text.size())
                return i;
            double d = 0;
            auto [p2, ec2] = std::from_chars(text.data(), text.data() + text.size(), d);
            if (ec2 == std::errc() && p2 == text.data() + text.size())
                return d;
        }
        return std::string(text);
    }

    Json parse_key_values(const std::vector<std::string> &lines, bool keep_other_lines)
    {
        Json out = Json::object();
        Json other = Json::array();
        for (const auto &line : lines)
        {
            auto eq = line.find('=');
            bool is_kv = eq != std::string::npos && eq > 0 && line.find(' ') > eq;
            if (is_kv)
            {
                std::string key = line.substr(0, eq);
                Json value = typed_value(std::string_view(line).substr(eq + 1));
                if (out.contains(key))
                {
                    if (!out[key].is_array())
                        out[key] = Json::array({out[key]});
                    out[key].push_back(value);
                }
                else
                {
                    out[key] = value;
                }
            }
            else if (keep_other_lines)
            {
                other.push_back(line);
            }
        }
        if (keep_other_lines && !other.empty())
            out["lines"] = other;
        return out;
    }

    std::string arg_string(const Json &args, const std::string &param)
    {
        auto it = args.find(param);
        if (it == args.end() || it->is_null())
            return {};
        if (it->is_string())
            return it->get<std::string>();
        return it->dump();
    }

    void add_option(std::vector<std::string> &out, const Json &args, const std::string &param, const std::string &flag)
    {
        auto it = args.find(param);
        if (it == args.end() || it->is_null())
            return;
        out.push_back(flag);
        if (it->is_array())
        {
            std::string joined;
            for (const auto &v : *it)
                joined += (joined.empty() ? "" : ",") + (v.is_string() ? v.get<std::string>() : v.dump());
            out.push_back(joined);
        }
        else
        {
            out.push_back(arg_string(args, param));
        }
    }

    void add_flag(std::vector<std::string> &out, const Json &args, const std::string &param, const std::string &flag)
    {
        auto it = args.find(param);
        if (it != args.end() && it->is_boolean() && it->get<bool>())
            out.push_back(flag);
    }
} // namespace studio
