#pragma once
// The asset index: every EBX asset, resource and chunk in the game's Data folder with its name, type,
// first bundle and sizes, built once (minutes) and cached in the data folder keyed by the Skate.exe
// hash and the TOC files, so searching it afterwards takes milliseconds.
#include "core/registry.h"
#include "native/game_files.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace studio::native
{

    enum class Kind : std::uint8_t
    {
        ebx = 0,
        res = 1,
        chunk = 2
    };
    std::string_view kind_name(Kind kind);                // "ebx", "res", "chunk"
    std::optional<Kind> kind_from(std::string_view text); // also "resource"

    // Groups of types the browser filters by. Derived from the type name, so they cost nothing to store.
    enum class Category : std::uint8_t
    {
        other,
        texture,
        mesh,
        lua,
        level,
        shader,
        animation,
        audio,
        video,
        blueprint
    };
    std::string_view category_name(Category category);
    std::optional<Category> category_from(std::string_view text);
    const std::vector<std::string_view> &category_names();

    // One asset. Plain data, written to the cache file as it is.
    struct Entry
    {
        std::uint32_t name_offset = 0; // into AssetIndex::strings
        std::uint16_t name_length = 0;
        Kind kind = Kind::ebx;
        std::uint8_t flags = 0;
        std::uint32_t type = 0;           // into AssetIndex::types; 0 is "unknown"
        std::uint32_t bundle = 0;         // the first bundle it was found in, into AssetIndex::bundles
        std::uint32_t bundle_count = 0;   // how many bundles carry it
        std::uint64_t size = 0;           // decoded bytes
        std::uint32_t resource_type = 0;  // res: the resource type hash
        std::uint32_t logical_offset = 0; // chunk: first byte this payload holds of the whole chunk
        std::uint64_t resource_id = 0;    // res
        std::array<std::byte, 16> guid{}; // ebx: file guid; chunk: chunk id
        std::array<std::byte, 16> meta{}; // res: resource metadata (texture headers need it)
        std::uint32_t install_chunk = 0;
        std::uint32_t offset = 0;
        std::uint32_t stored = 0; // bytes in the archive
        std::uint16_t archive = 0;
        std::uint8_t patch = 0;
        std::uint8_t reserved = 0;

        CasLocation location() const
        {
            return {install_chunk, archive, patch != 0, offset, stored};
        }
    };
    inline constexpr std::uint8_t entry_type_failed = 1; // ebx: the root type could not be read

    struct Bundle
    {
        std::string name;
        std::string superbundle; // the TOC it is in, relative to Data
    };

    struct TypeInfo
    {
        std::string name;
        Category category = Category::other;
    };

    class AssetIndex
    {
      public:
        std::string key;           // what the cache is valid for
        std::string game_root;     // UTF-8
        std::int64_t built_at = 0; // seconds since 1970
        double build_seconds = 0;
        std::vector<char> strings;
        std::vector<Entry> entries; // sorted by name, then kind
        std::vector<TypeInfo> types;
        std::vector<Bundle> bundles;
        std::size_t superbundles = 0;
        std::size_t type_failures = 0;

        std::string_view name(const Entry &e) const
        {
            return {strings.data() + e.name_offset, e.name_length};
        }
        // The name in lower case, for searching.
        std::string_view lower_name(const Entry &e) const
        {
            return {lower_.data() + e.name_offset, e.name_length};
        }
        const std::string &type_name(const Entry &e) const
        {
            return types[e.type].name;
        }
        Category category(const Entry &e) const
        {
            return types[e.type].category;
        }

        const Entry *find(Kind kind, std::string_view name) const;
        const Entry *find_chunk(const fb::Guid &id) const;
        const Entry *find_ebx_guid(const std::array<std::byte, 16> &guid) const;
        const Entry *find_resource_id(std::uint64_t id) const;
        std::size_t count(Kind kind) const;

        void finish(); // prepares searching after load or build; the lookup tables follow on first use

      private:
        void lookups() const;
        std::vector<char> lower_;
        mutable std::once_flag lookups_once_;
        mutable std::unordered_map<std::string_view, std::uint32_t> by_name_[3];
        mutable std::unordered_map<std::string, std::uint32_t> by_guid_[3];
        mutable std::unordered_map<std::uint64_t, std::uint32_t> by_resource_id_;
        std::size_t counts_[3]{};
    };

    // The cache file for a game folder and the key it must match. The key covers the Skate.exe SHA-256
    // and the size and time of layout.toc and every superbundle TOC.
    struct CacheKey
    {
        std::string key;
        std::string skate_sha256;
        std::filesystem::path file;
    };
    CacheKey cache_key(const std::filesystem::path &game_root);

    // The index for `game_root`: from memory, else from the cache file. Throws Error("asset_index_missing")
    // or Error("asset_index_stale") when it has to be built first (asset index).
    std::shared_ptr<const AssetIndex> load_index(Context &context, const std::filesystem::path &game_root);
    // Builds the index, saves it and keeps it in memory. With `refresh` false, a valid cache is used.
    std::shared_ptr<const AssetIndex> build_index(Context &context,
                                                  const std::filesystem::path &game_root,
                                                  bool refresh,
                                                  bool *from_cache = nullptr);
    // Opened game files for a game folder, shared by the commands of one process.
    std::shared_ptr<GameFiles> game_files(const std::filesystem::path &game_root);

    // Calls fn(i) for i in [0, count) on worker threads. Meanwhile, on the calling thread, `tick(done)` runs
    // a few times a second (progress belongs on this thread); returning true from it stops the work.
    // The first exception thrown by fn is rethrown here once every worker has stopped.
    void parallel_for(std::size_t count,
                      const std::function<void(std::size_t)> &fn,
                      const std::function<bool(std::size_t done)> &tick = nullptr);

} // namespace studio::native
