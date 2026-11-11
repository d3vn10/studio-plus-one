#pragma once
// Finds single assets in the installed game by name (EBX, resource) or id (chunk) and reads their bytes,
// using the TOC, bundle and CAS readers ported from ReSkate. Reads only; nothing in the game is changed.
#include "Engine/Vfs/game_bundles.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace studio::native
{

    namespace fb = dingosdk::frostbite;

    struct GameAsset
    {
        fb::AssetKind kind{};
        std::string name;             // EBX / resource name, or the chunk id as text
        std::string bundle;           // the first bundle it was found in ("" for a TOC-level chunk)
        std::string toc;              // that bundle's superbundle, relative to Data
        fb::BundleAsset meta;         // resource type / id / meta, chunk logical offset and size
        std::vector<std::byte> bytes; // decoded payload
    };

    // Text form of a chunk id, "6a76d5b0-f8e8-556c-454e-c038e7d63973" (Frostbite's mixed-endian order).
    std::string guid_text(const fb::Guid &id);
    // Parses guid_text; nullopt when it is not a GUID.
    std::optional<fb::Guid> parse_guid(std::string_view text);

    class GameAssets
    {
      public:
        // `game_root` is the folder with Skate.exe; its Data folder is read.
        explicit GameAssets(std::filesystem::path game_root);
        ~GameAssets();
        GameAssets(GameAssets &&) noexcept;

        // Called with a short message while the superbundles are scanned (once per GameAssets).
        std::function<void(std::string_view)> on_progress;

        // The resource / EBX called `name` (case-insensitive), or nullopt.
        std::optional<GameAsset> find(fb::AssetKind kind, std::string_view name);
        // The chunk with this id, from a bundle or a superbundle's chunk table.
        std::optional<GameAsset> find_chunk(const fb::Guid &id);
        // Resource names containing `text` (lower case), with their resource type, sorted; at most `limit`.
        std::vector<std::pair<std::string, std::uint32_t>> find_resources(std::string_view text,
                                                                          std::uint32_t type,
                                                                          std::size_t limit);
        // Every bundle that lists this asset (bundle names, lower case).
        std::vector<std::string> bundles_with(fb::AssetKind kind, std::string_view name);

        const std::filesystem::path &game_root() const
        {
            return root_;
        }

      private:
        struct Index;
        void scan();
        std::filesystem::path root_;
        std::filesystem::path rroot_;
        std::unique_ptr<dingosdk::vfs::GameData> data_;
        std::unique_ptr<Index> index_;
    };

} // namespace studio::native
