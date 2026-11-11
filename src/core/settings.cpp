#include "core/settings.h"
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace studio
{

    std::wstring utf8_to_wide(std::string_view text)
    {
        if (text.empty())
            return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
        return out;
    }

    std::string wide_to_utf8(std::wstring_view text)
    {
        if (text.empty())
            return {};
        int n =
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    std::string path_utf8(const fs::path &path)
    {
        return wide_to_utf8(path.native());
    }

    fs::path executable_dir()
    {
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (n < buffer.size())
            {
                buffer.resize(n);
                break;
            }
            buffer.resize(buffer.size() * 2);
        }
        return fs::path(buffer).parent_path();
    }

    fs::path Settings::data_dir()
    {
        // RSSP_DATA_DIR gives a test run its own settings, so parallel runs never share one file.
        if (wchar_t env[MAX_PATH]; GetEnvironmentVariableW(L"RSSP_DATA_DIR", env, MAX_PATH) > 0)
            return env;
        PWSTR local = nullptr;
        fs::path base;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
            base = local;
        CoTaskMemFree(local);
        return base / L"ReSkateStudioPlus";
    }

    fs::path Settings::file()
    {
        return data_dir() / L"settings.json";
    }

    namespace
    {
        std::wstring registry_string(HKEY root, const wchar_t *key, const wchar_t *value)
        {
            wchar_t buffer[1024];
            DWORD size = sizeof(buffer);
            if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
                return {};
            return buffer;
        }

        std::vector<fs::path> steam_libraries()
        {
            std::vector<fs::path> out;
            fs::path steam = registry_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
            if (steam.empty())
                steam = L"C:\\Program Files (x86)\\Steam";
            steam = steam.lexically_normal().make_preferred();
            out.push_back(steam);
            std::ifstream vdf(steam / L"steamapps" / L"libraryfolders.vdf");
            std::string line;
            static const std::regex path_line("\"path\"\\s+\"([^\"]+)\"");
            while (std::getline(vdf, line))
            {
                std::smatch m;
                if (std::regex_search(line, m, path_line))
                {
                    std::string p = m[1];
                    std::string unescaped;
                    for (size_t i = 0; i < p.size(); ++i)
                    {
                        if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\')
                            ++i;
                        unescaped += p[i];
                    }
                    out.emplace_back(utf8_to_wide(unescaped));
                }
            }
            return out;
        }

        bool same_name(const fs::path &name, const wchar_t *expected)
        {
            return _wcsicmp(name.c_str(), expected) == 0;
        }

        std::string lower_utf8(const fs::path &path)
        {
            std::wstring w = path.native();
            if (!w.empty())
                CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));
            return wide_to_utf8(w);
        }

        // Calls fn(entry) for each entry of `dir`, at most `cap` of them. Never throws.
        template <class Fn> void for_each_entry(const fs::path &dir, int cap, Fn fn)
        {
            std::error_code ec;
            fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
            for (int n = 0; !ec && it != fs::directory_iterator() && n < cap; ++n)
            {
                fn(*it);
                it.increment(ec);
            }
        }

        fs::path mods_folder_of(const fs::path &folder)
        {
            // ReSkate's launcher_mods::mods_root: the game reads ModData\Default\Mods when a ModData folder exists.
            std::error_code ec;
            return fs::is_directory(folder / L"ModData", ec) ? folder / L"ModData" / L"Default" / L"Mods"
                                                             : folder / L"Mods";
        }

        // SHA-256 with Windows CNG, as ReSkate's launcher does (Engine/Core/Platform/launcher_pe.cpp). "" on failure.
        std::string file_sha256(const fs::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return {};
            BCRYPT_ALG_HANDLE alg = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
                return {};
            bool ok = BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
            std::vector<char> buffer(1 << 20);
            while (ok && in)
            {
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto n = in.gcount();
                if (n > 0)
                    ok = BCRYPT_SUCCESS(
                        BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(n), 0));
            }
            ok = ok && in.eof();
            unsigned char digest[32] = {};
            ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof digest, 0));
            if (hash)
                BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(alg, 0);
            if (!ok)
                return {};
            static constexpr char digits[] = "0123456789abcdef";
            std::string hex;
            for (unsigned char b : digest)
            {
                hex.push_back(digits[b >> 4]);
                hex.push_back(digits[b & 15]);
            }
            return hex;
        }

        // Hashes of big files, keyed by path, size and modified time, kept in data_dir()\file-hashes.json.
        std::string cached_sha256(const fs::path &path, std::uint64_t size)
        {
            static std::mutex mutex;
            std::lock_guard lock(mutex);
            std::error_code ec;
            const auto mtime = fs::last_write_time(path, ec);
            if (ec)
                return file_sha256(path);
            const long long stamp = static_cast<long long>(mtime.time_since_epoch().count());
            const fs::path cache_file = Settings::data_dir() / L"file-hashes.json";
            const std::string key = lower_utf8(path);
            Json cache = Json::object();
            if (std::ifstream in(cache_file); in)
            {
                try
                {
                    cache = Json::parse(in);
                }
                catch (...)
                {
                    cache = Json::object();
                }
                if (!cache.is_object())
                    cache = Json::object();
            }
            // A damaged entry (wrong types) is ignored and rewritten, never an error.
            if (auto it = cache.find(key); it != cache.end() && it->is_object())
            {
                const Json &e = *it;
                if (e.contains("size") && e["size"].is_number_unsigned() && e["size"].get<std::uint64_t>() == size &&
                    e.contains("mtime") && e["mtime"].is_number_integer() && e["mtime"].get<long long>() == stamp &&
                    e.contains("sha256") && e["sha256"].is_string())
                {
                    std::string sha = e["sha256"].get<std::string>();
                    if (sha.size() == 64)
                        return sha;
                }
            }
            std::string sha = file_sha256(path);
            if (sha.empty())
                return sha;
            cache[key] = {{"size", size}, {"mtime", stamp}, {"sha256", sha}};
            fs::create_directories(Settings::data_dir(), ec);
            fs::path tmp = cache_file;
            tmp += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                out << cache.dump(2) << "\n";
            }
            fs::rename(tmp, cache_file, ec);
            if (ec)
                fs::remove(tmp, ec);
            return sha;
        }

        // How good a ReSkate folder `folder` is: 0 none, then plain Skate.exe < ReSkate.dll + Skate.exe <
        // ReSkate.dll + Skate.exe + Mods. The launcher breaks ties.
        int folder_score(const fs::path &folder, bool need_reskate)
        {
            std::error_code ec;
            if (!fs::is_regular_file(folder / L"Skate.exe", ec))
                return 0;
            const bool dll = fs::is_regular_file(folder / L"ReSkate.dll", ec);
            const bool launcher = fs::is_regular_file(folder / L"ReSkateLauncher.exe", ec);
            if (need_reskate && !dll && !launcher)
                return 0;
            const bool mods = fs::is_directory(mods_folder_of(folder), ec);
            const int rank = dll ? (mods ? 3 : 2) : 1;
            return rank * 2 + (launcher ? 1 : 0);
        }

        fs::path known_folder(REFKNOWNFOLDERID id)
        {
            PWSTR p = nullptr;
            fs::path out;
            if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)))
                out = p;
            CoTaskMemFree(p);
            return out;
        }
    } // namespace

    fs::path on_disk_path(const fs::path &path)
    {
        if (path.empty())
            return path;
        // Drop a \\?\ long-path prefix (\\?\UNC\server\share is \\server\share): the walk below needs a plain drive or
        // UNC root.
        std::wstring text = path.native();
        std::replace(text.begin(), text.end(), L'/', L'\\');
        if (text.starts_with(L"\\\\?\\UNC\\"))
            text = L"\\\\" + text.substr(8);
        else if (text.starts_with(L"\\\\?\\") || text.starts_with(L"\\??\\"))
            text = text.substr(4);
        std::error_code ec;
        fs::path full = fs::absolute(text, ec);
        if (ec)
            full = text;
        full = full.lexically_normal().make_preferred();
        if (full.has_relative_path() && !full.has_filename())
            full = full.parent_path();
        std::wstring root = full.root_name().native();
        if (root.size() == 2 && root[1] == L':')
            root[0] = static_cast<wchar_t>(towupper(root[0]));
        fs::path out = fs::path(root) / full.root_directory();
        bool exists = true;
        for (const auto &part : full.relative_path())
        {
            fs::path next = out / part;
            if (exists)
            {
                WIN32_FIND_DATAW data;
                HANDLE find = FindFirstFileW(next.c_str(), &data);
                if (find != INVALID_HANDLE_VALUE)
                {
                    FindClose(find);
                    next = out / data.cFileName;
                }
                else
                {
                    exists = false;
                }
            }
            out = std::move(next);
        }
        return out;
    }

    fs::path game_folder_from(const fs::path &value)
    {
        std::error_code ec;
        fs::path folder = value;
        if (fs::is_regular_file(value, ec))
        {
            const fs::path name = value.filename();
            if (!same_name(name, L"ReSkateLauncher.exe") && !same_name(name, L"Skate.exe") &&
                !same_name(name, L"ReSkate.dll"))
                throw Error("not_a_reskate_file",
                            path_utf8(value) + " is not ReSkateLauncher.exe, Skate.exe or ReSkate.dll. Give the " +
                                std::string(game_root_name) + ", or one of those files in it.",
                            {{"path", path_utf8(value)}});
            folder = value.parent_path();
        }
        return on_disk_path(folder);
    }

    GameFolderCheck check_game_folder(const fs::path &folder, bool hash)
    {
        GameFolderCheck c;
        c.folder = folder;
        if (folder.empty())
            return c;
        std::error_code ec;
        const fs::path skate = folder / L"Skate.exe";
        c.has_skate = fs::is_regular_file(skate, ec);
        c.has_reskate_dll = fs::is_regular_file(folder / L"ReSkate.dll", ec);
        c.has_launcher = fs::is_regular_file(folder / L"ReSkateLauncher.exe", ec);
        c.mods_dir = mods_folder_of(folder);
        c.has_mods = fs::is_directory(c.mods_dir, ec);
        if (c.has_mods)
            for_each_entry(c.mods_dir,
                           100000,
                           [&](const fs::directory_entry &e)
                           {
                               std::error_code e_ec;
                               const std::wstring name = e.path().filename().native();
                               if (!name.empty() && name.front() != L'.' && e.is_directory(e_ec))
                                   ++c.mod_count;
                           });
        if (c.has_skate)
        {
            c.skate_size = fs::file_size(skate, ec);
            if (ec)
                c.skate_size = 0;
            if (c.skate_size != supported_build::game_file_size)
            {
                if (c.skate_size)
                    c.build = GameFolderCheck::Build::different;
            }
            else if (hash)
            {
                c.skate_sha256 = cached_sha256(skate, c.skate_size);
                if (!c.skate_sha256.empty())
                    c.build = c.skate_sha256 == supported_build::game_sha256 ? GameFolderCheck::Build::supported
                                                                             : GameFolderCheck::Build::different;
            }
        }
        return c;
    }

    std::string GameFolderCheck::build_text() const
    {
        const std::string id(supported_build::steam_build_id);
        if (!has_skate)
            return "no Skate.exe";
        switch (build)
        {
        case Build::supported:
            return "Skate.exe is build " + id + ", the one ReSkate supports";
        case Build::different:
            if (skate_size != supported_build::game_file_size)
                return "Skate.exe is not build " + id + ", the one ReSkate supports (it is " +
                       std::to_string(skate_size) + " bytes, build " + id + " is " +
                       std::to_string(supported_build::game_file_size) + ")";
            return "Skate.exe is not build " + id + ", the one ReSkate supports (SHA-256 " + skate_sha256 + ")";
        case Build::not_checked:
            break;
        }
        return "Skate.exe was not checked against build " + id;
    }

    Json GameFolderCheck::to_json() const
    {
        const char *b = build == Build::supported   ? "supported"
                        : build == Build::different ? "different"
                                                    : "not_checked";
        return {{"folder", path_utf8(folder)},
                {"skate_exe", has_skate},
                {"reskate_dll", has_reskate_dll},
                {"launcher", has_launcher},
                {"plain_steam_copy", has_skate && !has_reskate_dll && !has_launcher},
                {"build", b},
                {"supported_build", supported_build::steam_build_id},
                {"build_detail", build_text()},
                {"skate_size", skate_size},
                {"skate_sha256", skate_sha256.empty() ? Json(nullptr) : Json(skate_sha256)},
                {"mods_folder", has_mods ? Json(path_utf8(mods_dir)) : Json(nullptr)},
                {"mod_count", mod_count}};
    }

    fs::path detect_game_root()
    {
        std::error_code ec;
        fs::path best;
        int best_score = 0;
        std::vector<std::string> seen;
        auto consider = [&](const fs::path &folder, bool need_reskate)
        {
            std::string key = lower_utf8(folder.lexically_normal());
            if (std::find(seen.begin(), seen.end(), key) != seen.end())
                return;
            seen.push_back(key);
            const int score = folder_score(folder, need_reskate);
            if (score > best_score)
            {
                best_score = score;
                best = folder;
            }
        };
        // A Steam copy counts even without ReSkate; it is the fallback.
        for (const auto &library : steam_libraries())
            consider(library / L"steamapps" / L"common" / L"Skate", false);

        // Elsewhere only folders with ReSkate in them, one level deep, with a cap on how many are looked at.
        std::vector<fs::path> bases;
        for (auto id : {FOLDERID_Downloads, FOLDERID_Desktop, FOLDERID_Documents})
            if (fs::path p = known_folder(id); !p.empty())
                bases.push_back(p);
        if (fs::path profile = known_folder(FOLDERID_Profile); !profile.empty())
            bases.push_back(profile / L"Games");
        bases.push_back(L"C:\\Games");
        bases.push_back(L"D:\\Games");
        const DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; ++i)
        {
            if (!(drives & (1u << i)))
                continue;
            const std::wstring root{static_cast<wchar_t>(L'A' + i), L':', L'\\'};
            if (GetDriveTypeW(root.c_str()) == DRIVE_FIXED)
                bases.push_back(root);
        }
        int budget = 4000; // folders looked into, in all
        for (const auto &base : bases)
        {
            if (budget <= 0 || !fs::is_directory(base, ec))
                continue;
            consider(base, true);
            for_each_entry(base,
                           std::min(budget, 1000),
                           [&](const fs::directory_entry &e)
                           {
                               --budget;
                               std::error_code e_ec;
                               if (e.is_directory(e_ec) && !e.is_symlink(e_ec))
                                   consider(e.path(), true);
                           });
        }
        return best.empty() ? best : on_disk_path(best);
    }

    fs::path detect_blender()
    {
        std::error_code ec;
        if (wchar_t env[MAX_PATH];
            GetEnvironmentVariableW(L"RESKATE_BLENDER", env, MAX_PATH) > 0 && fs::exists(env, ec))
            return env;
        std::vector<fs::path> found;
        for (const wchar_t *base :
             {L"C:\\Program Files\\Blender Foundation", L"C:\\Program Files (x86)\\Blender Foundation"})
        {
            if (!fs::exists(base, ec))
                continue;
            for (const auto &entry : fs::directory_iterator(base, ec))
                if (fs::exists(entry.path() / L"blender.exe", ec))
                    found.push_back(entry.path() / L"blender.exe");
        }
        for (const auto &library : steam_libraries())
        {
            fs::path steam_blender = library / L"steamapps" / L"common" / L"Blender" / L"blender.exe";
            if (fs::exists(steam_blender, ec))
                found.push_back(steam_blender);
        }
        if (found.empty())
            return {};
        // Folder names are "Blender 4.2", "Blender 5.2": the highest sorts last.
        std::sort(found.begin(), found.end());
        return found.back();
    }

    Settings Settings::load()
    {
        Settings s;
        std::ifstream in(file());
        if (in)
        {
            try
            {
                Json j = Json::parse(in);
                if (j.contains("game-root"))
                    s.game_root = utf8_to_wide(j["game-root"].get<std::string>());
                if (j.contains("blender"))
                    s.blender = utf8_to_wide(j["blender"].get<std::string>());
                if (j.contains("engine-dir"))
                    s.engine_dir = utf8_to_wide(j["engine-dir"].get<std::string>());
            }
            catch (...)
            {
                // A damaged settings file falls back to detection; it is rewritten on the next save.
            }
        }
        // A saved launcher, Skate.exe or ReSkate.dll path means the folder holding it.
        if (std::error_code ec; !s.game_root.empty() && fs::is_regular_file(s.game_root, ec))
            s.game_root = s.game_root.parent_path();
        if (s.game_root.empty())
            s.game_root = detect_game_root();
        if (s.blender.empty())
            s.blender = detect_blender();
        return s;
    }

    Json Settings::to_json() const
    {
        return {{"game-root", path_utf8(game_root)},
                {"blender", path_utf8(blender)},
                {"engine-dir", path_utf8(engine_dir)}};
    }

    void Settings::save() const
    {
        std::error_code ec;
        fs::create_directories(data_dir(), ec);
        fs::path tmp = file();
        tmp += L".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << to_json().dump(2) << "\n";
            if (!out)
                throw Error("settings_write_failed", "Could not write " + path_utf8(tmp));
        }
        fs::rename(tmp, file(), ec);
        if (ec)
            throw Error("settings_write_failed", "Could not save " + path_utf8(file()) + ": " + ec.message());
    }

    std::vector<std::string> Settings::keys()
    {
        return {"game-root", "blender", "engine-dir"};
    }

    void Settings::set(std::string_view key, const fs::path &value)
    {
        if (key == "game-root")
            game_root = value;
        else if (key == "blender")
            blender = value;
        else if (key == "engine-dir")
            engine_dir = value;
        else
            throw Error("unknown_setting",
                        "Unknown setting '" + std::string(key) + "'. Settings: game-root, blender, engine-dir");
    }

    fs::path Settings::resolved_engine_dir() const
    {
        if (!engine_dir.empty())
            return engine_dir;
        if (wchar_t env[MAX_PATH]; GetEnvironmentVariableW(L"RSSP_ENGINE", env, MAX_PATH) > 0)
            return env;
        return executable_dir() / L"engine";
    }

    fs::path Settings::reskate_cli() const
    {
        fs::path cli = resolved_engine_dir() / L"reskate_cli.exe";
        std::error_code ec;
        if (!fs::exists(cli, ec))
            throw Error("engine_missing",
                        "reskate_cli.exe was not found at " + path_utf8(cli) +
                            ". Put the ReSkate Studio files in an 'engine' folder beside Studio+, or run: studio-plus "
                            "studio set engine-dir <folder>",
                        {{"expected", path_utf8(cli)}});
        return cli;
    }
} // namespace studio
