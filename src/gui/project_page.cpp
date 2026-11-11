// PROJECT & MODS. Every action here is a registry command run on the job system (mod list, project info,
// mod info, mod compile, mod deploy, game validate, game patch-audit, game initfs), so the CLI and the MCP
// server can do the same. Only Launch ReSkate and Open folder are the window's own: they start a program.
#include "gui/project_page.h"

#include "gui/look.h"
#include "gui/widgets.h"

#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <optional>

namespace studio::gui
{
    namespace fs = std::filesystem;
    namespace
    {

        // ---------------------------------------------------------------- small helpers

        std::string lower(std::string text)
        {
            for (auto &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        std::string upper(std::string text)
        {
            for (auto &c : text)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return text;
        }

        // A path as compared: one kind of slash, no trailing one, any case.
        std::string folded(std::string path)
        {
            for (auto &c : path)
                if (c == '/')
                    c = '\\';
            while (path.size() > 3 && path.back() == '\\')
                path.pop_back();
            return lower(path);
        }

        bool contains(const std::string &text, const std::string &needle_lower)
        {
            return needle_lower.empty() || lower(text).find(needle_lower) != std::string::npos;
        }

        std::string text_of(const Json &value)
        {
            if (value.is_null())
                return {};
            return value.is_string() ? value.get<std::string>() : value.dump();
        }

        std::string size_text(double bytes)
        {
            const char *units[]{"B", "KB", "MB", "GB"};
            int unit = 0;
            while (bytes >= 1024 && unit < 3)
            {
                bytes /= 1024;
                ++unit;
            }
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.1f %s", bytes, units[unit]);
            return buffer;
        }

        // A member of a result object, or null when it is not there (operator[] on a const Json must not miss).
        const Json &get(const Json &object, const char *key)
        {
            static const Json none;
            if (!object.is_object())
                return none;
            const auto it = object.find(key);
            return it == object.end() ? none : *it;
        }

        double number_of(const Json &object, const char *key)
        {
            const Json &value = get(object, key);
            return value.is_number() ? value.get<double>() : 0;
        }

        std::string file_name(const std::string &path)
        {
            return wide_to_utf8(fs::path(utf8_to_wide(path)).filename().native());
        }
        std::string parent_folder(const std::string &path)
        {
            return wide_to_utf8(fs::path(utf8_to_wide(path)).parent_path().native());
        }

        bool file_exists(const std::string &path)
        {
            std::error_code ec;
            return !path.empty() && fs::exists(fs::path(utf8_to_wide(path)), ec);
        }

        const Command *command(const char *group, const char *name)
        {
            return Registry::instance().find(group, name);
        }

        void heading(const char *text)
        {
            ImGui::PushFont(g_fonts.tile);
            ImGui::TextUnformatted(text);
            ImGui::PopFont();
        }

        void tile_heading(const char *text)
        {
            ImGui::PushFont(g_fonts.heading);
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

        void coloured(ImU32 colour, const std::string &text)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        // A pill in the text flow, on the line's baseline.
        void inline_badge(const std::string &text, ImU32 fill, ImU32 ink = color::ink)
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float height = g_fonts.caption->FontSize + S(8);
            const float y = at.y + std::max(0.0f, (ImGui::GetFrameHeight() - height) * 0.5f);
            badge(ImGui::GetWindowDrawList(), ImVec2(at.x, y), text, fill, ink);
            ImGui::Dummy(ImVec2(badge_width(text), std::max(height, ImGui::GetFrameHeight())));
        }

        void open_in_explorer(const std::string &path)
        {
            ShellExecuteW(nullptr, L"open", utf8_to_wide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }

        const std::vector<FileFilter> project_filter{{L"ReSkate projects (*.fbproject)", L"*.fbproject"}};
        const std::vector<FileFilter> fbmod_filter{{L"Frosty mods (*.fbmod)", L"*.fbmod"}};
        const std::vector<FileFilter> build_filter{
            {L"Mods and projects (*.fbmod, *.fbproject)", L"*.fbmod;*.fbproject"}};

        // ---------------------------------------------------------------- a command's last run on this page

        struct Run
        {
            std::shared_ptr<Job> job;
            Json result;       // the last run that worked
            bool taken = true; // the finished run's result has been taken in
            bool ok = false;   // whether that run worked

            void start(const Command &c, Json args)
            {
                job = g_jobs.start(c, std::move(args));
                taken = false;
                ok = false;
            }
            bool busy() const
            {
                return job && !job->done.load();
            }
            // Takes the result in once the run finishes. True on the frame it does, whether it worked or not.
            bool poll()
            {
                if (taken || !job || !job->done.load())
                    return false;
                taken = true;
                std::lock_guard lock(job->mutex);
                ok = job->outcome.value("ok", false);
                if (ok)
                    result = job->outcome["result"];
                return true;
            }
            // True once poll() has taken in a run that worked, so `result` is that run's (a job can finish between
            // poll() and the drawing later in the same frame).
            bool worked() const
            {
                return job && taken && ok;
            }
        };

        // Progress and Cancel while it runs, the error when it failed, and the whole result folded away when it worked.
        void run_status(Run &run)
        {
            if (!run.job)
                return;
            ImGui::PushID(run.job->id);
            if (!run.worked())
            {
                draw_job(*run.job, true);
            }
            else if (ImGui::CollapsingHeader(
                         ("Result JSON and command line, done in " + format_seconds(run.job->seconds()) + "###full")
                             .c_str()))
            {
                draw_job(*run.job, true);
            }
            ImGui::PopID();
        }

        // The command line a button runs, with COPY, as the runner shows it.
        void command_line(const Command &c, const Json &args)
        {
            const std::string line = cli_line(c, args);
            ImGui::PushID(c.id().c_str());
            ImGui::PushFont(g_fonts.caption);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("COMMAND LINE");
            ImGui::PopFont();
            ImGui::SameLine();
            copy_button("COPY##line", line);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopID();
        }

        // "decoded_assets" -> "DECODED ASSETS".
        std::string key_label(const std::string &key)
        {
            std::string out = upper(key);
            for (auto &c : out)
                if (c == '_')
                    c = ' ';
            return out;
        }

        // The plain values of a result object as captioned cells, four to a row.
        void value_grid(const char *id, const Json &object, std::initializer_list<const char *> skip = {})
        {
            if (!object.is_object())
                return;
            if (!ImGui::BeginTable(id, 4, ImGuiTableFlags_SizingStretchSame))
                return;
            for (const auto &[key, value] : object.items())
            {
                if (value.is_structured())
                    continue;
                if (std::any_of(skip.begin(), skip.end(), [&](const char *s) { return key == s; }))
                    continue;
                std::string shown = value.is_null()      ? "none"
                                    : value.is_boolean() ? (value.get<bool>() ? "yes" : "no")
                                                         : text_of(value);
                if (value.is_number() && key.find("bytes") != std::string::npos)
                    shown += "  (" + size_text(value.get<double>()) + ")";
                if (value.is_number() && key == "seconds")
                    shown = format_seconds(value.get<double>());
                ImGui::TableNextColumn();
                field(key_label(key).c_str(), shown);
            }
            ImGui::EndTable();
        }

        // A row of tabs for the page's sections, cut like the rail's tiles.
        bool section_tab(const char *label, bool selected, const std::string &count, float width)
        {
            auto *draw = ImGui::GetWindowDrawList();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 size(width, S(42));
            bool hovered{};
            const bool pressed = tile_hit(label, at, size, true, hovered);
            const ImVec2 end(at.x + size.x, at.y + size.y);
            rough_rect(draw,
                       at,
                       end,
                       selected  ? color::blue
                       : hovered ? color::tile_grey
                                 : color::tile,
                       seed_of(label));
            draw->AddText(g_fonts.bold,
                          g_fonts.bold->FontSize,
                          ImVec2(at.x + S(16), at.y + (size.y - g_fonts.bold->FontSize) * 0.5f),
                          selected ? color::ink : color::text,
                          label);
            if (!count.empty())
            {
                const float height = g_fonts.caption->FontSize + S(8);
                badge(draw,
                      ImVec2(end.x - S(12) - badge_width(count), at.y + (size.y - height) * 0.5f),
                      count,
                      selected ? rgba(0, 0, 0, 0.32f) : rgba(255, 255, 255, 0.14f),
                      color::text);
            }
            ImGui::SetCursorScreenPos(ImVec2(end.x + S(10), at.y));
            return pressed;
        }

        // The same rules `mod deploy` checks, so a bad name shows before the button is pressed.
        std::string mod_folder_problem(const std::string &name)
        {
            if (name.empty())
                return "Type a folder name.";
            if (name.size() > 64)
                return "At most 64 characters.";
            if (name[0] == '.')
                return "It must not start with '.'.";
            for (char c : name)
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-' || c == '.'))
                    return "Only letters, digits, spaces, '_', '-' and '.'.";
            if (name.back() == ' ' || name.back() == '.')
                return "It must not end with a space or '.'.";
            std::string base = lower(name.substr(0, name.find('.')));
            while (!base.empty() && base.back() == ' ')
                base.pop_back();
            const bool device = base == "con" || base == "prn" || base == "aux" || base == "nul" ||
                                (base.size() == 4 && (base.rfind("com", 0) == 0 || base.rfind("lpt", 0) == 0) &&
                                 std::isdigit(static_cast<unsigned char>(base[3])));
            if (device)
                return "That is a reserved Windows name.";
            return {};
        }

        // ---------------------------------------------------------------- what the page remembers

        enum class Section
        {
            mods,
            project,
            build,
            health
        };

        // What the launcher's pill would say: OUTDATED wins over NOT LOADED, as in `mod list`.
        enum class ModState
        {
            loads,
            disabled,
            outdated,
            excluded,
            not_loaded
        };

        // One installed mod, read from a `mod list` entry. `mod list` gives them in load order, highest priority first.
        struct ModRow
        {
            Json mod;
            std::string name, title, author, version, levels;
            bool enabled = true;
            ModState state = ModState::loads;
        };

        struct State
        {
            bool loaded = false;
            Section section = Section::mods;
            std::string folder;       // the ReSkate folder: the saved Skate folder unless changed here
            std::string saved_folder; // the saved Skate folder `folder` was last synced with
            std::string launcher_for; // the folder `has_launcher` was checked for
            bool has_launcher = false;

            // Installed mods
            Run mod_list;
            bool check_hash = false;
            std::string mod_filter;
            std::vector<ModRow> mods;
            std::string listed_folder; // the folder the shown list was read from
            std::string selected_mod;

            // Project
            Run project;
            std::string project_path;
            std::vector<std::string> recent;
            std::string resource_filter;
            int kind_filter = 0;
            bool new_only = false;
            std::vector<int> resource_order; // indices into the resources, in the table's sort order
            bool resort = true;
            Run exported; // project export of the open project
            Run fbmod_info;
            std::string fbmod_path;
            bool fbmod_verbose = false;

            // Build and install
            std::vector<std::string> fbmods;
            std::string staging;
            std::string mod_folder = "ReSkateStudio_Mods";
            Run compile;
            Run deploy;
            // Taking a mod out and putting a backup back (mod uninstall / mod restore), behind a confirmation.
            Run change;
            bool confirm_change = false;
            const Command *change_command = nullptr;
            Json change_args;
            std::string change_title, change_what;

            bool confirm = false; // open the install confirmation this frame
            Json confirm_args;    // what INSTALL runs, fixed when the confirmation opened
            std::string confirm_target;

            // Health
            std::string validate_count = "100";
            Run validate, audit, initfs;

            Json written; // what project-page.json holds
        };
        State g;
        std::string g_unclaimed_drop; // a drop no field on this page wants lands here, not in the first path field

        fs::path state_file()
        {
            return Settings::data_dir() / L"project-page.json";
        }
        fs::path recent_file()
        {
            return Settings::data_dir() / L"recent.json";
        }

        std::optional<Json> read_json(const fs::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return std::nullopt;
            try
            {
                return Json::parse(in);
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        void write_json(const fs::path &path, const Json &value)
        {
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << value.dump(2) << "\n";
        }

        const char *section_key(Section s)
        {
            switch (s)
            {
            case Section::project:
                return "project";
            case Section::build:
                return "build";
            case Section::health:
                return "health";
            default:
                return "mods";
            }
        }

        Json snapshot()
        {
            return {{"section", section_key(g.section)},
                    {"project", g.project_path},
                    {"fbmod", g.fbmod_path},
                    {"fbmods", g.fbmods},
                    {"staging-dir", g.staging},
                    {"mod-folder", g.mod_folder},
                    {"check-hash", g.check_hash}};
        }

        void save_recent()
        {
            write_json(recent_file(), Json{{"projects", g.recent}});
        }

        void remember_project(const std::string &path)
        {
            const std::string key = folded(path);
            g.recent.erase(std::remove_if(g.recent.begin(),
                                          g.recent.end(),
                                          [&](const std::string &p) { return folded(p) == key; }),
                           g.recent.end());
            g.recent.insert(g.recent.begin(), path);
            if (g.recent.size() > 12)
                g.recent.resize(12);
            save_recent();
        }

        // Arguments for a command that reads the game: --game-root only when the folder is not the saved one.
        Json with_game(Json args)
        {
            if (!g.folder.empty() && folded(g.folder) != folded(g.saved_folder))
                args["game-root"] = g.folder;
            return args;
        }

        std::string folder_in_use()
        {
            return g.folder.empty() ? g.saved_folder : g.folder;
        }

        // ---------------------------------------------------------------- actions, each one a command

        void refresh_mods()
        {
            if (const Command *c = command("mod", "list"); c && !g.mod_list.busy())
            {
                Json args = Json::object();
                if (g.check_hash)
                    args["check-hash"] = true;
                g.mod_list.start(*c, with_game(args));
                g.listed_folder = folder_in_use();
            }
        }

        void open_project(const std::string &path, bool show = true)
        {
            if (path.empty())
                return;
            g.project_path = path;
            if (show)
                g.section = Section::project;
            if (const Command *c = command("project", "info"))
                g.project.start(*c, Json{{"file", path}});
        }

        void inspect_fbmod(const std::string &path)
        {
            if (path.empty())
                return;
            g.fbmod_path = path;
            Json args = {{"file", path}};
            if (g.fbmod_verbose)
                args["verbose"] = true;
            if (const Command *c = command("mod", "info"))
                g.fbmod_info.start(*c, args);
        }

        bool in_build(const std::string &path)
        {
            const std::string key = folded(path);
            return std::any_of(
                g.fbmods.begin(), g.fbmods.end(), [&](const std::string &p) { return folded(p) == key; });
        }

        // Adds an .fbmod or .fbproject to the build list (mod compile exports a project itself).
        void add_fbmod(const std::string &path)
        {
            if (!in_build(path))
                g.fbmods.push_back(path);
        }

        bool is_project(const std::string &path)
        {
            return lower(wide_to_utf8(fs::path(utf8_to_wide(path)).extension().native())) == ".fbproject";
        }

        // ---------------------------------------------------------------- reading `mod list`

        // "levels/game/dingolevel_x/dingolevel_x" -> "dingolevel_x".
        std::string level_name(const std::string &asset)
        {
            const auto slash = asset.find_last_of("/\\");
            return slash == std::string::npos ? asset : asset.substr(slash + 1);
        }

        void build_rows(const Json &result)
        {
            g.mods.clear();
            if (!result.contains("mods") || !result["mods"].is_array())
                return;
            for (const auto &m : result["mods"])
            {
                ModRow row;
                row.mod = m;
                row.name = m.value("name", "");
                row.title = text_of(get(m, "title"));
                row.author = text_of(get(m, "author"));
                row.version = text_of(get(m, "version"));
                row.enabled = m.value("enabled", true);
                if (get(m, "levels").is_array())
                    for (const auto &level : m["levels"])
                        row.levels += (row.levels.empty() ? "" : ", ") + level_name(text_of(level));
                row.state = m.value("loads", false)         ? ModState::loads
                            : !get(m, "outdated").is_null() ? ModState::outdated
                            : !row.enabled                  ? ModState::disabled
                            : !get(m, "excluded").is_null() ? ModState::excluded
                                                            : ModState::not_loaded;
                g.mods.push_back(std::move(row));
            }
        }

        const char *state_label(ModState s)
        {
            switch (s)
            {
            case ModState::loads:
                return "LOADS";
            case ModState::disabled:
                return "DISABLED";
            case ModState::outdated:
                return "OUTDATED";
            case ModState::excluded:
                return "EXCLUDED";
            default:
                return "NOT LOADED";
            }
        }

        ImU32 state_fill(ModState s)
        {
            switch (s)
            {
            case ModState::loads:
                return color::good;
            case ModState::disabled:
                return rgba(255, 255, 255, 0.14f);
            case ModState::excluded:
                return color::warning;
            default:
                return color::danger;
            }
        }

        ImU32 state_ink(ModState s)
        {
            return s == ModState::disabled ? color::text : color::ink;
        }

        // ---------------------------------------------------------------- the ReSkate folder and Launch

        void folder_tile(App &app)
        {
            begin_tile("##reskate_folder", 401);
            tile_heading("RESKATE FOLDER");
            const bool custom = !g.folder.empty() && folded(g.folder) != folded(g.saved_folder);
            wrapped_muted(
                custom
                    ? "Changed on this page only: commands here get --game-root. Settings still has " +
                          (g.saved_folder.empty() ? std::string("no Skate folder") : g.saved_folder) + "."
                    : "The folder with Skate.exe, ReSkateLauncher.exe and Mods. It is the Skate folder from Settings.");
            path_field("##folder", g.folder, app.window, true, {}, S(110));

            // Launch ReSkate: starts the launcher the folder holds. Not a command, it opens a program.
            const std::string folder = folder_in_use();
            if (g.launcher_for != folded(folder))
            {
                g.launcher_for = folded(folder);
                std::error_code ec;
                g.has_launcher =
                    !folder.empty() && fs::is_regular_file(fs::path(utf8_to_wide(folder)) / L"ReSkateLauncher.exe", ec);
            }
            ImGui::BeginDisabled(!g.has_launcher);
            if (primary_button("LAUNCH RESKATE", ImVec2(S(190), 0)))
            {
                const auto root = fs::path(utf8_to_wide(folder));
                ShellExecuteW(
                    app.window, L"open", (root / L"ReSkateLauncher.exe").c_str(), nullptr, root.c_str(), SW_SHOWNORMAL);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(g.has_launcher
                                      ? "Start ReSkateLauncher.exe from this folder, to test your mods in the game"
                                      : "No ReSkateLauncher.exe in this folder");
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            if (g.has_launcher)
                ImGui::TextDisabled("Starts %s\\ReSkateLauncher.exe", folder.c_str());
            else
                ImGui::TextDisabled("No ReSkateLauncher.exe in this folder");
            if (custom)
            {
                ImGui::SameLine(0, S(16));
                if (ImGui::Button("USE SAVED", ImVec2(S(120), 0)))
                    g.folder = g.saved_folder;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Back to the Skate folder from Settings");
            }
            end_tile();
        }

        // ---------------------------------------------------------------- installed mods

        std::string or_none(const std::string &text, const char *none = "not recorded")
        {
            return text.empty() ? none : text;
        }

        std::string joined(const Json &list, const char *separator = ", ")
        {
            std::string out;
            if (list.is_array())
                for (const auto &item : list)
                    out += (out.empty() ? "" : separator) + text_of(item);
            return out;
        }

        void ask_change(const char *group, const char *name, Json args, std::string title, std::string what)
        {
            g.change_command = command(group, name);
            if (!g.change_command)
                return;
            // The mods shown were read from listed_folder: act on that folder, whatever the folder field says now.
            args["game-root"] = g.listed_folder;
            g.change_args = std::move(args);
            g.change_title = std::move(title);
            g.change_what = std::move(what);
            g.confirm_change = true;
        }

        void mod_details(const ModRow &row)
        {
            const Json &m = row.mod;
            begin_tile("##mod_details", seed_of(row.name.c_str()));
            {
                // The status reads first: a coloured edge and the pill, as on the STATUS tile.
                auto *draw = ImGui::GetWindowDrawList();
                const ImVec2 tile_pos = ImGui::GetWindowPos();
                draw->AddRectFilled(
                    tile_pos, ImVec2(tile_pos.x + S(4), tile_pos.y + ImGui::GetWindowSize().y), state_fill(row.state));
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float width = ImGui::GetContentRegionAvail().x;
                const char *pill = state_label(row.state);
                badge(draw,
                      ImVec2(at.x + width - badge_width(pill),
                             at.y + (g_fonts.heading->FontSize - g_fonts.caption->FontSize - S(8)) * 0.5f),
                      pill,
                      state_fill(row.state),
                      state_ink(row.state));
            }
            tile_heading(row.title.c_str());
            if (row.title != row.name)
                wrapped_muted("Folder " + row.name);
            coloured(row.state == ModState::loads      ? color::good
                     : row.state == ModState::disabled ? color::muted
                     : row.state == ModState::excluded ? color::warning
                                                       : color::danger,
                     "Status: " + m.value("status", ""));
            if (!get(m, "description").is_null())
            {
                ImGui::Spacing();
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(text_of(m["description"]).c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::Spacing();
            const Json &sha = get(m, "skate_sha256");
            const bool studio = m.value("studio_marker", false);
            if (ImGui::BeginTable("##facts", 3, ImGuiTableFlags_SizingStretchSame))
            {
                ImGui::TableNextColumn();
                field("AUTHOR", or_none(row.author));
                field("VERSION", or_none(row.version));
                field("ENABLED", row.enabled ? "yes" : "no");
                ImGui::TableNextColumn();
                field("TOOL", or_none(text_of(get(m, "tool"))));
                field("BUILT", or_none(text_of(get(m, "built"))));
                field("SHIPS",
                      std::string(m.value("provides_layout", false) ? "game data (layout.toc)" : "no game data") +
                          (m.value("provides_levels", false) ? ", reskate-levels.json" : ""));
                ImGui::TableNextColumn();
                field("STUDIO MARKER", studio ? "yes (.reskate-studio-patch)" : "no");
                field("MADE FOR SKATE.EXE",
                      sha.is_string() ? sha.get<std::string>().substr(0, 16) + "..."
                      : studio        ? "not recorded"
                                      : "-");
                field("THIS GAME",
                      m.value("built_for_this_game", false) ? "yes, the build ReSkate supports"
                      : sha.is_string()                     ? "no, another game version"
                                                            : "unknown");
                ImGui::EndTable();
            }
            if (!get(m, "outdated").is_null())
                coloured(color::danger, "Outdated: " + text_of(m["outdated"]) + ".");
            if (const Json &excluded = get(m, "excluded"); !excluded.is_null())
            {
                coloured(color::warning,
                         "Excluded: the game could not merge it cleanly last time and leaves it out until its files "
                         "change.");
                for (const auto &problem : excluded)
                {
                    ImGui::Bullet();
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextUnformatted(text_of(problem).c_str());
                    ImGui::PopTextWrapPos();
                }
                if (m.value("excluded_retried_next_launch", false))
                    wrapped_muted("ReSkate.dll changed since, so the game tries it again next launch.");
            }
            if (const Json &levels = get(m, "levels"); levels.is_array() && !levels.empty())
            {
                caption("LEVELS");
                for (const auto &level : levels)
                    ImGui::TextUnformatted(text_of(level).c_str());
            }
            else if (m.value("provides_levels", false))
            {
                field("LEVELS", "reskate-levels.json registers no level");
            }
            if (const Json &parks = get(m, "park_maps"); parks.is_array() && !parks.empty())
                field("PARK MAPS", joined(parks));
            field("PATH", m.value("path", ""));
            if (ImGui::Button("OPEN FOLDER"))
                open_in_explorer(m.value("path", ""));
            if (studio)
            {
                ImGui::SameLine();
                ImGui::BeginDisabled(g.change.busy());
                if (ImGui::Button("TAKE IT OUT..."))
                    ask_change("mod",
                               "uninstall",
                               Json{{"mod-folder", row.name}},
                               "TAKE THIS MOD OUT?",
                               "Moves " + m.value("path", "") +
                                   " into .ReSkateStudio-Mod-backup in the Skate folder, so the game "
                                   "stops loading it. Nothing is deleted: RESTORE under MODS FOLDER puts it back.");
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("mod uninstall: kept as a backup you can restore");
            }
            end_tile();
        }

        void mods_folder_tile(const Json &result)
        {
            begin_tile("##mods_folder", 405);
            tile_heading("MODS FOLDER");
            field("FOLDER", result.value("mods_dir", ""));
            const Json &mods_json = get(result, "mods_json");
            field("MODS.JSON", mods_json.value("path", "") + "   (" + mods_json.value("state", "") + ")");
            if (get(result, "game_sha256").is_string())
            {
                const bool supported = result.value("game_is_supported_build", false);
                field("SKATE.EXE SHA-256", text_of(result["game_sha256"]));
                coloured(supported ? color::good : color::danger,
                         supported ? "This Skate.exe is the build ReSkate supports."
                                   : "This Skate.exe is not the build ReSkate supports.");
            }
            const Json &last = get(result, "last_deploy");
            if (last.is_object())
            {
                caption("LAST INSTALL FROM RESKATE STUDIO");
                value_grid("##last", last, {"path", "version"});
            }
            if (result.contains("backups") && !result["backups"].empty())
            {
                caption("KEPT VERSIONS: WHAT AN INSTALL REPLACED, AND MODS TAKEN OUT");
                for (const auto &b : result["backups"])
                {
                    const std::string name = b.value("name", "");
                    ImGui::PushID(name.c_str());
                    ImGui::BeginDisabled(g.change.busy());
                    if (ImGui::SmallButton("RESTORE"))
                        ask_change(
                            "mod",
                            "restore",
                            Json{{"backup", name}},
                            "PUT THIS VERSION BACK?",
                            "Moves " + b.value("path", "") +
                                " back into the Mods folder. If that mod is installed now, the "
                                "installed version is kept as a backup instead, so this can be undone the same way.");
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextUnformatted(name.c_str());
                    ImGui::PopID();
                }
            }
            if (ImGui::Button("OPEN MODS FOLDER"))
                open_in_explorer(result.value("mods_dir", ""));
            run_status(g.change);
            if (g.change.worked() && g.change.result.is_object())
            {
                const Json &r = g.change.result;
                if (r.contains("backup"))
                    coloured(color::good, "Taken out. Kept as \"" + r.value("backup", "") + "\".");
                else
                    coloured(color::good,
                             "Restored as " + r.value("mod_path", "") +
                                 (r.contains("previous_kept_as") ? "; the version it replaced is kept as \"" +
                                                                       r.value("previous_kept_as", "") + "\""
                                                                 : "") +
                                 ".");
            }
            end_tile();
        }

        // The confirmation for mod uninstall / mod restore: what moves where, and the command line.
        void change_confirmation()
        {
            if (g.confirm_change)
            {
                ImGui::OpenPopup("##confirm_change");
                g.confirm_change = false;
            }
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(S(640), 0));
            if (!ImGui::BeginPopupModal("##confirm_change",
                                        nullptr,
                                        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoResize))
                return;
            heading(g.change_title.c_str());
            wrapped_muted("This changes the game folder:");
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(g.change_what.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Bullet();
            ImGui::TextWrapped("Skate must be closed.");
            ImGui::Spacing();
            if (g.change_command)
                command_line(*g.change_command, g.change_args);
            ImGui::Spacing();
            if (primary_button("GO AHEAD", ImVec2(S(150), S(36))) && g.change_command)
            {
                g.change.start(*g.change_command, g.change_args);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("CANCEL", ImVec2(S(130), S(36))) || ImGui::IsKeyPressed(ImGuiKey_Escape))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        void mods_section()
        {
            const Command *list = command("mod", "list");
            if (!list)
            {
                wrapped_muted("mod list is not in the registry.");
                return;
            }
            if (!g.mod_list.job && !folder_in_use().empty())
                refresh_mods();

            // No ReSkate.dll beside Skate.exe: nothing in this Mods folder loads, whatever the list says.
            if (const Json &shown = g.mod_list.result;
                shown.is_object() && shown.contains("reskate_installed") && !shown.value("reskate_installed", true))
            {
                begin_tile("##no_reskate", 406);
                auto *draw = ImGui::GetWindowDrawList();
                const ImVec2 tile_pos = ImGui::GetWindowPos();
                draw->AddRectFilled(
                    tile_pos, ImVec2(tile_pos.x + S(4), tile_pos.y + ImGui::GetWindowSize().y), color::danger);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const char *pill = "NO MOD LOADS";
                badge(draw,
                      ImVec2(at.x + ImGui::GetContentRegionAvail().x - badge_width(pill),
                             at.y + (g_fonts.heading->FontSize - g_fonts.caption->FontSize - S(8)) * 0.5f),
                      pill,
                      color::danger,
                      color::ink);
                tile_heading("RESKATE IS NOT INSTALLED HERE");
                coloured(
                    color::danger,
                    shown.value("warning", "No ReSkate.dll beside Skate.exe in " + shown.value("game_root", "") + "."));
                wrapped_muted("Set the ReSkate folder above to the folder that holds ReSkateLauncher.exe.");
                end_tile();
                ImGui::Spacing();
            }

            begin_tile("##installed", 402);
            tile_heading("INSTALLED MODS");
            wrapped_muted("Read only. Turning mods on and off and their order belong to the ReSkate launcher.");
            ImGui::BeginDisabled(g.mod_list.busy());
            if (primary_button("REFRESH", ImVec2(S(130), 0)))
                refresh_mods();
            ImGui::EndDisabled();
            ImGui::SameLine(0, S(18));
            toggle("##check_hash", &g.check_hash);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Check Skate.exe");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "--check-hash: hash Skate.exe (about 145 MB) to tell whether it is the build ReSkate supports");
            ImGui::SameLine(0, S(18));
            ImGui::SetNextItemWidth(-1);
            input_text("##mod_filter", g.mod_filter, 0, "Filter by name, title, author or level");
            Json args = Json::object();
            if (g.check_hash)
                args["check-hash"] = true;
            command_line(*list, with_game(args));
            run_status(g.mod_list);

            const Json &result = g.mod_list.result;
            if (result.contains("mods"))
            {
                if (folded(g.listed_folder) != folded(folder_in_use()))
                    coloured(color::warning,
                             "This list is from " + g.listed_folder + ". REFRESH to read the folder above.");
                const int count = static_cast<int>(g.mods.size());
                ImGui::Spacing();
                if (!result.value("present", false))
                {
                    wrapped_muted("There is no Mods folder in " + result.value("game_root", "") + " yet (" +
                                  result.value("mods_dir", "") + ").");
                }
                else
                {
                    const Json &counts = get(result, "counts");
                    const auto n = [&](const char *key) { return text_of(get(counts, key)); };
                    ImGui::PushFont(g_fonts.bold);
                    ImGui::TextUnformatted((std::to_string(count) + (count == 1 ? " mod in " : " mods in ") +
                                            result.value("mods_dir", "") + ", highest priority first")
                                               .c_str());
                    ImGui::PopFont();
                    wrapped_muted(n("loads") + " load, " + n("enabled") + " enabled, " + n("disabled") + " disabled, " +
                                  n("outdated") + " outdated, " + n("excluded") + " excluded.");
                }
                const Json &mods_json = get(result, "mods_json");
                if (!get(mods_json, "issue").is_null())
                    coloured(color::danger, "mods.json: " + text_of(mods_json["issue"]));
                const std::string meaning =
                    (mods_json.value("state", "") == "exists" ? "mods.json: " : "") + mods_json.value("meaning", "");
                if (!meaning.empty())
                    coloured(mods_json.value("state", "") == "malformed" ? color::danger : color::muted, meaning);
                if (const Json &missing = get(result, "missing"); missing.is_array() && !missing.empty())
                    coloured(color::warning,
                             "mods.json lists " + joined(missing) +
                                 (missing.size() == 1 ? ", which is" : ", which are") + " not in the Mods folder.");
                if (const Json &notes = get(result, "notes"); notes.is_array())
                    for (const auto &note : notes)
                        coloured(color::warning, text_of(note));
                const std::string filter = lower(g.mod_filter);
                std::vector<std::pair<int, const ModRow *>> shown; // load-order position, row
                for (size_t i = 0; i < g.mods.size(); ++i)
                {
                    const ModRow &row = g.mods[i];
                    if (contains(row.name, filter) || contains(row.title, filter) || contains(row.author, filter) ||
                        contains(row.levels, filter))
                        shown.emplace_back(static_cast<int>(i) + 1, &row);
                }
                ImGui::Spacing();
                const float row_height = S(46);
                // Rows are row_height plus the cell padding; the header is one text line.
                const float row_pitch = row_height + ImGui::GetStyle().CellPadding.y * 2;
                const float height =
                    std::min(row_pitch * 12,
                             row_pitch * static_cast<float>(shown.size()) + ImGui::GetTextLineHeight() +
                                 ImGui::GetStyle().CellPadding.y * 2 + S(4));
                const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                                              ImGuiTableFlags_PadOuterX;
                if (!shown.empty() && ImGui::BeginTable("##mods", 7, flags, ImVec2(0, height)))
                {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, S(28));
                    ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthFixed, S(140));
                    ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, S(64));
                    ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, S(64));
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, S(104));
                    ImGui::TableSetupColumn("Levels", ImGuiTableColumnFlags_WidthFixed, S(190));
                    ImGui::TableHeadersRow();
                    for (const auto &[position, row] : shown)
                    {
                        ImGui::PushID(row->name.c_str());
                        ImGui::TableNextRow(0, row_height);
                        ImGui::TableNextColumn();
                        const float top = ImGui::GetCursorPosY();
                        const bool selected = g.selected_mod == row->name;
                        // Ticked like a list row: a blue-grey fill and a blue edge.
                        if (selected)
                        {
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, rgba(28, 38, 52));
                            const ImVec2 cell = ImGui::GetCursorScreenPos();
                            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cell.x - S(8), cell.y - S(4)),
                                                                      ImVec2(cell.x - S(5), cell.y + row_height - S(4)),
                                                                      color::blue);
                        }
                        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                              ImGui::ColorConvertU32ToFloat4(rgba(255, 255, 255, 0.055f)));
                        ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                              ImGui::ColorConvertU32ToFloat4(rgba(255, 255, 255, 0.09f)));
                        if (ImGui::Selectable("##row",
                                              selected,
                                              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                              ImVec2(0, row_height - S(4))))
                            g.selected_mod = selected ? std::string() : row->name;
                        ImGui::PopStyleColor(3);
                        // Single-line cells sit in the middle of the two-line row.
                        const auto middle = [&]
                        { ImGui::SetCursorPosY(top + (row_height - S(4) - ImGui::GetFrameHeight()) * 0.5f); };
                        ImGui::SameLine(0, 0);
                        middle();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%d", position);
                        ImGui::TableNextColumn();
                        {
                            // The title, and the folder name under it when it differs.
                            const ImVec2 at = ImGui::GetCursorScreenPos();
                            const float width = ImGui::GetContentRegionAvail().x;
                            auto *draw = ImGui::GetWindowDrawList();
                            const ImVec4 clip(at.x, at.y, at.x + width, at.y + row_height);
                            const bool two = row->title != row->name;
                            draw->AddText(g_fonts.bold,
                                          g_fonts.bold->FontSize,
                                          ImVec2(at.x, at.y + (two ? S(3) : S(11))),
                                          color::text,
                                          row->title.c_str(),
                                          nullptr,
                                          0,
                                          &clip);
                            if (two)
                                draw->AddText(g_fonts.caption,
                                              g_fonts.caption->FontSize,
                                              ImVec2(at.x, at.y + S(22)),
                                              color::muted,
                                              row->name.c_str(),
                                              nullptr,
                                              0,
                                              &clip);
                            ImGui::Dummy(ImVec2(width, row_height - S(6)));
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", row->mod.value("path", "").c_str());
                        }
                        const auto plain = [&](const std::string &text)
                        {
                            ImGui::TableNextColumn();
                            middle();
                            ImGui::AlignTextToFramePadding();
                            if (text.empty())
                                ImGui::TextDisabled("-");
                            else
                                ImGui::TextUnformatted(text.c_str());
                        };
                        plain(row->author);
                        plain(row->version);
                        plain(row->enabled ? "yes" : "no");
                        ImGui::TableNextColumn();
                        middle();
                        inline_badge(state_label(row->state), state_fill(row->state), state_ink(row->state));
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", row->mod.value("status", "").c_str());
                        plain(row->levels);
                        if (!row->levels.empty() && ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", joined(get(row->mod, "levels"), "\n").c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                else if (count)
                {
                    wrapped_muted("No mod matches the filter.");
                }
                if (g.selected_mod.empty())
                    wrapped_muted("Pick a mod for its details.");
            }
            end_tile();

            for (const auto &row : g.mods)
                if (row.name == g.selected_mod)
                {
                    ImGui::Spacing();
                    mod_details(row);
                }
            if (result.contains("mods"))
            {
                ImGui::Spacing();
                mods_folder_tile(result);
            }
            change_confirmation();
        }

        // ---------------------------------------------------------------- project and .fbmod files

        void recent_list()
        {
            if (g.recent.empty())
            {
                wrapped_muted("Projects you open show up here.");
                return;
            }
            const float width = ImGui::GetContentRegionAvail().x;
            std::string forget;
            for (const auto &path : g.recent)
            {
                ImGui::PushID(path.c_str());
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float height = S(46);
                const bool there = file_exists(path);
                const bool current = folded(path) == folded(g.project_path);
                if (list_row("##recent", width, height, current) && there)
                    open_project(path);
                auto *draw = ImGui::GetWindowDrawList();
                const ImVec4 clip(at.x, at.y, at.x + width - S(110), at.y + height);
                draw->AddText(g_fonts.bold,
                              g_fonts.bold->FontSize,
                              ImVec2(at.x + S(14), at.y + S(5)),
                              there ? color::text : color::muted,
                              file_name(path).c_str(),
                              nullptr,
                              0,
                              &clip);
                draw->AddText(g_fonts.caption,
                              g_fonts.caption->FontSize,
                              ImVec2(at.x + S(14), at.y + S(25)),
                              color::muted,
                              (there ? parent_folder(path) : "Missing: " + path).c_str(),
                              nullptr,
                              0,
                              &clip);
                const ImVec2 after = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(
                    ImVec2(at.x + width - S(92), at.y + (height - ImGui::GetFrameHeight()) * 0.5f));
                if (ImGui::SmallButton("FORGET"))
                    forget = path;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Take it off this list. The file stays.");
                ImGui::SetCursorScreenPos(after);
                ImGui::PopID();
            }
            if (!forget.empty())
            {
                g.recent.erase(std::remove(g.recent.begin(), g.recent.end(), forget), g.recent.end());
                save_recent();
            }
        }

        void resources_table(const Json &resources)
        {
            static const char *kinds[]{"All kinds", "ebx", "res", "chunk"};
            ImGui::SetNextItemWidth(S(150));
            if (ImGui::BeginCombo("##kind", kinds[g.kind_filter]))
            {
                for (int i = 0; i < 4; ++i)
                    if (ImGui::Selectable(kinds[i], g.kind_filter == i))
                        g.kind_filter = i;
                ImGui::EndCombo();
            }
            ImGui::SameLine(0, S(14));
            toggle("##new_only", &g.new_only);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("New only");
            ImGui::SameLine(0, S(14));
            ImGui::SetNextItemWidth(-1);
            input_text("##resource_filter", g.resource_filter, 0, "Filter by name or GUID");

            const std::string filter = lower(g.resource_filter);
            std::vector<int> shown;
            for (int i : g.resource_order)
            {
                const Json &r = resources[static_cast<size_t>(i)];
                if (g.kind_filter && r.value("kind", "") != kinds[g.kind_filter])
                    continue;
                if (g.new_only && !r.value("added", false))
                    continue;
                if (!contains(r.value("name", ""), filter))
                    continue;
                shown.push_back(i);
            }
            const float row_height = ImGui::GetTextLineHeightWithSpacing() + S(6);
            const float height = std::min(S(420), row_height * static_cast<float>(shown.size() + 1) + S(10));
            const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                                          ImGuiTableFlags_Sortable | ImGuiTableFlags_PadOuterX;
            if (shown.empty())
            {
                wrapped_muted("No resource matches.");
                return;
            }
            if (!ImGui::BeginTable("##resources", 4, flags, ImVec2(0, height)))
                return;
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, S(70));
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
            ImGui::TableSetupColumn(
                "Size", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, S(100));
            ImGui::TableSetupColumn("New", ImGuiTableColumnFlags_WidthFixed, S(60));
            ImGui::TableHeadersRow();
            if (ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs();
                specs && (specs->SpecsDirty || g.resort) && specs->SpecsCount > 0)
            {
                const auto &spec = specs->Specs[0];
                const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
                std::stable_sort(g.resource_order.begin(),
                                 g.resource_order.end(),
                                 [&](int a, int b)
                                 {
                                     const Json &x = resources[static_cast<size_t>(a)];
                                     const Json &y = resources[static_cast<size_t>(b)];
                                     int order = 0;
                                     switch (spec.ColumnIndex)
                                     {
                                     case 0:
                                         order = x.value("kind", "").compare(y.value("kind", ""));
                                         break;
                                     case 2:
                                         order = number_of(x, "bytes") < number_of(y, "bytes")   ? -1
                                                 : number_of(x, "bytes") > number_of(y, "bytes") ? 1
                                                                                                 : 0;
                                         break;
                                     case 3:
                                         order = static_cast<int>(x.value("added", false)) -
                                                 static_cast<int>(y.value("added", false));
                                         break;
                                     default:
                                         order = lower(x.value("name", "")).compare(lower(y.value("name", "")));
                                         break;
                                     }
                                     return ascending ? order < 0 : order > 0;
                                 });
                specs->SpecsDirty = false;
                g.resort = false;
            }
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(shown.size()), row_height);
            while (clipper.Step())
                for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n)
                {
                    const Json &r = resources[static_cast<size_t>(shown[static_cast<size_t>(n)])];
                    ImGui::TableNextRow(0, row_height);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", r.value("kind", "").c_str());
                    ImGui::TableNextColumn();
                    const std::string name = r.value("name", "");
                    ImGui::TextUnformatted(name.c_str());
                    if (ImGui::BeginPopupContextItem(("##copy" + std::to_string(n)).c_str()))
                    {
                        if (ImGui::MenuItem("Copy name"))
                            copy_text(name);
                        ImGui::EndPopup();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\nRight-click to copy", name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(size_text(number_of(r, "bytes")).c_str());
                    ImGui::TableNextColumn();
                    if (r.value("added", false))
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, color::good);
                        ImGui::TextUnformatted("new");
                        ImGui::PopStyleColor();
                    }
                    else
                    {
                        ImGui::TextDisabled("-");
                    }
                }
            ImGui::EndTable();
        }

        void project_view(const Json &p)
        {
            const Json resources = p.contains("resources") ? p["resources"] : Json::array();
            int ebx = 0, res = 0, chunk = 0, added = 0;
            double bytes = 0;
            for (const auto &r : resources)
            {
                const std::string kind = r.value("kind", "");
                ebx += kind == "ebx";
                res += kind == "res";
                chunk += kind == "chunk";
                added += r.value("added", false) ? 1 : 0;
                bytes += number_of(r, "bytes");
            }
            tile_heading(p.value("title", file_name(p.value("file", ""))).c_str());
            if (ImGui::BeginTable("##meta", 4, ImGuiTableFlags_SizingStretchSame))
            {
                ImGui::TableNextColumn();
                field("AUTHOR", get(p, "author").is_null() ? "not set" : text_of(get(p, "author")));
                ImGui::TableNextColumn();
                field("PROFILE", text_of(get(p, "profile")));
                ImGui::TableNextColumn();
                field("GAME DATA BUILD", text_of(get(p, "head")));
                ImGui::TableNextColumn();
                field("RESOURCES", text_of(get(p, "resource_count")));
                ImGui::EndTable();
            }
            field("FILE", p.value("file", ""));
            wrapped_muted(std::to_string(resources.size()) + " changed resources: " + std::to_string(ebx) + " ebx, " +
                          std::to_string(res) + " res, " + std::to_string(chunk) + " chunk; " + std::to_string(added) +
                          " new; " + size_text(bytes) + " in all.");
            if (g.resource_order.size() != resources.size())
            {
                g.resource_order.resize(resources.size());
                for (size_t i = 0; i < resources.size(); ++i)
                    g.resource_order[i] = static_cast<int>(i);
                g.resort = true;
            }
            resources_table(resources);

            // Building: the project can go in the build list as it is (mod compile exports it on the way), or be
            // exported as an .fbmod to share.
            ImGui::Spacing();
            const std::string file = p.value("file", "");
            const Command *export_command = command("project", "export");
            ImGui::BeginDisabled(!export_command || file.empty() || g.exported.busy());
            if (primary_button("EXPORT .FBMOD", ImVec2(S(170), 0)))
                g.exported.start(*export_command, Json{{"file", file}});
            ImGui::EndDisabled();
            ImGui::SameLine();
            const bool listed = in_build(file);
            ImGui::BeginDisabled(listed || file.empty());
            if (ImGui::Button(listed ? "IN THE BUILD" : "ADD TO BUILD"))
                add_fbmod(file);
            ImGui::EndDisabled();
            wrapped_muted("EXPORT .FBMOD writes it as an .fbmod next to the project, to share or keep. ADD TO BUILD "
                          "puts the project "
                          "itself in the build list under BUILD & INSTALL; the build exports it on the way.");
            if (export_command)
                command_line(*export_command, Json{{"file", file.empty() ? "<file>" : file}});
            run_status(g.exported);
            if (g.exported.worked() && g.exported.result.is_object())
            {
                const std::string wrote = g.exported.result.value("output", "");
                coloured(color::good,
                         "Exported " + text_of(get(g.exported.result, "resources")) + " resources, " +
                             size_text(number_of(g.exported.result, "bytes")) + ".");
                field("FBMOD", wrote);
                const bool wrote_listed = in_build(wrote);
                ImGui::BeginDisabled(wrote_listed);
                if (ImGui::Button(wrote_listed ? "FBMOD IN THE BUILD" : "ADD THE FBMOD TO BUILD"))
                    add_fbmod(wrote);
                ImGui::EndDisabled();
            }
        }

        void fbmod_view(const Json &m)
        {
            tile_heading(m.value("title", file_name(m.value("file", ""))).c_str());
            if (ImGui::BeginTable("##fbmod_meta", 4, ImGuiTableFlags_SizingStretchSame))
            {
                ImGui::TableNextColumn();
                field("AUTHOR", get(m, "author").is_null() ? "not set" : text_of(get(m, "author")));
                ImGui::TableNextColumn();
                field("PROFILE", text_of(get(m, "profile")));
                ImGui::TableNextColumn();
                field("GAME DATA BUILD", text_of(get(m, "head")));
                ImGui::TableNextColumn();
                field("FORMAT / RESOURCES", text_of(get(m, "version")) + " / " + text_of(get(m, "resource_count")));
                ImGui::EndTable();
            }
            field("FILE", m.value("file", ""));
            if (m.contains("details") &&
                ImGui::CollapsingHeader(("Details (" + std::to_string(m["details"].size()) + ")").c_str()))
                for (const auto &line : m["details"])
                    ImGui::TextUnformatted(text_of(line).c_str());
        }

        void project_section(App &app)
        {
            const Command *info = command("project", "info");
            const Command *fbmod = command("mod", "info");

            begin_tile("##project", 411);
            tile_heading("PROJECT");
            wrapped_muted("Open a ReSkate Studio .fbproject to see what it changes. Drop a .fbproject or .fbmod "
                          "anywhere on the window.");
            path_field("##project_path", g.project_path, app.window, false, project_filter, S(110));
            if (g_dropped_into == &g.project_path)
                open_project(g.project_path);
            ImGui::BeginDisabled(!info || g.project_path.empty() || g.project.busy());
            if (primary_button("OPEN", ImVec2(S(130), 0)))
                open_project(g.project_path);
            ImGui::EndDisabled();
            if (info)
                command_line(*info, Json{{"file", g.project_path.empty() ? "<file>" : g.project_path}});
            run_status(g.project);
            if (g.project.worked() && g.project.result.is_object())
            {
                ImGui::Spacing();
                project_view(g.project.result);
            }
            end_tile();

            ImGui::Spacing();
            begin_tile("##recent", 412);
            tile_heading("RECENT PROJECTS");
            recent_list();
            end_tile();

            ImGui::Spacing();
            begin_tile("##fbmod", 413);
            tile_heading("FBMOD FILE");
            wrapped_muted(
                "Read the header of a .fbmod: title, author, the game data it was made for and its resources.");
            path_field("##fbmod_path", g.fbmod_path, app.window, false, fbmod_filter, S(110));
            if (g_dropped_into == &g.fbmod_path)
                inspect_fbmod(g.fbmod_path);
            ImGui::BeginDisabled(!fbmod || g.fbmod_path.empty() || g.fbmod_info.busy());
            if (primary_button("INSPECT", ImVec2(S(130), 0)))
                inspect_fbmod(g.fbmod_path);
            ImGui::EndDisabled();
            ImGui::SameLine(0, S(18));
            toggle("##verbose", &g.fbmod_verbose);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Costume detail");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("--verbose: the native costume recipe, for costume mods");
            if (fbmod)
            {
                Json args = {{"file", g.fbmod_path.empty() ? "<file>" : g.fbmod_path}};
                if (g.fbmod_verbose)
                    args["verbose"] = true;
                command_line(*fbmod, args);
            }
            run_status(g.fbmod_info);
            if (g.fbmod_info.worked() && g.fbmod_info.result.is_object())
            {
                ImGui::Spacing();
                fbmod_view(g.fbmod_info.result);
                const std::string file = g.fbmod_info.result.value("file", "");
                const bool listed = std::any_of(
                    g.fbmods.begin(), g.fbmods.end(), [&](const std::string &p) { return folded(p) == folded(file); });
                ImGui::BeginDisabled(listed);
                if (ImGui::Button(listed ? "IN THE BUILD" : "ADD TO BUILD"))
                    add_fbmod(file);
                ImGui::EndDisabled();
            }
            end_tile();
        }

        // ---------------------------------------------------------------- build and install

        void fbmod_list(App &app)
        {
            caption("MOD FILES, IN THE ORDER THEY APPLY");
            const float width = ImGui::GetContentRegionAvail().x;
            int move = -1, direction = 0, remove = -1;
            for (size_t i = 0; i < g.fbmods.size(); ++i)
            {
                const std::string &path = g.fbmods[i];
                ImGui::PushID(static_cast<int>(i));
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float height = S(46);
                list_row("##fbmod_row", width, height, false);
                auto *draw = ImGui::GetWindowDrawList();
                const ImVec4 clip(at.x, at.y, at.x + width - S(330), at.y + height);
                const std::string number = std::to_string(i + 1);
                draw->AddText(g_fonts.bold,
                              g_fonts.bold->FontSize,
                              ImVec2(at.x + S(14), at.y + S(13)),
                              color::muted,
                              number.c_str());
                const bool there = file_exists(path);
                draw->AddText(g_fonts.bold,
                              g_fonts.bold->FontSize,
                              ImVec2(at.x + S(44), at.y + S(5)),
                              there ? color::text : color::danger,
                              file_name(path).c_str(),
                              nullptr,
                              0,
                              &clip);
                draw->AddText(g_fonts.caption,
                              g_fonts.caption->FontSize,
                              ImVec2(at.x + S(44), at.y + S(25)),
                              color::muted,
                              (there ? parent_folder(path) : "Missing: " + path).c_str(),
                              nullptr,
                              0,
                              &clip);
                const ImVec2 after = ImGui::GetCursorScreenPos();
                float buttons = 0;
                for (const char *label : {"UP", "DOWN", "INSPECT", "REMOVE"})
                    buttons += ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2 +
                               ImGui::GetStyle().ItemSpacing.x;
                ImGui::SetCursorScreenPos(
                    ImVec2(at.x + width - buttons - S(6), at.y + (height - ImGui::GetTextLineHeight()) * 0.5f - S(2)));
                ImGui::BeginDisabled(i == 0);
                if (ImGui::SmallButton("UP"))
                {
                    move = static_cast<int>(i);
                    direction = -1;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(i + 1 == g.fbmods.size());
                if (ImGui::SmallButton("DOWN"))
                {
                    move = static_cast<int>(i);
                    direction = 1;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::SmallButton("INSPECT"))
                {
                    if (is_project(path))
                        open_project(path);
                    else
                        inspect_fbmod(path);
                    g.section = Section::project;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("REMOVE"))
                    remove = static_cast<int>(i);
                ImGui::SetCursorScreenPos(after);
                ImGui::PopID();
            }
            if (move >= 0)
                std::swap(g.fbmods[static_cast<size_t>(move)], g.fbmods[static_cast<size_t>(move + direction)]);
            if (remove >= 0)
                g.fbmods.erase(g.fbmods.begin() + remove);
            if (g.fbmods.empty())
                wrapped_muted("Nothing to build yet.");
            ImGui::Dummy(ImVec2(0, S(2)));
            if (ImGui::Button("ADD MOD FILE", ImVec2(S(150), 0)))
            {
                const auto start = g.fbmods.empty() ? std::wstring() : utf8_to_wide(g.fbmods.back());
                const auto picked = pick_path(app.window, false, start, build_filter);
                if (!picked.empty())
                    add_fbmod(wide_to_utf8(picked));
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled(
                "or drop .fbmod / .fbproject files here. The last one wins where two change the same resource.");
        }

        void build_section(App &app)
        {
            const Command *compile = command("mod", "compile");
            const Command *deploy = command("mod", "deploy");
            const std::string game = folder_in_use();

            // ------------------------------------------------ 1. compile
            begin_tile("##compile", 421);
            tile_heading("1  BUILD");
            wrapped_muted("Compile .fbmod files against the game into a staging folder. Reads the game, writes only "
                          "the staging folder.");
            fbmod_list(app);
            ImGui::Spacing();
            ImGui::PushFont(g_fonts.bold);
            ImGui::TextUnformatted("Staging folder");
            ImGui::PopFont();
            inline_caption("--staging-dir");
            wrapped_muted("Outside the ReSkate folder. Emptied and rebuilt on every build.");
            path_field("##staging", g.staging, app.window, true, {}, S(110));
            field("COMPILES AGAINST", game.empty() ? "No Skate folder: set one in Settings" : game);
            Json compile_args = {{"staging-dir", g.staging}, {"fbmod", g.fbmods}};
            compile_args = with_game(compile_args);
            if (compile)
                command_line(*compile, compile_args);
            const bool can_build = compile && !g.fbmods.empty() && !g.staging.empty() && !g.compile.busy();
            ImGui::BeginDisabled(!can_build);
            if (primary_button("BUILD", ImVec2(S(140), S(36))))
                g.compile.start(*compile, compile_args);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !can_build && !g.compile.busy())
                ImGui::SetTooltip(g.fbmods.empty() ? "Add an .fbmod or .fbproject first"
                                                   : "Choose a staging folder first");
            run_status(g.compile);
            if (g.compile.worked())
            {
                const Json &r = g.compile.result;
                coloured(color::good,
                         "Built: " + text_of(get(r, "bundles")) + " bundles, " + text_of(get(r, "resources")) +
                             " resources, " + text_of(get(r, "tocs")) + " TOCs in " +
                             format_seconds(number_of(r, "seconds")) + ".");
                field("PATCH FOLDER", r.value("patch_dir", ""));
                field("BUILT FOR SKATE.EXE", text_of(get(r, "skate_sha256")));
            }
            end_tile();
            ImGui::Spacing();

            // ------------------------------------------------ 2. deploy
            begin_tile("##deploy", 422);
            tile_heading("2  INSTALL INTO RESKATE");
            wrapped_muted("Copy the staging build into the ReSkate folder's Mods. A mod already there under the same "
                          "name is replaced and "
                          "its previous version kept. Skate must be closed.");
            ImGui::PushFont(g_fonts.bold);
            ImGui::TextUnformatted("Mod folder name");
            ImGui::PopFont();
            inline_caption("--mod-folder");
            ImGui::SetNextItemWidth(S(360));
            input_text("##mod_folder", g.mod_folder, 0, "e.g. ScooterPush");
            const std::string name_problem = mod_folder_problem(g.mod_folder);
            if (!name_problem.empty())
            {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, color::warning);
                ImGui::TextUnformatted(name_problem.c_str());
                ImGui::PopStyleColor();
            }
            const std::string target =
                game.empty() ? std::string() : game + (game.back() == '\\' ? "" : "\\") + "Mods\\" + g.mod_folder;
            std::error_code ec;
            const bool staged = !g.staging.empty() &&
                                fs::is_regular_file(fs::path(utf8_to_wide(g.staging)) / L"Patch" / L"layout.toc", ec);
            bool installed = false;
            if (folded(g.listed_folder) == folded(game))
                for (const auto &row : g.mods)
                    installed |= lower(row.name) == lower(g.mod_folder);
            field("FROM",
                  g.staging.empty() ? "Choose a staging folder under BUILD"
                                    : g.staging + (staged ? "" : "   (no build here yet)"));
            field("INSTALLS INTO", target.empty() ? "No Skate folder: set one in Settings" : target);
            if (installed)
                wrapped_muted("A mod with this name is installed: it is updated, and the current version is kept in "
                              ".ReSkateStudio-Mod-backup.");
            Json deploy_args = with_game(Json{{"staging-dir", g.staging}, {"mod-folder", g.mod_folder}});
            if (deploy)
                command_line(*deploy, deploy_args);
            const bool can_deploy =
                deploy && staged && name_problem.empty() && !target.empty() && !g.deploy.busy() && !g.compile.busy();
            ImGui::BeginDisabled(!can_deploy);
            if (primary_button("INSTALL...", ImVec2(S(160), S(36))))
            {
                g.confirm = true;
                g.confirm_args = deploy_args;
                g.confirm_target = target;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !can_deploy && !g.deploy.busy())
                ImGui::SetTooltip("%s",
                                  !staged                 ? "BUILD first: the staging folder holds no build"
                                  : !name_problem.empty() ? name_problem.c_str()
                                                          : "Set the ReSkate folder");
            ImGui::SameLine();
            inline_badge("CHANGES GAME FILES", color::warning);
            run_status(g.deploy);
            if (g.deploy.worked())
            {
                const Json &r = g.deploy.result;
                coloured(color::good, "Installed into " + r.value("mod_path", g.confirm_target) + ".");
                value_grid("##deployed", r, {"mod_path"});
                if (get(r, "notes").is_array())
                    for (const auto &n : r["notes"])
                        wrapped_muted(text_of(n));
            }

            // The confirmation names the exact folder it writes to.
            if (g.confirm)
            {
                ImGui::OpenPopup("##confirm_install");
                g.confirm = false;
            }
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(S(640), 0));
            if (ImGui::BeginPopupModal("##confirm_install",
                                       nullptr,
                                       ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoResize))
            {
                heading("INSTALL INTO THE GAME?");
                wrapped_muted("This writes into the game folder:");
                ImGui::PushFont(g_fonts.bold);
                coloured(color::warning, g.confirm_target);
                ImGui::PopFont();
                ImGui::Spacing();
                const std::string root =
                    g.confirm_args.contains("game-root") ? text_of(g.confirm_args["game-root"]) : game;
                ImGui::Bullet();
                ImGui::TextWrapped("Copies the build from %s", g.confirm_args.value("staging-dir", "").c_str());
                ImGui::Bullet();
                ImGui::TextWrapped("A version already in that folder moves to %s\\.ReSkateStudio-Mod-backup\\%s",
                                   root.c_str(),
                                   g.confirm_args.value("mod-folder", "").c_str());
                ImGui::Bullet();
                ImGui::TextWrapped("Writes the receipt %s\\.ReSkateStudio-last-deploy.db", root.c_str());
                ImGui::Bullet();
                ImGui::TextWrapped("Skate must be closed.");
                ImGui::Spacing();
                if (deploy)
                    command_line(*deploy, g.confirm_args);
                ImGui::Spacing();
                if (primary_button("INSTALL", ImVec2(S(150), S(36))) && deploy)
                {
                    g.deploy.start(*deploy, g.confirm_args);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("CANCEL", ImVec2(S(130), S(36))) || ImGui::IsKeyPressed(ImGuiKey_Escape))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            end_tile();
        }

        // ---------------------------------------------------------------- health

        void health_tile(const char *id,
                         const char *title,
                         const char *blurb,
                         const Command *c,
                         Run &run,
                         const Json &args,
                         const std::function<void()> &options,
                         const std::function<void(const Json &)> &view)
        {
            begin_tile(id, seed_of(id));
            tile_heading(title);
            wrapped_muted(blurb);
            if (!c)
            {
                wrapped_muted("Not in the registry.");
                end_tile();
                return;
            }
            if (options)
                options();
            command_line(*c, args);
            ImGui::BeginDisabled(run.busy());
            if (primary_button("RUN", ImVec2(S(130), 0)))
                run.start(*c, args);
            ImGui::EndDisabled();
            run_status(run);
            if (run.worked() && run.result.is_object())
                view(run.result);
            end_tile();
        }

        void health_section()
        {
            Json validate_args = Json::object();
            if (!g.validate_count.empty() && g.validate_count != "100")
                validate_args["count"] = g.validate_count;
            health_tile(
                "##validate",
                "READ THE GAME",
                "Decode the first assets through the CAS and Oodle pipeline: is this folder usable? About 3 s.",
                command("game", "validate"),
                g.validate,
                with_game(validate_args),
                []
                {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Assets to decode");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(S(120));
                    input_text("##count", g.validate_count, ImGuiInputTextFlags_CharsDecimal);
                },
                [](const Json &r)
                {
                    coloured(r.value("ok", false) ? color::good : color::danger,
                             r.value("ok", false) ? "All " + text_of(get(r, "requested")) + " assets decoded."
                                                  : "Not every asset decoded.");
                    value_grid("##validate_values", r, {"ok", "game_root"});
                });
            ImGui::Spacing();
            health_tile("##audit",
                        "PATCH FOLDER AUDIT",
                        "Check <game>\\Patch: what it changes, and that it matches the bundle manifests. "
                        "Installed mods in Mods are not part of it. About 6 s.",
                        command("game", "patch-audit"),
                        g.audit,
                        with_game(Json::object()),
                        nullptr,
                        [](const Json &r)
                        {
                            if (!r.value("patch_present", false))
                                wrapped_muted("There is no Patch folder in " + r.value("game_root", "") + ".");
                            value_grid("##counts", get(r, "counts"));
                            const Json &stock = get(r, "stock_replacements");
                            if (stock.is_array() && !stock.empty() &&
                                ImGui::CollapsingHeader(
                                    ("Stock assets replaced (" + std::to_string(stock.size()) + ")").c_str()))
                                for (const auto &s : stock)
                                    ImGui::TextUnformatted(text_of(s).c_str());
                        });
            ImGui::Spacing();
            health_tile("##initfs",
                        "INITFS",
                        "Count the files in the game's initfs_win32, the archive it reads first.",
                        command("game", "initfs"),
                        g.initfs,
                        with_game(Json::object()),
                        nullptr,
                        [](const Json &r) { value_grid("##initfs_values", r, {"game_root"}); });
        }

        // ---------------------------------------------------------------- drops and state

        // On BUILD & INSTALL, dropped .fbmod and .fbproject files go into the build list. Elsewhere a .fbproject
        // opens and an .fbmod is inspected. Anything else is left for the path field it landed on.
        void take_drops()
        {
            std::vector<std::string> projects, fbmods;
            {
                std::lock_guard lock(g_drop.mutex);
                if (!g_drop.pending)
                    return;
                for (const auto &p : g_drop.paths)
                {
                    const std::wstring ext = fs::path(p).extension().native();
                    std::string e = lower(wide_to_utf8(ext));
                    if (e == ".fbproject")
                        projects.push_back(wide_to_utf8(p));
                    else if (e == ".fbmod")
                        fbmods.push_back(wide_to_utf8(p));
                }
                if (projects.empty() && fbmods.empty())
                    return;
                g_drop.pending = false;
                g_drop.paths.clear();
            }
            if (g.section == Section::build)
            {
                // On BUILD & INSTALL everything dropped goes into the build list, projects included.
                for (const auto &f : projects)
                    add_fbmod(f);
                for (const auto &f : fbmods)
                    add_fbmod(f);
                return;
            }
            if (!projects.empty())
                open_project(projects.front());
            if (fbmods.empty())
                return;
            {
                inspect_fbmod(fbmods.front());
                g.section = Section::project;
            }
        }

        void load_state()
        {
            g.loaded = true;
            g.staging = path_utf8(Settings::data_dir() / L"Builds" / L"Stage");
            if (auto recent = read_json(recent_file());
                recent && recent->contains("projects") && (*recent)["projects"].is_array())
                for (const auto &p : (*recent)["projects"])
                    if (p.is_string())
                        g.recent.push_back(p.get<std::string>());
            if (auto s = read_json(state_file()); s && s->is_object())
            {
                const std::string section = s->value("section", "mods");
                g.section = section == "project"  ? Section::project
                            : section == "build"  ? Section::build
                            : section == "health" ? Section::health
                                                  : Section::mods;
                g.project_path = s->value("project", "");
                g.fbmod_path = s->value("fbmod", "");
                if (s->contains("fbmods") && (*s)["fbmods"].is_array())
                    for (const auto &f : (*s)["fbmods"])
                        if (f.is_string())
                            g.fbmods.push_back(f.get<std::string>());
                g.staging = s->value("staging-dir", g.staging);
                g.mod_folder = s->value("mod-folder", g.mod_folder);
                g.check_hash = s->value("check-hash", false);
            }
            g.written = snapshot();
            // The project and fbmod open last time are read again, so the page comes back as it was left.
            if (!g.project_path.empty() && file_exists(g.project_path))
                open_project(g.project_path, false);
            if (!g.fbmod_path.empty() && file_exists(g.fbmod_path))
                inspect_fbmod(g.fbmod_path);
        }

        // Every run this page started is taken in here, whichever section is showing.
        void poll_runs()
        {
            if (g.mod_list.poll() && g.mod_list.worked())
                build_rows(g.mod_list.result);
            if (g.project.poll())
            {
                if (g.project.worked())
                {
                    remember_project(g.project.result.value("file", g.project_path));
                    g.resource_order.clear();
                }
                else
                {
                    g.project.result = nullptr; // not the old project under the new one's name
                }
            }
            g.fbmod_info.poll();
            g.exported.poll();
            if (g.change.poll() && g.change.worked())
                refresh_mods();
            g.compile.poll();
            // An install changes what is installed: read the list again.
            if (g.deploy.poll() && g.deploy.worked() && folded(g.listed_folder) == folded(folder_in_use()))
                refresh_mods();
            g.validate.poll();
            g.audit.poll();
            g.initfs.poll();
        }

        void save_state()
        {
            // Not while a field is being typed in: once it is left.
            if (ImGui::IsAnyItemActive())
                return;
            Json now = snapshot();
            if (now == g.written)
                return;
            write_json(state_file(), now);
            g.written = std::move(now);
        }

        void ensure_loaded(App &app)
        {
            if (g.loaded)
                return;
            const std::string saved = path_utf8(app.settings.game_root);
            g.folder = saved;
            g.saved_folder = saved;
            load_state();
        }

    } // namespace

    void build_in_project(App &app, const std::string &file)
    {
        ensure_loaded(app);
        if (!file.empty())
            add_fbmod(file);
        g.section = Section::build;
        app.page = page::project;
    }

    void project_page(App &app)
    {
        const std::string saved = path_utf8(app.settings.game_root);
        ensure_loaded(app);
        // Settings changed the Skate folder: follow it, unless this page was pointed somewhere else.
        if (saved != g.saved_folder)
        {
            if (folded(g.folder) == folded(g.saved_folder))
                g.folder = saved;
            g.saved_folder = saved;
        }
        g_drop_fallback = &g_unclaimed_drop;
        take_drops();
        poll_runs();

        heading("PROJECT & MODS");
        wrapped_muted(
            "The mods in your ReSkate folder, your project and its .fbmod files, and getting them into the game.");
        ImGui::Spacing();
        folder_tile(app);
        ImGui::Spacing();

        const float gap = S(10);
        const float tab = (ImGui::GetContentRegionAvail().x - gap * 3) / 4;
        const ImVec2 row = ImGui::GetCursorScreenPos();
        const auto count = [](size_t n) { return n ? std::to_string(n) : std::string(); };
        const Json &project = g.project.result;
        const size_t resources = project.is_object() && project.contains("resources") ? project["resources"].size() : 0;
        if (section_tab("INSTALLED MODS", g.section == Section::mods, count(g.mods.size()), tab))
            g.section = Section::mods;
        if (section_tab("PROJECT", g.section == Section::project, count(resources), tab))
            g.section = Section::project;
        if (section_tab("BUILD & INSTALL", g.section == Section::build, count(g.fbmods.size()), tab))
            g.section = Section::build;
        if (section_tab("HEALTH", g.section == Section::health, {}, tab))
            g.section = Section::health;
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + S(42) + S(14)));
        ImGui::Dummy(ImVec2(0, 0));

        switch (g.section)
        {
        case Section::mods:
            mods_section();
            break;
        case Section::project:
            project_section(app);
            break;
        case Section::build:
            build_section(app);
            break;
        case Section::health:
            health_section();
            break;
        }
        save_state();
    }

} // namespace studio::gui
