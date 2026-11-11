#pragma once

#include <filesystem>
#include <functional>
#include <cstddef>
#include <atomic>
#include <string>
#include <string_view>
#include <vector>

namespace studio::native
{
    struct ModDumpReport
    {
        std::filesystem::path directory;
        std::size_t bundles = 0, ebx = 0, resources = 0, chunks = 0, cas_blocks = 0, textures = 0, meshes = 0,
                    fbxs = 0, loose_files = 0, ebx_asset_references = 0, resolved_ebx_asset_references = 0;
        std::string fbx_status;
        std::vector<std::string> warnings;
    };

    using ModDumpProgress = std::function<void(std::string_view)>;

    // Writes a read-only extraction of a Frostbite mod folder with root layout.toc to <mod>/.dump.
    ModDumpReport dump_mod_folder(const std::filesystem::path &mod_directory,
                                  const std::filesystem::path &game_root,
                                  const std::filesystem::path &blender,
                                  const ModDumpProgress &progress = {},
                                  std::atomic<bool> *cancel = nullptr);
} // namespace studio::native
