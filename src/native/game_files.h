#pragma once
// Read access to the game's Data folder: superbundle TOCs, bundle manifests and payloads in the cas
// archives, decoded with the game's own Oodle. Built on ReSkate's engine code (third_party/reskate).
// Read only: nothing here opens a game file for writing.
#include <Engine/Resource/binary_bundle.h>
#include <Engine/Resource/toc.h>
#include <Engine/Vfs/game_archives.h>

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace studio::native
{

    namespace fb = dingosdk::frostbite;

    // Where a payload lives in cas.
    struct CasLocation
    {
        std::uint32_t install_chunk = 0;
        std::uint16_t archive = 0;
        bool patch = false;
        std::uint32_t offset = 0;
        std::uint32_t size = 0; // bytes as stored (usually compressed)

        static CasLocation from(const fb::BundleFileInfo &info)
        {
            return {info.location.installChunk, info.location.archive, info.location.patch, info.offset, info.size};
        }
    };

    // Opens the Data layer of `game_root` once and reads from it on any thread.
    class GameFiles
    {
      public:
        explicit GameFiles(std::filesystem::path game_root);
        ~GameFiles();
        GameFiles(const GameFiles&) = delete;
        GameFiles &operator=(const GameFiles&) = delete;

        const std::filesystem::path &game_root() const
        {
            return game_root_;
        }
        const std::filesystem::path &data_dir() const
        {
            return data_dir_;
        }

        // Every superbundle TOC under Data\Win32, relative to Data with forward slashes, sorted.
        std::vector<std::string> toc_files() const;
        fb::TocDocument read_toc(const std::string &relative) const;
        // A bundle's manifest and the payload location of each of its assets (ebx, then res, then chunks).
        struct Bundle
        {
            fb::BinaryBundle manifest;
            std::vector<fb::BundleFileInfo> payloads;
        };
        Bundle read_bundle(const fb::TocBundle &bundle) const;

        std::vector<std::byte> read_stored(const CasLocation &at) const; // as it is in the archive
        std::vector<std::byte> read(const CasLocation &at) const;        // decoded
        // "<package>/cas_NN.cas" for messages.
        std::string describe(const CasLocation &at) const;

      private:
        HANDLE archive(const CasLocation &at) const;

        std::filesystem::path game_root_, data_dir_;
        std::unique_ptr<dingosdk::vfs::GameArchives> archives_;
        mutable std::mutex mutex_;
        mutable std::map<std::wstring, HANDLE> handles_;
    };

    std::string hex32(std::uint32_t value); // "0x6BDE20BA"
    std::string hex64(std::uint64_t value); // "0x00000000DEADBEEF"
    std::string hex_bytes(const std::byte *data, std::size_t size);

} // namespace studio::native
