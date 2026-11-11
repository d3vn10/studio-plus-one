#pragma once
// Writes the two mod formats Studio+ produces: ReSkate Studio's .fbproject ("RSPROJT1") and the
// Frosty binary .fbmod (version 6) that mod compile takes. Formats in docs/discovery/mesh-replace.md.
#include "Engine/Resource/binary_io.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace studio::native
{

    struct ModResource
    {
        enum class Kind : std::uint32_t
        {
            ebx = 1,
            res = 2,
            chunk = 3
        };
        Kind kind = Kind::res;
        bool added = false;                    // a new asset rather than a change to a game asset
        std::string name;                      // asset name; for a chunk, its id as text
        std::string user_data;                 // ReSkate Studio's tag, e.g. reskate-cosmetic-original-mesh:v1|donor=...
        std::uint32_t res_type = 0;            // res
        std::uint64_t res_rid = 0;             // res
        std::vector<std::byte> res_meta;       // res
        dingosdk::frostbite::Guid id;          // chunk id (or EBX guid)
        std::vector<std::string> bundles;      // bundles the asset is in
        std::vector<std::string> superbundles; // added chunks only
        std::vector<std::string> links;        // a MeshSet lists its chunk ids
        std::vector<std::byte> data;           // uncompressed payload
        // Chunk placement, as a project records it. logical_size 0 means the payload's size.
        std::uint32_t range_start = 0, range_end = 0, logical_offset = 0;
        std::uint64_t logical_size = 0;
        std::int32_t first_mip = -1;
    };

    struct ModInfo
    {
        std::string title, author, category, version = "1.0", description, link;
        std::uint32_t head = 625200; // the game build's head, as ReSkate Studio writes it for build 25414733
    };

    void write_fbproject(const std::filesystem::path &path,
                         const ModInfo &info,
                         const std::vector<ModResource> &resources);

    struct Project
    {
        ModInfo info;
        std::string profile;
        std::vector<ModResource> resources;
    };
    // Reads a ReSkate Studio .fbproject. Throws std::runtime_error when the file is not one, or is cut short.
    Project read_fbproject(const std::filesystem::path &path);
    // `game_root` lets payloads be Kraken-compressed with the game's own Oodle; without it (or if Oodle
    // fails) they are stored as raw blocks, which read back the same.
    void write_fbmod(const std::filesystem::path &path,
                     const ModInfo &info,
                     const std::vector<ModResource> &resources,
                     const std::filesystem::path &game_root);

    // Frosty's bundle name hash: h = 5381; h = (h * 33) ^ byte.
    std::uint32_t frosty_hash(std::string_view text);

} // namespace studio::native
