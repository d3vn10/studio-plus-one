#include "native/asset_index.h"

#include "core/settings.h"

#include <Engine/Resource/ebx_document.h>
#include <Engine/Resource/texture.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <numeric>
#include <thread>

namespace fs = std::filesystem;

namespace studio::native
{

    // ---------------------------------------------------------------- names

    std::string_view kind_name(Kind kind)
    {
        switch (kind)
        {
        case Kind::ebx:
            return "ebx";
        case Kind::res:
            return "res";
        default:
            return "chunk";
        }
    }

    std::optional<Kind> kind_from(std::string_view text)
    {
        if (text == "ebx")
            return Kind::ebx;
        if (text == "res" || text == "resource")
            return Kind::res;
        if (text == "chunk")
            return Kind::chunk;
        return std::nullopt;
    }

    const std::vector<std::string_view> &category_names()
    {
        static const std::vector<std::string_view> names{
            "other", "texture", "mesh", "lua", "level", "shader", "animation", "audio", "video", "blueprint"};
        return names;
    }

    std::string_view category_name(Category category)
    {
        return category_names()[static_cast<std::size_t>(category)];
    }

    std::optional<Category> category_from(std::string_view text)
    {
        const auto &names = category_names();
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == text)
                return static_cast<Category>(i);
        return std::nullopt;
    }

    namespace
    {

        bool contains(std::string_view text, std::string_view part)
        {
            return text.find(part) != std::string_view::npos;
        }

        Category category_of(std::string_view type)
        {
            if (type.empty())
                return Category::other;
            if (contains(type, "Movie"))
                return Category::video;
            if (contains(type, "Shader"))
                return Category::shader;
            if (contains(type, "Texture"))
                return Category::texture;
            if (type.ends_with("MeshAsset") || type == "MeshSet")
                return Category::mesh;
            if (contains(type, "Lua"))
                return Category::lua;
            if (type == "LevelData" || type == "SubWorldData" || type == "LevelDescriptionAsset")
                return Category::level;
            if (contains(type, "Shader"))
                return Category::shader;
            if (contains(type, "Anim") || type.starts_with("Ant"))
                return Category::animation;
            if (contains(type, "Wave") || contains(type, "Sound") || contains(type, "Audio"))
                return Category::audio;
            if (type.ends_with("Blueprint"))
                return Category::blueprint;
            return Category::other;
        }

        // Resource type hashes with a known name (from ReSkate's engine and Frostbite tooling).
        std::string resource_type_name(std::uint32_t type)
        {
            switch (type)
            {
            case 0x6BDE20BA:
                return "Texture";
            case 0x49B156D4:
                return "MeshSet";
            case 0xEC383B87:
                return "LuaScript";
            case 0xE2E6955B:
                return "ShaderProgramLookup";
            case 0x2D254A89:
                return "ShaderTextureLookup";
            case 0x41759364:
                return "PhysicsResource";
            default:
                return "Resource " + hex32(type);
            }
        }

        std::uint64_t fnv64(std::string_view text, std::uint64_t hash = 0xcbf29ce484222325ull)
        {
            for (unsigned char c : text)
                hash = (hash ^ c) * 0x100000001b3ull;
            return hash;
        }

        // Whether a chunk copy starting at `logical_offset` with `stored` bytes holds more of the chunk than `old`:
        // one that starts at the beginning wins, then the bigger one.
        bool more_complete(std::uint32_t logical_offset, std::uint32_t stored, const Entry &old)
        {
            const auto start = logical_offset & 0xFFFF0000u, old_start = old.logical_offset & 0xFFFF0000u;
            if (start != old_start)
                return start < old_start;
            return stored > old.stored;
        }

        std::string guid_key(const std::array<std::byte, 16> &guid)
        {
            return std::string(reinterpret_cast<const char *>(guid.data()), guid.size());
        }

        // ---------------------------------------------------------------- cache file

        constexpr char magic[8] = {'R', 'S', 'S', 'P', 'I', 'D', 'X', '1'};
        constexpr std::uint32_t format_version = 4;

        struct Writer
        {
            std::ofstream out;
            void raw(const void *data, std::size_t size)
            {
                out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
            }
            template <class T> void pod(const T &value)
            {
                raw(&value, sizeof(T));
            }
            void text(const std::string &value)
            {
                pod(static_cast<std::uint64_t>(value.size()));
                raw(value.data(), value.size());
            }
        };

        struct Reader
        {
            std::vector<char> data;
            std::size_t at = 0;
            void raw(void *out, std::size_t size)
            {
                if (size > data.size() - at)
                    throw std::runtime_error("The asset index cache is truncated");
                std::memcpy(out, data.data() + at, size);
                at += size;
            }
            template <class T> T pod()
            {
                T value{};
                raw(&value, sizeof(T));
                return value;
            }
            std::uint64_t count(std::size_t item_size)
            {
                const auto n = pod<std::uint64_t>();
                if (item_size && n > (data.size() - at) / item_size)
                    throw std::runtime_error("The asset index cache is damaged");
                return n;
            }
            std::string text()
            {
                const auto n = count(1);
                std::string value(data.data() + at, static_cast<std::size_t>(n));
                at += static_cast<std::size_t>(n);
                return value;
            }
        };

        std::vector<char> read_file(const fs::path &path, std::size_t limit = SIZE_MAX)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                throw std::runtime_error("Cannot open " + path_utf8(path));
            in.seekg(0, std::ios::end);
            const auto size = std::min<std::size_t>(static_cast<std::size_t>(in.tellg()), limit);
            in.seekg(0);
            std::vector<char> data(size);
            in.read(data.data(), static_cast<std::streamsize>(size));
            return data;
        }

        void save(const AssetIndex &index, const fs::path &file)
        {
            std::error_code ec;
            fs::create_directories(file.parent_path(), ec);
            const auto temp = fs::path(file).concat(L".tmp");
            {
                Writer w{std::ofstream(temp, std::ios::binary | std::ios::trunc)};
                if (!w.out)
                    throw Error("cache_write_failed", "Cannot write " + path_utf8(temp));
                w.raw(magic, sizeof(magic));
                w.pod(format_version);
                w.text(index.key);
                w.text(index.game_root);
                w.pod(index.built_at);
                w.pod(index.build_seconds);
                w.pod(static_cast<std::uint64_t>(index.superbundles));
                w.pod(static_cast<std::uint64_t>(index.type_failures));
                w.pod(static_cast<std::uint64_t>(index.types.size()));
                for (const auto &t : index.types)
                    w.text(t.name);
                w.pod(static_cast<std::uint64_t>(index.bundles.size()));
                for (const auto &b : index.bundles)
                {
                    w.text(b.name);
                    w.text(b.superbundle);
                }
                w.pod(static_cast<std::uint64_t>(index.strings.size()));
                w.raw(index.strings.data(), index.strings.size());
                w.pod(static_cast<std::uint64_t>(index.entries.size()));
                w.raw(index.entries.data(), index.entries.size() * sizeof(Entry));
                if (!w.out)
                    throw Error("cache_write_failed", "Cannot write " + path_utf8(temp));
            }
            fs::rename(temp, file, ec);
            if (ec)
                throw Error("cache_write_failed", "Cannot replace " + path_utf8(file) + ": " + ec.message());
        }

        // The key stored in a cache file, without reading the rest of it.
        std::string stored_key(const fs::path &file)
        {
            Reader r{read_file(file, 4096)};
            char m[sizeof(magic)];
            r.raw(m, sizeof(m));
            if (std::memcmp(m, magic, sizeof(magic)) != 0 || r.pod<std::uint32_t>() != format_version)
                return {};
            return r.text();
        }

        std::shared_ptr<AssetIndex> load(const fs::path &file)
        {
            Reader r{read_file(file)};
            char m[sizeof(magic)];
            r.raw(m, sizeof(m));
            if (std::memcmp(m, magic, sizeof(magic)) != 0 || r.pod<std::uint32_t>() != format_version)
                throw std::runtime_error("Not a Studio+ asset index: " + path_utf8(file));
            auto index = std::make_shared<AssetIndex>();
            index->key = r.text();
            index->game_root = r.text();
            index->built_at = r.pod<std::int64_t>();
            index->build_seconds = r.pod<double>();
            index->superbundles = static_cast<std::size_t>(r.pod<std::uint64_t>());
            index->type_failures = static_cast<std::size_t>(r.pod<std::uint64_t>());
            index->types.resize(static_cast<std::size_t>(r.count(8)));
            for (auto &t : index->types)
            {
                t.name = r.text();
                t.category = category_of(t.name);
            }
            index->bundles.resize(static_cast<std::size_t>(r.count(16)));
            for (auto &b : index->bundles)
            {
                b.name = r.text();
                b.superbundle = r.text();
            }
            index->strings.resize(static_cast<std::size_t>(r.count(1)));
            r.raw(index->strings.data(), index->strings.size());
            index->entries.resize(static_cast<std::size_t>(r.count(sizeof(Entry))));
            r.raw(index->entries.data(), index->entries.size() * sizeof(Entry));
            for (const auto &e : index->entries)
                if (e.type >= index->types.size() || e.bundle >= std::max<std::size_t>(1, index->bundles.size()) ||
                    std::size_t{e.name_offset} + e.name_length > index->strings.size())
                    throw std::runtime_error("The asset index cache is damaged: " + path_utf8(file));
            index->finish();
            return index;
        }

        // ---------------------------------------------------------------- the copy in memory

        struct Loaded
        {
            std::mutex mutex;
            std::string key;
            std::shared_ptr<const AssetIndex> index;
        };
        Loaded &loaded()
        {
            static Loaded value;
            return value;
        }

        std::int64_t file_time(const fs::path &path)
        {
            std::error_code ec;
            return static_cast<std::int64_t>(fs::last_write_time(path, ec).time_since_epoch().count());
        }

    } // namespace

    // ---------------------------------------------------------------- AssetIndex

    void AssetIndex::finish()
    {
        lower_ = strings;
        for (auto &c : lower_)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c + ('a' - 'A'));
        for (auto &n : counts_)
            n = 0;
        for (const auto &e : entries)
            ++counts_[static_cast<std::size_t>(e.kind)];
    }

    void AssetIndex::lookups() const
    {
        std::call_once(lookups_once_,
                       [this]
                       {
                           for (std::size_t k = 0; k < 3; ++k)
                               by_name_[k].reserve(counts_[k]);
                           static const std::array<std::byte, 16> zero{};
                           for (std::uint32_t i = 0; i < entries.size(); ++i)
                           {
                               const auto &e = entries[i];
                               const auto k = static_cast<std::size_t>(e.kind);
                               by_name_[k].emplace(lower_name(e), i);
                               if (e.kind != Kind::res)
                               {
                                   if (e.guid != zero)
                                       by_guid_[k].emplace(guid_key(e.guid), i);
                               }
                               else if (e.resource_id)
                               {
                                   by_resource_id_.emplace(e.resource_id, i);
                               }
                           }
                       });
    }

    const Entry *AssetIndex::find(Kind kind, std::string_view text) const
    {
        std::string lower(text);
        for (auto &c : lower)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c + ('a' - 'A'));
        lookups();
        const auto &map = by_name_[static_cast<std::size_t>(kind)];
        const auto it = map.find(lower);
        return it == map.end() ? nullptr : &entries[it->second];
    }

    const Entry *AssetIndex::find_chunk(const fb::Guid &id) const
    {
        lookups();
        const auto it = by_guid_[2].find(guid_key(id.bytes));
        return it == by_guid_[2].end() ? nullptr : &entries[it->second];
    }

    const Entry *AssetIndex::find_ebx_guid(const std::array<std::byte, 16> &guid) const
    {
        lookups();
        const auto it = by_guid_[0].find(guid_key(guid));
        return it == by_guid_[0].end() ? nullptr : &entries[it->second];
    }

    const Entry *AssetIndex::find_resource_id(std::uint64_t id) const
    {
        lookups();
        const auto it = by_resource_id_.find(id);
        return it == by_resource_id_.end() ? nullptr : &entries[it->second];
    }

    std::size_t AssetIndex::count(Kind kind) const
    {
        return counts_[static_cast<std::size_t>(kind)];
    }

    // ---------------------------------------------------------------- helpers

    void parallel_for(std::size_t count,
                      const std::function<void(std::size_t)> &fn,
                      const std::function<bool(std::size_t)> &tick)
    {
        const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 2u, 16u);
        std::atomic<std::size_t> next{0}, done{0};
        std::atomic<bool> stop{false};
        std::exception_ptr failure;
        std::mutex failure_mutex;
        std::vector<std::thread> workers;
        for (unsigned t = 0; t < threads; ++t)
            workers.emplace_back(
                [&]
                {
                    while (!stop.load())
                    {
                        const auto i = next.fetch_add(1);
                        if (i >= count)
                            break;
                        try
                        {
                            fn(i);
                        }
                        catch (...)
                        {
                            std::lock_guard lock(failure_mutex);
                            if (!failure)
                                failure = std::current_exception();
                            stop = true;
                        }
                        done.fetch_add(1);
                    }
                });
        while (done.load() < count && !stop.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (tick && tick(done.load()))
                stop = true;
        }
        for (auto &w : workers)
            w.join();
        if (failure)
            std::rethrow_exception(failure);
    }

    std::shared_ptr<GameFiles> game_files(const fs::path &game_root)
    {
        static std::mutex mutex;
        static fs::path open_root;
        static std::shared_ptr<GameFiles> open;
        std::lock_guard lock(mutex);
        if (!open || open_root != game_root)
        {
            open = std::make_shared<GameFiles>(game_root);
            open_root = game_root;
        }
        return open;
    }

    CacheKey cache_key(const fs::path &game_root)
    {
        CacheKey out;
        const auto check = check_game_folder(game_root, true);
        std::string text = "skate:" + std::to_string(check.skate_size) + ":" + std::to_string(file_time(game_root / L"Skate.exe"));
        const auto data = game_root / L"Data";
        std::error_code ec;
        std::vector<fs::path> files{data / L"layout.toc"};
        for (fs::recursive_directory_iterator it(data / L"Win32", ec), end; it != end && !ec; it.increment(ec))
        {
            std::error_code e2;
            if (it->is_regular_file(e2) && it->path().extension() == L".toc")
                files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
        for (const auto &f : files)
        {
            std::error_code e3;
            text += ";" + path_utf8(fs::relative(f, data, e3).generic_wstring()) + ":" +
                    std::to_string(fs::file_size(f, e3)) + ":" + std::to_string(file_time(f));
        }
        char hash[20];
        std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(fnv64(text)));
        out.key = "v" + std::to_string(format_version) + "-" + hash;
        // One file per game build, so a second install of the same build shares it.
        const std::string stem = out.skate_sha256.empty() ? std::string(hash) : out.skate_sha256.substr(0, 16);
        out.file = Settings::data_dir() / L"asset-index" / fs::path(utf8_to_wide("assets-" + stem + ".bin"));
        return out;
    }

    std::shared_ptr<const AssetIndex> load_index(Context &context, const fs::path &game_root)
    {
        const auto key = cache_key(game_root);
        auto &memory = loaded();
        std::lock_guard lock(memory.mutex);
        if (memory.index && memory.key == key.key)
            return memory.index;
        std::error_code ec;
        if (!fs::exists(key.file, ec))
            throw Error("asset_index_missing",
                        "The asset index for this game has not been built yet. Run: studio-plus asset index",
                        {{"cache", path_utf8(key.file)}});
        if (stored_key(key.file) != key.key)
            throw Error("asset_index_stale",
                        "The game files changed since the asset index was built. Run: studio-plus asset index",
                        {{"cache", path_utf8(key.file)}});
        context.progress(-1, "Loading the asset index");
        memory.index = load(key.file);
        memory.key = key.key;
        return memory.index;
    }

    std::shared_ptr<const AssetIndex> build_index(Context &context,
                                                  const fs::path &game_root,
                                                  bool refresh,
                                                  bool *from_cache)
    {
        if (from_cache)
            *from_cache = false;
        const auto started = std::chrono::steady_clock::now();
        context.progress(-1, "Checking Skate.exe and the TOC files");
        const auto key = cache_key(game_root);
        if (!refresh)
        {
            try
            {
                auto index = load_index(context, game_root);
                if (from_cache)
                    *from_cache = true;
                return index;
            }
            catch (const Error &)
            {
                // missing, stale or damaged: build it
            }
            catch (const std::exception &)
            {
            }
        }
        auto files = game_files(game_root);
        auto index = std::make_shared<AssetIndex>();
        index->key = key.key;
        index->game_root = path_utf8(game_root);
        index->types.push_back({"", Category::other});

        // 1. The superbundle TOCs.
        const auto tocs = files->toc_files();
        std::vector<fb::TocDocument> documents(tocs.size());
        for (std::size_t i = 0; i < tocs.size(); ++i)
        {
            if (context.cancelled())
                throw Error("cancelled", "Cancelled");
            context.progress(0.02 * static_cast<double>(i) / static_cast<double>(tocs.size()), "Reading " + tocs[i]);
            documents[i] = files->read_toc(tocs[i]);
        }
        index->superbundles = tocs.size();

        // 2. Every bundle's manifest, on all cores.
        struct Task
        {
            std::size_t toc, bundle;
        };
        std::vector<Task> tasks;
        for (std::size_t t = 0; t < documents.size(); ++t)
            for (std::size_t b = 0; b < documents[t].bundles.size(); ++b)
                tasks.push_back({t, b});
        std::vector<std::optional<GameFiles::Bundle>> manifests(tasks.size());
        std::vector<std::string> problems(tasks.size());
        parallel_for(
            tasks.size(),
            [&](std::size_t i)
            {
                try
                {
                    manifests[i] = files->read_bundle(documents[tasks[i].toc].bundles[tasks[i].bundle]);
                }
                catch (const std::exception &e)
                {
                    problems[i] = e.what();
                }
            },
            [&](std::size_t done)
            {
                context.progress(0.02 + 0.13 * static_cast<double>(done) /
                                            static_cast<double>(std::max<std::size_t>(1, tasks.size())),
                                 "Reading bundle manifests: " + std::to_string(done) + " of " +
                                     std::to_string(tasks.size()));
                return context.cancelled();
            });
        if (context.cancelled())
            throw Error("cancelled", "Cancelled");
        std::size_t failed_bundles = 0;
        for (std::size_t i = 0; i < problems.size(); ++i)
            if (!problems[i].empty() && failed_bundles++ < 5)
                context.log("warning",
                            "Bundle " + documents[tasks[i].toc].bundles[tasks[i].bundle].name + ": " + problems[i]);

        // 3. One entry per asset: the first bundle that carries it gives its payload.
        context.progress(0.16, "Merging bundles");
        std::string pool;
        std::unordered_map<std::string, std::uint32_t> seen[3];
        auto add = [&](Kind kind, const std::string &name, const std::string &unique) -> Entry *
        {
            auto [it, added] = seen[static_cast<std::size_t>(kind)].try_emplace(
                unique, static_cast<std::uint32_t>(index->entries.size()));
            if (!added)
            {
                ++index->entries[it->second].bundle_count;
                return nullptr;
            }
            Entry e;
            e.kind = kind;
            e.name_offset = static_cast<std::uint32_t>(pool.size());
            e.name_length = static_cast<std::uint16_t>(std::min<std::size_t>(name.size(), 0xFFFF));
            pool.append(name.data(), e.name_length);
            e.bundle = static_cast<std::uint32_t>(index->bundles.size() ? index->bundles.size() - 1 : 0);
            e.bundle_count = 1;
            index->entries.push_back(e);
            return &index->entries.back();
        };
        auto place = [](Entry &e, const fb::BundleFileInfo *at)
        {
            if (!at)
                return;
            e.install_chunk = at->location.installChunk;
            e.archive = at->location.archive;
            e.patch = at->location.patch ? 1 : 0;
            e.offset = at->offset;
            e.stored = at->size;
        };
        for (std::size_t i = 0; i < tasks.size(); ++i)
        {
            if (!manifests[i])
                continue;
            const auto &toc_bundle = documents[tasks[i].toc].bundles[tasks[i].bundle];
            index->bundles.push_back({toc_bundle.name, tocs[tasks[i].toc]});
            const auto &m = manifests[i]->manifest;
            const auto &payloads = manifests[i]->payloads;
            auto payload = [&](std::size_t at) -> const fb::BundleFileInfo *
            { return at < payloads.size() ? &payloads[at] : nullptr; };
            for (std::size_t a = 0; a < m.ebx.size(); ++a)
            {
                if (Entry *e = add(Kind::ebx, m.ebx[a].name, m.ebx[a].name))
                {
                    e->size = m.ebx[a].originalSize;
                    place(*e, payload(a));
                }
            }
            for (std::size_t a = 0; a < m.resources.size(); ++a)
            {
                const auto &r = m.resources[a];
                if (Entry *e = add(Kind::res, r.name, r.name))
                {
                    e->size = r.originalSize;
                    e->resource_type = r.resourceType;
                    e->resource_id = r.resourceId;
                    std::copy_n(
                        r.resourceMeta.begin(), std::min<std::size_t>(16, r.resourceMeta.size()), e->meta.begin());
                    place(*e, payload(m.ebx.size() + a));
                }
            }
            for (std::size_t a = 0; a < m.chunks.size(); ++a)
            {
                const auto &c = m.chunks[a];
                const auto *at = payload(m.ebx.size() + m.resources.size() + a);
                if (Entry *e = add(Kind::chunk, c.name, guid_key(c.guid.bytes)))
                {
                    e->size = c.originalSize;
                    e->logical_offset = c.logicalOffset;
                    e->guid = c.guid.bytes;
                    place(*e, at);
                }
                else if (at)
                {
                    // A streamed texture's bundles carry only the piece with its small mips; keep the whole chunk.
                    Entry &old = index->entries[seen[2][guid_key(c.guid.bytes)]];
                    if (more_complete(c.logicalOffset, at->size, old))
                    {
                        old.size = c.originalSize;
                        old.logical_offset = c.logicalOffset;
                        place(old, at);
                    }
                }
            }
            manifests[i].reset();
        }
        // Chunks the TOCs carry outside any bundle. Their bundle is the superbundle itself.
        for (std::size_t t = 0; t < documents.size(); ++t)
        {
            if (documents[t].chunks.empty())
                continue;
            index->bundles.push_back({"(" + tocs[t] + " chunks)", tocs[t]});
            for (const auto &c : documents[t].chunks)
            {
                if (c.removed)
                    continue;
                fb::BundleFileInfo at{c.location, c.offset, c.size};
                if (Entry *e = add(Kind::chunk, c.guid.string(), guid_key(c.guid.bytes)))
                {
                    e->guid = c.guid.bytes;
                    place(*e, &at);
                }
                else
                {
                    Entry &old = index->entries[seen[2][guid_key(c.guid.bytes)]];
                    if (more_complete(0, c.size, old))
                    {
                        old.size = 0;
                        old.logical_offset = 0;
                        place(old, &at);
                    }
                }
            }
        }
        documents.clear();
        index->strings.assign(pool.begin(), pool.end());
        pool.clear();

        // 4. EBX root types and file guids, read from every EBX payload.
        std::vector<std::uint32_t> ebx;
        for (std::uint32_t i = 0; i < index->entries.size(); ++i)
            if (index->entries[i].kind == Kind::ebx && index->entries[i].stored)
                ebx.push_back(i);
        // In archive order, so the disk reads mostly move forward.
        std::sort(ebx.begin(),
                  ebx.end(),
                  [&](std::uint32_t a, std::uint32_t b)
                  {
                      const auto &x = index->entries[a];
                      const auto &y = index->entries[b];
                      return std::tie(x.install_chunk, x.archive, x.offset) <
                             std::tie(y.install_chunk, y.archive, y.offset);
                  });
        std::vector<std::string> root_types(ebx.size());
        std::atomic<std::size_t> failures{0};
        parallel_for(
            ebx.size(),
            [&](std::size_t i)
            {
                auto &e = index->entries[ebx[i]];
                try
                {
                    const auto info = fb::ebx::read_root_info(files->read(e.location()));
                    root_types[i] = info.rootType;
                    e.guid = info.fileGuid.bytes;
                }
                catch (const std::exception &)
                {
                    e.flags |= entry_type_failed;
                    failures.fetch_add(1);
                }
            },
            [&](std::size_t done)
            {
                context.progress(0.17 + 0.78 * static_cast<double>(done) /
                                            static_cast<double>(std::max<std::size_t>(1, ebx.size())),
                                 "Reading EBX types: " + std::to_string(done) + " of " + std::to_string(ebx.size()));
                return context.cancelled();
            });
        if (context.cancelled())
            throw Error("cancelled", "Cancelled");
        index->type_failures = failures.load();

        context.progress(0.96, "Sorting");
        std::map<std::string, std::uint32_t> type_ids;
        auto intern = [&](const std::string &name) -> std::uint32_t
        {
            if (name.empty())
                return 0;
            auto [it, added] = type_ids.try_emplace(name, static_cast<std::uint32_t>(index->types.size()));
            if (added)
                index->types.push_back({name, category_of(name)});
            return it->second;
        };
        for (std::size_t i = 0; i < ebx.size(); ++i)
            index->entries[ebx[i]].type = intern(root_types[i]);
        // A resource type without a known name is named after the EBX type its assets pair with (an EBX of
        // the same name), when nearly all of them agree: "ClipControllerAsset resource".
        std::unordered_map<std::uint32_t, std::map<std::uint32_t, std::size_t>> pairs;
        {
            std::unordered_map<std::string_view, std::uint32_t> ebx_type;
            for (const auto &e : index->entries)
                if (e.kind == Kind::ebx)
                    ebx_type.emplace(std::string_view(index->strings.data() + e.name_offset, e.name_length), e.type);
            for (const auto &e : index->entries)
            {
                if (e.kind != Kind::res)
                    continue;
                const auto it = ebx_type.find(std::string_view(index->strings.data() + e.name_offset, e.name_length));
                ++pairs[e.resource_type][it == ebx_type.end() ? 0u : it->second];
            }
        }
        std::unordered_map<std::uint32_t, std::string> res_names;
        for (const auto &[res_type, counts] : pairs)
        {
            std::string name = resource_type_name(res_type);
            if (name.starts_with("Resource "))
            {
                std::size_t total = 0, best = 0;
                std::uint32_t best_type = 0;
                for (const auto &[t, n] : counts)
                {
                    total += n;
                    if (n > best)
                    {
                        best = n;
                        best_type = t;
                    }
                }
                if (best_type && best * 10 >= total * 9)
                    name = index->types[best_type].name + " resource";
            }
            res_names[res_type] = name;
        }
        for (auto &e : index->entries)
            if (e.kind == Kind::res)
                e.type = intern(res_names[e.resource_type]);
            else if (e.kind == Kind::chunk)
                e.type = intern("Chunk");

        // Sorted by name so a folder is one contiguous run.
        std::vector<std::uint32_t> order(index->entries.size());
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(),
                  order.end(),
                  [&](std::uint32_t a, std::uint32_t b)
                  {
                      const auto &x = index->entries[a];
                      const auto &y = index->entries[b];
                      const auto c = index->name(x).compare(index->name(y));
                      return c != 0 ? c < 0 : x.kind < y.kind;
                  });
        std::vector<Entry> sorted;
        sorted.reserve(order.size());
        for (auto i : order)
            sorted.push_back(index->entries[i]);
        index->entries = std::move(sorted);

        index->built_at = static_cast<std::int64_t>(std::time(nullptr));
        index->build_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        index->finish();
        context.progress(0.98, "Saving the cache");
        save(*index, key.file);
        auto &memory = loaded();
        std::lock_guard lock(memory.mutex);
        memory.index = index;
        memory.key = key.key;
        return index;
    }

} // namespace studio::native
