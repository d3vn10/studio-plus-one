// Studio+ itself: settings, health check, version and a raw pass-through to reskate_cli.
#include "commands/commands.h"
#include "core/engine.h"
#include "core/settings.h"
#include "version.h"
#include <filesystem>

namespace fs = std::filesystem;

namespace studio
{

    void register_studio_commands(Registry &r)
    {
        r.add({
            .group = "studio",
            .name = "version",
            .summary = "Show the Studio+ version",
            .description = "Prints the Studio+ version and where its files and settings live.",
            .examples = {"studio version"},
            .run = [](Context &c, const Json &) -> Json
            {
                return {{"version", STUDIO_PLUS_VERSION},
                        {"executable_dir", path_utf8(executable_dir())},
                        {"settings_file", path_utf8(Settings::file())},
                        {"engine_dir", path_utf8(c.settings.resolved_engine_dir())}};
            },
        });

        r.add({
            .group = "studio",
            .name = "settings",
            .summary = "Show the saved settings",
            .description =
                "Shows the ReSkate folder (where ReSkateLauncher.exe is, saved as game-root), Blender and the "
                "engine folder Studio+ will use. Empty values are auto-detected on every start (a ReSkate "
                "folder in Downloads, Desktop, Documents, a Games folder or a drive root, else the Steam copy; "
                "Program Files, Steam and RESKATE_BLENDER for Blender).",
            .examples = {"studio settings", "studio settings --json"},
            .run = [](Context &c, const Json &) -> Json
            {
                Json j = c.settings.to_json();
                j["engine-dir (resolved)"] = path_utf8(c.settings.resolved_engine_dir());
                j["file"] = path_utf8(Settings::file());
                return j;
            },
        });

        r.add({
            .group = "studio",
            .name = "set",
            .summary = "Change a setting",
            .description =
                "Saves one setting. Keys: game-root (the ReSkate folder, where ReSkateLauncher.exe, ReSkate.dll "
                "and Skate.exe are: give the folder or one of those three files), blender (blender.exe), "
                "engine-dir (folder with reskate_cli.exe). An empty value clears it, so it is auto-detected "
                "again. A folder without Skate.exe is refused; one without ReSkate.dll, or whose Skate.exe is "
                "not the build ReSkate supports, is saved with a warning.",
            .params =
                {
                    {"key",
                     ParamType::Enum,
                     "Which setting (game-root is the ReSkate folder, where ReSkateLauncher.exe is)",
                     true,
                     true,
                     {"game-root", "blender", "engine-dir"}},
                    {"value", ParamType::Path, "New value; empty clears it", false, true},
                },
            .examples = {"studio set game-root \"C:\\Users\\me\\Downloads\\ReSkate\"",
                         "studio set game-root \"C:\\Users\\me\\Downloads\\ReSkate\\ReSkateLauncher.exe\"",
                         "studio set blender \"C:\\Program Files\\Blender Foundation\\Blender 5.2\\blender.exe\""},
            .run = [](Context &c, const Json &a) -> Json
            {
                std::string key = a["key"];
                std::string value = arg_string(a, "value");
                // Explorer's "Copy as path" wraps the path in quotes, which no Windows path can contain.
                const auto blank = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
                while (!value.empty() && blank(value.back()))
                    value.pop_back();
                while (!value.empty() && blank(value.front()))
                    value.erase(value.begin());
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                    value = value.substr(1, value.size() - 2);
                fs::path path = utf8_to_wide(value);
                std::error_code ec;
                if (!value.empty() && !fs::exists(path, ec))
                    throw Error("path_not_found", "Path does not exist: " + value, {{"path", value}});
                Json warnings = Json::array();
                Json found;
                if (key == "game-root" && !value.empty())
                {
                    path = game_folder_from(path);
                    const GameFolderCheck folder = check_game_folder(path);
                    found = folder.to_json();
                    if (!folder.has_skate)
                        throw Error("not_a_game_root",
                                    "No Skate.exe in " + path_utf8(path) + ". Give your " +
                                        std::string(game_root_name) + ", or the launcher in it.",
                                    {{"path", path_utf8(path)}, {"found", found}});
                    if (!folder.has_reskate_dll && folder.has_launcher)
                        warnings.push_back(
                            "ReSkateLauncher.exe is here but ReSkate.dll is not, so mods built here will not "
                            "load until the launcher has set ReSkate up. Saved anyway.");
                    else if (!folder.has_reskate_dll)
                        warnings.push_back(
                            "No ReSkate.dll here: this is a plain Steam copy without ReSkate, so mods "
                            "built here will not load. Saved anyway; point Studio+ at your ReSkate folder "
                            "if you have one.");
                    else if (!folder.has_launcher)
                        warnings.push_back("No ReSkateLauncher.exe here, though ReSkate.dll is.");
                    if (folder.build == GameFolderCheck::Build::different)
                        warnings.push_back(folder.build_text() + ". ReSkate will not load mods into it. Saved anyway.");
                    else if (folder.build == GameFolderCheck::Build::not_checked)
                        warnings.push_back("Skate.exe could not be read to check its build.");
                }
                c.settings.set(key, path);
                c.settings.save();
                Json out = c.settings.to_json();
                if (!found.is_null())
                {
                    out["found"] = found;
                    out["warnings"] = warnings;
                    std::string message = "Saved the ReSkate folder: " + path_utf8(path) + ".";
                    for (const auto &w : warnings)
                        message += " Warning: " + w.get<std::string>();
                    out["message"] = message;
                }
                return out;
            },
        });

        r.add({
            .group = "studio",
            .name = "doctor",
            .summary = "Check that everything Studio+ needs is in place",
            .description =
                "Checks the ReSkate folder (Skate.exe, ReSkate.dll, the launcher, that Skate.exe is the build "
                "ReSkate supports, the Mods folder), Blender, and the engine (reskate_cli.exe and its Native "
                "folder), and says what to fix for anything missing.",
            .examples = {"studio doctor", "studio doctor --json"},
            .run = [](Context &c, const Json &) -> Json
            {
                std::error_code ec;
                Json checks = Json::array();
                auto check = [&](std::string name, bool ok, std::string detail, std::string fix)
                {
                    Json j = {{"check", name}, {"ok", ok}, {"detail", detail}};
                    if (!ok)
                        j["fix"] = fix;
                    checks.push_back(j);
                    return ok;
                };
                const fs::path game = c.settings.game_root;
                const GameFolderCheck folder = check_game_folder(game);
                const std::string set_game =
                    "studio-plus studio set game-root <your " + std::string(game_root_name) + ">";
                check("game-root",
                      folder.has_skate,
                      game.empty()       ? "not found"
                      : folder.has_skate ? path_utf8(game)
                                         : path_utf8(game) + " (no Skate.exe in it)",
                      "Point Studio+ at your " + std::string(game_root_name) + ": " + set_game);
                if (folder.has_skate)
                {
                    if (folder.has_launcher)
                        check("reskate",
                              folder.has_reskate_dll,
                              folder.has_reskate_dll ? "ReSkate.dll is next to Skate.exe"
                                                     : "No ReSkate.dll, though ReSkateLauncher.exe is here",
                              "ReSkate is not set up in this folder: mods built here will not load. Start "
                              "ReSkateLauncher.exe once so it installs ReSkate, or point Studio+ at your ReSkate "
                              "folder: " +
                                  set_game);
                    else
                        check(
                            "reskate",
                            folder.has_reskate_dll,
                            folder.has_reskate_dll ? "ReSkate.dll is next to Skate.exe"
                                                   : "No ReSkate.dll: this is a plain Steam copy without ReSkate",
                            "This is a plain Steam copy without ReSkate: mods built here will not load; point Studio+ "
                            "at your ReSkate folder: " +
                                set_game);
                    check("game-build",
                          folder.build == GameFolderCheck::Build::supported,
                          folder.build_text(),
                          folder.build == GameFolderCheck::Build::not_checked
                              ? "Skate.exe could not be read; check it is not locked or damaged"
                              : "ReSkate supports Skate build " + std::string(supported_build::steam_build_id) +
                                    " only: let ReSkateLauncher.exe set it up, or point Studio+ at the ReSkate folder "
                                    "that has it: " +
                                    set_game);
                    check("launcher",
                          folder.has_launcher,
                          folder.has_launcher ? path_utf8(game / L"ReSkateLauncher.exe")
                                              : "No ReSkateLauncher.exe in this folder",
                          "ReSkateLauncher.exe starts Skate with ReSkate. Point Studio+ at the folder it is in: " +
                              set_game);
                    check("mods",
                          true,
                          folder.has_mods ? path_utf8(folder.mods_dir) + ": " + std::to_string(folder.mod_count) +
                                                (folder.mod_count == 1 ? " mod" : " mods")
                                          : "No Mods folder yet. ReSkate makes it when the first mod is installed.",
                          "");
                }
                fs::path engine = c.settings.resolved_engine_dir();
                bool cli = fs::exists(engine / L"reskate_cli.exe", ec);
                check("engine",
                      cli,
                      path_utf8(engine / L"reskate_cli.exe"),
                      "studio-plus studio set engine-dir <folder with reskate_cli.exe>");
                check("engine-native",
                      fs::exists(engine / L"Native" / L"Blender" / L"studio_map_import.py", ec),
                      path_utf8(engine / L"Native"),
                      "The engine folder needs the Native folder from the ReSkate Studio zip");
                fs::path blender = c.settings.blender;
                check("blender",
                      !blender.empty() && fs::exists(blender, ec),
                      blender.empty() ? "not found (only needed for .blend and .fbx maps)" : path_utf8(blender),
                      "studio-plus studio set blender <path to blender.exe>");
                bool all_ok = true;
                for (const auto &j : checks)
                    all_ok = all_ok && j["ok"].get<bool>();
                Json out = {{"ok", all_ok}, {"checks", checks}};
                if (!game.empty())
                    out["game_folder"] = folder.to_json();
                return out;
            },
        });

        r.add({
            .group = "studio",
            .name = "raw",
            .summary = "Run reskate_cli directly with your own arguments",
            .description = "Escape hatch: passes the arguments straight to reskate_cli.exe and returns its output. "
                           "Prefer the named commands, which check arguments and return structured results.",
            .params = {{"args", ParamType::List, "Arguments for reskate_cli, in order", true, true}},
            .examples = {"studio raw find-name \"C:\\...\\Skate\" baker_popsicle 5"},
            .long_running = true,
            .run = [](Context &c, const Json &a) -> Json
            {
                std::vector<std::string> args;
                for (const auto &v : a["args"])
                    args.push_back(v.get<std::string>());
                EngineRun run = run_engine(c, args);
                return {{"exit_code", run.exit_code},
                        {"stdout", run.out_lines},
                        {"stderr", run.err_lines},
                        {"seconds", run.seconds}};
            },
        });
    }
} // namespace studio
