// The shell's own pages (Home, Settings and All commands), the rail, and the frame every page sits in.
#include "gui/app.h"

#include "gui/look.h"
#include "gui/project_page.h"
#include "gui/maps_page.h"
#include "gui/assets_page.h"
#include "gui/cosmetics_page.h"
#include "gui/animations_page.h"
#include "gui/renderer.h"
#include "gui/widgets.h"
#include "version.h"

#include <imgui_internal.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>

namespace studio::gui
{
    namespace
    {

        std::string lower(std::string text)
        {
            for (auto &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        const Command *find_command(const char *group, const char *name)
        {
            return Registry::instance().find(group, name);
        }

        std::string saved_value(const Settings &settings, const std::string &key)
        {
            if (key == "game-root")
                return path_utf8(settings.game_root);
            if (key == "blender")
                return path_utf8(settings.blender);
            return path_utf8(settings.engine_dir);
        }

        void heading(const char *text)
        {
            ImGui::PushFont(g_fonts.tile);
            ImGui::TextUnformatted(text);
            ImGui::PopFont();
        }

        void caption(const char *text)
        {
            ImGui::PushFont(g_fonts.caption);
            ImGui::TextDisabled("%s", text);
            ImGui::PopFont();
        }

        void wrapped_muted(const std::string &text)
        {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", text.c_str());
            ImGui::PopTextWrapPos();
        }

        // ---------------------------------------------------------------- rail pages

        // The pages in the left rail, in order, between HOME and SETTINGS.
        struct Area
        {
            page page;
            const char *nav;
        };

        constexpr Area areas[]{
            {page::assets, "ASSETS"},
            {page::project, "PROJECT & MODS"},
            {page::cosmetics, "COSMETICS"},
            {page::maps, "CUSTOM MAPS"},
            {page::animations, "ANIMATIONS"},
        };

        // A list row for a command: its id over its summary.
        bool command_row(const Command &c, float width, bool selected)
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float height = S(50);
            const bool pressed = list_row(("##" + c.id()).c_str(), width, height, selected);
            auto *draw = ImGui::GetWindowDrawList();
            const ImVec4 clip(at.x, at.y, at.x + width - S(10), at.y + height);
            draw->AddText(
                g_fonts.bold, g_fonts.bold->FontSize, ImVec2(at.x + S(14), at.y + S(7)), color::text, c.id().c_str());
            draw->AddText(g_fonts.caption,
                          g_fonts.caption->FontSize,
                          ImVec2(at.x + S(14), at.y + S(28)),
                          color::muted,
                          c.summary.c_str(),
                          nullptr,
                          0,
                          &clip);
            if (c.writes_game)
            {
                const std::string mark = "WRITES";
                const float bw = badge_width(mark);
                badge(draw, ImVec2(at.x + width - bw - S(10), at.y + S(7)), mark, color::warning, color::ink);
            }
            return pressed;
        }

        // ---------------------------------------------------------------- home

        struct CheckInfo
        {
            const char *check;
            const char *label;
            const char *key; // the setting that fixes it
            bool optional;
        };
        constexpr CheckInfo check_infos[]{
            {"game-root", "ReSkate folder (where ReSkateLauncher.exe is)", "game-root", false},
            {"reskate", "ReSkate (ReSkate.dll)", "game-root", false},
            {"game-build", "Skate build ReSkate supports", "game-root", false},
            {"launcher", "ReSkate launcher (ReSkateLauncher.exe)", "game-root", true},
            {"mods", "Mods folder", "game-root", true},
            {"engine", "Engine (reskate_cli.exe)", "engine-dir", false},
            {"engine-native", "Engine Native folder", "engine-dir", false},
            {"blender", "Blender", "blender", true},
        };

        const CheckInfo *check_info(const std::string &name)
        {
            for (const auto &info : check_infos)
                if (name == info.check)
                    return &info;
            return nullptr;
        }

        void go_fix(App &app, const char *key)
        {
            app.page = page::settings;
            app.focus_key = key;
            app.focus_until = ImGui::GetTime() + 2.5;
        }

        void home_page(App &app)
        {
            const float time = static_cast<float>(ImGui::GetTime());
            heading("STATUS");
            wrapped_muted(
                "Studio+ makes Skate maps, cosmetics and mods. Everything it does is also a studio-plus command and an "
                "MCP tool, so whatever you do here can be repeated by a script or an AI.");
            ImGui::Spacing();

            // ------------------------------------------------ STATUS, from studio doctor
            const bool checking = app.doctor && !app.doctor->done.load();
            Json result;
            bool failed_run = false;
            if (app.doctor && !checking)
            {
                std::scoped_lock lock(app.doctor->mutex);
                if (app.doctor->outcome.value("ok", false))
                    result = app.doctor->outcome["result"];
                else
                    failed_run = true;
            }
            int missing = 0;
            if (result.contains("checks"))
                for (const auto &c : result["checks"])
                {
                    const auto *info = check_info(c.value("check", ""));
                    if (!c.value("ok", false) && !(info && info->optional))
                        ++missing;
                }

            begin_tile("##status", 5);
            {
                auto *draw = ImGui::GetWindowDrawList();
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float width = ImGui::GetContentRegionAvail().x;
                const char *pill = checking ? "CHECKING" : failed_run ? "PROBLEM" : missing ? "ACTION NEEDED" : "READY";
                const ImU32 accent = checking     ? color::blue
                                     : failed_run ? color::danger
                                     : missing    ? color::warning
                                                  : color::good;
                // The state is readable before a word of it is: a coloured edge and a pill.
                const ImVec2 tile_pos = ImGui::GetWindowPos();
                draw->AddRectFilled(tile_pos, ImVec2(tile_pos.x + S(4), tile_pos.y + ImGui::GetWindowSize().y), accent);
                ImGui::PushFont(g_fonts.tile);
                ImGui::TextUnformatted("STATUS");
                ImGui::PopFont();
                badge(draw,
                      ImVec2(at.x + width - badge_width(pill),
                             at.y + (g_fonts.tile->FontSize - g_fonts.caption->FontSize - S(8)) * 0.5f),
                      pill,
                      accent,
                      color::ink);
            }
            if (checking)
            {
                ImGui::TextDisabled("Checking the ReSkate folder, the engine and Blender...");
            }
            else if (failed_run)
            {
                std::lock_guard lock(app.doctor->mutex);
                ImGui::PushStyleColor(ImGuiCol_Text, color::danger);
                wrapped_muted(app.doctor->outcome["error"].value("message", "studio doctor failed"));
                ImGui::PopStyleColor();
            }
            else if (result.contains("checks"))
            {
                auto *draw = ImGui::GetWindowDrawList();
                for (const auto &c : result["checks"])
                {
                    const std::string name = c.value("check", "");
                    const auto *info = check_info(name);
                    const bool ok = c.value("ok", false);
                    ImGui::PushID(name.c_str());
                    const ImVec2 at = ImGui::GetCursorScreenPos();
                    const auto icon = ok                       ? skate_theme::Icon::check
                                      : info && info->optional ? skate_theme::Icon::warning
                                                               : skate_theme::Icon::fail;
                    draw_status_icon(
                        draw, ImVec2(at.x + S(10), at.y + g_fonts.bold->FontSize * 0.5f + S(2)), icon, time);
                    ImGui::SetCursorScreenPos(ImVec2(at.x + S(30), at.y));
                    ImGui::BeginGroup();
                    ImGui::PushFont(g_fonts.bold);
                    ImGui::TextUnformatted(info ? info->label : name.c_str());
                    ImGui::PopFont();
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - S(140));
                    ImGui::TextDisabled("%s", c.value("detail", "").c_str());
                    if (!ok && c.contains("fix"))
                        ImGui::TextDisabled("Fix: %s", c.value("fix", "").c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndGroup();
                    if (!ok && info)
                    {
                        const float button = S(120);
                        ImGui::SetCursorScreenPos(ImVec2(at.x + ImGui::GetContentRegionAvail().x - button, at.y));
                        if (primary_button("FIX", ImVec2(button, S(32))))
                            go_fix(app, info->key);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Open Settings at %s", info->key);
                        ImGui::SetCursorScreenPos(ImVec2(at.x, std::max(ImGui::GetItemRectMax().y, at.y)));
                        ImGui::Dummy(ImVec2(0, 0));
                    }
                    ImGui::PopID();
                    ImGui::Dummy(ImVec2(0, S(2)));
                }
            }
            ImGui::BeginDisabled(checking);
            if (ImGui::Button("CHECK AGAIN"))
                run_doctor(app);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("SETTINGS"))
                app.page = page::settings;
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Same as  studio-plus studio doctor");
            end_tile();
            ImGui::Spacing();

            // ------------------------------------------------ STUDIO+ and where to go next
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(7), 0));
            const bool tiles =
                ImGui::BeginTable("##home_tiles", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX);
            ImGui::PopStyleVar();
            if (tiles)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                begin_tile("##about", 11);
                ImGui::PushFont(g_fonts.heading);
                ImGui::TextUnformatted("RESKATE+ NOOB-STOODIO");
                ImGui::PopFont();
                field("VERSION", STUDIO_PLUS_VERSION);
                field("SETTINGS", path_utf8(Settings::file()));
                if (app.renderer)
                    field("DRAWN BY", wide_to_utf8(app.renderer->adapter()));
                if (ImGui::Button("OPEN DATA FOLDER"))
                {
                    std::error_code ec;
                    std::filesystem::create_directories(Settings::data_dir(), ec);
                    ShellExecuteW(nullptr, L"open", Settings::data_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                end_tile();
                ImGui::TableNextColumn();
                begin_tile("##next", 12);
                ImGui::PushFont(g_fonts.heading);
                ImGui::TextUnformatted("EVERY COMMAND, RIGHT HERE");
                ImGui::PopFont();
                wrapped_muted(std::to_string(Registry::instance().all().size()) +
                              " commands are in the registry. ALL COMMANDS runs any of them with a form, and ACTIVITY "
                              "keeps the command line for each run.");
                if (primary_button("ALL COMMANDS"))
                    app.page = page::commands;
                ImGui::SameLine();
                if (ImGui::Button("ACTIVITY"))
                    app.activity_open = true;
                end_tile();
                ImGui::EndTable();
            }
        }

