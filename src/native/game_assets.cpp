#include "native/game_assets.h"

#include "Engine/Core/Platform/path_text.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <stdexcept>

namespace studio::native
{
    namespace fs = std::filesystem;

    namespace
    {
        std::string lower(std::string text)
        {
            for (auto &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        struct Where
        {
            std::size_t toc = 0;
            std::size_t bundle = 0;
            std::size_t index = 0; // in the bundle's ebx / resources / chunks list
        };
    } // namespace

    std::string guid_text(const fb::Guid &id)
    {
        return id.string();
    }

    std::optional<fb::Guid> parse_guid(std::string_view text)
    {
        if (text.size() == 38 && text.front() == '{' && text.back() == '}')
            text = text.substr(1, 36);
        if (text.size() != 36)
            return std::nullopt;
        std::array<std::uint8_t, 16> canonical{};
        std::size_t at = 0;
        for (std::size_t i = 0; i < text.size();)
        {
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (text[i] != '-')
                    return std::nullopt;
                ++i;
                continue;
            }
            auto hex = [](char c) -> int
            {
                if (c >= '0' && c <= '9')
                    return c - '0';
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                return -1;
            };
            const int hi = hex(text[i]), lo = hex(text[i + 1]);
            if (hi < 0 || lo < 0)
                return std::nullopt;
            canonical[at++] = static_cast<std::uint8_t>(hi * 16 + lo);
            i += 2;
        }
        const std::array<std::size_t, 16> order{3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
        fb::Guid id;
        for (std::size_t i = 0; i < 16; ++i)
            id.bytes[i] = static_cast<std::byte>(canonical[order[i]]);
        return id;
    }

    struct GameAssets::Index
    {
        std::vector<std::string> toc_names;
        std::vector<fb::TocDocument> tocs;
        // bundles[toc][bundle], read lazily would need the region again, so they are kept.
        std::vector<std::vector<dingosdk::vfs::GameBundle>> bundles;
        std::map<std::string, std::vector<Where>> ebx, res;
        std::map<fb::Guid, std::vector<Where>> chunks;
        std::map<fb::Guid, std::pair<std::size_t, std::size_t>> toc_chunks; // toc, chunk index
    };

    GameAssets::GameAssets(fs::path game_root) : root_(std::move(game_root))
    {
    }
    GameAssets::~GameAssets() = default;
    GameAssets::GameAssets(GameAssets &&) noexcept = default;

    void GameAssets::scan()
    {
        if (index_)
            return;
        data_ = std::make_unique<dingosdk::vfs::GameData>(root_);
        auto index = std::make_unique<Index>();
        const fs::path data_dir = root_ / L"Data";
        std::vector<fs::path> files;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(data_dir / L"Win32", ec), end; it != end && !ec; it.increment(ec))
            if (it->is_regular_file(ec) && lower(dingosdk::path_utf8(it->path().extension())) == ".toc")
                files.push_back(it->path());
        std::sort(files.begin(), files.end());
        for (const auto &file : files)
        {
            const std::string relative = lower(fs::relative(file, data_dir, ec).generic_string());
            if (on_progress)
                on_progress("reading " + relative);
            fb::TocDocument toc;
            try
            {
                toc = data_->read_toc(relative);
            }
            catch (const std::exception &)
            {
                continue; // a TOC this reader does not take (not one with bundles)
            }
            const std::size_t t = index->tocs.size();
            std::vector<dingosdk::vfs::GameBundle> bundles;
            bundles.reserve(toc.bundles.size());
            for (std::size_t b = 0; b < toc.bundles.size(); ++b)
            {
                std::optional<dingosdk::vfs::GameBundle> bundle;
                try
                {
                    bundle = data_->read_bundle(toc, toc.bundles[b].name);
                }
                catch (const std::exception &)
                {
                }
                if (!bundle)
                    bundle.emplace();
                const auto &m = bundle->manifest;
                for (std::size_t i = 0; i < m.ebx.size(); ++i)
                    index->ebx[lower(m.ebx[i].name)].push_back({t, b, i});
                for (std::size_t i = 0; i < m.resources.size(); ++i)
                    index->res[lower(m.resources[i].name)].push_back({t, b, i});
                for (std::size_t i = 0; i < m.chunks.size(); ++i)
                    index->chunks[m.chunks[i].guid].push_back({t, b, i});
                bundles.push_back(std::move(*bundle));
            }
            for (std::size_t c = 0; c < toc.chunks.size(); ++c)
                if (!toc.chunks[c].removed)
                    index->toc_chunks.emplace(toc.chunks[c].guid, std::make_pair(t, c));
            index->toc_names.push_back(relative);
            index->tocs.push_back(std::move(toc));
            index->bundles.push_back(std::move(bundles));
        }
        index_ = std::move(index);
    }

