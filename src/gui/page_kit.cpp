#include "gui/page_kit.h"

#include "core/settings.h"
#include "gui/look.h"
#include "gui/widgets.h"

#include <imgui_internal.h>

#include <cctype>
#include <filesystem>

namespace studio::gui::kit
{

    void heading(const char *text)
    {
        ImGui::PushFont(g_fonts.tile);
        ImGui::TextUnformatted(text);
        ImGui::PopFont();
    }

    void tile_title(const char *text)
    {
        ImGui::PushFont(g_fonts.heading);
        ImGui::TextUnformatted(text);
        ImGui::PopFont();
    }

    void step_title(int number, const char *text)
    {
        auto *draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float size = g_fonts.heading->FontSize + S(6);
        draw->AddRectFilled(at, ImVec2(at.x + size, at.y + size), color::blue);
        const std::string n = std::to_string(number);
        const ImVec2 ts = g_fonts.heading->CalcTextSizeA(g_fonts.heading->FontSize, 1e9f, 0, n.c_str());
        draw->AddText(g_fonts.heading,
                      g_fonts.heading->FontSize,
                      ImVec2(at.x + (size - ts.x) * 0.5f, at.y + (size - ts.y) * 0.5f),
                      color::ink,
                      n.c_str());
        ImGui::SetCursorScreenPos(ImVec2(at.x + size + S(12), at.y + S(3)));
        tile_title(text);
        ImGui::SetCursorScreenPos(ImVec2(at.x, std::max(ImGui::GetCursorScreenPos().y, at.y + size + S(6))));
    }

    void caption(const char *text)
    {
        ImGui::PushFont(g_fonts.caption);
        ImGui::TextDisabled("%s", text);
        ImGui::PopFont();
    }

    void muted(const std::string &text)
    {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", text.c_str());
        ImGui::PopTextWrapPos();
    }

