#pragma once
#include "core/registry.h"

namespace studio
{
    // One function per file in src/commands. register_all_commands() in all.cpp calls each of them.
    void register_studio_commands(Registry &r);
    void register_asset_commands(Registry &r);    // asset_commands.cpp
    void register_map_commands(Registry &r);      // map_commands.cpp
    void register_mod_commands(Registry &r);      // mod_commands.cpp
    void register_cosmetic_commands(Registry &r); // cosmetic_commands.cpp
    void register_media_commands(Registry &r);    // media_commands.cpp
    void register_browse_commands(Registry &r);   // browse_commands.cpp

    // Shared checks, in media_commands.cpp.
    // Whether `child` is inside `parent` (case-insensitive, after resolving . and ..).
    bool path_inside(const std::filesystem::path &child, const std::filesystem::path &parent);
    // Throws Error("output_in_game_folder") when `output` is inside the ReSkate folder of this run or the saved one:
    // commands that only write their own output never change the game install.
    void refuse_game_folder(const Context &c,
                            const std::filesystem::path &game_root,
                            const std::filesystem::path &output,
                            const std::string &param);
} // namespace studio
