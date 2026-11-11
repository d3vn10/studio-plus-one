#include "gui/jobs.h"

#include "core/settings.h"

#include <cstdio>
#include <iterator>
#include <thread>

namespace studio::gui
{

    JobState Job::state() const
    {
        if (!done.load())
            return JobState::running;
        std::lock_guard lock(mutex);
        if (outcome.value("ok", false))
            return JobState::ok;
        if (outcome.contains("error") && outcome["error"].value("code", "") == "cancelled")
            return JobState::cancelled;
        return JobState::failed;
    }

    double Job::seconds() const
    {
        const auto end = done.load() ? finished : std::chrono::steady_clock::now();
        return std::chrono::duration<double>(end - started).count();
    }

    std::shared_ptr<Job> Jobs::start(const Command &command, Json args)
    {
        auto job = std::make_shared<Job>();
        job->id = next_id_++;
        job->command = &command;
        job->cli = cli_line(command, args);
        job->args = std::move(args);
        job->started_at = std::chrono::system_clock::now();
        job->started = std::chrono::steady_clock::now();
        jobs_.push_back(job);
        // Activity keeps the last runs, not every one of a long session (results can be megabytes). A page that
        // still shows a run holds its own reference, so only the Activity row goes.
        constexpr std::size_t kept = 300;
        for (auto it = jobs_.begin(); jobs_.size() > kept && it != jobs_.end();)
            it = (*it)->done.load() ? jobs_.erase(it) : std::next(it);
        // Detached: the job keeps itself alive, and closing the window cancels and waits for it.
        std::thread(
            [job]
            {
                Settings settings = Settings::load();
                Context context(settings);
                context.cancel = &job->cancel;
                context.on_progress = [&](double fraction, std::string_view message)
                {
                    std::lock_guard lock(job->mutex);
                    job->fraction = fraction;
                    job->message = message;
                };
                context.on_log = [&](std::string_view level, std::string_view message)
                {
                    std::lock_guard lock(job->mutex);
                    job->log.emplace_back(level, message);
                };
                Json outcome = execute(context, *job->command, job->args);
                {
                    std::lock_guard lock(job->mutex);
                    job->outcome = std::move(outcome);
                    job->finished = std::chrono::steady_clock::now();
                }
                job->done = true;
            })
            .detach();
        return job;
    }

    int Jobs::running() const
    {
        int count = 0;
        for (const auto &job : jobs_)
            count += job->done.load() ? 0 : 1;
        return count;
    }

    void Jobs::cancel_all()
    {
        for (const auto &job : jobs_)
            job->cancel = true;
    }

    bool Jobs::wait_all(int milliseconds) const
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (running())
        {
            if (std::chrono::steady_clock::now() >= until)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return true;
    }

    std::string quote_arg(std::string_view text)
    {
        if (!text.empty() && text.find_first_of(" \t\n\v\"") == std::string_view::npos)
            return std::string(text);
        std::string out = "\"";
        size_t backslashes = 0;
        for (char c : text)
        {
            if (c == '\\')
            {
                ++backslashes;
                continue;
            }
            if (c == '"')
                out.append(backslashes * 2 + 1, '\\');
            else
                out.append(backslashes, '\\');
            backslashes = 0;
            out += c;
        }
        // Backslashes before the closing quote are doubled so they stay backslashes.
        out.append(backslashes * 2, '\\');
        out += '"';
        return out;
    }

    namespace
    {
        std::string as_text(const Json &value)
        {
            return value.is_string() ? value.get<std::string>() : value.dump();
        }

        bool is_true(const Json &value)
        {
            if (value.is_boolean())
                return value.get<bool>();
            if (value.is_string())
            {
                const auto s = value.get<std::string>();
                return s == "true" || s == "1" || s == "yes";
            }
            return false;
        }

        bool is_default(const Param &param, const Json &value)
        {
            if (!param.default_value)
                return false;
            if (param.type == ParamType::Boolean)
                return is_true(value) == is_true(*param.default_value);
            if (param.type == ParamType::List)
                return value == *param.default_value;
            return as_text(value) == as_text(*param.default_value);
        }
    } // namespace

    std::string cli_line(const Command &command, const Json &args)
    {
        std::string line = "studio-plus " + command.group + " " + command.name;
        // A positional only goes in order while every positional before it did too; after a gap the CLI
        // would hand the value to the earlier parameter, so the rest are named.
        bool in_order = true;
        for (size_t index = 0; index < command.params.size(); ++index)
        {
            const Param &param = command.params[index];
            const auto found = args.find(param.name);
            const bool given = found != args.end() && !found->is_null() && !(found->is_array() && found->empty()) &&
                               !is_default(param, *found);
            if (!given)
            {
                if (param.positional)
                    in_order = false;
                continue;
            }
            const Json &value = *found;
            if (param.type == ParamType::Boolean)
            {
                if (is_true(value))
                    line += " --" + param.name;
                else
                    line += " --no-" + param.name;
                continue;
            }
            std::vector<std::string> values;
            if (value.is_array())
                for (const auto &item : value)
                    values.push_back(as_text(item));
            else
                values.push_back(as_text(value));

            bool positional = param.positional && in_order;
            // A list takes every positional after it, so it can only go in order when it is the last one used.
            if (positional && param.type == ParamType::List)
                for (size_t later = index + 1; later < command.params.size(); ++later)
                {
                    const auto &next = command.params[later];
                    if (next.positional && args.contains(next.name) && !args[next.name].is_null())
                        positional = false;
                }
            // A value that looks like an option would be read as one.
            for (const auto &v : values)
                if (v.size() > 1 && v[0] == '-')
                    positional = false;
            if (!positional && param.positional)
                in_order = false;

            for (const auto &v : values)
            {
                if (positional)
                    line += " " + quote_arg(v);
                else
                    line += " --" + param.name + " " + quote_arg(v);
            }
        }
        return line;
    }

    std::string format_seconds(double seconds)
    {
        char buffer[32];
        if (seconds < 10)
            std::snprintf(buffer, sizeof(buffer), "%.1f s", seconds);
        else if (seconds < 60)
            std::snprintf(buffer, sizeof(buffer), "%d s", static_cast<int>(seconds));
        else
            std::snprintf(
                buffer, sizeof(buffer), "%d m %02d s", static_cast<int>(seconds) / 60, static_cast<int>(seconds) % 60);
        return buffer;
    }

} // namespace studio::gui
