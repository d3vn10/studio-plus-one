#pragma once
#include "core/registry.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace studio
{
    std::wstring utf8_to_wide(std::string_view text);
    std::string wide_to_utf8(std::wstring_view text);
    std::string path_utf8(const std::filesystem::path &path);

    // Saved in %LOCALAPPDATA%\ReSkateStudioPlus\settings.json, or in %RSSP_DATA_DIR% when that is set.
    struct Settings
    {
        // The ReSkate folder: where ReSkateLauncher.exe, ReSkate.dll and Skate.exe are. The key stays
        // "game-root" for the CLI, the MCP tools and the settings file.
        std::filesystem::path game_root;
        std::filesystem::path blender;    // blender.exe
        std::filesystem::path engine_dir; // folder holding reskate_cli.exe and Native\ (from the ReSkate Studio zip)

        static std::filesystem::path data_dir();
        static std::filesystem::path file();
        static Settings load(); // also fills empty fields by auto-detection
        void save() const;
        Json to_json() const;
        static std::vector<std::string> keys();                             // game-root, blender, engine-dir
        void set(std::string_view key, const std::filesystem::path &value); // throws Error("unknown_setting")

        // Where reskate_cli.exe lives: engine_dir, else RSSP_ENGINE, else <exe>\engine.
        std::filesystem::path resolved_engine_dir() const;
        std::filesystem::path reskate_cli() const; // throws Error("engine_missing")
    };

    // What every user-facing text calls the game-root setting.
    inline constexpr std::string_view game_root_name = "ReSkate folder (where ReSkateLauncher.exe is)";

    // The one Skate build ReSkate supports. Ported from ReSkate's Engine/Game/Build/supported_build.h
    // (ReSkate is GPL, the same licence as Studio+).
    namespace supported_build
    {
        inline constexpr std::uint64_t game_file_size = 144567168;
        inline constexpr std::string_view game_sha256 =
            "fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9";
        inline constexpr std::string_view steam_build_id = "25414733";
    } // namespace supported_build

    // What a folder holds, as far as Studio+ cares: is it a ReSkate folder that mods load from?
    struct GameFolderCheck
    {
        enum class Build
        {
            not_checked,
            supported,
            different
        };

        std::filesystem::path folder;
        bool has_skate = false;       // Skate.exe
        bool has_reskate_dll = false; // ReSkate.dll
        bool has_launcher = false;    // ReSkateLauncher.exe
        bool has_mods = false;        // the Mods folder exists
        std::filesystem::path
            mods_dir;      // <folder>\Mods, or <folder>\ModData\Default\Mods when ModData exists (as ReSkate does)
        int mod_count = 0; // folders in Mods, not counting ReSkate's own dot folders (.reskate)
        std::uint64_t skate_size = 0;
        std::string skate_sha256; // empty unless hashed (only done when the size matches)
        Build build = Build::not_checked;

        bool is_reskate() const
        {
            return has_skate && has_reskate_dll;
        }
        std::string build_text() const; // one line about Skate.exe against the supported build
        Json to_json() const;
    };

    // Describes `folder`. With `hash`, Skate.exe is compared with the supported build: size first, then
    // SHA-256, cached by path, size and modified time in data_dir() so the 144 MB file is read once.
    GameFolderCheck check_game_folder(const std::filesystem::path &folder, bool hash = true);

    // The folder for a game-root value: the value itself, or the folder holding it when it is
    // ReSkateLauncher.exe, Skate.exe or ReSkate.dll. Throws Error("not_a_reskate_file") for any other
    // file. The result is absolute, uses backslashes and has the casing stored on disk.
    std::filesystem::path game_folder_from(const std::filesystem::path &value);

    // `path` made absolute and normal, with backslashes and the casing stored on disk for every part that exists.
    std::filesystem::path on_disk_path(const std::filesystem::path &path);

    std::filesystem::path executable_dir();
    // The best ReSkate folder found: ReSkate.dll + Skate.exe + Mods, then ReSkate.dll + Skate.exe, then
    // a plain Skate.exe (a Steam copy). Looks in the Steam libraries, and one folder deep in Downloads,
    // Desktop, Documents, %USERPROFILE%\Games, C:\Games, D:\Games and the root of each fixed drive.
    std::filesystem::path detect_game_root(); // empty if not found
    std::filesystem::path detect_blender();   // empty if not found
} // namespace studio
