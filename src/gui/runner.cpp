#include "gui/runner.h"

#include "gui/look.h"
#include "gui/widgets.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace studio::gui
{
    namespace
    {
        std::string json_text(const Json &value)
        {
            return value.is_string() ? value.get<std::string>() : value.dump();
        }

        std::string upper(std::string text)
        {
            for (auto &c : text)
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
            return text;
        }

        std::shared_ptr<Job> latest_job(const Command *command)
        {
            const auto &all = g_jobs.all();
            for (auto it = all.rbegin(); it != all.rend(); ++it)
                if ((*it)->command == command)
                    return *it;
            return {};
        }

        ImU32 state_colour(JobState state)
        {
            switch (state)
            {
            case JobState::ok:
                return color::good;
            case JobState::failed:
                return color::danger;
            case JobState::cancelled:
                return color::warning;
            default:
                return color::blue;
            }
        }

        skate_theme::Icon state_icon(JobState state)
        {
            switch (state)
            {
            case JobState::ok:
                return skate_theme::Icon::check;
            case JobState::failed:
                return skate_theme::Icon::fail;
            case JobState::cancelled:
                return skate_theme::Icon::warning;
            default:
                return skate_theme::Icon::busy;
            }
        }

        // The yellow meter when the fraction is known; a sliding block when it is not.
        void job_progress(float width, double fraction, float time)
        {
            auto *draw = ImGui::GetWindowDrawList();
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const ImVec2 b(a.x + width, a.y + S(14));
            if (fraction >= 0)
            {
                progress_bar(draw, a, b, static_cast<float>(fraction), time);
            }
            else
            {
                draw->AddRectFilled(a, b, IM_COL32(10, 10, 10, 255));
                const float block = width * 0.25f;
                const float x = a.x + std::fmod(time * 0.6f, 1.0f) * (width + block) - block;
                draw->PushClipRect(a, b, true);
                draw->AddRectFilled(ImVec2(x, a.y), ImVec2(x + block, b.y), skate_theme::bar);
                draw->PopClipRect();
                draw->AddRect(
                    ImVec2(a.x - S(1), a.y - S(1)), ImVec2(b.x + S(1), b.y + S(1)), skate_theme::black, 0, 0, S(2));
            }
            ImGui::Dummy(ImVec2(width, S(14)));
        }

        void json_node(const std::string &key, const Json &value, int depth)
        {
            if (value.is_structured())
            {
                const bool object = value.is_object();
                const std::string label =
                    key + (object ? "  {" : "  [") + std::to_string(value.size()) + (object ? "}" : "]");
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
                // The result's own lists open, however long; nested ones only when short.
                if ((depth == 0 && value.size() <= 500) || (depth == 1 && value.size() <= 40))
                    flags |= ImGuiTreeNodeFlags_DefaultOpen;
                if (value.empty())
                    flags |= ImGuiTreeNodeFlags_Leaf;
                const bool open = ImGui::TreeNodeEx("##node", flags, "%s", label.c_str());
                if (ImGui::BeginPopupContextItem())
                {
                    if (ImGui::MenuItem("Copy as JSON"))
                        copy_text(value.dump(2));
                    ImGui::EndPopup();
                }
                if (open)
                {
                    int index = 0;
                    for (auto it = value.begin(); it != value.end(); ++it, ++index)
                    {
                        ImGui::PushID(index);
                        std::string child = object ? it.key() : "[" + std::to_string(index) + "]";
                        // An object in a list is named by its name, check or id, else its first text value,
                        // so a list of matches or checks reads as one.
                        if (!object && it->is_object())
                        {
                            std::string title;
                            for (const char *naming : {"name", "check", "id", "path"})
                                if (it->contains(naming) && (*it)[naming].is_string())
                                {
                                    title = (*it)[naming].get<std::string>();
                                    break;
                                }
                            if (title.empty())
                                for (const auto &field : it->items())
                                    if (field.value().is_string())
                                    {
                                        title = field.value().get<std::string>();
                                        break;
                                    }
                            if (!title.empty())
                                child += "  " + title;
                        }
                        json_node(child, *it, depth + 1);
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
                return;
            }
            ImGui::TreeNodeEx("##leaf",
                              ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Bullet,
                              "%s",
                              key.c_str());
            const std::string text = json_text(value);
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Copy value"))
                    copy_text(text);
                if (ImGui::MenuItem("Copy as JSON"))
                    copy_text(value.dump());
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            ImU32 colour = color::text;
            if (value.is_boolean())
                colour = value.get<bool>() ? color::good : color::danger;
            else if (value.is_number())
                colour = skate_theme::blue_hover;
            else if (value.is_null())
                colour = color::muted;
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(value.is_null() ? "null" : text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    } // namespace

    bool param_wants_folder(const Param &param)
    {
        for (const char *word : {"dir", "folder", "root", "staging", "output", "out"})
            if (param.name.find(word) != std::string::npos)
                return true;
        return false;
    }

    void Runner::bind(const Command *c)
    {
        if (command == c)
            return;
        command = c;
        problem.clear();
        fields.assign(c ? c->params.size() : 0, FieldState{});
        job = latest_job(c);
        if (!c)
            return;
        for (size_t i = 0; i < c->params.size(); ++i)
        {
            const Param &p = c->params[i];
            auto &f = fields[i];
            if (!p.default_value)
                continue;
            const Json &d = *p.default_value;
            switch (p.type)
            {
            case ParamType::Boolean:
                f.flag = d.is_boolean() && d.get<bool>();
                break;
            case ParamType::Enum:
            {
                const auto at = std::find(p.choices.begin(), p.choices.end(), json_text(d));
                f.choice = at == p.choices.end() ? -1 : static_cast<int>(at - p.choices.begin());
                break;
            }
            case ParamType::List:
                if (d.is_array())
                    for (const auto &item : d)
                        f.items.push_back(json_text(item));
                break;
            default:
                f.text = json_text(d);
                break;
            }
        }
    }

    void Runner::show(const std::shared_ptr<Job> &past)
    {
        command = nullptr;
        bind(past->command);
        job = past;
        fill(past->args);
    }

    bool Runner::run()
    {
        if (!command)
            return false;
        problem.clear();
        const Json a = args();
        try
        {
            normalise_args(*command, a);
        }
        catch (const Error &e)
        {
            problem = e.what();
            return false;
        }
        job = g_jobs.start(*command, a);
        reveal = 2;
        return true;
    }

    void Runner::fill(const Json &values)
    {
        if (!command)
            return;
        for (size_t i = 0; i < command->params.size(); ++i)
        {
            const Param &p = command->params[i];
            const auto it = values.find(p.name);
            if (it == values.end() || it->is_null())
                continue;
            auto &f = fields[i];
            switch (p.type)
            {
            case ParamType::Boolean:
                f.flag = it->is_boolean() ? it->get<bool>() : json_text(*it) == "true";
                break;
            case ParamType::Enum:
            {
                const auto at = std::find(p.choices.begin(), p.choices.end(), json_text(*it));
                f.choice = at == p.choices.end() ? -1 : static_cast<int>(at - p.choices.begin());
                break;
            }
            case ParamType::List:
                f.items.clear();
                if (it->is_array())
                    for (const auto &item : *it)
                        f.items.push_back(json_text(item));
                else
                    f.items.push_back(json_text(*it));
                break;
            default:
                f.text = json_text(*it);
                break;
            }
        }
    }

    Json Runner::args() const
    {
        Json out = Json::object();
        if (!command)
            return out;
        for (size_t i = 0; i < command->params.size(); ++i)
        {
            const Param &p = command->params[i];
            const auto &f = fields[i];
            switch (p.type)
            {
            case ParamType::Boolean:
            {
                const bool by_default =
                    p.default_value && p.default_value->is_boolean() && p.default_value->get<bool>();
                if (f.flag != by_default)
                    out[p.name] = f.flag;
                break;
            }
            case ParamType::Enum:
                if (f.choice >= 0 && f.choice < static_cast<int>(p.choices.size()))
                    out[p.name] = p.choices[static_cast<size_t>(f.choice)];
                break;
            case ParamType::List:
            {
                Json list = Json::array();
                for (const auto &item : f.items)
                    list.push_back(item);
                if (!f.next_item.empty())
                    list.push_back(f.next_item);
                if (!list.empty())
                    out[p.name] = list;
                break;
            }
            default:
                if (!f.text.empty())
                    out[p.name] = f.text;
                break;
            }
        }
        return out;
    }

    void json_tree(const Json &value)
    {
        if (!value.is_structured())
        {
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(json_text(value).c_str());
            ImGui::PopTextWrapPos();
            return;
        }
        int index = 0;
        for (auto it = value.begin(); it != value.end(); ++it, ++index)
        {
            ImGui::PushID(index);
            json_node(value.is_object() ? it.key() : "[" + std::to_string(index) + "]", *it, 0);
            ImGui::PopID();
        }
    }

    void draw_job(Job &job, bool show_cli)
    {
        const float time = static_cast<float>(ImGui::GetTime());
        const JobState state = job.state();
        auto *draw = ImGui::GetWindowDrawList();
        ImGui::PushID(job.id);
        if (state == JobState::running)
        {
            double fraction;
            std::string message;
            {
                std::lock_guard lock(job.mutex);
                fraction = job.fraction;
                message = job.message;
            }
            const ImVec2 at = ImGui::GetCursorScreenPos();
            draw_status_icon(
                draw, ImVec2(at.x + S(10), at.y + g_fonts.bold->FontSize * 0.5f + S(1)), skate_theme::Icon::busy, time);
            ImGui::SetCursorScreenPos(ImVec2(at.x + S(28), at.y));
            ImGui::PushFont(g_fonts.bold);
            ImGui::Text("Running  %s", format_seconds(job.seconds()).c_str());
            ImGui::PopFont();
            if (!message.empty())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", message.c_str());
            }
            job_progress(ImGui::GetContentRegionAvail().x, fraction, time);
            ImGui::BeginDisabled(job.cancel.load());
            if (ImGui::Button(job.cancel.load() ? "CANCELLING..." : "CANCEL", ImVec2(S(130), 0)))
                job.cancel = true;
            ImGui::EndDisabled();
        }
        else
        {
            Json outcome;
            {
                std::lock_guard lock(job.mutex);
                outcome = job.outcome;
            }
            const ImVec2 at = ImGui::GetCursorScreenPos();
            draw_status_icon(
                draw, ImVec2(at.x + S(10), at.y + g_fonts.bold->FontSize * 0.5f + S(1)), state_icon(state), time);
            ImGui::SetCursorScreenPos(ImVec2(at.x + S(28), at.y));
            ImGui::PushFont(g_fonts.bold);
            ImGui::PushStyleColor(ImGuiCol_Text, state_colour(state));
            if (state == JobState::ok)
                ImGui::Text("Done in %s", format_seconds(job.seconds()).c_str());
            else if (state == JobState::cancelled)
                ImGui::Text("Cancelled after %s", format_seconds(job.seconds()).c_str());
            else
                ImGui::Text("Failed: %s", outcome["error"].value("code", "error").c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            if (state == JobState::failed)
            {
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(outcome["error"].value("message", "").c_str());
                ImGui::PopTextWrapPos();
            }
            const Json &shown = state == JobState::ok ? outcome["result"] : outcome["error"];
            if (ImGui::Button("COPY RESULT JSON"))
                copy_text(shown.dump(2));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The same JSON studio-plus prints with --json");
            ImGui::SameLine();
            if (ImGui::Button("COPY COMMAND LINE"))
                copy_text(job.cli);
            if (show_cli)
            {
                ImGui::PushFont(g_fonts.caption);
                ImGui::TextDisabled("RAN AS");
                ImGui::PopFont();
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(job.cli.c_str());
                ImGui::PopTextWrapPos();
            }
            // As tall as the result: the page scrolls, so there is one scrollbar, not two.
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(rgba(16, 16, 18, 0.9f)));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(4)));
            ImGui::BeginChild(
                "##result", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
            json_tree(shown);
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        }
        std::vector<std::pair<std::string, std::string>> log;
        {
            std::lock_guard lock(job.mutex);
            log = job.log;
        }
        if (!log.empty() && ImGui::CollapsingHeader(("Log (" + std::to_string(log.size()) + ")###log").c_str()))
        {
            for (const auto &[level, message] : log)
            {
                ImGui::TextDisabled("%s", level.c_str());
                ImGui::SameLine();
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(message.c_str());
                ImGui::PopTextWrapPos();
            }
        }
        ImGui::PopID();
    }

    void draw_runner(Runner &runner, HWND owner)
    {
        const Command *command = runner.command;
        if (!command)
        {
            ImGui::TextDisabled("Pick a command on the left.");
            return;
        }
        // ------------------------------------------------ what it is
        ImGui::PushFont(g_fonts.tile);
        ImGui::TextUnformatted(upper(command->id()).c_str());
        ImGui::PopFont();
        {
            auto *draw = ImGui::GetWindowDrawList();
            ImVec2 at = ImGui::GetCursorScreenPos();
            float x = at.x;
            const auto pill = [&](const std::string &text, ImU32 fill)
            {
                badge(draw, ImVec2(x, at.y), text, fill, color::ink);
                x += badge_width(text) + S(6);
            };
            pill("MCP " + command->tool_name(), rgba(255, 255, 255, 0.75f));
            if (command->writes_game)
                pill("CHANGES GAME FILES", color::warning);
            if (command->long_running)
                pill("CAN TAKE A WHILE", color::blue);
            ImGui::Dummy(ImVec2(x - at.x, g_fonts.caption->FontSize + S(8)));
        }
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(command->summary.c_str());
        if (!command->description.empty() && command->description != command->summary)
            ImGui::TextDisabled("%s", command->description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        // ------------------------------------------------ the form
        const bool running = runner.job && !runner.job->done.load() && runner.job->command == command;
        if (!command->params.empty())
        {
            begin_tile("##form", seed_of(command->id().c_str()));
            ImGui::BeginDisabled(running);
            for (size_t i = 0; i < command->params.size(); ++i)
            {
                const Param &p = command->params[i];
                auto &f = runner.fields[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::PushFont(g_fonts.bold);
                ImGui::TextUnformatted(p.name.c_str());
                ImGui::PopFont();
                std::string tag = upper(std::string(to_string(p.type)));
                if (p.required)
                    tag += "  REQUIRED";
                if (p.positional)
                    tag += "  POSITIONAL";
                inline_caption(tag.c_str());
                if (!p.help.empty())
                {
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextDisabled("%s", p.help.c_str());
                    ImGui::PopTextWrapPos();
                }
                switch (p.type)
                {
                case ParamType::Path:
                    path_field("##path", f.text, owner, param_wants_folder(p));
                    break;
                case ParamType::String:
                    ImGui::SetNextItemWidth(-1);
                    input_text("##text", f.text, ImGuiInputTextFlags_EnterReturnsTrue);
                    break;
                case ParamType::Integer:
                    ImGui::SetNextItemWidth(S(200));
                    input_text("##integer", f.text, ImGuiInputTextFlags_CharsDecimal);
                    break;
                case ParamType::Number:
                    ImGui::SetNextItemWidth(S(200));
                    input_text("##number", f.text, ImGuiInputTextFlags_CharsScientific);
                    break;
                case ParamType::Boolean:
                    toggle("##flag", &f.flag);
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(f.flag ? "On" : "Off");
                    break;
                case ParamType::Enum:
                {
                    ImGui::SetNextItemWidth(S(280));
                    const char *shown = f.choice >= 0 ? p.choices[static_cast<size_t>(f.choice)].c_str() : "(choose)";
                    if (ImGui::BeginCombo("##enum", shown))
                    {
                        if (!p.required && ImGui::Selectable("(none)", f.choice < 0))
                            f.choice = -1;
                        for (size_t c = 0; c < p.choices.size(); ++c)
                            if (ImGui::Selectable(p.choices[c].c_str(), f.choice == static_cast<int>(c)))
                                f.choice = static_cast<int>(c);
                        ImGui::EndCombo();
                    }
                    break;
                }
                case ParamType::List:
                {
                    for (size_t item = 0; item < f.items.size(); ++item)
                    {
                        ImGui::PushID(static_cast<int>(item));
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(46));
                        input_text("##item", f.items[item]);
                        ImGui::SameLine();
                        if (ImGui::Button("X", ImVec2(S(36), 0)))
                        {
                            f.items.erase(f.items.begin() + static_cast<std::ptrdiff_t>(item));
                            ImGui::PopID();
                            break;
                        }
                        ImGui::PopID();
                    }
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(86));
                    const bool enter = input_text(
                        "##next", f.next_item, ImGuiInputTextFlags_EnterReturnsTrue, "Add a value, Enter to add");
                    take_drop(f.next_item);
                    ImGui::SameLine();
                    if ((ImGui::Button("ADD", ImVec2(S(76), 0)) || enter) && !f.next_item.empty())
                    {
                        f.items.push_back(f.next_item);
                        f.next_item.clear();
                        ImGui::SetKeyboardFocusHere(-1);
                    }
                    break;
                }
                }
                ImGui::PopID();
                if (i + 1 < command->params.size())
                    ImGui::Dummy(ImVec2(0, S(2)));
            }
            ImGui::EndDisabled();
            end_tile();
        }

        // ------------------------------------------------ the equivalent command line, then Run
        const Json args = runner.args();
        const std::string line = cli_line(*command, args);
        ImGui::PushFont(g_fonts.caption);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("COMMAND LINE");
        ImGui::PopFont();
        ImGui::SameLine();
        copy_button("COPY##line", line);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopTextWrapPos();

        ImGui::BeginDisabled(running);
        const bool run = primary_button("RUN", ImVec2(S(140), S(36))) ||
                         (!running && ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_Enter, false));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Ctrl+Enter");
        ImGui::SameLine();
        if (ImGui::Button("RESET", ImVec2(S(100), S(36))))
        {
            auto keep = runner.job;
            runner.command = nullptr;
            runner.bind(command);
            runner.job = keep;
        }
        if (run && !running)
            runner.run();
        if (!runner.problem.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, color::danger);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(runner.problem.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        if (runner.job && runner.job->command == command)
        {
            ImGui::Spacing();
            // A run just started, and again when it finishes: bring its progress, then its result, into view.
            // The result is only measured a frame after it first draws, so the scroll repeats for a few frames.
            const bool done = runner.job->done.load();
            if (runner.reveal == 2)
            {
                ImGui::SetScrollHereY(0.1f);
                runner.reveal = 1;
            }
            else if (runner.reveal == 1 && done)
            {
                runner.reveal = -3;
            }
            if (runner.reveal < 0)
            {
                ImGui::SetScrollHereY(0.1f);
                ++runner.reveal;
            }
            draw_job(*runner.job, false);
        }
    }

    float draw_activity(ImVec2 position,
                        float width,
                        float max_height,
                        bool &open,
                        float &open_height,
                        const std::function<void(const std::shared_ptr<Job> &)> &open_job)
    {
        const float header = S(38);
        const float height = open ? std::clamp(open_height, S(140), std::max(S(140), max_height)) : header;
        const ImVec2 end(position.x + width, position.y + height);
        auto *draw = ImGui::GetWindowDrawList();
        const float time = static_cast<float>(ImGui::GetTime());
        draw->AddRectFilled(position, end, rgba(20, 20, 22, 0.97f));
        draw->AddLine(position, ImVec2(end.x, position.y), color::outline, S(1));

        // The top edge drags the drawer taller or shorter.
        if (open)
        {
            ImGui::SetCursorScreenPos(ImVec2(position.x, position.y - S(3)));
            ImGui::InvisibleButton("##activity_resize", ImVec2(width, S(6)));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                draw->AddLine(position, ImVec2(end.x, position.y), color::blue, S(2));
            }
            if (ImGui::IsItemActive())
                open_height = std::clamp(height - ImGui::GetIO().MouseDelta.y, S(140), max_height);
        }

        // The header: a click anywhere on it opens or closes the drawer.
        bool hovered{};
        if (tile_hit("##activity_header",
                     ImVec2(position.x, position.y + S(2)),
                     ImVec2(width, header - S(2)),
                     true,
                     hovered))
            open = !open;
        if (hovered)
            draw->AddRectFilled(
                ImVec2(position.x, position.y + S(1)), ImVec2(end.x, position.y + header), rgba(255, 255, 255, 0.04f));
        const auto &all = g_jobs.all();
        const int running = g_jobs.running();
        float x = position.x + S(16);
        const float text_y = position.y + (header - g_fonts.bold->FontSize) * 0.5f;
        draw->AddText(g_fonts.bold, g_fonts.bold->FontSize, ImVec2(x, text_y), color::text, "ACTIVITY");
        x += g_fonts.bold->CalcTextSizeA(g_fonts.bold->FontSize, FLT_MAX, 0, "ACTIVITY").x + S(10);
        const float pill_y = position.y + (header - g_fonts.caption->FontSize - S(8)) * 0.5f;
        const std::string count = std::to_string(all.size());
        badge(draw, ImVec2(x, pill_y), count, rgba(255, 255, 255, 0.14f), color::text);
        x += badge_width(count) + S(6);
        if (running)
        {
            const std::string text = std::to_string(running) + " RUNNING";
            badge(draw, ImVec2(x, pill_y), text, color::blue, color::ink);
            x += badge_width(text) + S(6);
        }
        draw->AddText(g_fonts.body,
                      g_fonts.body->FontSize,
                      ImVec2(x + S(8), position.y + (header - g_fonts.body->FontSize) * 0.5f),
                      color::muted,
                      "Every command run here, with the command line that repeats it");
        // A chevron: up to open, down to close.
        const ImVec2 c(end.x - S(24), position.y + header * 0.5f);
        const float d = open ? 1.0f : -1.0f;
        const ImVec2 chevron[]{
            ImVec2(c.x - S(6), c.y - d * S(3)), ImVec2(c.x, c.y + d * S(3)), ImVec2(c.x + S(6), c.y - d * S(3))};
        draw->AddPolyline(chevron, 3, color::text, 0, S(2));
        if (!open)
            return height;

        ImGui::SetCursorScreenPos(ImVec2(position.x + S(12), position.y + header));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##activity_list", ImVec2(width - S(24), height - header - S(8)));
        ImGui::PopStyleVar();
        if (all.empty())
        {
            ImGui::TextDisabled("Nothing has run yet. Commands you run in Studio+ show up here.");
        }
        else if (ImGui::BeginTable("##activity_table",
                                   6,
                                   ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                                       ImGuiTableFlags_BordersInnerH))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(26));
            ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthFixed, S(150));
            ImGui::TableSetupColumn("Started", ImGuiTableColumnFlags_WidthFixed, S(70));
            ImGui::TableSetupColumn("Took", ImGuiTableColumnFlags_WidthFixed, S(64));
            ImGui::TableSetupColumn("Command line", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(132));
            ImGui::TableHeadersRow();
            for (auto it = all.rbegin(); it != all.rend(); ++it)
            {
                const auto &job = *it;
                const JobState state = job->state();
                ImGui::PushID(job->id);
                ImGui::TableNextRow(0, S(30));
                ImGui::TableNextColumn();
                const ImVec2 cell = ImGui::GetCursorScreenPos();
                draw_status_icon(
                    ImGui::GetWindowDrawList(), ImVec2(cell.x + S(11), cell.y + S(13)), state_icon(state), time);
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(job->command->id().c_str());
                ImGui::TableNextColumn();
                const std::time_t when = std::chrono::system_clock::to_time_t(job->started_at);
                std::tm local{};
                localtime_s(&local, &when);
                char clock[16];
                std::strftime(clock, sizeof(clock), "%H:%M:%S", &local);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", clock);
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", format_seconds(job->seconds()).c_str());
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(job->cli.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", job->cli.c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("COPY"))
                    copy_text(job->cli);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Copy the command line");
                ImGui::SameLine();
                if (ImGui::SmallButton("OPEN"))
                    open_job(job);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Show this run and its result");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        return height;
    }

} // namespace studio::gui