    std::optional<GameAsset> GameAssets::find(fb::AssetKind kind, std::string_view name)
    {
        scan();
        const auto &table = kind == fb::AssetKind::ebx ? index_->ebx : index_->res;
        const auto it = table.find(lower(std::string(name)));
        if (it == table.end())
            return std::nullopt;
        for (const auto &w : it->second)
        {
            const auto &bundle = index_->bundles[w.toc][w.bundle];
            const auto *payload = bundle.payload(kind, w.index);
            if (!payload)
                continue;
            GameAsset out;
            out.kind = kind;
            out.meta = kind == fb::AssetKind::ebx ? bundle.manifest.ebx[w.index] : bundle.manifest.resources[w.index];
            out.name = out.meta.name;
            out.bundle = index_->tocs[w.toc].bundles[w.bundle].name;
            out.toc = index_->toc_names[w.toc];
            out.bytes = data_->read(*payload);
            return out;
        }
        return std::nullopt;
    }

    std::optional<GameAsset> GameAssets::find_chunk(const fb::Guid &id)
    {
        scan();
        if (const auto it = index_->chunks.find(id); it != index_->chunks.end())
        {
            for (const auto &w : it->second)
            {
                const auto &bundle = index_->bundles[w.toc][w.bundle];
                const auto *payload = bundle.payload(fb::AssetKind::chunk, w.index);
                if (!payload)
                    continue;
                GameAsset out;
                out.kind = fb::AssetKind::chunk;
                out.meta = bundle.manifest.chunks[w.index];
                out.name = guid_text(id);
                out.bundle = index_->tocs[w.toc].bundles[w.bundle].name;
                out.toc = index_->toc_names[w.toc];
                out.bytes = data_->read(*payload);
                return out;
            }
        }
        if (const auto it = index_->toc_chunks.find(id); it != index_->toc_chunks.end())
        {
            const auto &chunk = index_->tocs[it->second.first].chunks[it->second.second];
            GameAsset out;
            out.kind = fb::AssetKind::chunk;
            out.name = guid_text(id);
            out.meta.kind = fb::AssetKind::chunk;
            out.meta.guid = id;
            out.toc = index_->toc_names[it->second.first];
            out.bytes = data_->read(fb::BundleFileInfo{chunk.location, chunk.offset, chunk.size});
            out.meta.logicalSize = static_cast<std::uint32_t>(out.bytes.size());
            return out;
        }
        return std::nullopt;
    }

    std::vector<std::pair<std::string, std::uint32_t>> GameAssets::find_resources(std::string_view text,
                                                                                  std::uint32_t type,
                                                                                  std::size_t limit)
    {
        scan();
        const std::string needle = lower(std::string(text));
        std::vector<std::pair<std::string, std::uint32_t>> out;
        for (const auto &[name, where] : index_->res)
        {
            if (name.find(needle) == std::string::npos)
                continue;
            const auto &w = where.front();
            const auto &asset = index_->bundles[w.toc][w.bundle].manifest.resources[w.index];
            if (type && asset.resourceType != type)
                continue;
            out.emplace_back(asset.name, asset.resourceType);
            if (limit && out.size() >= limit)
                break;
        }
        return out;
    }

    std::vector<std::string> GameAssets::bundles_with(fb::AssetKind kind, std::string_view name)
    {
        scan();
        std::vector<std::string> out;
        const std::vector<Where> *where = nullptr;
        if (kind == fb::AssetKind::chunk)
        {
            const auto id = parse_guid(name);
            if (!id)
                return out;
            if (auto it = index_->chunks.find(*id); it != index_->chunks.end())
                where = &it->second;
        }
        else
        {
            const auto &table = kind == fb::AssetKind::ebx ? index_->ebx : index_->res;
            if (auto it = table.find(lower(std::string(name))); it != table.end())
                where = &it->second;
        }
        if (!where)
            return out;
        for (const auto &w : *where)
        {
            std::string bundle = index_->tocs[w.toc].bundles[w.bundle].name;
            if (std::find(out.begin(), out.end(), bundle) == out.end())
                out.push_back(std::move(bundle));
        }
        return out;
    }

} // namespace studio::native