    void coloured(const std::string &text, ImU32 colour)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, colour);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    bool running(const std::shared_ptr<Job> &job)
    {
        return job && !job->done.load();
    }

    bool succeeded(const std::shared_ptr<Job> &job)
    {
        return job && job->done.load() && job->state() == JobState::ok;
    }

    Json outcome(const std::shared_ptr<Job> &job)
    {
        if (!job || !job->done.load())
            return Json::object();
        std::lock_guard lock(job->mutex);
        return job->outcome;
    }

    Json result(const std::shared_ptr<Job> &job)
    {
        const Json o = outcome(job);
        return o.value("ok", false) ? o["result"] : Json();
    }

    std::shared_ptr<Job> run(const char *group, const char *name, Json args)
    {
        const Command *command = Registry::instance().find(group, name);
        if (!command)
            return nullptr;
        Json clean = Json::object();
        for (auto it = args.begin(); it != args.end(); ++it)
        {
            if (it->is_null() || (it->is_string() && it->get<std::string>().empty()))
                continue;
            if (it->is_array() && it->empty())
                continue;
            clean[it.key()] = *it;
        }
        return g_jobs.start(*command, clean);
    }

    namespace
    {
        const char *hint_for(const std::string &code)
        {
            if (code == "not_a_game_root" || code == "game_root_missing")
                return "Point Settings at the ReSkate folder that holds Skate.exe.";
            if (code == "engine_missing")
                return "Point Settings at the engine folder from the ReSkate Studio zip.";
            if (code == "blender_missing")
                return "Set Blender in Settings, or export a .glb instead of an .fbx.";
            if (code == "mesh_not_found")
                return "Use FIND to pick a mesh name the game has.";
            if (code == "donor_has_no_geometry_slot")
                return "Deck, wheel and grip items share one mesh. Use REPLACE A GAME MESH for those.";
            if (code == "mesh_material_unroutable")
                return "Name your materials after the donor's material sections, or pick another donor.";
            if (code == "not_a_clip")
                return "Pick a clip that does not start with subt_ or blend.";
            if (code == "invalid_model_name")
                return "Rename the model file: ASCII letters, numbers, spaces, _ and - only.";
            return nullptr;
        }
    } // namespace

    void status(const std::shared_ptr<Job> &job, const char *done_text)
    {
        if (!job)
            return;
        const float time = static_cast<float>(ImGui::GetTime());
        auto *draw = ImGui::GetWindowDrawList();
        ImGui::PushID(job.get());
        if (!job->done.load())
        {
            double fraction;
            std::string message;
            {
                std::lock_guard lock(job->mutex);
                fraction = job->fraction;
                message = job->message;
            }
            const float width = ImGui::GetContentRegionAvail().x;
            const ImVec2 a = ImGui::GetCursorScreenPos();
            progress_bar(draw, a, ImVec2(a.x + width, a.y + S(10)), static_cast<float>(std::max(0.0, fraction)), time);
            ImGui::Dummy(ImVec2(width, S(10)));
            ImGui::TextDisabled(
                "%s  %s", format_seconds(job->seconds()).c_str(), message.empty() ? "Working..." : message.c_str());
            ImGui::BeginDisabled(job->cancel.load());
            if (ImGui::Button(job->cancel.load() ? "CANCELLING..." : "CANCEL"))
                job->cancel = true;
            ImGui::EndDisabled();
            ImGui::PopID();
            return;
        }
        const JobState state = job->state();
        const Json o = outcome(job);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const auto icon = state == JobState::ok          ? skate_theme::Icon::check
                          : state == JobState::cancelled ? skate_theme::Icon::warning
                                                         : skate_theme::Icon::fail;
        draw_status_icon(draw, ImVec2(at.x + S(10), at.y + g_fonts.bold->FontSize * 0.5f + S(1)), icon, time);
        ImGui::SetCursorScreenPos(ImVec2(at.x + S(28), at.y));
        ImGui::BeginGroup();
        ImGui::PushFont(g_fonts.bold);
        if (state == JobState::ok)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, color::good);
            ImGui::Text("%s in %s", done_text, format_seconds(job->seconds()).c_str());
        }
        else if (state == JobState::cancelled)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, color::warning);
            ImGui::Text("Cancelled after %s", format_seconds(job->seconds()).c_str());
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, color::danger);
            ImGui::Text("Failed: %s", o["error"].value("code", "error").c_str());
        }
        ImGui::PopStyleColor();
        ImGui::PopFont();
        if (state == JobState::failed)
        {
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(o["error"].value("message", "").c_str());
            ImGui::PopTextWrapPos();
            if (const char *hint = hint_for(o["error"].value("code", "")))
                muted(hint);
        }
        if (ImGui::SmallButton("COPY COMMAND LINE"))
            copy_text(job->cli);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", job->cli.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("COPY RESULT JSON"))
            copy_text((state == JobState::ok ? o["result"] : o.value("error", Json())).dump(2));
        ImGui::EndGroup();
        ImGui::PopID();
    }

    void scroll_here(std::string &target, int &frames, const char *name)
    {
        if (target != name)
            return;
        // Tiles are child windows that do not scroll; scroll the nearest window that does.
        ImGuiWindow *window = ImGui::GetCurrentWindow();
        while (window->ScrollMax.y <= 0.0f && (window->Flags & ImGuiWindowFlags_ChildWindow) && window->ParentWindow)
            window = window->ParentWindow;
        ImGui::SetScrollFromPosY(window, ImGui::GetCursorScreenPos().y - window->Pos.y, 0.0f);
        if (++frames > 20)
            target.clear();
    }

    std::string data_path(const wchar_t *folder, const std::string &file)
    {
        const std::filesystem::path dir = Settings::data_dir() / folder;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return path_utf8(dir / utf8_to_wide(file));
    }

    std::string leaf(const std::string &asset)
    {
        const auto slash = asset.find_last_of('/');
        return slash == std::string::npos ? asset : asset.substr(slash + 1);
    }

    std::string lower(std::string text)
    {
        for (auto &c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    std::string thousands(long long value)
    {
        std::string digits = std::to_string(value);
        for (int at = static_cast<int>(digits.size()) - 3; at > (digits[0] == '-' ? 1 : 0); at -= 3)
            digits.insert(static_cast<size_t>(at), ",");
        return digits;
    }

} // namespace studio::gui::kit
