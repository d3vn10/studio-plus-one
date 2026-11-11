#include "native/game_files.h"

#include "core/registry.h"
#include "core/settings.h"

#include <Engine/Resource/cas_codec.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;

namespace studio::native
{

    std::string hex32(std::uint32_t value)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08X", value);
        return text;
    }

    std::string hex64(std::uint64_t value)
    {
        char text[24];
        std::snprintf(text, sizeof(text), "0x%016llX", static_cast<unsigned long long>(value));
        return text;
    }

    std::string hex_bytes(const std::byte *data, std::size_t size)
    {
        static const char digits[] = "0123456789abcdef";
        std::string out;
        out.reserve(size * 2);
        for (std::size_t i = 0; i < size; ++i)
        {
            const auto b = std::to_integer<unsigned>(data[i]);
            out.push_back(digits[b >> 4]);
            out.push_back(digits[b & 15]);
        }
        return out;
    }

    GameFiles::GameFiles(fs::path game_root) : game_root_(std::move(game_root)), data_dir_(game_root_ / L"Data")
    {
        std::error_code ec;
        if (!fs::exists(data_dir_ / L"layout.toc", ec))
            throw Error("game_data_missing",
                        "No Data\\layout.toc in " + path_utf8(game_root_) +
                            ". Point Studio+ at a folder with the game's Data folder.",
                        {{"path", path_utf8(game_root_)}});
        const auto layout = dingosdk::vfs::read_layout(data_dir_ / L"layout.toc");
        archives_ = std::make_unique<dingosdk::vfs::GameArchives>(data_dir_, layout.root);
    }

    GameFiles::~GameFiles()
    {
        for (auto &handle : handles_ | std::views::values)
            if (handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
    }

    std::vector<std::string> GameFiles::toc_files() const
    {
        std::vector<std::string> out;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(data_dir_ / L"Win32", ec), end; it != end && !ec; it.increment(ec))
        {
            std::error_code file_ec;
            if (!it->is_regular_file(file_ec))
                continue;
            auto ext = it->path().extension().wstring();
            std::ranges::transform(ext, ext.begin(), ::towlower);
            if (ext != L".toc")
                continue;
            out.push_back(path_utf8(fs::relative(it->path(), data_dir_, file_ec).generic_wstring()));
        }
        std::ranges::sort(out);
        return out;
    }

    fb::TocDocument GameFiles::read_toc(const std::string &relative) const
    {
        const auto path = data_dir_ / fs::path(utf8_to_wide(relative));
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("Cannot open " + path_utf8(path));
        std::vector<std::byte> bytes(static_cast<std::size_t>(fs::file_size(path)));
        if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            throw std::runtime_error("Cannot read " + path_utf8(path));
        return fb::read_toc(bytes);
    }

    GameFiles::Bundle GameFiles::read_bundle(const fb::TocBundle &bundle) const
    {
        auto [files, inlineManifest] = fb::read_bundle_region(bundle.region);
        Bundle result;
        if (!inlineManifest.empty())
        {
            result.manifest = fb::read_binary_bundle(inlineManifest);
            result.payloads = std::move(files);
            return result;
        }
        // The manifest is the region's first file, stored raw (or, rarely, compressed like a payload).
        if (files.empty())
            throw std::runtime_error("Bundle " + bundle.name + " has no manifest");
        const auto first = CasLocation::from(files.front());
        const auto raw = read_stored(first);
        try
        {
            result.manifest = fb::read_binary_bundle(raw);
        }
        catch (const std::exception &)
        {
            result.manifest = fb::read_binary_bundle(fb::decode_cas(raw, {game_root_}));
        }
        result.payloads.assign(files.begin() + 1, files.end());
        return result;
    }

    HANDLE GameFiles::archive(const CasLocation &at) const
    {
        if (at.patch)
            throw std::runtime_error("This payload is in the Patch layer, which Studio+ does not read yet");
        const auto path = data_dir_ / L"Win32" / fs::path(utf8_to_wide(archives_->directory(at.install_chunk))) /
                          fs::path(utf8_to_wide(dingosdk::vfs::archive_file(at.archive)));
        std::lock_guard lock(mutex_);
        auto [it, added] = handles_.try_emplace(path.wstring(), INVALID_HANDLE_VALUE);
        if (added)
        {
            // Read only, and shared so the game and other tools are never locked out.
            it->second = CreateFileW(path.c_str(),
                                     GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr,
                                     OPEN_EXISTING,
                                     FILE_FLAG_RANDOM_ACCESS,
                                     nullptr);
        }
        if (it->second == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot open " + path_utf8(path));
        return it->second;
    }

    std::vector<std::byte> GameFiles::read_stored(const CasLocation &at) const
    {
        HANDLE file = archive(at);
        std::vector<std::byte> bytes(at.size);
        std::size_t done = 0;
        while (done < bytes.size())
        {
            // A positional read: safe on a shared handle from many threads at once.
            OVERLAPPED where{};
            const std::uint64_t offset = std::uint64_t{at.offset} + done;
            where.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
            where.OffsetHigh = static_cast<DWORD>(offset >> 32);
            DWORD got = 0;
            const auto want = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 30));
            if (!ReadFile(file, bytes.data() + done, want, &got, &where) || got == 0)
                throw std::runtime_error("Cannot read " + describe(at));
            done += got;
        }
        return bytes;
    }

    std::vector<std::byte> GameFiles::read(const CasLocation &at) const
    {
        fb::CasDecodeOptions options;
        options.gameRoot = game_root_;
        options.maximumOutputSize = 1024ull * 1024ull * 1024ull;
        return fb::decode_cas(read_stored(at), options);
    }

    std::string GameFiles::describe(const CasLocation &at) const
    {
        return archives_->describe({at.patch, at.install_chunk, at.archive}) + " @" + std::to_string(at.offset);
    }

} // namespace studio::native