        // ---------------------------------------------------------------- settings

        void save_setting(SettingRow &row)
        {
            const Command *set = find_command("studio", "set");
            if (!set)
                return;
            Json args = {{"key", row.key}, {"value", row.edit}};
            row.job = g_jobs.start(*set, args);
            row.handled = false;
        }

        // What the last studio doctor found in the ReSkate folder, one line each.
        void game_folder_findings(const App &app)
        {
            if (!app.doctor || !app.doctor->done.load())
            {
                ImGui::TextDisabled("Checking this folder...");
                return;
            }
            Json found;
            {
                std::lock_guard<std::mutex> lock(app.doctor->mutex);
                const Json &outcome = app.doctor->outcome;
                if (outcome.value("ok", false) && outcome["result"].contains("game_folder"))
                    found = outcome["result"]["game_folder"];
            }
            if (found.is_null())
                return;
            auto line = [](const bool ok, const std::string &text)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ok ? color::good : color::warning);
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(text.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            };
            if (!found.value("skate_exe", false))
            {
                line(false, "No Skate.exe in this folder.");
                return;
            }
            line(true, "Skate.exe found.");
            if (found.value("reskate_dll", false))
                line(true, "ReSkate.dll found.");
            else if (found.value("launcher", false))
                line(false, "No ReSkate.dll yet: ReSkate is not set up here. Mods built here will not load.");
            else
                line(false,
                     "No ReSkate.dll: this is a plain Steam copy without ReSkate. Mods built here will not load.");
            line(found.value("build", "") == "supported", found.value("build_detail", "") + ".");
            line(found.value("launcher", false),
                 found.value("launcher", false) ? "ReSkateLauncher.exe found." : "No ReSkateLauncher.exe here.");
            if (found["mods_folder"].is_string())
            {
                const int count = found.value("mod_count", 0);
                line(true,
                     "Mods folder: " + std::to_string(count) + (count == 1 ? " mod" : " mods") + " in " +
                         found["mods_folder"].get<std::string>());
            }
            else
            {
                ImGui::TextDisabled("No Mods folder yet. ReSkate makes it when the first mod is installed.");
            }
        }

        void settings_page(App &app)
        {
            heading("SETTINGS");
            wrapped_muted("Where Studio+ finds your ReSkate folder, the engine and Blender. These are the same "
                          "settings studio-plus "
                          "uses on the command line and the MCP server uses for AI.");
            ImGui::Spacing();
            begin_tile("##paths", 21);
            ImGui::PushFont(g_fonts.heading);
            ImGui::TextUnformatted("PATHS");
            ImGui::PopFont();
            const double now = ImGui::GetTime();
            for (auto &row : app.rows)
            {
                ImGui::PushID(row.key.c_str());
                const ImVec2 start = ImGui::GetCursorScreenPos();
                ImGui::BeginGroup();
                ImGui::PushFont(g_fonts.bold);
                ImGui::TextUnformatted(row.label.c_str());
                ImGui::PopFont();
                inline_caption(("studio set " + row.key).c_str());
                wrapped_muted(row.help);
                std::vector<FileFilter> filters;
                if (row.key == "blender")
                    filters = {{L"Blender (blender.exe)", L"blender.exe"}, {L"Programs (*.exe)", L"*.exe"}};
                const bool game_row = row.key == "game-root";
                if (game_row)
                    filters = {{L"ReSkate launcher (ReSkateLauncher.exe)", L"ReSkateLauncher.exe"},
                               {L"ReSkate.dll", L"ReSkate.dll"}};
                const bool research_row = row.key == "research-root";
                if (research_row)
                    filters = {{L"Skate game (Skate.exe)", L"Skate.exe"}};
                path_field("##path", row.edit, app.window, row.folder, filters, S(110));
                if (g_dropped_into == &row.edit)
                    save_setting(row);
                const std::string saved = saved_value(app.settings, row.key);
                const bool saving = row.job && !row.job->done.load();
                // Blue only when there is something to save.
                const bool changed = !saving && row.edit != saved;
                ImGui::BeginDisabled(!changed);
                if (changed ? primary_button("SAVE", ImVec2(S(110), 0)) : ImGui::Button("SAVE", ImVec2(S(110), 0)))
                    save_setting(row);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(saving);
                if (ImGui::Button("AUTO-DETECT", ImVec2(S(140), 0)))
                {
                    row.edit.clear();
                    save_setting(row);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Clear the saved value, so Studio+ looks for it again");
                if (game_row)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("PICK LAUNCHER", ImVec2(S(170), 0)))
                    {
                        const auto picked = pick_path(app.window, false, utf8_to_wide(row.edit), filters);
                        if (!picked.empty())
                            row.edit = wide_to_utf8(picked);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Choose ReSkateLauncher.exe; its folder is saved");
                }
                if (research_row)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("PICK GAME", ImVec2(S(170), 0)))
                    {
                        const auto picked = pick_path(app.window, false, utf8_to_wide(row.edit), filters);
                        if (!picked.empty())
                            row.edit = wide_to_utf8(picked);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Choose Skate.exe; its folder is saved");
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                Json warnings;
                if (saving)
                {
                    ImGui::TextDisabled("Saving...");
                }
                else if (row.job)
                {
                    std::lock_guard lock(row.job->mutex);
                    const Json &outcome = row.job->outcome;
                    if (outcome.value("ok", false))
                    {
                        if (outcome["result"].contains("warnings"))
                            warnings = outcome["result"]["warnings"];
                        const bool warned = warnings.is_array() && !warnings.empty();
                        ImGui::PushStyleColor(ImGuiCol_Text, warned ? color::warning : color::good);
                        ImGui::TextUnformatted(warned ? "Saved, with a warning" : "Saved");
                        ImGui::PopStyleColor();
                    }
                    else
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, color::danger);
                        ImGui::PushTextWrapPos(0);
                        ImGui::TextUnformatted(outcome["error"].value("message", "Could not save").c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::PopStyleColor();
                    }
                }
                else if (row.edit != saved)
                {
                    ImGui::TextDisabled("Not saved yet");
                }
                if (warnings.is_array())
                    for (const auto &w : warnings)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, color::warning);
                        ImGui::PushTextWrapPos(0);
                        ImGui::TextUnformatted(w.get<std::string>().c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::PopStyleColor();
                    }
                if (game_row && !saved.empty())
                    game_folder_findings(app);
                ImGui::EndGroup();
                // A FIX button on Home points here: the row flashes blue for a moment.
                if (app.focus_key == row.key && now < app.focus_until)
                {
                    const float alpha = static_cast<float>(std::min(1.0, (app.focus_until - now) / 1.0));
                    const ImVec2 end(start.x + ImGui::GetContentRegionAvail().x, ImGui::GetItemRectMax().y);
                    ImGui::GetWindowDrawList()->AddRect(ImVec2(start.x - S(8), start.y - S(6)),
                                                        ImVec2(end.x + S(8), end.y + S(6)),
                                                        rgba(1, 131, 255, alpha),
                                                        0,
                                                        0,
                                                        S(2));
                }
                ImGui::PopID();
                ImGui::Dummy(ImVec2(0, S(6)));
            }
            end_tile();
            ImGui::Spacing();
            begin_tile("##files", 22);
            ImGui::PushFont(g_fonts.heading);
            ImGui::TextUnformatted("FILES");
            ImGui::PopFont();
            field("SETTINGS FILE", path_utf8(Settings::file()));
            field("WINDOW SIZE AND PLACE", path_utf8(Settings::data_dir() / L"window.json"));
            field("ENGINE FOLDER IN USE", path_utf8(app.settings.resolved_engine_dir()));
            wrapped_muted(
                "Empty paths are found again on every start. The ReSkate folder: a folder with ReSkate.dll or "
                "ReSkateLauncher.exe next to Skate.exe in Downloads, Desktop, Documents, a Games folder or a drive "
                "root, else the Steam copy. Blender: Program Files and Steam. The engine: RSSP_ENGINE or the engine "
                "folder beside Studio+.");
            end_tile();
        }

        // ---------------------------------------------------------------- all commands

        void commands_page(App &app, float height)
        {
            const float list_width = S(330);
            ImGui::BeginChild("##command_list", ImVec2(list_width, height), 0);
            heading("CLI COMMANDS");
            const auto &registry = Registry::instance();
            wrapped_muted(std::to_string(registry.all().size()) +
                          " commands, the same ones studio-plus and the MCP server offer.");
            ImGui::SetNextItemWidth(-1);
            input_text("##filter", app.command_filter, 0, "Search commands");
            const std::string filter = lower(app.command_filter);
            ImGui::BeginChild("##command_rows", ImVec2(0, 0), 0);
            const float width = ImGui::GetContentRegionAvail().x;
            for (const auto &group : registry.groups())
            {
                bool any = false;
                for (const auto &c : registry.all())
                {
                    if (c.group != group)
                        continue;
                    if (!filter.empty() && lower(c.id()).find(filter) == std::string::npos &&
                        lower(c.summary).find(filter) == std::string::npos)
                        continue;
                    if (!any)
                    {
                        ImGui::Dummy(ImVec2(0, S(2)));
                        std::string title = group;
                        for (auto &ch : title)
                            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                        caption(title.c_str());
                        any = true;
                    }
                    ImGui::PushID(&c);
                    if (command_row(c, width, app.runner.command == &c))
                        app.runner.bind(&c);
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::EndChild();
            ImGui::SameLine(0, S(24));
            ImGui::BeginChild("##command_runner", ImVec2(0, height), 0);
            draw_runner(app.runner, app.window);
            ImGui::EndChild();
        }

        // ---------------------------------------------------------------- rail

        void rail(App &app, float width)
        {
            const auto &registry = Registry::instance();
            std::string home_count;
            bool home_accent = false;
            if (app.doctor && !app.doctor->done.load())
            {
                home_count = "...";
            }
            else if (app.doctor)
            {
                std::lock_guard lock(app.doctor->mutex);
                int missing = 0;
                const Json &outcome = app.doctor->outcome;
                if (outcome.value("ok", false))
                    for (const auto &c : outcome["result"]["checks"])
                    {
                        const auto *info = check_info(c.value("check", ""));
                        if (!c.value("ok", false) && !(info && info->optional))
                            ++missing;
                    }
                else
                    missing = 1;
                if (missing)
                {
                    home_count = std::to_string(missing);
                    home_accent = true;
                }
            }
            for (const auto &area : areas)
                if (nav_tile(width, area.nav, app.page == area.page))
                    app.page = area.page;
            ImGui::Dummy(ImVec2(0, S(10)));
            if (nav_tile(width,
                         "STATUS",
                         app.page == page::home,
                         home_count,
                         home_accent,
                         home_accent ? "Something Studio+ needs is missing" : ""))
                app.page = page::home;
            if (nav_tile(width, "SETTINGS", app.page == page::settings))
                app.page = page::settings;
            ImGui::Dummy(ImVec2(0, S(10)));
            if (nav_tile(width, "ALL COMMANDS", app.page == page::commands, std::to_string(registry.all().size())))
                app.page = page::commands;

            const float version_y = ImGui::GetWindowHeight() - g_fonts.caption->FontSize - S(4);
            if (ImGui::GetCursorPosY() < version_y)
            {
                ImGui::SetCursorPosY(version_y);
                caption("RESKATE+ NOOB-STOODIO" STUDIO_PLUS_VERSION);
            }
        }

        void poll(App &app)
        {
            for (auto &row : app.rows)
            {
                if (row.handled || !row.job || !row.job->done.load())
                    continue;
                row.handled = true;
                app.settings = Settings::load();
                if (row.job->state() == JobState::ok)
                    row.edit = saved_value(app.settings, row.key);
                run_doctor(app);
            }
        }

        void close_prompt(App &app)
        {
            if (!app.close_requested)
                return;
            if (!ImGui::IsPopupOpen("Commands still running"))
                ImGui::OpenPopup("Commands still running");
            const ImVec2 size = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(S(520), 0));
            if (ImGui::BeginPopupModal("Commands still running",
                                       nullptr,
                                       ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoResize))
            {
                heading("STILL WORKING");
                wrapped_muted(std::to_string(g_jobs.running()) +
                              " command(s) are still running. Closing cancels them.");
                if (primary_button("CANCEL AND CLOSE"))
                {
                    app.close_confirmed = true;
                    app.close_requested = false;
                    ImGui::CloseCurrentPopup();
                    PostMessageW(app.window, WM_CLOSE, 0, 0);
                }
                ImGui::SameLine();
                if (ImGui::Button("KEEP WORKING"))
                {
                    app.close_requested = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
    } // namespace

    void run_doctor(App &app)
    {
        if (app.doctor && !app.doctor->done.load())
            return;
        if (const Command *doctor = find_command("studio", "doctor"))
            app.doctor = g_jobs.start(*doctor, Json::object());
    }

    void init_app(App &app)
    {
        app.settings = Settings::load();
        app.rows = {{
            {"game-root",
             "ReSkate folder (where ReSkateLauncher.exe is)",
             "The folder with ReSkateLauncher.exe, ReSkate.dll and Skate.exe, where ReSkate loads mods from. Not the "
             "plain "
             "Steam copy: mods built there will not load. BROWSE picks the folder; PICK LAUNCHER picks "
             "ReSkateLauncher.exe "
             "and keeps its folder.",
             true},
            {"engine-dir",
             "Engine folder",
             "The folder with reskate_cli.exe and its Native folder, from the ReSkate Studio zip.",
             true},
            {"blender", "Blender", "blender.exe. Only needed to build maps from .blend and .fbx scenes.", false},
        }};
        for (auto &row : app.rows)
            row.edit = saved_value(app.settings, row.key);
        app.activity_height = S(220);
        run_doctor(app);
    }

    namespace
    {

        std::string g_page_error; // the last exception a page threw, shown until dismissed

        const char *page_name(page page)
        {
            switch (page)
            {
            case page::assets:
                return "ASSETS";
            case page::project:
                return "PROJECT & MODS";
            case page::cosmetics:
                return "COSMETICS";
            case page::maps:
                return "CUSTOM MAPS";
            case page::animations:
                return "ANIMATIONS";
            case page::home:
                return "STATUS";
            case page::settings:
                return "SETTINGS";
            case page::commands:
                return "CLI COMMANDS";
            }
            return "?";
        }

        void recover_page(const ImGuiErrorRecoveryState &before, page, const char*)
        {
            ImGuiIO &io = ImGui::GetIO();
            const bool assert_on = io.ConfigErrorRecoveryEnableAssert, tooltip_on = io.ConfigErrorRecoveryEnableTooltip;
            io.ConfigErrorRecoveryEnableAssert = io.ConfigErrorRecoveryEnableTooltip = false;
            ImGui::ErrorRecoveryTryToRecoverState(&before);
            io.ConfigErrorRecoveryEnableAssert = assert_on;
            io.ConfigErrorRecoveryEnableTooltip = tooltip_on;
            //g_page_error = std::string(page_name(page)) + ": " + what;
        }

        void page_error_banner()
        {
            if (g_page_error.empty())
                return;
            ImGui::PushStyleColor(ImGuiCol_Text, color::danger);
            ImGui::TextWrapped("A page hit an error and skipped part of a frame (%s). Please report it.",
                               g_page_error.c_str());
            ImGui::PopStyleColor();
            if (ImGui::SmallButton("DISMISS"))
                g_page_error.clear();
            ImGui::Spacing();
        }

    } // namespace

    void draw_frame(App &app)
    {
        const auto &io = ImGui::GetIO();
        const ImVec2 size = io.DisplaySize;
        const float time = static_cast<float>(ImGui::GetTime());
        g_drop_fallback = nullptr;
        g_dropped_into = nullptr;
        poll(app);

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##root",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        auto *draw = ImGui::GetWindowDrawList();
        draw_background(draw, size, time);
        window_buttons(draw, app.window, size);

        const float rail_x = S(28), rail_width = S(236);
        const float content_x = rail_x + rail_width + S(26);
        const float top = S(52);
        const float title_size = S(58);

        page_title(draw, ImVec2(rail_x - 2, top - 24), "RESKATE+", 64);
        page_title(draw, ImVec2(rail_x + 24, top + 22), "NOOB-STOODIO", 36);

        const float rail_top = top + title_size + S(22);
        ImGui::SetCursorPos(ImVec2(rail_x, rail_top));
        ImGui::BeginChild("##rail", ImVec2(rail_width, size.y - rail_top - S(18)), 0, ImGuiWindowFlags_NoScrollbar);
        rail(app, rail_width);
        ImGui::EndChild();

        const float content_width = size.x - content_x - S(28);
        const float drawer_max = (size.y - top) * 0.65f;
        const float drawer =
            app.activity_open ? std::clamp(app.activity_height, S(140), std::max(S(140), drawer_max)) : S(38);
        const float content_height = size.y - top - drawer - S(16);
        ImGui::SetCursorPos(ImVec2(content_x, top));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (app.page == page::commands)
        {
            ImGui::BeginChild("##content", ImVec2(content_width, content_height), 0, ImGuiWindowFlags_NoScrollbar);
            ImGui::PopStyleVar();
            commands_page(app, content_height);
        }
        else
        {
            ImGui::BeginChild("##content", ImVec2(content_width, content_height), 0);
            ImGui::PopStyleVar();
            page_error_banner();
            // A page that throws (e.g. a result field it did not expect) must not take the window down:
            // unwind ImGui's stacks back to here and show the error above the page instead.
            ImGuiErrorRecoveryState before;
            ImGui::ErrorRecoveryStoreState(&before);
            try
            {
                if (app.page == page::home)
                    home_page(app);
                else if (app.page == page::maps)
                    maps_page(app);
                else if (app.page == page::settings)
                    settings_page(app);
                else if (app.page == page::project)
                    project_page(app);
                else if (app.page == page::assets)
                    assets_page(app);
                else if (app.page == page::cosmetics)
                    cosmetics_page(app);
                else if (app.page == page::animations)
                    animations_page(app);
            }
            catch (const std::exception &failure)
            {
                recover_page(before, app.page, failure.what());
            }
            catch (...)
            {
                recover_page(before, app.page, "unknown error");
            }
            ImGui::Dummy(ImVec2(0, S(8)));
        }
        ImGui::EndChild();

        draw_activity(ImVec2(content_x, size.y - drawer),
                      content_width,
                      drawer_max,
                      app.activity_open,
                      app.activity_height,
                      [&app](const std::shared_ptr<Job> &job)
                      {
                          app.runner.show(job);
                          app.page = page::commands;
                      });
        ImGui::End();

        close_prompt(app);

        // A drop that missed every path field goes to the first one on the page.
        std::lock_guard lock(g_drop.mutex);
        if (g_drop.pending)
        {
            if (g_drop_fallback && !g_drop.paths.empty())
                *g_drop_fallback = wide_to_utf8(g_drop.paths.front());
            g_drop.pending = false;
            g_drop.paths.clear();
        }
    }

} // namespace studio::gui
