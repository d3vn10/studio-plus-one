// The CUSTOM MAPS page. Pick a Blender scene or a normalized map folder, see what is in it, set the
// build options, build it with map compile and install it with mod deploy. Every action is a registry
// command run on the job system, so Activity keeps the studio-plus line that repeats it; the page
// itself only holds what is on screen and the recent maps list.
#include "gui/maps_page.h"

#include "core/settings.h"
#include "gui/app.h"
#include "gui/jobs.h"
#include "gui/look.h"
#include "gui/runner.h"
#include "gui/widgets.h"

#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>

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

        void wrapped_colour(const std::string &text, ImU32 colour)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        void bold(const char *text)
        {
            ImGui::PushFont(g_fonts.bold);
            ImGui::TextUnformatted(text);
            ImGui::PopFont();
        }

        const Command *command(const char *group, const char *name)
        {
            return Registry::instance().find(group, name);
        }

        const Param *param_of(const Command &c, const std::string &name)
        {
            for (const auto &p : c.params)
                if (p.name == name)
                    return &p;
            return nullptr;
        }

        std::string text_of(const Json &value)
        {
            return value.is_string() ? value.get<std::string>() : value.dump();
        }

        std::string number_text(const Json &value)
        {
            if (value.is_number_integer())
            {
                std::string digits = std::to_string(value.get<long long>());
                for (int at = static_cast<int>(digits.size()) - 3; at > (digits[0] == '-' ? 1 : 0); at -= 3)
                    digits.insert(static_cast<size_t>(at), ",");
                return digits;
            }
            if (value.is_number())
            {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%.1f", value.get<double>());
                return buffer;
            }
            return value.is_null() ? "-" : text_of(value);
        }

        bool same_path(const std::string &a, const std::string &b)
        {
            std::string x = lower(a), y = lower(b);
            for (auto *s : {&x, &y})
            {
                std::replace(s->begin(), s->end(), '/', '\\');
                while (s->size() > 3 && s->back() == '\\')
                    s->pop_back();
            }
            return x == y;
        }

        bool job_running(const std::shared_ptr<Job> &job)
        {
            return job && !job->done.load();
        }

        // An empty object until the job is done, so .value() on it is always safe.
        Json outcome_of(const std::shared_ptr<Job> &job)
        {
            if (!job || !job->done.load())
                return Json::object();
            std::lock_guard lock(job->mutex);
            return job->outcome;
        }

        // ---------------------------------------------------------------- what the page remembers

        enum class Kind
        {
            none,
            blend,
            fbx,
            folder,
            invalid
        };

        struct RecentMap
        {
            std::string source, staging, mod_folder, when;
            bool ok = false;
            Json triangles; // from the build's stats
            Json options = Json::object();
        };

        struct LogLine
        {
            double at;
            std::string text;
        };

        struct MapsState
        {
            bool started = false;
            std::string source, staging, mod_folder, game_root;
            bool staging_edited = false, mod_folder_edited = false;
            std::string checked; // the source `kind` was worked out for
            Kind kind = Kind::none;
            std::string kind_problem;
            Json options = Json::object(); // map compile options that differ from the engine's defaults

            std::shared_ptr<Job> inspect, build, install, mods;
            std::shared_ptr<Job> scene_look; // map inspect-scene of a .blend / .fbx
            std::string build_source;        // what `build` was started for
            bool build_handled = true, install_handled = true, inspect_handled = true, deploys = false;
            std::vector<LogLine> log;
            std::string last_message;
            std::string problem, install_problem;
            int install_reveal = 0;
            int reveal = 0; // frames left to scroll the build into view; -1 while it runs, to scroll again at the end

            bool confirm = false;       // the install question is up
            bool confirm_build = false; // ... for BUILD & INSTALL rather than INSTALL
            bool install_after_build = false;
            bool run_on_start = false;
            std::string scroll_to;
            int scroll_frames = 0;

            std::vector<RecentMap> recent;
            bool staging_ready = false;
            double staging_checked = -100;
            double game_root_changed = -100;
        };
        MapsState g_maps;

        Json g_startup; // from maps_startup, taken on the first frame
        bool g_startup_run = false;

        // ---------------------------------------------------------------- the source, and names from it

        Kind detect(const std::string &text, std::string &problem)
        {
            problem.clear();
            if (text.empty())
                return Kind::none;
            std::error_code ec;
            const fs::path path(utf8_to_wide(text));
            if (fs::is_directory(path, ec))
            {
                if (fs::is_regular_file(path / L"map.json", ec))
                    return Kind::folder;
                problem = ec ? "Cannot read " + path_utf8(path / L"map.json") + ": " + ec.message()
                             : "This folder has no map.json. Choose a .blend or .fbx scene, or a map folder the "
                               "converter made.";
                return Kind::invalid;
            }
            if (!fs::is_regular_file(path, ec))
            {
                problem = "Not found: " + text;
                return Kind::invalid;
            }
            const std::string ext = lower(path_utf8(path.extension()));
            if (ext == ".blend")
                return Kind::blend;
            if (ext == ".fbx")
                return Kind::fbx;
            if (lower(path_utf8(path.filename())) == "map.json")
                return Kind::folder;
            problem = "Studio+ builds maps from .blend and .fbx scenes, or from a map folder holding map.json.";
            return Kind::invalid;
        }

        fs::path folder_of(const std::string &source)
        {
            fs::path path(utf8_to_wide(source));
            std::error_code ec;
            return fs::is_regular_file(path, ec) ? path.parent_path() : path;
        }

        std::string stem_of(const std::string &source, Kind kind)
        {
            const fs::path path(utf8_to_wide(source));
            if (kind == Kind::folder)
                return path_utf8(folder_of(source).filename());
            return path_utf8(path.stem());
        }

        // The level map compile makes: dingolevel_reskate_ and the file name in lower case.
        std::string level_of(const std::string &stem)
        {
            std::string id;
            for (char c : lower(stem))
                id += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
            return "dingolevel_reskate_" + id;
        }

        // A Mods folder name from the map's name: the characters mod deploy takes, at most 64.
        std::string mod_name_from(const std::string &stem)
        {
            std::string out;
            for (char c : stem)
                if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-' || c == '.')
                    out += c;
            while (!out.empty() && (out.front() == '.' || out.front() == ' '))
                out.erase(out.begin());
            while (!out.empty() && (out.back() == '.' || out.back() == ' '))
                out.pop_back();
            if (out.size() > 64)
                out.resize(64);
            return out.empty() ? "Custom Map" : out;
        }

        // The map's name: a scene's file name, or for a map folder the name in its map.json (from map
        // inspect, once that has run for it).
        std::string inspected_name(const std::string &source)
        {
            const auto &s = g_maps;
            if (!s.inspect || !s.inspect->done.load())
                return {};
            if (!same_path(s.inspect->args.value("folder", ""), path_utf8(folder_of(source))))
                return {};
            std::lock_guard lock(s.inspect->mutex);
            const Json &outcome = s.inspect->outcome;
            if (!outcome.value("ok", false))
                return {};
            const Json name = outcome["result"].value("name", Json());
            return name.is_string() ? name.get<std::string>() : std::string();
        }

        std::string map_name(const std::string &source, Kind kind)
        {
            if (kind == Kind::folder)
            {
                const std::string name = inspected_name(source);
                if (!name.empty())
                    return name;
            }
            return stem_of(source, kind);
        }

        std::string default_staging(const std::string &stem)
        {
            return path_utf8(Settings::data_dir() / L"Maps" / utf8_to_wide(stem));
        }

        void source_changed()
        {
            auto &s = g_maps;
            s.checked = s.source;
            s.kind = detect(s.source, s.kind_problem);
            if (s.kind == Kind::none || s.kind == Kind::invalid)
                return;
            const std::string stem = map_name(s.source, s.kind);
            if (!s.staging_edited)
                s.staging = default_staging(stem);
            if (!s.mod_folder_edited)
                s.mod_folder = mod_name_from(stem);
            s.staging_checked = -100;
        }

        // ---------------------------------------------------------------- recent maps (Studio+'s own list)

        fs::path recent_file()
        {
            return Settings::data_dir() / L"recent-maps.json";
        }

        void load_recent()
        {
            g_maps.recent.clear();
            std::ifstream in(recent_file(), std::ios::binary);
            if (!in)
                return;
            const Json list = Json::parse(in, nullptr, false);
            if (!list.is_array())
                return;
            for (const auto &item : list)
            {
                if (!item.is_object())
                    continue;
                RecentMap r;
                r.source = item.value("source", "");
                r.staging = item.value("staging", "");
                r.mod_folder = item.value("mod_folder", "");
                r.when = item.value("when", "");
                r.ok = item.value("ok", false);
                r.triangles = item.contains("triangles") ? item["triangles"] : Json();
                if (item.contains("options") && item["options"].is_object())
                    r.options = item["options"];
                if (!r.source.empty())
                    g_maps.recent.push_back(std::move(r));
            }
        }

        void save_recent()
        {
            Json list = Json::array();
            for (const auto &r : g_maps.recent)
                list.push_back({{"source", r.source},
                                {"staging", r.staging},
                                {"mod_folder", r.mod_folder},
                                {"when", r.when},
                                {"ok", r.ok},
                                {"triangles", r.triangles},
                                {"options", r.options}});
            std::error_code ec;
            fs::create_directories(Settings::data_dir(), ec);
            std::ofstream out(recent_file(), std::ios::binary | std::ios::trunc);
            out << list.dump(2) << "\n";
        }

        std::string now_text()
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
            localtime_s(&local, &now);
            char buffer[32];
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
            return buffer;
        }

        void remember(bool ok, const Json &triangles)
        {
            auto &s = g_maps;
            std::erase_if(s.recent, [&](const RecentMap &r) { return same_path(r.source, s.build_source); });
            RecentMap r;
            r.source = s.build_source;
            r.staging = s.staging;
            r.mod_folder = s.mod_folder;
            r.when = now_text();
            r.ok = ok;
            r.triangles = triangles;
            r.options = s.options;
            s.recent.insert(s.recent.begin(), std::move(r));
            if (s.recent.size() > 8)
                s.recent.resize(8);
            save_recent();
        }

        // ---------------------------------------------------------------- the options, grouped

        enum class Control
        {
            toggle,
            choice,
            number,
            text
        };

        struct Choice
        {
            const char *label;
            const char *value; // nullptr: leave it to the engine (the option is not passed)
        };

        struct Option
        {
            const char *param;
            const char *label;
            const char *help;
            Control control;
            std::vector<Choice> choices;
        };

        struct Group
        {
            const char *title;
            const char *blurb;
            std::vector<Option> options;
            std::vector<Option> fine; // under FINE TUNING
        };

        const std::vector<Group> &left_groups()
        {
            static const std::vector<Group> groups{
                {"LIGHTING & GI",
                 "How the map is lit. Baked lighting looks best and takes longest to build.",
                 {{"gi",
                   "Baked lighting (GI)",
                   "Bakes light bouncing off surfaces into probes. Off builds faster; the map is then lit by sun and "
                   "sky only.",
                   Control::toggle,
                   {}},
                  {"auto-reflection",
                   "Reflections",
                   "Places reflection probes for you, so shiny surfaces reflect the map around them.",
                   Control::toggle,
                   {}},
                  {"bam-lighting",
                   "Time-of-day lighting",
                   "Ships the game's own sky and sun so the light follows the clock. It applies to every level while "
                   "this map is installed.",
                   Control::toggle,
                   {}},
                  {"live-tod",
                   "Live time of day",
                   "Lets the time of day change while you skate the map.",
                   Control::toggle,
                   {}},
                  {"enlighten",
                   "Enlighten (experimental)",
                   "Also builds Enlighten probes and input systems.",
                   Control::toggle,
                   {}},
                  {"enlighten-open-sky",
                   "Open sky for Enlighten",
                   "Treat the map as open to the sky when Enlighten is on.",
                   Control::toggle,
                   {}}},
                 {{"gi-intensity", "Bake intensity", "Multiplies the baked light.", Control::number, {}},
                  {"gi-sun", "Sun (lux)", "Sun light used for the bake.", Control::number, {}},
                  {"gi-sky", "Sky (lux)", "Sky light used for the bake.", Control::number, {}},
                  {"gi-bounce", "Bounce", "How much light carries on after hitting a surface.", Control::number, {}},
                  {"gi-sun-rotation", "Sun direction", "x,y in degrees, for example 45,30.", Control::text, {}},
                  {"gi-rays", "Rays", "Rays per probe. More is smoother and slower.", Control::number, {}},
                  {"gi-lods", "GI detail levels", "1 to 4.", Control::number, {}},
                  {"gi-enlighten-spacing",
                   "Enlighten probe spacing (m)",
                   "Distance between Enlighten probes.",
                   Control::number,
                   {}},
                  {"gi-enlighten-gain", "Enlighten gain", "Multiplies the Enlighten light.", Control::number, {}}}},
            };
            return groups;
        }

        const std::vector<Group> &right_groups()
        {
            static const std::vector<Group> groups{
                {"TIME OF DAY",
                 nullptr,
                 {{"time-of-day",
                   "Start at",
                   "The time of day the map starts at.",
                   Control::choice,
                   {{"Default", nullptr},
                    {"Morning", "morning"},
                    {"Noon", "noon"},
                    {"Afternoon", "afternoon"},
                    {"Evening", "evening"},
                    {"Night", "night"}}}},
                 {}},
                {"STREAMING",
                 "Big maps load in chunks around you as you skate; small ones stay loaded whole.",
                 {{"stream",
                   "Streaming",
                   "Automatic lets the engine decide by the map's size.",
                   Control::choice,
                   {{"Automatic", nullptr}, {"On", "true"}, {"Off", "false"}}},
                  {"cell-size",
                   "Chunk size",
                   "Smaller chunks load less at a time.",
                   Control::choice,
                   {{"Default", nullptr}, {"200 m", "200"}, {"100 m", "100"}, {"50 m", "50"}}},
                  {"cell-load-distance", "Chunk load distance (m)", "How far ahead chunks load.", Control::number, {}}},
                 {{"vista-ratio",
                   "Far view simplification",
                   "How much the far-away view of the map is simplified.",
                   Control::number,
                   {}},
                  {"vista-error",
                   "Far view error (m)",
                   "How far the simplified view may stray from the real one.",
                   Control::number,
                   {}}}},
                {"DETAIL LEVELS (LODS)",
                 nullptr,
                 {{"lods",
                   "Detail levels",
                   "Makes simpler copies of big meshes to draw when they are far away. Off builds faster.",
                   Control::toggle,
                   {}}},
                 {{"lod-ratios",
                   "Triangle ratios",
                   "For each level, the share of triangles kept, for example 0.5,0.25.",
                   Control::text,
                   {}},
                  {"lod-distances",
                   "Switch distances (m)",
                   "Where each level takes over, for example 40,120. As many as the ratios.",
                   Control::text,
                   {}},
                  {"lod-min-triangles",
                   "Smallest mesh (triangles)",
                   "Meshes with fewer triangles keep one level.",
                   Control::number,
                   {}}}},
                {"PAUSE MAP",
                 "The overhead picture in the pause menu, rendered by Blender. A scene with assets\\<name>\\map.png "
                 "next to it uses that picture instead.",
                 {{"pause-map",
                   "Picture",
                   "3D adds shadows and edge shading; 2D is flat and quicker. None skips the render, the slowest step, "
                   "and keeps "
                   "San Van's picture (needs the 2.12 converter in the engine folder; older ones render 3D).",
                   Control::choice,
                   {{"3D with shadows", "3d"}, {"2D flat", "2d"}, {"None", "none"}}}},
                 {}},
            };
            return groups;
        }

        struct Preset
        {
            const char *label;
            const char *key;
            const char *tip;
            Json values; // param -> value; null removes it (the engine default)
        };

        const std::vector<Preset> &presets()
        {
            static const std::vector<Preset> list{
                {"QUICK TEST",
                 "quick",
                 "Fastest build to try the map in game: no baked lighting, reflections or detail levels, flat pause "
                 "map",
                 Json{{"gi", false},
                      {"auto-reflection", false},
                      {"lods", false},
                      {"enlighten", false},
                      {"pause-map", "2d"}}},
                {"FULL QUALITY",
                 "full",
                 "Everything on, as the engine builds by default: baked lighting, reflections, detail levels, the 3D "
                 "pause map",
                 Json{{"gi", true},
                      {"auto-reflection", true},
                      {"lods", true},
                      {"bam-lighting", true},
                      {"live-tod", true},
                      {"pause-map", "3d"}}},
            };
            return list;
        }

        const Command *compile_command()
        {
            return command("map", "compile");
        }

        Json default_of(const std::string &name)
        {
            const Command *c = compile_command();
            const Param *p = c ? param_of(*c, name) : nullptr;
            return p && p->default_value ? *p->default_value : Json();
        }

        // The value the option has now: what is set, else the engine's default.
        Json option_value(const std::string &name)
        {
            const auto it = g_maps.options.find(name);
            if (it != g_maps.options.end())
                return *it;
            return default_of(name);
        }

        void set_option(const std::string &name, const Json &value)
        {
            const Json fallback = default_of(name);
            if (value.is_null() || (value.is_string() && value.get<std::string>().empty()) ||
                (!fallback.is_null() && value == fallback))
                g_maps.options.erase(name);
            else
                g_maps.options[name] = value;
        }

        void apply_preset(const Preset &preset)
        {
            for (const auto &[name, value] : preset.values.items())
                set_option(name, value);
        }

        bool preset_active(const Preset &preset)
        {
            for (const auto &[name, value] : preset.values.items())
                if (option_value(name) != value)
                    return false;
            return true;
        }

        // A row of buttons, the chosen one blue. Returns the index pressed, or -1.
        int segmented(const char *id, const std::vector<Choice> &choices, int current)
        {
            ImGui::PushID(id);
            int pressed = -1;
            const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            for (size_t i = 0; i < choices.size(); ++i)
            {
                const float width = ImGui::CalcTextSize(choices[i].label).x + ImGui::GetStyle().FramePadding.x * 2;
                if (i > 0 && ImGui::GetItemRectMax().x + spacing + width <= right)
                    ImGui::SameLine();
                ImGui::PushID(static_cast<int>(i));
                const bool chosen = static_cast<int>(i) == current;
                if (chosen ? primary_button(choices[i].label) : ImGui::Button(choices[i].label))
                    pressed = static_cast<int>(i);
                ImGui::PopID();
            }
            ImGui::PopID();
            return pressed;
        }

        void option_row(const Option &o)
        {
            ImGui::PushID(o.param);
            const Json value = option_value(o.param);
            if (o.control == Control::toggle)
            {
                bool on = value.is_boolean() && value.get<bool>();
                if (toggle("##on", &on))
                    set_option(o.param, on);
                ImGui::SameLine(0, S(12));
                ImGui::BeginGroup();
                bold(o.label);
                wrapped_muted(o.help);
                ImGui::EndGroup();
            }
            else
            {
                bold(o.label);
                wrapped_muted(o.help);
                if (o.control == Control::choice)
                {
                    int current = 0;
                    const std::string now = value.is_null() ? "" : text_of(value);
                    for (size_t i = 0; i < o.choices.size(); ++i)
                        if (o.choices[i].value ? now == o.choices[i].value : now.empty())
                            current = static_cast<int>(i);
                    const int pressed = segmented("##choice", o.choices, current);
                    if (pressed >= 0)
                    {
                        const char *chosen = o.choices[static_cast<size_t>(pressed)].value;
                        if (!chosen)
                            set_option(o.param, Json());
                        else if (std::string(chosen) == "true" || std::string(chosen) == "false")
                            set_option(o.param, std::string(chosen) == "true");
                        else
                            set_option(o.param, chosen);
                    }
                }
                else
                {
                    // Numbers and lists stay text, as typed: map compile checks them and names the field when one is
                    // wrong.
                    std::string text = value.is_array()  ? (value.empty() ? "" : text_of(value[0]))
                                       : value.is_null() ? ""
                                                         : text_of(value);
                    ImGui::SetNextItemWidth(std::min(S(260), ImGui::GetContentRegionAvail().x));
                    if (input_text("##value",
                                   text,
                                   o.control == Control::number ? ImGuiInputTextFlags_CharsScientific : 0,
                                   "Default"))
                    {
                        const Command *c = compile_command();
                        const Param *p = c ? param_of(*c, o.param) : nullptr;
                        if (text.empty())
                            set_option(o.param, Json());
                        else if (p && p->type == ParamType::List)
                            set_option(o.param, Json::array({text}));
                        else
                            set_option(o.param, text);
                    }
                }
            }
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, S(4)));
        }

        void group_block(const Group &group, bool scene)
        {
            ImGui::PushID(group.title);
            caption(group.title);
            ImGui::Separator();
            if (group.blurb)
                wrapped_muted(group.blurb);
            const bool pause_map = std::string(group.title) == "PAUSE MAP";
            ImGui::BeginDisabled(pause_map && !scene);
            for (const auto &o : group.options)
                option_row(o);
            ImGui::EndDisabled();
            if (pause_map && !scene)
                wrapped_muted("Only for .blend and .fbx scenes: a map folder already has its picture.");
            if (!group.fine.empty())
            {
                if (ImGui::TreeNodeEx("Fine tuning", ImGuiTreeNodeFlags_SpanAvailWidth))
                {
                    for (const auto &o : group.fine)
                        option_row(o);
                    ImGui::TreePop();
                }
            }
            ImGui::Dummy(ImVec2(0, S(8)));
            ImGui::PopID();
        }

        void mods_block(HWND owner)
        {
            caption("EXTRA MODS");
            ImGui::Separator();
            wrapped_muted("Merge .fbmod files into the same Patch, so they install together with the map.");
            Json list = g_maps.options.contains("mods") ? g_maps.options["mods"] : Json::array();
            bool changed = false;
            for (size_t i = 0; i < list.size(); ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(path_utf8(fs::path(utf8_to_wide(text_of(list[i]))).filename()).c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", text_of(list[i]).c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("REMOVE"))
                {
                    list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (ImGui::Button("ADD .FBMOD"))
            {
                const auto picked = pick_path(owner, false, {}, {{L"Mods (*.fbmod)", L"*.fbmod"}});
                if (!picked.empty())
                {
                    list.push_back(wide_to_utf8(picked));
                    changed = true;
                }
            }
            if (changed)
            {
                if (list.empty())
                    g_maps.options.erase("mods");
                else
                    g_maps.options["mods"] = list;
            }
        }

        // ---------------------------------------------------------------- commands

        std::string saved_game(const App &app)
        {
            return path_utf8(app.settings.game_root);
        }

        // game-root only when it is not the saved one, so the command line stays short.
        void add_game_root(const App &app, Json &args)
        {
            if (!g_maps.game_root.empty() && !same_path(g_maps.game_root, saved_game(app)))
                args["game-root"] = g_maps.game_root;
        }

        Json build_args(const App &app, bool deploy)
        {
            const auto &s = g_maps;
            Json args = {{"map", s.source}, {"staging-root", s.staging}};
            for (const auto &[name, value] : s.options.items())
                args[name] = value;
            if (s.kind == Kind::folder)
                args.erase("pause-map");
            if (deploy)
            {
                args["deploy"] = true;
                args["mod-folder"] = s.mod_folder;
            }
            add_game_root(app, args);
            return args;
        }

        Json install_args(const App &app)
        {
            Json args = {{"staging-dir", g_maps.staging}, {"mod-folder", g_maps.mod_folder}};
            add_game_root(app, args);
            return args;
        }

        // The engine keeps one Mods folder per map: a map that is already installed is updated where it
        // is, whatever mod-folder says. That folder, from the last mod list, or empty.
        std::string installed_path()
        {
            const auto &s = g_maps;
            if (s.kind == Kind::none || s.kind == Kind::invalid)
                return {};
            const std::string level = level_of(map_name(s.source, s.kind));
            const Json listed = outcome_of(s.mods);
            if (!listed.value("ok", false))
                return {};
            // A map mod registers its level assets in reskate-levels.json; mod list gives them as strings.
            for (const auto &m : listed["result"].value("mods", Json::array()))
            {
                if (!m.is_object())
                    continue;
                for (const auto &l : m.value("levels", Json::array()))
                    if (l.is_string() && l.get_ref<const std::string &>().find("/" + level + "/") != std::string::npos)
                        return m.value("path", "");
            }
            return {};
        }

        std::string install_target(const App &app)
        {
            const std::string existing = installed_path();
            if (!existing.empty())
                return existing;
            const std::string game = g_maps.game_root.empty() ? saved_game(app) : g_maps.game_root;
            return path_utf8(fs::path(utf8_to_wide(game)) / L"Mods" / utf8_to_wide(g_maps.mod_folder));
        }

        bool busy()
        {
            return job_running(g_maps.build) || job_running(g_maps.install);
        }

        // Checks the arguments the way the CLI would, so a bad value is named before anything starts.
        bool check(const Command &c, const Json &args, std::string &problem)
        {
            problem.clear();
            try
            {
                normalise_args(c, args);
                return true;
            }
            catch (const Error &e)
            {
                problem = e.what();
                return false;
            }
        }

        void refresh_mods(const App &app)
        {
            if (job_running(g_maps.mods))
                return;
            const Command *list = command("mod", "list");
            if (!list)
                return;
            Json args = Json::object();
            add_game_root(app, args);
            g_maps.mods = g_jobs.start(*list, args);
        }

        void start_inspect()
        {
            const Command *inspect = command("map", "inspect");
            if (!inspect || job_running(g_maps.inspect))
                return;
            g_maps.inspect = g_jobs.start(*inspect, Json{{"folder", path_utf8(folder_of(g_maps.source))}});
            g_maps.inspect_handled = false;
        }

        void start_build(const App &app, bool deploy)
        {
            auto &s = g_maps;
            const Command *compile = compile_command();
            if (!compile || busy())
                return;
            if (s.kind == Kind::none || s.kind == Kind::invalid)
            {
                s.problem = s.kind == Kind::none ? "Choose a scene or map folder first." : s.kind_problem;
                return;
            }
            const Json args = build_args(app, deploy);
            if (!check(*compile, args, s.problem))
                return;
            s.build = g_jobs.start(*compile, args);
            s.build_source = s.source;
            s.build_handled = false;
            s.deploys = deploy;
            s.log.clear();
            s.last_message.clear();
            s.reveal = 6;
            s.install = nullptr;
        }

        void start_install(const App &app)
        {
            auto &s = g_maps;
            const Command *deploy = command("mod", "deploy");
            if (!deploy || busy())
                return;
            const Json args = install_args(app);
            if (!check(*deploy, args, s.install_problem))
                return;
            s.install = g_jobs.start(*deploy, args);
            s.install_handled = false;
            s.install_reveal = 6;
        }

        void poll(const App &app)
        {
            auto &s = g_maps;
            if (s.build)
            {
                std::string message;
                {
                    std::lock_guard lock(s.build->mutex);
                    message = s.build->message;
                }
                if (!message.empty() && message != s.last_message)
                {
                    s.last_message = message;
                    s.log.push_back({s.build->seconds(), message});
                }
            }
            if (s.build && !s.build_handled && s.build->done.load())
            {
                s.build_handled = true;
                s.staging_checked = -100;
                const Json outcome = outcome_of(s.build);
                const bool ok = outcome.value("ok", false);
                Json triangles;
                if (ok && outcome["result"].contains("stats"))
                    triangles = outcome["result"]["stats"].value("triangles", Json());
                if (s.build->state() != JobState::cancelled)
                    remember(ok, triangles);
                if (ok && s.deploys)
                    refresh_mods(app);
                if (ok && s.install_after_build && !s.deploys)
                    s.confirm = true;
                s.install_after_build = false;
            }
            if (s.install && !s.install_handled && s.install->done.load())
            {
                s.install_handled = true;
                refresh_mods(app);
            }
            // Whether the build folder holds a Patch that can be installed; looked at once a second.
            const double now = ImGui::GetTime();
            if (now - s.staging_checked > 1.0)
            {
                s.staging_checked = now;
                std::error_code ec;
                s.staging_ready = !s.staging.empty() &&
                                  fs::is_regular_file(fs::path(utf8_to_wide(s.staging)) / L"Patch" / L"layout.toc", ec);
            }
            // A changed Skate folder gets its own mod list, a moment after the typing stops.
            if (s.mods && !job_running(s.mods) && now - s.game_root_changed > 0.8)
            {
                const std::string listed_root = s.mods->args.value("game-root", saved_game(app));
                const std::string wanted = s.game_root.empty() ? saved_game(app) : s.game_root;
                std::error_code ec;
                if (!same_path(listed_root, wanted) && fs::is_directory(fs::path(utf8_to_wide(wanted)), ec))
                    refresh_mods(app);
            }
            // Startup requests: --run builds once the page has its source, install=confirm asks to install.
            if (s.run_on_start && s.kind != Kind::none && s.kind != Kind::invalid)
            {
                s.run_on_start = false;
                start_build(app, false);
            }
            if (s.install_after_build && !s.build && s.staging_ready)
            {
                s.install_after_build = false;
                s.confirm = true;
            }
            if (s.kind == Kind::folder && !job_running(s.inspect))
            {
                bool stale = !s.inspect;
                if (s.inspect)
                {
                    const std::string ran = s.inspect->args.value("folder", "");
                    stale = !same_path(ran, path_utf8(folder_of(s.source)));
                }
                if (stale)
                    start_inspect();
            }
            // Once map inspect names a map folder, the defaults follow that name.
            if (s.kind == Kind::folder && s.inspect && s.inspect->done.load() && !s.inspect_handled)
            {
                s.inspect_handled = true;
                const std::string name = inspected_name(s.source);
                if (!name.empty())
                {
                    if (!s.staging_edited)
                        s.staging = default_staging(name);
                    if (!s.mod_folder_edited)
                        s.mod_folder = mod_name_from(name);
                }
            }
        }

        void start(App &app)
        {
            auto &s = g_maps;
            s.started = true;
            s.game_root = saved_game(app);
            load_recent();
            // A startup request (--page maps --arg ...) wins over the empty page.
            if (g_startup.is_object())
            {
                for (const auto &[key, value] : g_startup.items())
                {
                    const std::string text = text_of(value);
                    if (key == "map")
                        s.source = text;
                    else if (key == "staging-root")
                    {
                        s.staging = text;
                        s.staging_edited = true;
                    }
                    else if (key == "mod-folder")
                    {
                        s.mod_folder = text;
                        s.mod_folder_edited = true;
                    }
                    else if (key == "game-root")
                        s.game_root = text;
                    else if (key == "view")
                        s.scroll_to = text;
                    else if (key == "install")
                        s.install_after_build = text == "confirm";
                    else if (key == "preset")
                    {
                        for (const auto &p : presets())
                            if (text == p.key)
                                apply_preset(p);
                    }
                    else if (compile_command() && param_of(*compile_command(), key))
                    {
                        const Param *p = param_of(*compile_command(), key);
                        if (p->type == ParamType::Boolean)
                            set_option(key, text == "true" || text == "1" || text == "yes");
                        else if (p->type == ParamType::List)
                            set_option(key, value.is_array() ? value : Json::array({text}));
                        else
                            set_option(key, text);
                    }
                }
                g_startup = Json();
            }
            if (!s.source.empty())
                source_changed();
            s.run_on_start = g_startup_run;
            refresh_mods(app);
        }

        // ---------------------------------------------------------------- pieces of the page

        void stat_grid(const std::vector<std::pair<const char *, Json>> &stats)
        {
            const float cell = S(150);
            const int columns = std::max(2, static_cast<int>(ImGui::GetContentRegionAvail().x / cell));
            if (!ImGui::BeginTable("##stats", columns, ImGuiTableFlags_SizingStretchSame))
                return;
            for (const auto &[label, value] : stats)
            {
                ImGui::TableNextColumn();
                const bool zero = value.is_number() && value.get<double>() == 0;
                ImGui::PushFont(g_fonts.heading);
                ImGui::PushStyleColor(ImGuiCol_Text, zero ? color::muted : color::text);
                ImGui::TextUnformatted(number_text(value).c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
                caption(label);
                ImGui::Dummy(ImVec2(0, S(4)));
            }
            ImGui::EndTable();
        }

        const char *kind_label(Kind kind)
        {
            switch (kind)
            {
            case Kind::blend:
                return "BLENDER SCENE";
            case Kind::fbx:
                return "FBX SCENE";
            case Kind::folder:
                return "MAP FOLDER";
            default:
                return "";
            }
        }

        void start_scene_look()
        {
            const Command *look = command("map", "inspect-scene");
            if (!look || job_running(g_maps.scene_look))
                return;
            g_maps.scene_look = g_jobs.start(*look, Json{{"scene", g_maps.source}});
        }

        // What map inspect-scene found in a .blend / .fbx: counts, problems, then the objects and materials.
        void scene_look_view()
        {
            auto &s = g_maps;
            const bool for_this = s.scene_look && same_path(s.scene_look->args.value("scene", ""), s.source);
            if (for_this && job_running(s.scene_look))
            {
                draw_job(*s.scene_look, false);
                return;
            }
            if (ImGui::Button(for_this ? "LOOK AGAIN" : "LOOK INSIDE", ImVec2(S(150), 0)))
                start_scene_look();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Opens it in Blender and lists what a build would make of it. Writes nothing.");
            if (!for_this || !s.scene_look->done.load())
                return;
            const Json outcome = outcome_of(s.scene_look);
            if (!outcome.value("ok", false))
            {
                draw_job(*s.scene_look, true);
                return;
            }
            const Json &r = outcome["result"];
            const Json sum = r.value("summary", Json::object());
            const Json audio = sum.value("audio", Json());
            ImGui::Spacing();
            stat_grid({{"OBJECTS", sum.value("mesh_instances", Json())},
                       {"TRIANGLES", sum.value("triangles", Json())},
                       {"MATERIALS", sum.value("materials", Json())},
                       {"SPAWN POINTS", sum.value("spawn_points", Json())},
                       {"LIGHTS", sum.value("lights", Json())},
                       {"GRIND CURVES", sum.value("grind_curves", Json())},
                       {"NPC ROUTES", sum.value("npc_routes", Json())},
                       {"REFLECTION PROBES", sum.value("reflection_probes", Json())},
                       {"AUDIO EMITTERS", audio.is_object() ? audio.value("emitters", Json()) : Json()},
                       {"TRAVEL POINTS", sum.value("travel_points", Json())}});
            field("LEVEL NAME", r.value("map_name", ""));
            if (const Json b = r.value("bounds", Json());
                b.is_object() && b.contains("size") && b["size"].is_array() && b["size"].size() == 3)
            {
                char size[96];
                std::snprintf(size,
                              sizeof size,
                              "%.1f x %.1f x %.1f m (width x height x depth)",
                              b["size"][0].get<double>(),
                              b["size"][1].get<double>(),
                              b["size"][2].get<double>());
                field("SIZE", size);
            }
            const std::string pause = r.value("pause_map", "");
            field("PAUSE MAP",
                  pause == "authored"      ? "Your own map.png"
                  : pause == "skipped"     ? "Skipped"
                  : pause == "rendered_2d" ? "Rendered by Blender, 2D"
                                           : "Rendered by Blender, 3D");
            std::string modes;
            const Json collision_modes = sum.value("collision_modes", Json::object()); // named: items() only borrows it
            for (const auto &[mode, count] : collision_modes.items())
                modes += (modes.empty() ? "" : ", ") + std::to_string(count.is_number() ? count.get<long long>() : 0) +
                         " " + mode;
            if (!modes.empty())
                field("COLLISION", modes);
            for (const auto &e : r.value("errors", Json::array()))
                wrapped_colour(e.value("message", e.value("code", std::string())), color::danger);
            for (const auto &w : r.value("warnings", Json::array()))
                wrapped_colour(w.value("message", w.value("code", std::string())), color::warning);
            const Json objects = r.value("objects", Json::array());
            if (!objects.empty() &&
                ImGui::CollapsingHeader(("Objects (" + std::to_string(objects.size()) +
                                         (r.value("objects_truncated", false) ? "+" : "") + ")###scene_objects")
                                            .c_str()))
            {
                if (ImGui::BeginTable("##scene_objects", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableSetupColumn("Object", ImGuiTableColumnFlags_WidthStretch, 3);
                    ImGui::TableSetupColumn("Collision", ImGuiTableColumnFlags_WidthStretch, 1);
                    ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthStretch, 1);
                    ImGui::TableSetupColumn("Materials", ImGuiTableColumnFlags_WidthStretch, 3);
                    ImGui::TableHeadersRow();
                    for (const auto &o : objects)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(o.value("name", "").c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(o.value("collision_mode", "").c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(number_text(o.value("triangles", Json())).c_str());
                        std::string mats;
                        for (const auto &m : o.value("materials", Json::array()))
                            mats += (mats.empty() ? "" : ", ") + text_of(m);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(mats.c_str());
                    }
                    ImGui::EndTable();
                }
            }
            const Json materials = r.value("materials", Json::array());
            if (!materials.empty() &&
                ImGui::CollapsingHeader(
                    ("Materials (" + std::to_string(materials.size()) + ")###scene_materials").c_str()))
            {
                if (ImGui::BeginTable(
                        "##scene_materials", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthStretch, 3);
                    ImGui::TableSetupColumn("Surface", ImGuiTableColumnFlags_WidthStretch, 2);
                    ImGui::TableSetupColumn("Colour", ImGuiTableColumnFlags_WidthStretch, 1);
                    ImGui::TableSetupColumn("Objects", ImGuiTableColumnFlags_WidthStretch, 1);
                    ImGui::TableHeadersRow();
                    for (const auto &m : materials)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(
                            (m.value("name", "") + (m.value("invisible", false) ? "  (invisible)" : "")).c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(text_of(m.value("surface", Json(""))).c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(text_of(m.value("base_color", Json(""))).c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(number_text(m.value("objects", Json())).c_str());
                    }
                    ImGui::EndTable();
                }
            }
            caption(("SAME AS  " + s.scene_look->cli).c_str());
        }

        void scene_tile(App &app)
        {
            auto &s = g_maps;
            begin_tile("##maps_scene", 31);
            tile_title("SCENE");
            wrapped_muted("A .blend or .fbx scene, or a map folder the converter already made (it holds map.json). "
                          "Drop it anywhere on this page.");
            ImGui::BeginDisabled(busy());
            if (path_field("##source",
                           s.source,
                           app.window,
                           false,
                           {{L"Blender or FBX scene (*.blend, *.fbx)", L"*.blend;*.fbx"}}))
                source_changed();
            ImGui::EndDisabled();
            if (s.source != s.checked)
                source_changed();

            if (s.kind == Kind::invalid)
            {
                wrapped_colour(s.kind_problem, color::danger);
            }
            else if (s.kind != Kind::none)
            {
                auto *draw = ImGui::GetWindowDrawList();
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const std::string label = kind_label(s.kind);
                badge(draw, at, label, s.kind == Kind::folder ? color::good : color::blue, color::ink);
                ImGui::Dummy(ImVec2(badge_width(label), g_fonts.caption->FontSize + S(8)));
                ImGui::Spacing();
                const std::string name = map_name(s.source, s.kind);
                field(s.kind == Kind::folder ? "MAP NAME, FROM MAP.JSON" : "MAP NAME, FROM THE FILE NAME", name);
                field("LEVEL", level_of(name));
            }
            ImGui::Spacing();
            bold("Build folder");
            inline_caption("staging-root");
            wrapped_muted(
                "Where the built Patch goes. It is made if missing; a folder holding other files is refused.");
            ImGui::BeginDisabled(busy());
            const std::string before = s.staging;
            if (path_field("##staging", s.staging, app.window, true) || s.staging != before)
            {
                s.staging_edited = true;
                s.staging_checked = -100;
            }
            ImGui::EndDisabled();
            end_tile();
        }

        void preview_tile()
        {
            auto &s = g_maps;
            begin_tile("##maps_preview", 32);
            tile_title("WHAT IS IN IT");
            if (s.kind == Kind::none)
            {
                wrapped_muted("Choose or drop a scene to see what it holds. Nothing is written to Skate.");
            }
            else if (s.kind == Kind::invalid)
            {
                wrapped_muted("Nothing to show yet.");
            }
            else if (s.kind == Kind::folder)
            {
                if (job_running(s.inspect))
                {
                    ImGui::TextDisabled("Reading map.json...");
                }
                else if (s.inspect)
                {
                    const Json outcome = outcome_of(s.inspect);
                    if (outcome.value("ok", false))
                    {
                        const Json &r = outcome["result"];
                        stat_grid({{"OBJECTS", r.value("objects", Json())},
                                   {"MATERIALS", r.value("materials", Json())},
                                   {"MESH FILES", r.value("glb_files", Json())},
                                   {"TRIANGLES", r.value("triangles", Json())},
                                   {"VERTICES", r.value("vertices", Json())},
                                   {"COLLISION OBJECTS", r.value("collision_objects", Json())},
                                   {"COLLISION TRIANGLES", r.value("collision_triangles", Json())},
                                   {"COLLISION PIECES", r.value("collision_resources", Json())}});
                        caption(("SAME AS  " + s.inspect->cli).c_str());
                    }
                    else
                    {
                        wrapped_colour(outcome["error"].value("message", "map inspect failed"), color::danger);
                    }
                }
            }
            else
            {
                std::error_code ec;
                const fs::path path(utf8_to_wide(s.source));
                const auto bytes = fs::file_size(path, ec);
                if (!ec)
                    field("FILE", path_utf8(path.filename()) + "   " + std::to_string((bytes + 1023) / 1024) + " KB");
                scene_look_view();
                // The last build of this scene, if it is still on screen.
                const Json outcome = s.build && same_path(s.build_source, s.source) ? outcome_of(s.build) : Json();
                if (outcome.is_object() && outcome.value("ok", false))
                {
                    const Json stats = outcome["result"].value("stats", Json::object());
                    ImGui::Spacing();
                    caption("FROM THE LAST BUILD");
                    stat_grid({{"TRIANGLES", stats.value("triangles", Json())},
                               {"COLLISION PIECES", stats.value("collision_resources", Json())},
                               {"LIGHTS", stats.value("lights", Json())},
                               {"RENDER CELLS", stats.value("render_cells", Json())}});
                }
            }
            end_tile();
        }

        void options_tile(HWND owner)
        {
            auto &s = g_maps;
            if (s.scroll_to == "options" && s.scroll_frames > 0)
                ImGui::SetScrollHereY(0.0f);
            begin_tile("##maps_options", 33);
            {
                const ImVec2 at = ImGui::GetCursorPos();
                tile_title("BUILD OPTIONS");
                // Presets on the right of the title: they only set the options below.
                float width = 0;
                const float spacing = ImGui::GetStyle().ItemSpacing.x;
                for (const auto &p : presets())
                    width += ImGui::CalcTextSize(p.label).x + ImGui::GetStyle().FramePadding.x * 2 + spacing;
                width += ImGui::CalcTextSize("RESET").x + ImGui::GetStyle().FramePadding.x * 2;
                const float end_y = ImGui::GetCursorPosY();
                ImGui::SetCursorPos(ImVec2(at.x + ImGui::GetContentRegionAvail().x - width, at.y));
                ImGui::BeginDisabled(busy());
                for (const auto &p : presets())
                {
                    if (preset_active(p) ? primary_button(p.label) : ImGui::Button(p.label))
                        apply_preset(p);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", p.tip);
                    ImGui::SameLine();
                }
                ImGui::BeginDisabled(s.options.empty());
                if (ImGui::Button("RESET"))
                    s.options = Json::object();
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Leave every option to the engine");
                ImGui::EndDisabled();
                ImGui::SetCursorPosY(std::max(end_y, ImGui::GetCursorPosY()));
            }
            wrapped_muted("Presets only set the options below. Anything left at Default is the engine's own choice.");
            ImGui::Spacing();
            ImGui::BeginDisabled(busy());
            const bool scene = s.kind == Kind::blend || s.kind == Kind::fbx;
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(14), 0));
            if (ImGui::BeginTable(
                    "##option_columns", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX))
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                for (const auto &group : left_groups())
                    group_block(group, scene);
                mods_block(owner);
                ImGui::TableNextColumn();
                for (const auto &group : right_groups())
                    group_block(group, scene);
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
            ImGui::EndDisabled();
            end_tile();
        }

        struct Step
        {
            const char *label;
            bool shown;
        };

        // Which of the build's steps it is on: Blender's export, the level, the install.
        int current_step(const Job &job, bool scene, bool deploy)
        {
            double fraction;
            std::string message;
            {
                std::lock_guard lock(job.mutex);
                fraction = job.fraction;
                message = lower(job.message);
            }
            if (deploy && (message.find("deploy") != std::string::npos ||
                           message.find("install") != std::string::npos || message.find("mods\\") != std::string::npos))
                return 2;
            if (scene && fraction < 0.40 && message.find("[export] finished") == std::string::npos)
                return 0;
            return 1;
        }

        void build_progress(Job &job)
        {
            auto &s = g_maps;
            const float time = static_cast<float>(ImGui::GetTime());
            const bool scene = s.kind == Kind::blend || s.kind == Kind::fbx ||
                               lower(s.build_source).ends_with(".blend") || lower(s.build_source).ends_with(".fbx");
            const int step = current_step(job, scene, s.deploys);
            const Step steps[]{{scene ? "Reading the scene in Blender" : "Reading the map folder", true},
                               {"Building the level", true},
                               {"Installing into Skate", s.deploys}};
            const int count = s.deploys ? 3 : 2;
            auto *draw = ImGui::GetWindowDrawList();
            for (int i = 0; i < 3; ++i)
            {
                if (!steps[i].shown)
                    continue;
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const ImVec2 centre(at.x + S(10), at.y + g_fonts.bold->FontSize * 0.5f + S(1));
                if (i < step)
                    draw_status_icon(draw, centre, skate_theme::Icon::check, time);
                else if (i == step)
                    draw_status_icon(draw, centre, skate_theme::Icon::busy, time);
                else
                    draw->AddCircle(centre, S(8), color::muted, 0, S(2));
                ImGui::SetCursorScreenPos(ImVec2(at.x + S(28), at.y));
                ImGui::PushFont(g_fonts.bold);
                ImGui::PushStyleColor(ImGuiCol_Text, i <= step ? color::text : color::muted);
                ImGui::Text("Step %d of %d: %s", i + 1, count, steps[i].label);
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
            double fraction;
            std::string message;
            {
                std::lock_guard lock(job.mutex);
                fraction = job.fraction;
                message = job.message;
            }
            ImGui::Spacing();
            const float width = ImGui::GetContentRegionAvail().x;
            const ImVec2 a = ImGui::GetCursorScreenPos();
            progress_bar(draw, a, ImVec2(a.x + width, a.y + S(14)), static_cast<float>(std::max(0.0, fraction)), time);
            ImGui::Dummy(ImVec2(width, S(14)));
            // The export's own percentage while Blender runs; the whole build's after that.
            std::string detail = std::to_string(static_cast<int>(std::max(0.0, fraction) * 100)) + "%";
            if (step == 0 && scene)
                detail =
                    "Blender export " + std::to_string(static_cast<int>(std::max(0.0, fraction) / 0.40 * 100)) + "%";
            const std::string prefix = "Blender export: ";
            if (message.starts_with(prefix))
                message = message.substr(prefix.size());
            ImGui::PushFont(g_fonts.bold);
            ImGui::Text("%s", detail.c_str());
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", format_seconds(job.seconds()).c_str());
            wrapped_muted(message);
            ImGui::BeginDisabled(job.cancel.load());
            if (ImGui::Button(job.cancel.load() ? "CANCELLING..." : "CANCEL", ImVec2(S(130), 0)))
                job.cancel = true;
            ImGui::EndDisabled();
        }

        const char *hint_for(const std::string &code)
        {
            if (code == "blender_not_found")
                return "Set Blender in Settings, or pass the blender option.";
            if (code == "staging_not_owned")
                return "Pick an empty or new build folder.";
            if (code == "map_invalid")
                return "The map folder or the scene's export is not a valid map; the message says why.";
            if (code == "game_running")
                return "Close Skate and try again.";
            if (code == "not_a_game_root" || code == "game_root_missing")
                return "Point Settings at the Skate folder that holds Skate.exe.";
            if (code == "engine_missing")
                return "Point Settings at the engine folder from the ReSkate Studio zip.";
            if (code == "invalid_arguments")
                return "One of the options has a value the engine does not take.";
            return nullptr;
        }

        void build_result(Job &job)
        {
            auto &s = g_maps;
            const float time = static_cast<float>(ImGui::GetTime());
            const JobState state = job.state();
            const Json outcome = outcome_of(g_maps.build);
            auto *draw = ImGui::GetWindowDrawList();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const auto icon = state == JobState::ok          ? skate_theme::Icon::check
                              : state == JobState::cancelled ? skate_theme::Icon::warning
                                                             : skate_theme::Icon::fail;
            draw_status_icon(draw, ImVec2(at.x + S(10), at.y + g_fonts.bold->FontSize * 0.5f + S(1)), icon, time);
            ImGui::SetCursorScreenPos(ImVec2(at.x + S(28), at.y));
            ImGui::PushFont(g_fonts.bold);
            if (state == JobState::ok)
            {
                const Json &r = outcome["result"];
                ImGui::PushStyleColor(ImGuiCol_Text, color::good);
                ImGui::Text(r.value("deployed", false) ? "Built and installed in %s" : "Built in %s",
                            format_seconds(job.seconds()).c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
                std::string level_name, level_asset;
                if (r.contains("levels") && r["levels"].is_array() && !r["levels"].empty())
                {
                    const Json &level = r["levels"][0];
                    if (level.is_object())
                    {
                        level_name = level.value("displayName", "");
                        level_asset = level.value("asset", "");
                    }
                    else
                    {
                        level_asset = text_of(level);
                    }
                }
                if (!level_name.empty())
                    field("MAP", level_name);
                if (!level_asset.empty())
                    field("LEVEL", level_asset);
                ImGui::Spacing();
                const Json &stats = r.contains("stats") ? r["stats"] : Json::object();
                const auto value = [&](const char *key) { return stats.value(key, Json()); };
                stat_grid({{"TRIANGLES", value("triangles")},
                           {"RENDER CELLS", value("render_cells")},
                           {"DETAIL MESHES", value("lod_meshes")},
                           {"LIGHTS", value("lights")},
                           {"TIME-OF-DAY LIGHTS", value("tod_lights")},
                           {"COLLISION PIECES", value("collision_resources")},
                           {"GI PROBES", value("gi_probes")},
                           {"MERGED MODS", value("included_mods")}});
                const Json warnings = r.value("warnings", Json::array());
                if (!warnings.empty())
                {
                    ImGui::Spacing();
                    caption(("BUILD NOTES (" + std::to_string(warnings.size()) + ")").c_str());
                    for (const auto &w : warnings)
                    {
                        const ImVec2 p = ImGui::GetCursorScreenPos();
                        draw_status_icon(ImGui::GetWindowDrawList(),
                                         ImVec2(p.x + S(9), p.y + ImGui::GetTextLineHeight() * 0.5f),
                                         skate_theme::Icon::warning,
                                         time);
                        ImGui::SetCursorScreenPos(ImVec2(p.x + S(26), p.y));
                        ImGui::PushTextWrapPos(0);
                        ImGui::TextUnformatted(text_of(w).c_str());
                        ImGui::PopTextWrapPos();
                    }
                }
                ImGui::Spacing();
                field("BUILD FOLDER", r.value("patch_dir", s.staging));
                if (r.value("deployed", false) && r.contains("deploy_target"))
                {
                    field("INSTALLED AT", text_of(r["deploy_target"]));
                    wrapped_muted("Start Skate and pick \"" + (level_name.empty() ? s.mod_folder : level_name) +
                                  "\" from the map list.");
                }
                if (ImGui::Button("OPEN BUILD FOLDER"))
                    ShellExecuteW(nullptr,
                                  L"open",
                                  utf8_to_wide(r.value("staging_root", s.staging)).c_str(),
                                  nullptr,
                                  nullptr,
                                  SW_SHOWNORMAL);
                ImGui::SameLine();
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, state == JobState::cancelled ? color::warning : color::danger);
                if (state == JobState::cancelled)
                    ImGui::Text("Cancelled after %s", format_seconds(job.seconds()).c_str());
                else
                    ImGui::Text("Build failed: %s", outcome["error"].value("code", "error").c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
                if (state == JobState::failed)
                {
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextUnformatted(outcome["error"].value("message", "").c_str());
                    ImGui::PopTextWrapPos();
                    if (const char *hint = hint_for(outcome["error"].value("code", "")))
                        wrapped_muted(hint);
                }
            }
            if (ImGui::Button("COPY RESULT JSON"))
                copy_text((state == JobState::ok ? outcome["result"] : outcome["error"]).dump(2));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The same JSON studio-plus prints with --json");
            ImGui::SameLine();
            if (ImGui::Button("COPY COMMAND LINE"))
                copy_text(job.cli);
        }

        void build_log(Job &job)
        {
            auto &s = g_maps;
            std::vector<std::pair<std::string, std::string>> warnings;
            {
                std::lock_guard lock(job.mutex);
                warnings = job.log;
            }
            const std::string title = "Build log (" + std::to_string(s.log.size()) + ")###build_log";
            if (s.log.empty() || !ImGui::CollapsingHeader(title.c_str()))
                return;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(rgba(16, 16, 18, 0.9f)));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
            ImGui::BeginChild("##log_lines", ImVec2(0, S(220)), ImGuiChildFlags_AlwaysUseWindowPadding);
            for (const auto &line : s.log)
            {
                ImGui::TextDisabled("%7.1f s", line.at);
                ImGui::SameLine();
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(line.text.c_str());
                ImGui::PopTextWrapPos();
            }
            for (const auto &[level, message] : warnings)
            {
                ImGui::TextDisabled("%7s", level.c_str());
                ImGui::SameLine();
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(message.c_str());
                ImGui::PopTextWrapPos();
            }
            if (job_running(s.build) && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - S(4))
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        void build_tile(App &app)
        {
            auto &s = g_maps;
            if (s.scroll_to == "build" && s.scroll_frames > 0)
                ImGui::SetScrollHereY(0.0f);
            // A build just started, and again when it ends: bring its progress, then its result, into view.
            // The tiles above settle their height over the first frames, so the scroll repeats for a few.
            if (s.reveal > 0)
            {
                ImGui::SetScrollHereY(0.05f);
                if (--s.reveal == 0 && !(s.build && s.build->done.load()))
                    s.reveal = -1;
            }
            else if (s.reveal == -1 && s.build && s.build->done.load())
            {
                s.reveal = 6;
            }
            begin_tile("##maps_build", 34);
            tile_title("BUILD");
            const Command *compile = compile_command();
            if (!compile)
            {
                wrapped_colour("map compile is not in the registry.", color::danger);
                end_tile();
                return;
            }
            const bool running = job_running(s.build);
            if (!running)
            {
                wrapped_muted(
                    "BUILD makes the map in the build folder; nothing in Skate changes. BUILD & INSTALL also puts it "
                    "in Skate's Mods folder, after you confirm.");
                const bool ready = s.kind != Kind::none && s.kind != Kind::invalid;
                const std::string line = cli_line(*compile, build_args(app, false));
                ImGui::PushFont(g_fonts.caption);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("COMMAND LINE");
                ImGui::PopFont();
                ImGui::SameLine();
                copy_button("COPY##build_line", line);
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(ready ? line.c_str() : "studio-plus map compile <scene> <build folder>");
                ImGui::PopTextWrapPos();
                const bool can_build = ready && !busy();
                ImGui::BeginDisabled(!can_build);
                // Blue only when it can be pressed, as SAVE in Settings.
                if (can_build ? primary_button("BUILD", ImVec2(S(160), S(38)))
                              : ImGui::Button("BUILD", ImVec2(S(160), S(38))))
                    start_build(app, false);
                ImGui::SameLine();
                if (ImGui::Button("BUILD & INSTALL", ImVec2(S(190), S(38))))
                {
                    s.confirm = true;
                    s.confirm_build = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("map compile with deploy: builds, then installs into %s",
                                      install_target(app).c_str());
                ImGui::EndDisabled();
                if (!s.problem.empty())
                    wrapped_colour(s.problem, color::danger);
            }
            if (s.build)
            {
                ImGui::Spacing();
                if (running)
                    build_progress(*s.build);
                else
                    build_result(*s.build);
                ImGui::Spacing();
                build_log(*s.build);
            }
            end_tile();
        }

        void install_tile(App &app)
        {
            auto &s = g_maps;
            if ((s.scroll_to == "install" && s.scroll_frames > 0) || s.install_reveal > 0)
            {
                ImGui::SetScrollHereY(0.0f);
                if (s.install_reveal > 0)
                    --s.install_reveal;
            }
            begin_tile("##maps_install", 35);
            {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                tile_title("INSTALL INTO SKATE");
                const std::string pill = "CHANGES GAME FILES";
                badge(ImGui::GetWindowDrawList(),
                      ImVec2(at.x + ImGui::GetContentRegionAvail().x - badge_width(pill),
                             at.y + (g_fonts.heading->FontSize - g_fonts.caption->FontSize - S(8)) * 0.5f),
                      pill,
                      color::warning,
                      color::ink);
            }
            wrapped_muted(
                "Copies the built map into Skate's Mods folder, so it shows up in the game's map list. Skate must be "
                "closed. Installing again under the same name updates it.");
            ImGui::BeginDisabled(busy());
            bold("Skate folder");
            inline_caption(same_path(s.game_root, saved_game(app))
                               ? "from Settings, used to build and install"
                               : "not the one in Settings, used to build and install");
            if (path_field("##game_root", s.game_root, app.window, true))
                s.game_root_changed = ImGui::GetTime();
            bold("Mod folder name");
            inline_caption("mod-folder");
            wrapped_muted("1 to 64 letters, digits, spaces, _ - or . The map's folder under Mods.");
            ImGui::SetNextItemWidth(std::min(S(360), ImGui::GetContentRegionAvail().x));
            if (input_text("##mod_folder", s.mod_folder))
                s.mod_folder_edited = true;
            ImGui::EndDisabled();
            ImGui::Spacing();
            if (!s.mod_folder.empty())
                field("INSTALLS TO", install_target(app));

            // Whether this map, or a mod by this name, is already there, from mod list.
            const std::string existing = installed_path();
            const Json listed = outcome_of(s.mods);
            if (!existing.empty())
            {
                wrapped_colour("This map is already installed in " + existing +
                                   ". Installing updates it there (Skate keeps one "
                                   "folder per map); the old one is kept in .ReSkateStudio-Mod-backup.",
                               color::warning);
            }
            else if (listed.value("ok", false))
            {
                for (const auto &m : listed["result"].value("mods", Json::array()))
                    if (m.is_object() && lower(m.value("name", "")) == lower(s.mod_folder))
                    {
                        wrapped_colour("A mod called " + s.mod_folder +
                                           " is already installed. Installing replaces it; the old "
                                           "one is kept in .ReSkateStudio-Mod-backup.",
                                       color::warning);
                        break;
                    }
            }

            const bool can_install = s.staging_ready && !busy() && !s.mod_folder.empty();
            ImGui::BeginDisabled(!can_install);
            if (can_install ? primary_button("INSTALL", ImVec2(S(160), S(38)))
                            : ImGui::Button("INSTALL", ImVec2(S(160), S(38))))
            {
                s.confirm = true;
                s.confirm_build = false;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(s.staging_ready ? "mod deploy: installs the build folder's Patch"
                                                  : "Build the map first: the build folder has no Patch yet");
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            if (s.staging_ready)
                ImGui::TextDisabled("Installs the last build in %s", s.staging.c_str());
            else
                ImGui::TextDisabled("Build the map first.");
            if (!s.install_problem.empty())
                wrapped_colour(s.install_problem, color::danger);

            if (s.install)
            {
                ImGui::Spacing();
                const float time = static_cast<float>(ImGui::GetTime());
                auto *draw = ImGui::GetWindowDrawList();
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const JobState state = s.install->state();
                const auto icon = state == JobState::running     ? skate_theme::Icon::busy
                                  : state == JobState::ok        ? skate_theme::Icon::check
                                  : state == JobState::cancelled ? skate_theme::Icon::warning
                                                                 : skate_theme::Icon::fail;
                draw_status_icon(draw, ImVec2(at.x + S(10), at.y + g_fonts.bold->FontSize * 0.5f + S(1)), icon, time);
                ImGui::SetCursorScreenPos(ImVec2(at.x + S(28), at.y));
                const Json outcome = outcome_of(s.install);
                ImGui::BeginGroup();
                ImGui::PushFont(g_fonts.bold);
                if (state == JobState::running)
                {
                    ImGui::TextUnformatted("Installing...");
                    ImGui::PopFont();
                }
                else if (state == JobState::ok)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, color::good);
                    ImGui::TextUnformatted("Installed");
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                    const Json &r = outcome["result"];
                    std::string where = install_target(app);
                    for (const char *key : {"mod_path", "target"})
                        if (r.contains(key) && r[key].is_string())
                        {
                            where = r[key].get<std::string>();
                            break;
                        }
                    field("INSTALLED AT", where);
                    wrapped_muted("Start Skate and pick \"" + map_name(s.source, s.kind) + "\" from the map list.");
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, color::danger);
                    ImGui::Text("Install failed: %s", outcome["error"].value("code", "error").c_str());
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextUnformatted(outcome["error"].value("message", "").c_str());
                    ImGui::PopTextWrapPos();
                    if (const char *hint = hint_for(outcome["error"].value("code", "")))
                        wrapped_muted(hint);
                }
                if (state != JobState::running)
                {
                    if (ImGui::Button("COPY COMMAND LINE##install"))
                        copy_text(s.install->cli);
                }
                ImGui::EndGroup();
            }
            end_tile();
        }

        void recent_tile()
        {
            auto &s = g_maps;
            begin_tile("##maps_recent", 36);
            tile_title("RECENT MAPS");
            if (s.recent.empty())
            {
                wrapped_muted("Maps you build show up here. Click one to load it again with its options.");
            }
            else
            {
                const float width = ImGui::GetContentRegionAvail().x;
                int forget = -1;
                for (size_t i = 0; i < s.recent.size(); ++i)
                {
                    const auto &r = s.recent[i];
                    ImGui::PushID(static_cast<int>(i));
                    const ImVec2 at = ImGui::GetCursorScreenPos();
                    const float height = S(50);
                    const bool selected = same_path(r.source, s.source);
                    ImGui::BeginDisabled(busy());
                    const bool pressed = list_row("##recent", width, height, selected);
                    ImGui::EndDisabled();
                    if (ImGui::BeginPopupContextItem("##recent_menu"))
                    {
                        if (ImGui::MenuItem("Forget this map"))
                            forget = static_cast<int>(i);
                        if (ImGui::MenuItem("Copy path"))
                            ImGui::SetClipboardText(r.source.c_str());
                        ImGui::EndPopup();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Click to load it with its options; right-click to forget it");
                    auto *draw = ImGui::GetWindowDrawList();
                    const ImVec4 clip(at.x, at.y, at.x + width - S(120), at.y + height);
                    const fs::path path(utf8_to_wide(r.source));
                    const std::string name = path_utf8(path.has_extension() ? path.stem() : path.filename());
                    draw->AddText(g_fonts.bold,
                                  g_fonts.bold->FontSize,
                                  ImVec2(at.x + S(14), at.y + S(7)),
                                  color::text,
                                  name.c_str());
                    draw->AddText(g_fonts.caption,
                                  g_fonts.caption->FontSize,
                                  ImVec2(at.x + S(14), at.y + S(28)),
                                  color::muted,
                                  r.source.c_str(),
                                  nullptr,
                                  0,
                                  &clip);
                    const std::string pill = r.ok ? "BUILT" : "FAILED";
                    badge(draw,
                          ImVec2(at.x + width - badge_width(pill) - S(10), at.y + S(7)),
                          pill,
                          r.ok ? color::good : color::danger,
                          color::ink);
                    const float when_width =
                        g_fonts.caption->CalcTextSizeA(g_fonts.caption->FontSize, FLT_MAX, 0, r.when.c_str()).x;
                    draw->AddText(g_fonts.caption,
                                  g_fonts.caption->FontSize,
                                  ImVec2(at.x + width - when_width - S(10), at.y + S(28)),
                                  color::muted,
                                  r.when.c_str());
                    if (pressed)
                    {
                        s.source = r.source;
                        s.staging = r.staging;
                        s.staging_edited = true;
                        s.mod_folder = r.mod_folder;
                        s.mod_folder_edited = true;
                        s.options = r.options;
                        source_changed();
                    }
                    ImGui::PopID();
                }
                if (forget >= 0)
                {
                    s.recent.erase(s.recent.begin() + forget);
                    save_recent();
                }
            }
            end_tile();
        }

        void installed_tile(App &app)
        {
            auto &s = g_maps;
            begin_tile("##maps_installed", 37);
            tile_title("INSTALLED IN SKATE");
            if (job_running(s.mods))
            {
                ImGui::TextDisabled("Reading the Mods folder...");
            }
            else if (s.mods)
            {
                const Json outcome = outcome_of(s.mods);
                if (!outcome.value("ok", false))
                {
                    wrapped_colour(outcome["error"].value("message", "mod list failed"), color::danger);
                }
                else
                {
                    const Json &r = outcome["result"];
                    const Json mods = r.value("mods", Json::array());
                    if (!r.value("reskate_installed", true))
                        wrapped_colour("ReSkate is not installed in " +
                                           r.value("game_root", std::string("this folder")) + ", so no mod loads.",
                                       color::warning);
                    if (!r.value("present", false))
                        wrapped_muted("No Mods folder in " + r.value("game_root", std::string("the Skate folder")) +
                                      " yet.");
                    else if (mods.empty())
                        wrapped_muted("The Mods folder is empty.");
                    for (const auto &m : mods)
                    {
                        const std::string name = m.value("name", "");
                        ImGui::PushID(name.c_str());
                        bold(name.c_str());
                        std::string detail;
                        const Json levels = m.value("levels", Json());
                        if (levels.is_array())
                            for (const auto &level : levels)
                                detail += (detail.empty() ? "Map: " : ", ") + text_of(level);
                        if (detail.empty())
                            detail = m.value("studio_marker", false) ? "Built by ReSkate Studio"
                                                                     : "Not built by ReSkate Studio";
                        const std::string status = m.value("status", std::string());
                        if (!status.empty() && status != "loads")
                            detail += "  (" + status + ")";
                        inline_caption(detail.c_str());
                        ImGui::PopID();
                    }
                    caption(("SAME AS  " + s.mods->cli).c_str());
                }
            }
            ImGui::BeginDisabled(job_running(s.mods));
            if (ImGui::Button("REFRESH"))
                refresh_mods(app);
            ImGui::EndDisabled();
            end_tile();
        }

        void confirm_modal(App &app)
        {
            auto &s = g_maps;
            if (s.confirm && !ImGui::IsPopupOpen("##install_confirm"))
                ImGui::OpenPopup("##install_confirm");
            const ImVec2 size = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(S(600), 0));
            if (!ImGui::BeginPopupModal("##install_confirm",
                                        nullptr,
                                        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoResize))
                return;
            heading(s.confirm_build ? "BUILD AND INSTALL?" : "INSTALL INTO SKATE?");
            wrapped_muted("This writes into your Skate folder:");
            ImGui::PushFont(g_fonts.bold);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(install_target(app).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
            if (!installed_path().empty())
                wrapped_muted("This map is already installed there, so it is updated in place; the old copy is kept in "
                              ".ReSkateStudio-Mod-backup. Skate must be closed.");
            else
                wrapped_muted("A mod already there under this name is replaced; the old one is kept in "
                              ".ReSkateStudio-Mod-backup. "
                              "Skate must be closed.");
            const Command *c = s.confirm_build ? compile_command() : command("mod", "deploy");
            if (c)
            {
                ImGui::PushFont(g_fonts.caption);
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("SAME AS  %s",
                                    cli_line(*c, s.confirm_build ? build_args(app, true) : install_args(app)).c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopFont();
            }
            ImGui::Spacing();
            if (primary_button(s.confirm_build ? "BUILD & INSTALL" : "INSTALL", ImVec2(S(190), S(36))))
            {
                if (s.confirm_build)
                    start_build(app, true);
                else
                    start_install(app);
                s.confirm = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("CANCEL", ImVec2(S(130), S(36))) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                s.confirm = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        void two_columns(const char *id, const std::function<void()> &left, const std::function<void()> &right)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(7), 0));
            const bool table =
                ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX);
            ImGui::PopStyleVar();
            if (!table)
                return;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            left();
            ImGui::TableNextColumn();
            right();
            ImGui::EndTable();
        }

    } // namespace

    void maps_startup(const Json &args, bool run)
    {
        g_startup = args;
        g_startup_run = run;
    }

    void maps_page(App &app)
    {
        auto &s = g_maps;
        if (!s.started)
            start(app);
        poll(app);

        heading("CUSTOM MAPS");
        wrapped_muted(
            "Build a Blender scene into a Skate map, then install it. Nothing is written to Skate until you install.");
        ImGui::Spacing();
        two_columns("##maps_top", [&] { scene_tile(app); }, [&] { preview_tile(); });
        ImGui::Spacing();
        options_tile(app.window);
        ImGui::Spacing();
        build_tile(app);
        ImGui::Spacing();
        install_tile(app);
        ImGui::Spacing();
        if (s.scroll_to == "lists" && s.scroll_frames > 0)
            ImGui::SetScrollHereY(0.0f);
        two_columns("##maps_lists", [&] { recent_tile(); }, [&] { installed_tile(app); });
        confirm_modal(app);
        // A startup view scrolls for a few frames, until the tiles above it have their size.
        if (s.scroll_frames > 0 && --s.scroll_frames == 0)
            s.scroll_to.clear();
        else if (!s.scroll_to.empty() && s.scroll_frames == 0)
            s.scroll_frames = 6;
    }

} // namespace studio::gui
