// Projects and mods (reskate_cli: compile-mod, deploy-mod, project-info, fbmod-info, validate, patch-audit,
// initfs-info, lua), plus "mod list", which reads the Mods folder directly with ReSkate's own rules.
#include "commands/commands.h"
#include "core/engine.h"
#include "core/settings.h"
#include "native/mod_dump.h"
#include "native/mod_files.h"
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <utility>

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace studio
{

    namespace
    {
        // ---- small helpers ---------------------------------------------------------------------------

        // The engine runs with its own folder as the working directory, so every path handed to it must be
        // absolute. The CLI makes Path params absolute, but not List items, and MCP clients may send relative paths.
        fs::path absolute_path(const fs::path &p)
        {
            if (p.empty())
                return p;
            std::error_code ec;
            fs::path out = fs::absolute(p, ec);
            return ec ? p : out.lexically_normal();
        }

        fs::path to_path(const Json &args, const std::string &param)
        {
            return absolute_path(fs::path(utf8_to_wide(arg_string(args, param))));
        }

        std::wstring lower(std::wstring s)
        {
            for (auto &ch : s)
                ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
            return s;
        }

        std::string lower_ascii(std::string s)
        {
            for (auto &ch : s)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return s;
        }

        // "decoded-assets" -> "decoded_assets", "gameRoot" -> "game_root".
        std::string snake(std::string_view key)
        {
            std::string out;
            for (char ch : key)
            {
                if (ch == '-' || ch == ' ')
                {
                    out.push_back('_');
                    continue;
                }
                if (ch >= 'A' && ch <= 'Z')
                {
                    if (!out.empty() && out.back() != '_')
                        out.push_back('_');
                    out.push_back(static_cast<char>(ch - 'A' + 'a'));
                    continue;
                }
                out.push_back(ch);
            }
            return out;
        }

        // Splits "key=value" (value may contain spaces and '='). Returns false for other lines.
        bool split_kv(std::string_view line, std::string &key, std::string &value)
        {
            auto eq = line.find('=');
            if (eq == std::string_view::npos || eq == 0)
                return false;
            std::string_view k = line.substr(0, eq);
            if (k.find(' ') != std::string_view::npos)
                return false;
            key.assign(k);
            value.assign(line.substr(eq + 1));
            return true;
        }

        fs::path normalised(const fs::path &p)
        {
            std::error_code ec;
            fs::path out = fs::weakly_canonical(fs::absolute(p, ec), ec);
            if (ec)
                out = fs::absolute(p, ec);
            return out.lexically_normal();
        }

        // True when `child` is `parent` or lies inside it (case-insensitive, after resolving).
        bool is_within(const fs::path &child, const fs::path &parent)
        {
            std::wstring c = lower(normalised(child).native()), p = lower(normalised(parent).native());
            while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/'))
                p.pop_back();
            while (c.size() > 3 && (c.back() == L'\\' || c.back() == L'/'))
                c.pop_back();
            if (c == p)
                return true;
            if (c.size() <= p.size() || c.compare(0, p.size(), p) != 0)
                return false;
            return c[p.size()] == L'\\' || c[p.size()] == L'/' || p.back() == L'\\';
        }

        bool path_exists(const fs::path &p)
        {
            std::error_code ec;
            return fs::exists(p, ec);
        }
        bool dir_exists(const fs::path &p)
        {
            std::error_code ec;
            return fs::is_directory(p, ec);
        }
        bool file_exists(const fs::path &p)
        {
            std::error_code ec;
            return fs::is_regular_file(p, ec);
        }

        std::string read_text(const fs::path &p)
        {
            std::ifstream f(p, std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }

        void require_file(const fs::path &p, const std::string &param)
        {
            if (!path_exists(p))
                throw Error("path_not_found",
                            "File does not exist: " + path_utf8(p),
                            {{"param", param}, {"path", path_utf8(p)}});
            if (!file_exists(p))
                throw Error("not_a_file", "Not a file: " + path_utf8(p), {{"param", param}, {"path", path_utf8(p)}});
        }

        // Checks a Skate folder before the engine spends 3 s loading it and fails with a vaguer message.
        fs::path checked_game_root(const Context &c, const Json &a, bool need_exe = true, bool need_data = true)
        {
            fs::path root = absolute_path(c.game_root(a));
            std::string s = path_utf8(root);
            if (!dir_exists(root))
                throw Error("game_root_not_found", "Skate folder does not exist: " + s, {{"game_root", s}});
            if (need_exe && !file_exists(root / L"Skate.exe"))
                throw Error("not_a_game_root",
                            "No Skate.exe in " + s +
                                " (pass --game-root or run: studio-plus studio set game-root <folder>)",
                            {{"game_root", s}});
            if (need_data && !dir_exists(root / L"Data"))
                throw Error("not_a_game_root", "No Data folder in " + s, {{"game_root", s}});
            return root;
        }

        // require_success, but with a stable error code for engine messages we know.
        void require_ok(const EngineRun &run, std::initializer_list<std::pair<const char *, const char *>> known)
        {
            try
            {
                require_success(run);
            }
            catch (Error &e)
            {
                std::string msg = e.what();
                if (msg.find("Skate Oodle codec was not found") != std::string::npos)
                    e.code = "oodle_missing";
                else if (msg.find("for SHA-256") != std::string::npos)
                    e.code = "not_a_game_root";
                else if (msg.find("could not open binary input") != std::string::npos)
                    e.code = "file_unreadable";
                for (const auto &[needle, code] : known)
                    if (msg.find(needle) != std::string::npos)
                    {
                        e.code = code;
                        break;
                    }
                throw;
            }
        }

        // Engine key=value output with snake_case keys and numbers as numbers. Other lines go to `other`.
        Json kv_object(const std::vector<std::string> &lines, Json *other = nullptr)
        {
            Json out = Json::object();
            for (const auto &line : lines)
            {
                std::string k, v;
                if (split_kv(line, k, v))
                    out[snake(k)] = typed_value(v);
                else if (other)
                    other->push_back(line);
            }
            return out;
        }

        Json path_or_null(const std::string &v)
        {
            return v.empty() ? Json(nullptr) : Json(v);
        }

        // ---- SHA-256 of a file (Windows CNG) -----------------------------------------------------------

        std::string sha256_file(const fs::path &p)
        {
            BCRYPT_ALG_HANDLE alg = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
                throw Error("hash_failed", "SHA-256 is not available");
            std::string hex;
            bool ok = BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
            std::ifstream f(p, std::ios::binary);
            ok = ok && f.is_open();
            std::vector<char> buf(1 << 20);
            while (ok && f)
            {
                f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                auto n = f.gcount();
                if (n > 0)
                    ok = BCRYPT_SUCCESS(
                        BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(n), 0));
            }
            unsigned char digest[32] = {};
            ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof digest, 0));
            if (hash)
                BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(alg, 0);
            if (!ok)
                throw Error("hash_failed", "Could not hash " + path_utf8(p), {{"path", path_utf8(p)}});
            static const char *digits = "0123456789abcdef";
            for (unsigned char b : digest)
            {
                hex.push_back(digits[b >> 4]);
                hex.push_back(digits[b & 15]);
            }
            return hex;
        }

        // ---- Is Skate running from this folder? --------------------------------------------------------

        bool skate_running_from(const fs::path &game_root)
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE)
                return false;
            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof pe;
            bool found = false;
            for (BOOL more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe))
            {
                if (lower(pe.szExeFile) != L"skate.exe")
                    continue;
                HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (!proc)
                {
                    found = true;
                    break;
                } // cannot tell where it runs from: assume this install
                wchar_t image[MAX_PATH * 4];
                DWORD size = static_cast<DWORD>(std::size(image));
                if (QueryFullProcessImageNameW(proc, 0, image, &size))
                    found = is_within(fs::path(image), game_root);
                else
                    found = true;
                CloseHandle(proc);
            }
            CloseHandle(snap);
            return found;
        }

        // ---- Staging and deployed-mod markers ------------------------------------------------------------

        // Reads "skate_sha256=<hex>" from a .reskate-studio-patch marker. "" if absent.
        std::string marker_sha(const fs::path &marker)
        {
            if (!file_exists(marker))
                return {};
            std::istringstream in(read_text(marker));
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.rfind("skate_sha256=", 0) == 0)
                    return line.substr(13);
            }
            return {};
        }

        // ---- Frostbite DbObject (the .ReSkateStudio-last-deploy.db receipt) -----------------------------

        struct DbReader
        {
            const std::string &d;
            size_t i = 0;
            bool ok = true;
            uint64_t varint()
            {
                uint64_t v = 0;
                for (int shift = 0; i < d.size() && shift < 64; shift += 7)
                {
                    auto b = static_cast<unsigned char>(d[i++]);
                    v |= static_cast<uint64_t>(b & 0x7f) << shift;
                    if (!(b & 0x80))
                        return v;
                }
                ok = false;
                return 0;
            }
            std::string cstring()
            {
                auto end = d.find('\0', i);
                if (end == std::string::npos)
                {
                    ok = false;
                    return {};
                }
                std::string s = d.substr(i, end - i);
                i = end + 1;
                return s;
            }
            std::string bytes(size_t n)
            {
                if (i + n > d.size())
                {
                    ok = false;
                    return {};
                }
                std::string s = d.substr(i, n);
                i += n;
                return s;
            }
            static std::string hex(const std::string &b)
            {
                static const char *digits = "0123456789abcdef";
                std::string out;
                for (char ch : b)
                {
                    auto u = static_cast<unsigned char>(ch);
                    out.push_back(digits[u >> 4]);
                    out.push_back(digits[u & 15]);
                }
                return out;
            }
            // Reads the value of an entry whose type byte was already consumed.
            Json value(unsigned char type)
            {
                switch (type & 0x1f)
                {
                case 0x01:
                case 0x02:
                { // list / object
                    size_t size = static_cast<size_t>(varint());
                    size_t end = i + size;
                    if (!ok || end > d.size())
                    {
                        ok = false;
                        return nullptr;
                    }
                    Json out = (type & 0x1f) == 0x02 ? Json::object() : Json::array();
                    while (ok && i < end)
                    {
                        auto t = static_cast<unsigned char>(d[i++]);
                        if (t == 0)
                            break;
                        std::string name = (t & 0x80) ? std::string() : cstring();
                        Json v = value(t);
                        if (out.is_object())
                            out[snake(name)] = v;
                        else
                            out.push_back(v);
                    }
                    i = end;
                    return out;
                }
                case 0x06:
                {
                    auto b = bytes(1);
                    return ok ? Json(b[0] != 0) : Json(nullptr);
                }
                case 0x07:
                {
                    std::string s = bytes(static_cast<size_t>(varint()));
                    if (!s.empty() && s.back() == '\0')
                        s.pop_back();
                    return s;
                }
                case 0x08:
                {
                    auto b = bytes(4);
                    if (!ok)
                        return nullptr;
                    int32_t v = 0;
                    memcpy(&v, b.data(), 4);
                    return v;
                }
                case 0x09:
                {
                    auto b = bytes(8);
                    if (!ok)
                        return nullptr;
                    int64_t v = 0;
                    memcpy(&v, b.data(), 8);
                    return v;
                }
                case 0x0b:
                {
                    auto b = bytes(4);
                    if (!ok)
                        return nullptr;
                    float v = 0;
                    memcpy(&v, b.data(), 4);
                    return v;
                }
                case 0x0c:
                {
                    auto b = bytes(8);
                    if (!ok)
                        return nullptr;
                    double v = 0;
                    memcpy(&v, b.data(), 8);
                    return v;
                }
                case 0x0f:
                    return hex(bytes(16));
                case 0x10:
                    return hex(bytes(20));
                case 0x13:
                    return hex(bytes(static_cast<size_t>(varint())));
                default:
                    ok = false;
                    return nullptr;
                }
            }
        };

        Json read_receipt(const fs::path &p)
        {
            if (!file_exists(p))
                return nullptr;
            std::string data = read_text(p);
            if (data.empty())
                return nullptr;
            DbReader r{data};
            auto type = static_cast<unsigned char>(data[r.i++]);
            Json v = r.value(type);
            if (!r.ok || !v.is_object())
                return {{"path", path_utf8(p)}, {"error", "receipt format not recognised"}};
            v["path"] = path_utf8(p);
            for (const char *key : {"game_root", "mod_root", "backup_root", "retired_patch", "displaced_root"})
                if (v.contains(key) && v[key].is_string())
                {
                    std::string s = v[key];
                    if (s.empty())
                        v[key] = nullptr;
                    else
                    {
                        for (auto &ch : s)
                            if (ch == '/')
                                ch = '\\';
                        v[key] = s;
                    }
                }
            return v;
        }

        // ---- mod list: ReSkate's own reading of the Mods folder --------------------------------------------
        // Ported from ReSkate (GPL-3.0, the same licence as Studio+):
        //   Engine/Vfs/mod_list.h/.cpp      scan_mods, read_order, valid_mod_name, ascii_path, mod_fingerprint,
        //                                   read_exclusions, the manifest / build / levels / studio-marker readers
        //   Engine/Core/Json/json.cpp       the strict JSON reader (duplicate keys, depth and event limits)
        //   Engine/Vfs/mod_catalog.cpp      sdk_identity (whether an exclusion still holds for the game itself)
        //   Launcher/mod_manager.cpp        mods_root (<game>\ModData\Default\Mods when <game>\ModData exists)
        //   Engine/Game/Build/supported_build.h   the Skate.exe SHA-256 a Studio stamp must record
        // The launcher's mod manager shows exactly what scan_mods returns, so the rules here must stay in step.
        namespace modlist
        {

            constexpr std::size_t maximum_mod_name = 64;
            constexpr std::size_t maximum_order_bytes = 4 * 1024 * 1024;
            constexpr std::size_t maximum_build_bytes = 16 * 1024;
            constexpr std::size_t maximum_info_bytes = 32 * 1024;
            constexpr std::size_t maximum_levels_bytes = 64 * 1024;
            constexpr std::size_t maximum_marker_bytes = 4096;
            constexpr std::size_t json_depth = 8;
            constexpr std::size_t json_events = 8192;
            // supported_build::game_sha256 and steam_build_id.
            constexpr std::string_view supported_game_sha256 =
                "fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9";
            constexpr std::string_view supported_steam_build = "25414733";

            std::string ascii_lower(std::string_view text)
            {
                std::string result(text);
                for (auto &ch : result)
                    if (ch >= 'A' && ch <= 'Z')
                        ch = static_cast<char>(ch + ('a' - 'A'));
                return result;
            }

            std::string read_file(const fs::path &path, std::size_t maximum)
            {
                std::ifstream input(path, std::ios::binary | std::ios::ate);
                if (!input)
                    throw std::runtime_error("Cannot open " + path_utf8(path));
                const auto length = input.tellg();
                if (length < 0 || static_cast<std::uint64_t>(length) > maximum)
                    throw std::runtime_error("File exceeds size limit: " + path_utf8(path));
                std::string bytes(static_cast<std::size_t>(length), '\0');
                input.seekg(0);
                if (!bytes.empty() && !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
                    throw std::runtime_error("Cannot read " + path_utf8(path));
                return bytes;
            }

            // Text bound for display, kept to printable characters and a sane length. UTF-8 passes through;
            // control bytes become '?'.
            std::string printable(std::string_view text, std::size_t limit)
            {
                std::string result;
                for (const auto value : text.substr(0, limit))
                {
                    const auto ch = static_cast<unsigned char>(value);
                    result += ch >= 0x20 && ch != 0x7f ? value : '?';
                }
                if (text.size() > limit)
                {
                    while (!result.empty() && (static_cast<unsigned char>(result.back()) & 0xc0) == 0x80)
                        result.pop_back();
                    if (!result.empty() && static_cast<unsigned char>(result.back()) >= 0xc0)
                        result.pop_back();
                    result += "...";
                }
                return result;
            }

            // ReSkate reads these files with RapidJSON through its own value type, which is stricter than
            // nlohmann's defaults: a repeated key, nesting deeper than 8 or more than 8192 parse events is an
            // error. This SAX reader applies the same limits while it builds the value.
            class StrictReader
            {
              public:
                Json root;
                std::string failure;

                bool null()
                {
                    return scalar(nullptr);
                }
                bool boolean(bool value)
                {
                    return scalar(value);
                }
                bool number_integer(Json::number_integer_t value)
                {
                    return scalar(value);
                }
                bool number_unsigned(Json::number_unsigned_t value)
                {
                    return scalar(value);
                }
                bool number_float(Json::number_float_t value, const Json::string_t &)
                {
                    return scalar(value);
                }
                bool string(Json::string_t &value)
                {
                    return scalar(value);
                }
                bool binary(Json::binary_t &)
                {
                    failure = "Invalid JSON: binary value";
                    return false;
                }
                bool start_object(std::size_t)
                {
                    return start(Json::object());
                }
                bool start_array(std::size_t)
                {
                    return start(Json::array());
                }
                bool end_object()
                {
                    return end();
                }
                bool end_array()
                {
                    return end();
                }
                bool key(Json::string_t &name)
                {
                    if (!event(stack_.size()))
                        return false;
                    auto &frame = stack_.back();
                    if (frame.value->contains(name))
                    {
                        failure = "Duplicate local profile JSON key";
                        return false;
                    }
                    frame.key = name;
                    return true;
                }
                bool parse_error(std::size_t position, const std::string &, const nlohmann::detail::exception &error)
                {
                    std::string what = error.what();
                    if (what.starts_with("[json.exception."))
                    {
                        if (auto close = what.find("] "); close != std::string::npos)
                            what.erase(0, close + 2);
                    }
                    failure = "Invalid JSON at byte " + std::to_string(position) + ": " + what;
                    return false;
                }

              private:
                struct Frame
                {
                    Json *value;
                    std::string key;
                };
                std::vector<Frame> stack_;
                std::size_t events_ = 0;

                bool event(std::size_t depth)
                {
                    if (depth > json_depth || ++events_ > json_events)
                    {
                        failure = "Local profile JSON is too complex";
                        return false;
                    }
                    return true;
                }
                Json *add(Json value)
                {
                    if (stack_.empty())
                    {
                        root = std::move(value);
                        return &root;
                    }
                    auto &frame = stack_.back();
                    if (frame.value->is_object())
                    {
                        auto &slot = (*frame.value)[frame.key];
                        slot = std::move(value);
                        return &slot;
                    }
                    frame.value->push_back(std::move(value));
                    return &frame.value->back();
                }
                bool scalar(Json value)
                {
                    if (!event(stack_.size()))
                        return false;
                    add(std::move(value));
                    return true;
                }
                bool start(Json value)
                {
                    if (!event(stack_.size()))
                        return false;
                    Json *node = add(std::move(value));
                    stack_.push_back({node, {}});
                    return true;
                }
                bool end()
                {
                    stack_.pop_back();
                    return event(stack_.size());
                }
            };

            // mod_list.cpp parse(): read with a size cap, skip a byte-order mark (Json::parse skips one more), then
            // parse strictly. Throws std::runtime_error with ReSkate's wording.
            Json parse(const fs::path &path, std::size_t maximum)
            {
                const std::string bytes = read_file(path, maximum);
                std::string_view text(bytes);
                if (text.starts_with("\xef\xbb\xbf"))
                    text.remove_prefix(3);
                if (text.starts_with("\xef\xbb\xbf"))
                    text.remove_prefix(3);
                // nlohmann would skip a third mark; RapidJSON does not.
                if (text.starts_with("\xef\xbb\xbf"))
                    throw std::runtime_error("Invalid JSON at byte 0: Invalid value.");
                StrictReader reader;
                if (!Json::sax_parse(text, &reader, Json::input_format_t::json, true, false))
                    throw std::runtime_error(reader.failure.empty() ? "Invalid JSON" : reader.failure);
                // nlohmann, like RapidJSON's MemoryStream, stops at a NUL byte as if the text ended there. ReSkate
                // then checks that it read the whole file (json.cpp), so "{...}\0garbage" is rejected, not read.
                if (text.find('\0') != std::string_view::npos)
                    throw std::runtime_error("Unexpected data after JSON root");
                return std::move(reader.root);
            }

            std::string string_field(const Json &root, const char *name, std::size_t limit)
            {
                return root.contains(name) && root.at(name).is_string()
                           ? printable(root.at(name).get_ref<const std::string &>(), limit)
                           : std::string{};
            }

            // Folder names travel into engine paths: plain ASCII letters, digits, space, '_', '-' and '.', at
            // most 64 characters, not starting with '.'.
            bool valid_mod_name(std::string_view name)
            {
                if (name.empty() || name.size() > maximum_mod_name || name.front() == '.')
                    return false;
                return std::all_of(name.begin(),
                                   name.end(),
                                   [](char value)
                                   {
                                       const auto ch = static_cast<unsigned char>(value);
                                       return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                              (ch >= '0' && ch <= '9') || value == '_' || value == '-' ||
                                              value == '.' || value == ' ';
                                   });
            }

            // The path in generic form as ASCII, or "" when it holds any other character.
            std::string ascii_path(const fs::path &path)
            {
                const auto wide = path.generic_wstring();
                std::string text;
                text.reserve(wide.size());
                for (const auto ch : wide)
                {
                    if (ch == 0 || ch > 0x7f)
                        return {};
                    text.push_back(static_cast<char>(ch));
                }
                return text;
            }

            struct Mod
            {
                std::string name;
                fs::path directory;
                bool provides_layout = false;
                bool provides_levels = false;
                std::vector<std::string> park_maps;
                std::string title, author, version, description;
                std::string tool, built;
                std::vector<std::string> levels;
                bool studio_marker = false;
                std::string skate_sha256;
                std::string outdated;
            };

            struct Entry
            {
                Mod mod;
                bool enabled = true;
            };

            struct Exclusion
            {
                std::string fingerprint;
                std::string sdk;
                std::vector<std::string> problems;
            };

            struct ModList
            {
                fs::path root;
                bool present = false;
                bool order_file = false; // Studio+ only: mods.json exists as a file
                std::vector<Entry> entries;
                std::vector<std::string> missing;
                std::map<std::string, std::vector<std::string>, std::less<>> excluded;
                std::map<std::string, std::string, std::less<>> excluded_sdk; // Studio+ only: the dll that left it out
                std::string issue;
                std::vector<std::string> notes;
            };

            // reskate-build.json: {"schema":1,"tool":"ReSkateStudio","version":"1.0.0","built":"..."}. A broken
            // file is a note, never a reason to skip the mod.
            void read_build_info(Mod &mod, std::vector<std::string> &notes)
            {
                const auto path = mod.directory / "reskate-build.json";
                std::error_code error;
                if (!fs::is_regular_file(path, error) || error)
                    return;
                try
                {
                    const auto root = parse(path, maximum_build_bytes);
                    if (!root.is_object())
                        throw std::runtime_error("not a JSON object");
                    mod.tool = string_field(root, "tool", 64);
                    mod.version = string_field(root, "version", 64);
                    mod.built = string_field(root, "built", 64);
                    if (mod.tool.empty() && mod.version.empty() && mod.built.empty())
                        throw std::runtime_error("it names no tool, version or build time");
                }
                catch (const std::exception &failure)
                {
                    notes.push_back("Mod " + mod.name + ": reskate-build.json could not be read (" + failure.what() +
                                    ")");
                }
            }

            // manifest.json, else reskate-mod.json (older mods): the author's own description of the mod.
            void read_mod_info(Mod &mod, std::vector<std::string> &notes)
            {
                std::error_code error;
                const bool manifest = fs::is_regular_file(mod.directory / "manifest.json", error);
                const char *name = manifest ? "manifest.json" : "reskate-mod.json";
                const auto path = mod.directory / name;
                if (!fs::is_regular_file(path, error) || error)
                    return;
                try
                {
                    const auto root = parse(path, maximum_info_bytes);
                    if (!root.is_object())
                        throw std::runtime_error("not a JSON object");
                    if (auto title = string_field(root, "name", 64); !title.empty())
                        mod.title = std::move(title);
                    mod.author = string_field(root, "author", 64);
                    auto version = string_field(root, manifest ? "version_number" : "version", 32);
                    if (version.empty())
                        version = string_field(root, manifest ? "version" : "version_number", 32);
                    if (!version.empty())
                        mod.version = std::move(version);
                    mod.description = string_field(root, "description", 1024);
                }
                catch (const std::exception &failure)
                {
                    notes.push_back("Mod " + mod.name + ": " + name + " could not be read (" + failure.what() + ")");
                }
            }

            // The level assets reskate-levels.json registers, read loosely (at most 64).
            void read_levels(Mod &mod)
            {
                if (!mod.provides_levels)
                    return;
                try
                {
                    const auto root = parse(mod.directory / "reskate-levels.json", maximum_levels_bytes);
                    if (!root.is_object() || !root.contains("levels") || !root.at("levels").is_array())
                        return;
                    for (const auto &row : root.at("levels"))
                    {
                        if (!row.is_object() || !row.contains("asset") || !row.at("asset").is_string())
                            continue;
                        mod.levels.push_back(printable(row.at("asset").get_ref<const std::string &>(), 160));
                        if (mod.levels.size() >= 64)
                            break;
                    }
                }
                catch (...)
                {
                    // The runtime reports what is wrong with the file when it reads it.
                }
            }

            // .reskate-studio-patch: the stamp line, then key=value lines. A mod that ships game data must record
            // the Skate.exe it was built for, and it must be the supported one.
            void read_studio_marker(Mod &mod)
            {
                const auto path = mod.directory / ".reskate-studio-patch";
                std::error_code error;
                if (fs::is_regular_file(path, error) && !error)
                {
                    mod.studio_marker = true;
                    try
                    {
                        const auto bytes = read_file(path, maximum_marker_bytes);
                        std::string_view text(bytes);
                        if (text.starts_with("\xef\xbb\xbf"))
                            text.remove_prefix(3);
                        for (std::size_t at = 0; at < text.size();)
                        {
                            const auto end = text.find('\n', at);
                            auto line =
                                text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
                            if (!line.empty() && line.back() == '\r')
                                line.remove_suffix(1);
                            if (line.starts_with("skate_sha256="))
                            {
                                auto value = ascii_lower(line.substr(13));
                                const bool hex =
                                    value.size() == 64 &&
                                    std::all_of(value.begin(),
                                                value.end(),
                                                [](char c)
                                                { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
                                if (hex)
                                    mod.skate_sha256 = std::move(value);
                            }
                            if (end == std::string_view::npos)
                                break;
                            at = end + 1;
                        }
                    }
                    catch (...)
                    {
                    }
                }
                if (mod.studio_marker)
                {
                    if (mod.skate_sha256.empty())
                        mod.outdated =
                            "it was built by an older ReSkate Studio that does not record which game version it is for";
                    else if (mod.skate_sha256 != supported_game_sha256)
                        mod.outdated =
                            "it was built for another game version (Skate.exe " + mod.skate_sha256.substr(0, 12) + ")";
                }
                else if (mod.provides_layout)
                {
                    mod.outdated = "it ships game data without a ReSkate Studio game version stamp";
                }
            }

            std::vector<std::string> folders_on_disk(const fs::path &root)
            {
                std::vector<std::string> names;
                std::error_code error;
                for (const auto &entry : fs::directory_iterator(root, error))
                {
                    if (error)
                        break;
                    if (!entry.is_directory(error) || error)
                    {
                        error.clear();
                        continue;
                    }
                    const auto name = ascii_path(entry.path().filename());
                    if (valid_mod_name(name))
                        names.push_back(name);
                }
                std::sort(names.begin(),
                          names.end(),
                          [](const std::string &a, const std::string &b) { return ascii_lower(a) < ascii_lower(b); });
                return names;
            }

            // mods.json rows in priority order. Throws when the file is malformed, so a typo cannot silently
            // change which mods load.
            std::vector<std::pair<std::string, bool>> read_order(const fs::path &path)
            {
                const auto root = parse(path, maximum_order_bytes);
                if (!root.is_object() || root.size() != 2 || !root.contains("schema") || !root.contains("mods"))
                    throw std::runtime_error("mods.json must hold exactly schema and mods");
                if (!root.at("schema").is_number_integer() || root.at("schema").get<std::uint64_t>() != 1)
                    throw std::runtime_error("mods.json schema must be integer 1");
                const auto &rows = root.at("mods");
                if (!rows.is_array())
                    throw std::runtime_error("mods.json mods must be an array");

                std::vector<std::pair<std::string, bool>> order;
                std::set<std::string, std::less<>> seen;
                for (const auto &row : rows)
                {
                    if (!row.is_object() || row.size() != 2 || !row.contains("name") || !row.contains("enabled"))
                        throw std::runtime_error("each mods.json row must hold exactly name and enabled");
                    const auto &name = row.at("name");
                    const auto &enabled = row.at("enabled");
                    if (!name.is_string() || !valid_mod_name(name.get_ref<const std::string &>()))
                        throw std::runtime_error("mods.json name is not a plain folder name");
                    if (!enabled.is_boolean())
                        throw std::runtime_error("mods.json enabled must be true or false");
                    const auto &text = name.get_ref<const std::string &>();
                    if (!seen.emplace(ascii_lower(text)).second)
                        throw std::runtime_error("mods.json lists " + text + " twice");
                    order.emplace_back(text, enabled.get<bool>());
                }
                return order;
            }

            // Names, sizes and write times of every file in the mod folder (FNV-1a), as ReSkate records it.
            std::string mod_fingerprint(const fs::path &directory)
            {
                std::vector<std::string> files;
                std::error_code error;
                for (fs::recursive_directory_iterator it(directory, error), end; !error && it != end;
                     it.increment(error))
                {
                    if (!it->is_regular_file(error))
                    {
                        error.clear();
                        continue;
                    }
                    const auto size = it->file_size(error);
                    const auto written = it->last_write_time(error).time_since_epoch().count();
                    const auto relative = fs::relative(it->path(), directory, error).generic_wstring();
                    std::string name;
                    for (const auto ch : relative)
                    {
                        if (ch < 0x80)
                            name.push_back(static_cast<char>(ch));
                        else
                            name += '#' + std::to_string(static_cast<unsigned>(ch));
                    }
                    files.push_back(ascii_lower(name) + '|' + std::to_string(size) + '|' + std::to_string(written));
                    error.clear();
                }
                std::sort(files.begin(), files.end());
                std::uint64_t hash = 1469598103934665603ull;
                for (const auto &file : files)
                    for (const auto c : file + '\n')
                    {
                        hash ^= static_cast<unsigned char>(c);
                        hash *= 1099511628211ull;
                    }
                return std::to_string(files.size()) + '-' + std::format("{:016x}", hash);
            }

            // <Mods>/.reskate-excluded.json. Rows are read in name order (ReSkate keeps them in a sorted map), and
            // a bad row stops the reading but keeps the rows read before it, as ReSkate does.
            std::map<std::string, Exclusion, std::less<>> read_exclusions(const fs::path &mods_root) noexcept
            {
                std::map<std::string, Exclusion, std::less<>> result;
                try
                {
                    const auto path = mods_root / ".reskate-excluded.json";
                    std::error_code error;
                    if (!fs::is_regular_file(path, error))
                        return result;
                    const auto root = parse(path, maximum_order_bytes);
                    if (!root.is_object() || !root.contains("mods") || !root.at("mods").is_object())
                        return result;
                    const auto &rows = root.at("mods");
                    std::vector<std::string> names;
                    for (auto it = rows.begin(); it != rows.end(); ++it)
                        names.push_back(it.key());
                    std::sort(names.begin(), names.end());
                    for (const auto &name : names)
                    {
                        const auto &row = rows.at(name);
                        if (!valid_mod_name(name) || !row.is_object())
                            continue;
                        Exclusion exclusion;
                        exclusion.fingerprint = row.value("fingerprint", std::string{});
                        exclusion.sdk = row.value("sdk", std::string{});
                        if (row.contains("problems") && row.at("problems").is_array())
                            for (const auto &problem : row.at("problems"))
                                if (problem.is_string() && exclusion.problems.size() < 16)
                                    exclusion.problems.push_back(
                                        printable(problem.get_ref<const std::string &>(), 600));
                        if (!exclusion.fingerprint.empty())
                            result.emplace(name, std::move(exclusion));
                    }
                }
                catch (...)
                { /* An unreadable file just means merging again. */
                }
                return result;
            }

            // mod_catalog.cpp sdk_identity(): ReSkate.dll beside Skate.exe, as size-writetime. The game honours an
            // exclusion only while the dll that wrote it is still the installed one.
            std::string sdk_identity(const fs::path &game_directory)
            {
                std::error_code error;
                const auto file = game_directory / L"ReSkate.dll";
                const auto size = fs::file_size(file, error);
                if (error)
                    return {};
                const auto written = fs::last_write_time(file, error).time_since_epoch().count();
                return error ? std::string{} : std::to_string(size) + "-" + std::to_string(written);
            }

            // Launcher/mod_manager.cpp mods_root(): the game is started with -dataPath "ModData/Default" when a
            // ModData folder exists, so that is where its Mods folder is.
            fs::path data_root(const fs::path &game_directory)
            {
                return dir_exists(game_directory / L"ModData") ? game_directory / L"ModData" / L"Default"
                                                               : game_directory;
            }

            // scan_mods(): every mod folder, highest priority first (mods.json rows in their order, then folders
            // it does not list, by name and enabled). Throws only Error("cancelled"); every other failure lands in
            // issue.
            ModList scan_mods(const fs::path &data_root, const Context &context)
            {
                ModList result;
                try
                {
                    result.root = data_root / "Mods";
                    std::error_code error;
                    if (!fs::is_directory(result.root, error) || error)
                        return result;
                    result.present = true;

                    const auto names = folders_on_disk(result.root);
                    std::map<std::string, std::string, std::less<>> present;
                    for (const auto &name : names)
                        present.emplace(ascii_lower(name), name);

                    std::vector<std::pair<std::string, bool>> order;
                    const auto order_path = result.root / "mods.json";
                    if (fs::is_regular_file(order_path, error))
                    {
                        result.order_file = true;
                        try
                        {
                            order = read_order(order_path);
                        }
                        catch (const std::exception &failure)
                        {
                            result.issue = failure.what();
                            order.clear();
                        }
                    }
                    std::set<std::string, std::less<>> listed;
                    for (const auto &row : order)
                        listed.emplace(ascii_lower(row.first));
                    for (const auto &name : names)
                        if (!listed.contains(ascii_lower(name)))
                            order.emplace_back(name, true);

                    for (const auto &[name, enabled] : order)
                    {
                        if (context.cancelled())
                            throw Error("cancelled", "Cancelled");
                        const auto found = present.find(ascii_lower(name));
                        if (found == present.end())
                        {
                            result.missing.push_back(name);
                            continue;
                        }
                        Entry entry;
                        entry.enabled = enabled;
                        auto &mod = entry.mod;
                        mod.name = found->second;
                        mod.title = mod.name;
                        mod.directory = result.root / mod.name;
                        mod.provides_layout = fs::is_regular_file(mod.directory / "layout.toc", error);
                        mod.provides_levels = fs::is_regular_file(mod.directory / "reskate-levels.json", error);
                        for (fs::directory_iterator it(mod.directory / "parks", error), end; !error && it != end;
                             it.increment(error))
                        {
                            auto file = ascii_path(it->path().filename());
                            if (!file.ends_with(".park.json") || mod.park_maps.size() >= 16)
                                continue;
                            file.resize(file.size() - 10);
                            mod.park_maps.push_back(std::move(file));
                        }
                        error.clear();
                        std::sort(mod.park_maps.begin(), mod.park_maps.end());
                        read_build_info(mod, result.notes);
                        read_mod_info(mod, result.notes);
                        read_levels(mod);
                        read_studio_marker(mod);
                        result.entries.push_back(std::move(entry));
                    }
                    for (const auto &[name, exclusion] : read_exclusions(result.root))
                        for (const auto &entry : result.entries)
                            if (entry.mod.name == name && mod_fingerprint(entry.mod.directory) == exclusion.fingerprint)
                            {
                                result.excluded.emplace(name, exclusion.problems);
                                result.excluded_sdk.emplace(name, exclusion.sdk);
                            }
                }
                catch (const Error &)
                {
                    throw;
                }
                catch (const std::exception &failure)
                {
                    if (result.issue.empty())
                        result.issue = failure.what();
                }
                catch (...)
                {
                    if (result.issue.empty())
                        result.issue = "The Mods folder could not be read";
                }
                return result;
            }

            std::string join(const std::vector<std::string> &parts, std::string_view separator)
            {
                std::string text;
                for (const auto &part : parts)
                {
                    if (!text.empty())
                        text += separator;
                    text += part;
                }
                return text;
            }

            Json text_or_null(const std::string &text)
            {
                return text.empty() ? Json(nullptr) : Json(text);
            }

        } // namespace modlist

    } // namespace

    // A Mods folder name: 1-64 letters, digits, spaces, _ - or ., not starting with '.', not ending with a space
    // or '.', and not a Windows device name. Throws Error("invalid_mod_folder").
    void check_mod_folder(const std::string &name, const char *param = "mod-folder")
    {
        bool name_ok = !name.empty() && name.size() <= 64 && name[0] != '.';
        for (char ch : name)
            if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == ' ' || ch == '_' || ch == '-' || ch == '.'))
                name_ok = false;
        if (!name_ok)
            throw Error("invalid_mod_folder",
                        "Mod folder name \"" + name +
                            "\" is not usable: 1-64 letters, digits, spaces, '_', '-' or '.', not starting with '.'",
                        {{"param", param}});
        // Windows device names cannot be folder names, with or without an extension ("nul.txt").
        std::string base = lower_ascii(name.substr(0, name.find('.')));
        while (!base.empty() && base.back() == ' ')
            base.pop_back();
        const bool device = base == "con" || base == "prn" || base == "aux" || base == "nul" ||
                            (base.size() == 4 && (base.rfind("com", 0) == 0 || base.rfind("lpt", 0) == 0) &&
                             base[3] >= '0' && base[3] <= '9');
        if (device)
            throw Error("invalid_mod_folder",
                        "Mod folder name \"" + name + "\" is a reserved Windows device name; pick another name",
                        {{"param", param}});
        if (name.back() == ' ' || name.back() == '.')
            throw Error("invalid_mod_folder",
                        "Mod folder name \"" + name +
                            "\" must not end with a space or '.' (Windows drops them from folder names)",
                        {{"param", param}});
    }

    // Where deploy backups go: <game>\.ReSkateStudio-Mod-backup.
    fs::path backup_root(const fs::path &game)
    {
        return game / L".ReSkateStudio-Mod-backup";
    }

    // The folder mod `name` is installed in: the Mods folder ReSkate reads (ModData\Default\Mods when ModData
    // exists), else <game>\Mods, where reskate_cli deploy-mod writes. Empty when it is in neither.
    fs::path installed_mod(const fs::path &game, const std::string &name)
    {
        for (const fs::path &mods : {modlist::data_root(game) / L"Mods", game / L"Mods"})
            if (const fs::path p = mods / utf8_to_wide(name); dir_exists(p))
                return p;
        return {};
    }

    // A folder ReSkate Studio (or Studio+) built: deploy writes the .reskate-studio-patch stamp into it.
    bool studio_built(const fs::path &folder)
    {
        return file_exists(folder / L".reskate-studio-patch");
    }

    // "2026-10-07 22-40-05", local time, for backup folder names.
    std::string stamp_now()
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char text[32];
        std::strftime(text, sizeof text, "%Y-%m-%d %H-%M-%S", &local);
        return text;
    }

    // Moves a folder within the game folder's drive. Throws Error("move_failed").
    void move_folder(const fs::path &from, const fs::path &to)
    {
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        fs::rename(from, to, ec);
        if (ec)
            throw Error("move_failed",
                        "Could not move " + path_utf8(from) + " to " + path_utf8(to) + ": " + ec.message() +
                            " (is a file in it open, or the game running?)",
                        {{"from", path_utf8(from)}, {"to", path_utf8(to)}});
    }

    // The ReSkate folder for Kraken compression, or empty (raw blocks, which read back the same) when none is set.
    fs::path oodle_root(const Context &c, const Json &a)
    {
        try
        {
            return c.game_root(a);
        }
        catch (const Error &)
        {
            return {};
        }
    }

    // Writes the .fbproject `project` as the .fbmod `output`: the same resources, with the project's title and
    // author unless `a` overrides them. ReSkate Studio did this with "Export .fbmod"; reskate_cli has no command for
    // it.
    Json export_project(const Context &c, const Json &a, const fs::path &project, const fs::path &output)
    {
        native::Project p;
        try
        {
            p = native::read_fbproject(project);
        }
        catch (const std::exception &e)
        {
            throw Error("not_a_project", path_utf8(project) + ": " + e.what(), {{"path", path_utf8(project)}});
        }
        for (const auto &[key, field] : {std::pair{"title", &p.info.title},
                                         {"author", &p.info.author},
                                         {"version", &p.info.version},
                                         {"description", &p.info.description}})
            if (const std::string v = a.contains(key) && a[key].is_string() ? a[key].get<std::string>() : "";
                !v.empty())
                *field = v;
        if (p.info.title.empty())
            p.info.title = path_utf8(project.stem());
        if (p.info.version.empty())
            p.info.version = "1.0";
        try
        {
            native::write_fbmod(output, p.info, p.resources, oodle_root(c, a));
        }
        catch (const std::exception &e)
        {
            throw Error(
                "write_failed", std::string(e.what()) + ": " + path_utf8(output), {{"path", path_utf8(output)}});
        }
        int ebx = 0, res = 0, chunks = 0, added = 0;
        for (const auto &r : p.resources)
        {
            (r.kind == native::ModResource::Kind::ebx   ? ebx
             : r.kind == native::ModResource::Kind::res ? res
                                                        : chunks) += 1;
            added += r.added ? 1 : 0;
        }
        std::error_code ec;
        const auto bytes = fs::file_size(output, ec);
        return {{"project", path_utf8(project)},
                {"output", path_utf8(output)},
                {"title", p.info.title},
                {"author", p.info.author.empty() ? Json(nullptr) : Json(p.info.author)},
                {"version", p.info.version},
                {"profile", p.profile},
                {"head", p.info.head},
                {"resources", p.resources.size()},
                {"ebx", ebx},
                {"res", res},
                {"chunks", chunks},
                {"added", added},
                {"bytes", ec ? 0 : static_cast<std::uint64_t>(bytes)}};
    }

    void register_mod_commands(Registry &r)
    {
        // ---- mod compile ------------------------------------------------------------------------------
        r.add({
            .group = "mod",
            .name = "compile",
            .summary = "Build .fbmod files into a staging folder ready to install",
            .description =
                "Compiles one or more binary .fbmod files against the installed game into a native Frostbite Patch "
                "staging folder (reskate_cli compile-mod). The fbmods are applied in the order given. Reads the game, "
                "writes only into the staging folder: .reskate-studio-staging, and Patch\\ with layout.toc, "
                "initfs_win32, "
                ".reskate-studio-patch (records the Skate.exe SHA-256), Win32\\...\\cas_01.cas archives and level TOCs "
                "(about 60 MB for a small animation mod). Install the result with 'mod deploy'.\n\n"
                "Gotchas: the staging folder must be outside the Skate folder, and if it exists it must be empty or a "
                "previous staging folder (which is wiped and rebuilt), and the fbmods must not sit inside it. A "
                ".fbproject is "
                "exported to an .fbmod first (as 'project export' does) into the Studio+ data folder's "
                "project-exports. The build is tied to the exact Skate.exe; after a game update, compile again. "
                "Not every fbmod compiles (a skin-tone fbmod failed with 'Cosmetic shader parameter dependency is "
                "unavailable'); a failure after loading leaves a staging folder holding only the marker. Takes 3-4 s "
                "for a small mod (2.7 s is the game index load).",
            .params =
                {
                    {"staging-dir",
                     ParamType::Path,
                     "Output staging folder, outside the Skate folder. Created if missing",
                     true,
                     true},
                    {"fbmod",
                     ParamType::List,
                     "One or more .fbmod (or .fbproject) files, applied in order",
                     true,
                     true},
                    game_root_param(),
                },
            .examples = {"mod compile C:\\mods\\stage C:\\mods\\anim.fbmod",
                         "mod compile C:\\mods\\stage base.fbmod tweaks.fbmod --json"},
            .long_running = true,
            .run = [](Context &c, const Json &a) -> Json
            {
                fs::path game = checked_game_root(c, a);
                fs::path staging = to_path(a, "staging-dir");
                std::string staging_s = path_utf8(staging);
                if (is_within(staging, game) || is_within(game, staging))
                    throw Error("staging_inside_game",
                                "The staging folder must be outside the Skate folder: " + staging_s,
                                {{"param", "staging-dir"}, {"path", staging_s}});
                if (path_exists(staging))
                {
                    if (!dir_exists(staging))
                        throw Error("not_a_folder", "Staging path is a file: " + staging_s, {{"param", "staging-dir"}});
                    std::error_code ec;
                    bool empty = fs::is_empty(staging, ec);
                    if (!empty && !file_exists(staging / L".reskate-studio-staging"))
                        throw Error("staging_not_owned",
                                    "Staging folder is not empty and is not a ReSkate Studio staging folder (it would "
                                    "be wiped): " +
                                        staging_s,
                                    {{"param", "staging-dir"}, {"path", staging_s}});
                }
                std::vector<std::string> args = {"compile-mod", path_utf8(game), staging_s};
                Json inputs = Json::array();
                Json converted = Json::array();
                for (const auto &item : a["fbmod"])
                {
                    fs::path p = absolute_path(fs::path(utf8_to_wide(item.get<std::string>())));
                    require_file(p, "fbmod");
                    if (is_within(p, staging))
                        throw Error("fbmod_inside_staging",
                                    path_utf8(p) + " is inside the staging folder, which is wiped before the build. "
                                                   "Move the fbmod out first.",
                                    {{"param", "fbmod"}, {"path", path_utf8(p)}});
                    std::wstring ext = lower(p.extension().native());
                    if (ext == L".fbproject")
                    {
                        // The engine only takes .fbmod: export the project first, as 'project export' does.
                        const fs::path folder = Settings::data_dir() / L"project-exports";
                        std::error_code ec;
                        fs::create_directories(folder, ec);
                        const fs::path fbmod =
                            folder / (std::to_wstring(converted.size() + 1) + L"-" + p.stem().native() + L".fbmod");
                        c.progress(-1, "Exporting " + path_utf8(p.filename()) + " as .fbmod");
                        export_project(c, a, p, fbmod);
                        converted.push_back({{"project", path_utf8(p)}, {"fbmod", path_utf8(fbmod)}});
                        p = fbmod;
                    }
                    args.push_back(path_utf8(p));
                    inputs.push_back(path_utf8(p));
                }
                c.progress(-1, "Compiling " + std::to_string(inputs.size()) + " fbmod(s) against the game");
                EngineRun run = run_engine(c, args);
                try
                {
                    require_ok(run,
                               {{"file is not a binary FBMOD", "not_an_fbmod"},
                                {"staging is not empty", "staging_not_owned"},
                                {"dedicated folder outside the game", "staging_inside_game"},
                                {"dependency is unavailable", "mod_not_compilable"}});
                }
                catch (Error &e)
                {
                    // The engine does not say which input it means.
                    if (e.code != "not_an_fbmod" && e.code != "file_unreadable")
                        throw;
                    std::string which;
                    for (const auto &in : inputs)
                        which += (which.empty() ? "" : ", ") + in.get<std::string>();
                    if (inputs.size() > 1)
                        which = "one of " + which;
                    throw Error(e.code, std::string(e.what()) + ": " + which, {{"param", "fbmod"}, {"fbmods", inputs}});
                }
                Json counts = kv_object(run.out_lines);
                Json out = {
                    {"staging_dir", staging_s}, {"patch_dir", path_utf8(staging / L"Patch")}, {"fbmods", inputs}};
                if (!converted.empty())
                    out["exported_projects"] = converted;
                for (const char *k : {"resources", "bundles", "tocs"})
                    out[k] = counts.contains(k) ? counts[k] : Json(nullptr);
                out["skate_sha256"] = path_or_null(marker_sha(staging / L"Patch" / L".reskate-studio-patch"));
                out["seconds"] = run.seconds;
                return out;
            },
        });

        // ---- mod deploy ------------------------------------------------------------------------------
        r.add({
            .group = "mod",
            .name = "deploy",
            .summary = "Install a compiled staging folder into the game's Mods folder",
            .description =
                "WRITES TO THE GAME FOLDER. Installs a 'mod compile' staging build into <game>\\Mods\\<mod-folder> as "
                "a "
                "transaction (reskate_cli deploy-mod): copies the staging Patch folder, verifies the copy, moves any "
                "previous version to <game>\\.ReSkateStudio-Mod-backup\\<mod-folder>, and writes the receipt "
                "<game>\\.ReSkateStudio-last-deploy.db. It may retire an old-style root Patch folder marked "
                ".frosty-managed-skate-patch. Skate must be closed (checked before starting).\n\n"
                "Gotchas: the build must have been compiled against the current Skate.exe (same SHA-256), otherwise "
                "compile again. The mod folder is a name, not a path: 1-64 letters, digits, spaces, '_', '-' or '.', "
                "not "
                "starting with '.', not ending with a space or '.', and not a Windows device name such as CON or NUL. "
                "An existing Mods folder that ReSkate Studio did not create is refused. Use the same "
                "name again to update a mod. See installed mods with 'mod list'.",
            .params =
                {
                    {"staging-dir",
                     ParamType::Path,
                     "Staging folder made by 'mod compile' (holds Patch\\layout.toc and Patch\\initfs_win32)",
                     true,
                     true},
                    {"mod-folder", ParamType::String, "Folder name under <game>\\Mods, e.g. ScooterPush", true, true},
                    game_root_param(),
                },
            .examples = {"mod deploy C:\\mods\\stage ScooterPush",
                         "mod deploy C:\\mods\\stage ScooterPush --game-root D:\\TestSkate --json"},
            .writes_game = true,
            .run = [](Context &c, const Json &a) -> Json
            {
                const std::string name = a["mod-folder"];
                check_mod_folder(name);
                fs::path game = checked_game_root(c, a);
                fs::path staging = to_path(a, "staging-dir");
                std::string staging_s = path_utf8(staging);
                if (!dir_exists(staging))
                    throw Error(
                        "path_not_found", "Staging folder does not exist: " + staging_s, {{"param", "staging-dir"}});
                if (!file_exists(staging / L"Patch" / L"layout.toc") ||
                    !file_exists(staging / L"Patch" / L"initfs_win32"))
                    throw Error("staging_incomplete",
                                "Not a compiled staging folder (needs Patch\\layout.toc and Patch\\initfs_win32): " +
                                    staging_s,
                                {{"param", "staging-dir"}});
                if (is_within(staging, game) || is_within(game, staging))
                    throw Error("staging_inside_game",
                                "Staging and Skate folders must be separate folder trees",
                                {{"param", "staging-dir"}});
                if (skate_running_from(game))
                    throw Error("game_running", "Skate is running. Close it before installing a mod.");
                EngineRun run = run_engine(c, {"deploy-mod", path_utf8(game), staging_s, name});
                require_ok(run,
                           {{"Skate.exe differs from this staged build", "game_version_mismatch"},
                            {"has no Skate.exe SHA-256", "game_version_mismatch"},
                            {"is not usable", "invalid_mod_folder"},
                            {"is not managed by ReSkate Studio", "mod_folder_not_managed"},
                            {"refusing to replace", "mod_folder_not_managed"},
                            {"already running", "deploy_busy"},
                            {"symbolic links or junctions", "links_not_allowed"},
                            {"does not contain the Skate Data", "not_a_game_root"}});
                Json out = {{"mod_folder", name}};
                Json notes = Json::array();
                for (const auto &line : run.out_lines)
                {
                    std::string k, v;
                    if (line.rfind("note:", 0) == 0)
                    {
                        auto start = line.find_first_not_of(' ', 5);
                        notes.push_back(start == std::string::npos ? std::string() : line.substr(start));
                    }
                    else if (split_kv(line, k, v))
                    {
                        if (k == "mod")
                            out["mod_path"] = v;
                        else if (k == "backup" || k == "retired-patch")
                            out[snake(k)] = path_or_null(v);
                        else
                            out[snake(k)] = typed_value(v);
                    }
                    else
                        notes.push_back(line);
                }
                out["notes"] = notes;
                return out;
            },
        });

        // ---- mod uninstall / restore --------------------------------------------------------------------
        r.add({
            .group = "mod",
            .name = "uninstall",
            .summary = "Take a Studio-built mod out of the Mods folder (kept as a backup you can restore)",
            .description =
                "WRITES TO THE GAME FOLDER. Moves <Mods>\\<mod-folder> into <game>\\.ReSkateStudio-Mod-backup as "
                "\"<mod-folder> (removed <date time>)\", so the game stops loading it and 'mod restore' can put it "
                "back. "
                "Nothing is deleted. Only mods ReSkate Studio or Studio+ installed (they hold .reskate-studio-patch) "
                "are "
                "taken out; other mods belong to the ReSkate launcher's mod manager. mods.json is left as it is (the "
                "launcher lists a row whose folder is gone as missing). Skate must be closed.",
            .params =
                {
                    {"mod-folder",
                     ParamType::String,
                     "The mod's folder name under Mods, as 'mod list' shows it",
                     true,
                     true},
                    game_root_param(),
                },
            .examples = {"mod uninstall Razor_Scooter_v2", "mod uninstall MyMap --json"},
            .writes_game = true,
            .run = [](Context &c, const Json &a) -> Json
            {
                const std::string name = a["mod-folder"];
                check_mod_folder(name);
                const fs::path game = checked_game_root(c, a);
                const fs::path folder = installed_mod(game, name);
                if (folder.empty())
                    throw Error("mod_not_found",
                                "No mod folder called " + name + " in the Mods folder; see 'mod list'",
                                {{"param", "mod-folder"}});
                if (!studio_built(folder))
                    throw Error("mod_folder_not_managed",
                                name + " was not installed by ReSkate Studio, so Studio+ leaves it alone. "
                                       "Turn it off or remove it in the ReSkate launcher's mod manager.",
                                {{"param", "mod-folder"}, {"path", path_utf8(folder)}});
                if (skate_running_from(game))
                    throw Error("game_running", "Skate is running. Close it before taking a mod out.");
                const std::string backup_name = name + " (removed " + stamp_now() + ")";
                const fs::path backup = backup_root(game) / utf8_to_wide(backup_name);
                c.progress(-1, "Moving " + name + " to the backups");
                move_folder(folder, backup);
                return {{"mod_folder", name},
                        {"removed_from", path_utf8(folder)},
                        {"backup", backup_name},
                        {"backup_path", path_utf8(backup)},
                        {"restore_with", "studio-plus mod restore \"" + backup_name + "\" --mod-folder " + name}};
            },
        });

        r.add({
            .group = "mod",
            .name = "restore",
            .summary = "Put a backed-up mod back into the Mods folder (undo a deploy or an uninstall)",
            .description =
                "WRITES TO THE GAME FOLDER. Moves a folder from <game>\\.ReSkateStudio-Mod-backup back into the Mods "
                "folder. 'mod deploy' keeps the version it replaced there under the mod's own name, and 'mod "
                "uninstall' "
                "keeps \"<name> (removed <date time>)\"; 'mod list' lists them all under backups. When the mod is "
                "installed now, that version swaps places with the backup (kept as \"<name> (replaced <date time>)\"), "
                "so "
                "restoring is itself undoable. Only Studio-built folders are moved. Skate must be closed.",
            .params =
                {
                    {"backup",
                     ParamType::String,
                     "Folder name under .ReSkateStudio-Mod-backup, as 'mod list' shows it under backups",
                     true,
                     true},
                    {"mod-folder",
                     ParamType::String,
                     "Mods folder name to restore it as (default: the backup's name without \" (removed ...)\")"},
                    game_root_param(),
                },
            .examples = {"mod restore Razor_Scooter_v2",
                         "mod restore \"MyMap (removed 2026-10-07 22-40-05)\" --mod-folder MyMap"},
            .writes_game = true,
            .run = [](Context &c, const Json &a) -> Json
            {
                const std::string backup_name = a["backup"];
                if (backup_name.empty() || backup_name.find_first_of("\\/:") != std::string::npos ||
                    backup_name == "." || backup_name == "..")
                    throw Error("invalid_arguments",
                                "--backup is a folder name under .ReSkateStudio-Mod-backup, not a path",
                                {{"param", "backup"}});
                std::string name = arg_string(a, "mod-folder");
                if (name.empty())
                    name = backup_name.substr(0, backup_name.find(" ("));
                check_mod_folder(name);
                const fs::path game = checked_game_root(c, a);
                const fs::path backup = backup_root(game) / utf8_to_wide(backup_name);
                if (!dir_exists(backup))
                    throw Error("backup_not_found",
                                "No backup called " + backup_name + " in " + path_utf8(backup_root(game)) +
                                    "; see 'mod list'",
                                {{"param", "backup"}});
                if (!studio_built(backup))
                    throw Error("mod_folder_not_managed",
                                backup_name + " is not a ReSkate Studio mod folder (no .reskate-studio-patch)",
                                {{"param", "backup"}});
                if (skate_running_from(game))
                    throw Error("game_running", "Skate is running. Close it before restoring a mod.");
                fs::path target = installed_mod(game, name);
                Json out = {{"mod_folder", name}, {"restored_from", path_utf8(backup)}};
                if (!target.empty())
                {
                    if (!studio_built(target))
                        throw Error("mod_folder_not_managed",
                                    "Mods\\" + name +
                                        " is there and was not installed by ReSkate Studio; "
                                        "restore under another --mod-folder",
                                    {{"param", "mod-folder"}, {"path", path_utf8(target)}});
                    const std::string kept = name + " (replaced " + stamp_now() + ")";
                    c.progress(-1, "Moving the installed " + name + " to the backups");
                    move_folder(target, backup_root(game) / utf8_to_wide(kept));
                    out["previous_kept_as"] = kept;
                    try
                    {
                        move_folder(backup, target);
                    }
                    catch (...)
                    {
                        // Put the installed version back so a failed restore changes nothing.
                        std::error_code ec;
                        fs::rename(backup_root(game) / utf8_to_wide(kept), target, ec);
                        throw;
                    }
                }
                else
                {
                    const fs::path ours = modlist::data_root(game) / L"Mods";
                    target = (dir_exists(ours) ? ours : game / L"Mods") / utf8_to_wide(name);
                    move_folder(backup, target);
                }
                out["mod_path"] = path_utf8(target);
                return out;
            },
        });

        // ---- mod dump -------------------------------------------------------------------------------
        r.add({
            .group = "mod",
            .name = "dump",
            .summary = "Dump a mod's TOCs and assets, including mesh GLBs/FBXs and texture DDS files",
            .description =
                "Reads a mod folder with layout.toc at its root and writes an extraction into "
                "<selected mod folder>\\\\.dump. The optional .reskate-studio-patch file is provenance metadata "
                "containing the Skate.exe SHA-256; it is not required and does not describe the mod's contents. "
                "The dumper follows layout.toc's superbundle TOC references, then each TOC's bundle manifests and "
                "CAS payload ranges under Win32\\\\. "
                "Copies the mod layout and superbundle TOCs, preserves bundle manifests and stored CAS payload "
                "ranges, and decodes changed EBX, RES and chunk payloads when their CAS blocks can be read. "
                "Writes asset-references.json with each raw EBX SHA-1, resolved resource/chunk references, dumped "
                "target paths, and source CAS range metadata. "
                "Also copies non-CAS loose files into source\\\\loose. "
                "Texture resources are converted to native-format DDS with available mips. MeshSet LODs are "
                "exported as static GLBs with material sections, positions, reconstructed normals and UV0; when "
                "Blender is configured, those GLBs are also exported as static FBXs. No armature, skin weights or "
                "donor skeleton is included because the compiled mod reader does not resolve donor assets or skeleton "
                "hierarchy/bind poses. Audio transcoding is not implemented. Unsupported mesh layouts, audio, levels "
                "and texture formats remain raw. Existing "
                "files in .dump are overwritten in place; unrelated files are left untouched. If --game-root is "
                "available, it is used to read inherited base-game payloads and Oodle-compressed data.",
            .params =
                {
                    {"mod-folder",
                     ParamType::Path,
                     "Mod folder containing layout.toc at its root and its referenced TOCs and Win32 CAS files",
                     true,
                     true},
                    game_root_param(),
                },
            .examples = {"mod dump C:\\\\Games\\\\Skate\\\\Mods\\\\ExampleMod",
                         "mod dump C:\\\\mods\\\\compiled\\\\ExampleMod --game-root C:\\\\Games\\\\Skate"},
            .long_running = true,
            .run = [](Context &c, const Json &a) -> Json
            {
                const auto mod = to_path(a, "mod-folder");
                fs::path game;
                if (a.contains("game-root") || !c.settings.game_root.empty())
                    game = c.game_root(a);
                c.progress(-1, "Preparing mod dump");
                native::ModDumpReport report;
                try
                {
                    report = native::dump_mod_folder(
                        mod, game, c.settings.blender,
                        [&c](std::string_view message) { c.progress(-1, message); }, c.cancel);
                }
                catch (const Error &)
                {
                    throw;
                }
                catch (const std::exception &failure)
                {
                    throw Error("mod_dump_failed", failure.what(), {{"mod_folder", path_utf8(mod)}});
                }
                Json warnings = Json::array();
                for (const auto &warning : report.warnings)
                    warnings.push_back(warning);
                return {{"mod_folder", path_utf8(mod)},
                        {"output", path_utf8(report.directory)},
                        {"asset_references", path_utf8(report.directory / L"asset-references.json")},
                        {"bundles", report.bundles},
                        {"ebx", report.ebx},
                        {"resources", report.resources},
                        {"chunks", report.chunks},
                        {"ebx_asset_references", report.ebx_asset_references},
                        {"resolved_ebx_asset_references", report.resolved_ebx_asset_references},
                        {"cas_blocks", report.cas_blocks},
                        {"loose_files", report.loose_files},
                        {"textures_dds", report.textures},
                        {"mesh_lods_glb", report.meshes},
                        {"mesh_lods_fbx", report.fbxs},
                        {"fbx_status", report.fbx_status},
                        {"warnings", warnings},
                        {"mesh_conversion",
                         "static GLB per LOD and static FBX when Blender is available; no armature, skin weights or "
                         "donor skeleton"},
                        {"audio_conversion", "unsupported; raw resources and chunks are retained"}};
            },
            .writes_files = true,
        });

        // ---- mod list --------------------------------------------------------------------------------
        r.add({
            .group = "mod",
            .name = "list",
            .summary = "List the mods in the game's Mods folder the way ReSkate reads them",
            .description =
                "Read only. Reads the Mods folder with ReSkate's own rules (ported from ReSkate's mod_list.cpp, which "
                "the launcher's mod manager and the game both use), so the list, order and status match what the "
                "launcher shows.\n\n"
                "Where: <game>\\ModData\\Default\\Mods when a <game>\\ModData folder exists (the launcher then starts "
                "the game with -dataPath ModData/Default), else <game>\\Mods.\n"
                "Which folders: every folder directly in Mods whose name is 1-64 plain ASCII characters (letters, "
                "digits, space, '_', '-', '.') not starting with '.'; anything else is ignored by ReSkate too.\n"
                "Order and switches: Mods\\mods.json ({\"schema\":1,\"mods\":[{\"name\":...,\"enabled\":...}]}, "
                "nothing "
                "else allowed) gives the priority order, highest first; folders it does not list follow by name, "
                "enabled. Without mods.json every folder is enabled, in name order. A malformed mods.json (wrong "
                "shape, a repeated key or name, a bad name) means the game loads no mod at all; it is reported under "
                "mods_json.issue. Rows naming folders that are not there are listed under missing.\n"
                "Per mod: title, author, version and description from manifest.json (or reskate-mod.json), tool, "
                "version and build time from reskate-build.json, levels from reskate-levels.json, park_maps from "
                "parks\\<map>.park.json, whether it ships layout.toc, and its .reskate-studio-patch stamp.\n"
                "Outdated: a Studio stamp without skate_sha256, a stamp for another Skate.exe than the supported build "
                "(25414733, SHA-256 fbce74d5...), or a layout.toc with no stamp at all. Outdated mods never load, "
                "enabled or not. built_for_this_game is true when the stamp records the supported Skate.exe.\n"
                "Excluded: mods listed in Mods\\.reskate-excluded.json whose files have not changed since the game "
                "failed to merge them; the launcher shows them as NOT LOADED with the recorded problems.\n\n"
                "Not covered: the game's own merge (problems found only while merging) and its layout.toc changelist "
                "check against Data\\layout.toc.\n"
                "No ReSkate.dll beside Skate.exe (a plain Steam copy, which auto-detection may pick) means ReSkate is "
                "not installed there and no mod loads: reskate_installed is false and warning says so.\n\n"
                "With --check-hash it also hashes the installed Skate.exe (about 145 MB) and says whether it is the "
                "build ReSkate supports. Also lists Studio+ deploy backups and the last deploy receipt.",
            .params =
                {
                    {"check-hash",
                     ParamType::Boolean,
                     "Hash Skate.exe and say whether it is the build ReSkate supports",
                     false,
                     false,
                     {},
                     Json(false)},
                    game_root_param(),
                },
            .examples = {"mod list", "mod list --json", "mod list --check-hash --game-root \"C:\\Games\\Skate\""},
            .run = [](Context &c, const Json &a) -> Json
            {
                using namespace modlist;
                fs::path game = checked_game_root(c, a, true, false);
                fs::path data = modlist::data_root(game);
                ModList list = scan_mods(data, c);
                const std::string sdk = sdk_identity(game);
                const bool rejected = !list.issue.empty();
                // ReSkate's launcher refuses to start without ReSkate.dll beside Skate.exe (launch.cpp), and
                // without ReSkate nothing reads the Mods folder. A plain Steam copy (what auto-detection finds
                // first) is such a folder.
                const bool reskate = file_exists(game / L"ReSkate.dll");

                Json out;
                out["game_root"] = path_utf8(game);
                out["reskate_installed"] = reskate;
                if (!reskate)
                    out["warning"] =
                        "No ReSkate.dll beside Skate.exe, so ReSkate is not installed in this folder and no mod "
                        "loads from it. Point --game-root at the folder holding ReSkateLauncher.exe.";
                out["mods_dir"] = path_utf8(list.root);
                out["present"] = list.present;
                if (a["check-hash"].get<bool>())
                {
                    c.progress(-1, "Hashing Skate.exe");
                    std::string sha = sha256_file(game / L"Skate.exe");
                    out["game_sha256"] = sha;
                    out["game_is_supported_build"] = sha == supported_game_sha256;
                }

                Json mods_json;
                mods_json["path"] = path_utf8(list.root / L"mods.json");
                if (!list.present)
                {
                    mods_json["state"] = "missing";
                    mods_json["meaning"] = "There is no Mods folder, so no mod loads.";
                }
                else if (rejected)
                {
                    mods_json["state"] = list.order_file ? "malformed" : "unreadable";
                    mods_json["issue"] = list.issue;
                    mods_json["meaning"] = list.order_file
                                               ? "ReSkate rejects this mods.json, so the game loads no mod. The "
                                                 "launcher rewrites it on the next change in its mod manager."
                                               : "The Mods folder could not be read.";
                }
                else if (list.order_file)
                {
                    mods_json["state"] = "exists";
                    mods_json["meaning"] =
                        "Rows give the order and on/off switches; folders it does not list follow by name, enabled.";
                }
                else
                {
                    mods_json["state"] = "missing";
                    mods_json["meaning"] = "No mods.json: every mod folder is enabled, in folder-name order. The "
                                           "launcher writes one on the first change in its mod manager.";
                }
                out["mods_json"] = mods_json;

                int enabled = 0, loads = 0, outdated = 0, excluded = 0;
                Json load_order = Json::array();
                Json mods = Json::array();
                for (std::size_t i = 0; i < list.entries.size(); ++i)
                {
                    const Entry &entry = list.entries[i];
                    const Mod &mod = entry.mod;
                    const auto left_out = list.excluded.find(mod.name);
                    const bool is_excluded = left_out != list.excluded.end();
                    const bool is_outdated = !mod.outdated.empty();
                    const bool mod_loads = reskate && !rejected && entry.enabled && !is_outdated && !is_excluded;
                    enabled += entry.enabled ? 1 : 0;
                    loads += mod_loads ? 1 : 0;
                    outdated += is_outdated ? 1 : 0;
                    excluded += is_excluded ? 1 : 0;

                    // The launcher's pills: OUTDATED wins over NOT LOADED.
                    // mod_catalog.cpp retries the merge when ReSkate.dll changed since it left the mod out.
                    bool retried = false;
                    if (is_excluded)
                    {
                        const auto recorded = list.excluded_sdk.find(mod.name);
                        retried = sdk.empty() || recorded == list.excluded_sdk.end() || recorded->second != sdk;
                    }
                    std::string status;
                    if (!reskate)
                        status = "not loaded: ReSkate is not installed in this folder";
                    else if (rejected)
                        status = "not loaded: mods.json is malformed, so no mod loads";
                    else if (is_outdated)
                        status = std::string(entry.enabled ? "NOT LOADED" : "disabled") + ", outdated: " + mod.outdated;
                    else if (!entry.enabled)
                        status = "disabled";
                    else if (is_excluded)
                        status = "NOT LOADED: the game could not merge it cleanly" +
                                 (left_out->second.empty() ? std::string{} : " (" + left_out->second.front() + ")") +
                                 (retried ? "; ReSkate.dll changed since, so the game tries it again next launch" : "");
                    else
                        status = "loads";

                    std::vector<std::string> content;
                    if (mod.provides_layout)
                        content.push_back("game data");
                    if (!mod.levels.empty())
                        content.push_back(std::to_string(mod.levels.size()) +
                                          (mod.levels.size() == 1 ? " level" : " levels"));
                    else if (mod.provides_levels)
                        content.push_back("no level registered");
                    if (!mod.park_maps.empty())
                        content.push_back("parks for " + join(mod.park_maps, ", "));
                    if (content.empty())
                        content.push_back("nothing the game loads");
                    std::string about = mod.title != mod.name ? "\"" + mod.title + "\"" : std::string{};
                    if (!mod.version.empty())
                        about += (about.empty() ? "v" : " v") + mod.version;
                    if (!mod.author.empty())
                        about += (about.empty() ? "by " : " by ") + mod.author;
                    load_order.push_back(std::to_string(i + 1) + ". " + mod.name + "  [" + status + "]  " +
                                         (about.empty() ? "" : about + "; ") + join(content, ", "));

                    Json m;
                    m["name"] = mod.name;
                    m["title"] = mod.title;
                    m["author"] = text_or_null(mod.author);
                    m["version"] = text_or_null(mod.version);
                    m["description"] = text_or_null(mod.description);
                    m["enabled"] = entry.enabled;
                    m["loads"] = mod_loads;
                    m["status"] = status;
                    m["provides_layout"] = mod.provides_layout;
                    m["provides_levels"] = mod.provides_levels;
                    m["levels"] = mod.levels;
                    m["park_maps"] = mod.park_maps;
                    m["tool"] = text_or_null(mod.tool);
                    m["built"] = text_or_null(mod.built);
                    m["studio_marker"] = mod.studio_marker;
                    m["skate_sha256"] = text_or_null(mod.skate_sha256);
                    m["built_for_this_game"] = mod.skate_sha256 == supported_game_sha256;
                    m["outdated"] = text_or_null(mod.outdated);
                    if (is_excluded)
                    {
                        m["excluded"] = left_out->second;
                        m["excluded_retried_next_launch"] = retried;
                    }
                    else
                    {
                        m["excluded"] = nullptr;
                    }
                    m["path"] = path_utf8(mod.directory);
                    mods.push_back(std::move(m));
                }

                Json counts;
                counts["mods"] = list.entries.size();
                counts["enabled"] = enabled;
                counts["disabled"] = static_cast<int>(list.entries.size()) - enabled;
                counts["loads"] = loads;
                counts["outdated"] = outdated;
                counts["excluded"] = excluded;
                counts["missing"] = list.missing.size();
                out["counts"] = counts;
                out["load_order"] = load_order;
                out["missing"] = list.missing;
                out["notes"] = list.notes;
                out["mods"] = mods;

                Json backups = Json::array();
                fs::path backup_dir = game / L".ReSkateStudio-Mod-backup";
                if (dir_exists(backup_dir))
                {
                    std::error_code ec;
                    for (fs::directory_iterator it(backup_dir, ec), end; !ec && it != end; it.increment(ec))
                        if (std::error_code e2; it->is_directory(e2))
                            backups.push_back(
                                {{"name", path_utf8(it->path().filename())}, {"path", path_utf8(it->path())}});
                }
                out["backups"] = backups;
                out["last_deploy"] = read_receipt(game / L".ReSkateStudio-last-deploy.db");
                return out;
            },
        });

        // ---- mod info --------------------------------------------------------------------------------
        r.add({
            .group = "mod",
            .name = "info",
            .summary = "Show the header of a binary .fbmod file",
            .description =
                "Read only. Prints the metadata of a binary .fbmod (reskate_cli fbmod-info): title, author, profile, "
                "head (the game data build it was made against), format version and resource count. Needs no game "
                "folder.\n\nGotchas: the resource count can be higher than the count the producing command reported "
                "(it seems to include metadata slots). A .fbproject is rejected; use 'project info' for those. "
                "--verbose is meant to add native-costume recipe detail; for animation and skin-tone mods it adds "
                "nothing.",
            .params =
                {
                    {"file", ParamType::Path, "Binary .fbmod file", true, true},
                    {"verbose",
                     ParamType::Boolean,
                     "Ask for extra detail (costume recipe and witness data for costume mods)",
                     false,
                     false,
                     {},
                     Json(false)},
                },
            .examples = {"mod info C:\\mods\\anim.fbmod", "mod info C:\\mods\\costume.fbmod --verbose --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                fs::path file = to_path(a, "file");
                require_file(file, "file");
                if (lower(file.extension().native()) == L".fbproject")
                    throw Error("not_an_fbmod",
                                path_utf8(file) + " is a project file; use: studio-plus project info",
                                {{"param", "file"}});
                std::vector<std::string> args = {"fbmod-info", path_utf8(file)};
                add_flag(args, a, "verbose", "--verbose");
                EngineRun run = run_engine(c, args);
                try
                {
                    require_ok(run,
                               {{"file is not a binary FBMOD", "not_an_fbmod"},
                                {"FBMOD format version is unsupported", "fbmod_version_unsupported"},
                                {"FBMOD payload exceeds bounds", "fbmod_corrupt"}});
                }
                catch (Error &e)
                {
                    if (e.code == "not_an_fbmod" || e.code == "file_unreadable")
                        throw Error(e.code,
                                    std::string(e.what()) + ": " + path_utf8(file),
                                    {{"param", "file"}, {"path", path_utf8(file)}});
                    throw;
                }
                // Header keys go to the top level. Anything else (the --verbose costume recipe lines such as
                // "costume=..." and " marker=...", which can repeat) is kept in order under "details".
                static const char *header[] = {"title", "author", "profile", "head", "version", "resources"};
                Json other = Json::array();
                Json out = {{"file", path_utf8(file)}};
                for (const auto &line : run.out_lines)
                {
                    std::string k, v;
                    bool is_header =
                        split_kv(line, k, v) && !out.contains(k) &&
                        std::any_of(std::begin(header), std::end(header), [&](const char *h) { return k == h; });
                    if (!is_header)
                    {
                        other.push_back(line);
                        continue;
                    }
                    if (k == "resources")
                        out["resource_count"] = typed_value(v);
                    else
                        out[k] = k == "author" && v.empty() ? Json(nullptr) : typed_value(v);
                }
                if (!other.empty())
                    out["details"] = other;
                return out;
            },
        });

        // ---- project info ----------------------------------------------------------------------------
        r.add({
            .group = "project",
            .name = "export",
            .summary = "Export a .fbproject as an .fbmod, ready for mod compile",
            .description =
                "Reads a ReSkate Studio .fbproject and writes the same resources as a binary .fbmod (the old Studio's "
                "'Export .fbmod'; reskate_cli has no command for it). Title, author, version and description come from "
                "the project unless given here. Payloads are compressed with the game's Oodle when a ReSkate folder is "
                "set, else stored uncompressed (bigger, but the game reads both). Writes only --output; it must not be "
                "inside the ReSkate folder. Build the result with 'mod compile'.",
            .params =
                {
                    {"file", ParamType::Path, ".fbproject to export", true, true},
                    {"output",
                     ParamType::Path,
                     ".fbmod to write (default: next to the project, same name)",
                     false,
                     true},
                    {"title", ParamType::String, "Mod title (default: the project's)"},
                    {"author", ParamType::String, "Mod author (default: the project's)"},
                    {"version", ParamType::String, "Mod version (default: the project's, else 1.0)"},
                    {"description", ParamType::String, "Mod description (default: the project's)"},
                    game_root_param(),
                },
            .examples = {"project export C:\\mods\\Untitled.fbproject",
                         "project export scooter.fbproject C:\\mods\\scooter.fbmod --title \"Razor Scooter\" --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                const fs::path file = to_path(a, "file");
                require_file(file, "file");
                if (lower(file.extension().native()) == L".fbmod")
                    throw Error("not_a_project",
                                path_utf8(file) + " is already an fbmod; build it with: studio-plus mod compile",
                                {{"param", "file"}});
                fs::path output = arg_string(a, "output").empty() ? fs::path(file).replace_extension(L".fbmod")
                                                                  : to_path(a, "output");
                if (lower(output.extension().native()) != L".fbmod")
                    throw Error(
                        "bad_output", "--output must end in .fbmod: " + path_utf8(output), {{"param", "output"}});
                if (const fs::path game = oodle_root(c, a); !game.empty() && is_within(output, game))
                    throw Error("output_in_game_folder",
                                "--output is inside the ReSkate folder (" + path_utf8(game) +
                                    "). Write it somewhere else; this command never changes the game install",
                                {{"param", "output"}});
                if (!dir_exists(output.parent_path()))
                    throw Error("folder_missing",
                                "The folder for --output does not exist: " + path_utf8(output.parent_path()),
                                {{"param", "output"}});
                c.progress(-1, "Exporting " + path_utf8(file.filename()));
                return export_project(c, a, file, output);
            },
        });

        r.add({
            .group = "project",
            .name = "info",
            .summary = "Show the metadata and resource list of a .fbproject",
            .description =
                "Read only. Reads a ReSkate Studio .fbproject (reskate_cli project-info) and returns its title, "
                "author, "
                "profile, head (game data build) and every resource it holds: kind (ebx asset, res resource or chunk "
                "GUID), name, size in bytes and whether it is a new asset ('added') rather than a replacement. Needs "
                "no "
                "game folder.\n\nGotchas: the ebx/res/chunk kind is read from the engine's numeric kind (1, 2, 3); the "
                "mapping is inferred from the names. Turn a project into an .fbmod with 'project export' ('mod "
                "compile' "
                "also takes a .fbproject and exports it itself).",
            .params =
                {
                    {"file", ParamType::Path, ".fbproject file", true, true},
                },
            .examples = {"project info C:\\mods\\Untitled.fbproject", "project info scooter.fbproject --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                fs::path file = to_path(a, "file");
                require_file(file, "file");
                std::wstring ext = lower(file.extension().native());
                if (ext == L".fbmod")
                    throw Error("not_a_project",
                                path_utf8(file) + " is an fbmod; use: studio-plus mod info",
                                {{"param", "file"}});
                EngineRun run = run_engine(c, {"project-info", path_utf8(file)});
                try
                {
                    require_ok(run, {{"project format is unsupported", "not_a_project"}});
                }
                catch (Error &e)
                {
                    if (e.code == "not_a_project" || e.code == "file_unreadable")
                        throw Error(e.code,
                                    std::string(e.what()) +
                                        " (expected a ReSkate Studio .fbproject): " + path_utf8(file),
                                    {{"param", "file"}, {"path", path_utf8(file)}});
                    throw;
                }
                Json out = {{"file", path_utf8(file)}};
                Json resources = Json::array();
                Json other = Json::array();
                for (const auto &line : run.out_lines)
                {
                    std::string k, v;
                    auto first_space = line.find(' ');
                    auto bytes_at = line.rfind(" bytes=");
                    if (!line.empty() && std::isdigit(static_cast<unsigned char>(line[0])) &&
                        first_space != std::string::npos && bytes_at != std::string::npos && bytes_at > first_space)
                    {
                        // "<kind> <name-or-guid> bytes=<n>[ added]" (the name is everything between kind and bytes=)
                        int kind = std::atoi(line.substr(0, first_space).c_str());
                        std::string name = line.substr(first_space + 1, bytes_at - first_space - 1);
                        std::istringstream in(line.substr(bytes_at + 1));
                        std::string size_tok, flag;
                        in >> size_tok >> flag;
                        if (!name.empty())
                        {
                            Json res = {{"kind",
                                         kind == 1   ? "ebx"
                                         : kind == 2 ? "res"
                                         : kind == 3 ? "chunk"
                                                     : "unknown"},
                                        {"kind_code", kind},
                                        {"name", name},
                                        {"bytes", typed_value(size_tok.substr(6))},
                                        {"added", flag == "added"}};
                            resources.push_back(res);
                            continue;
                        }
                    }
                    if (split_kv(line, k, v))
                    {
                        if (k == "resources")
                            out["resource_count"] = typed_value(v);
                        else
                            out[snake(k)] = k == "author" && v.empty() ? Json(nullptr) : typed_value(v);
                    }
                    else
                        other.push_back(line);
                }
                out["resources"] = resources;
                if (!other.empty())
                    out["lines"] = other;
                return out;
            },
        });

        // ---- game validate ---------------------------------------------------------------------------
        r.add({
            .group = "game",
            .name = "validate",
            .summary = "Check that the game install can be read and decoded",
            .description =
                "Read only. Locates and decodes the first <count> assets of the game through the CAS and Oodle "
                "pipeline "
                "(reskate_cli validate) and reports how many decoded and how many bytes. A quick 'is this Skate folder "
                "usable' check. Needs Skate.exe, Data and oo2core_9_win64.dll in the folder.\n\nGotchas: about 2.7 s "
                "at "
                "the default 100 or at 500. A count above the number of assets fails with count_too_large, but only "
                "after decoding everything (about 54 s).",
            .params =
                {
                    {"count", ParamType::Integer, "Number of assets to decode, at least 1", false, true, {}, Json(100)},
                    game_root_param(),
                },
            .examples = {"game validate", "game validate 500 --json"},
            .long_running = true,
            .run = [](Context &c, const Json &a) -> Json
            {
                long long count = a["count"];
                if (count < 1)
                    throw Error("invalid_arguments", "--count must be at least 1", {{"param", "count"}});
                fs::path game = checked_game_root(c, a);
                if (!file_exists(game / L"oo2core_9_win64.dll"))
                    throw Error("oodle_missing",
                                "No oo2core_9_win64.dll in " + path_utf8(game),
                                {{"game_root", path_utf8(game)}});
                c.progress(-1, "Decoding " + std::to_string(count) + " assets");
                EngineRun run = run_engine(c, {"validate", path_utf8(game), std::to_string(count)});
                require_ok(
                    run,
                    {{"not enough located assets", "count_too_large"}, {"not enough EBX assets", "count_too_large"}});
                Json other = Json::array();
                Json out = kv_object(run.out_lines, &other);
                out["requested"] = count;
                out["ok"] = out.contains("decoded_assets") && out["decoded_assets"] == count;
                out["game_root"] = path_utf8(game);
                out["seconds"] = run.seconds;
                if (!other.empty())
                    out["lines"] = other;
                return out;
            },
        });

        // ---- game patch-audit ------------------------------------------------------------------------
        r.add({
            .group = "game",
            .name = "patch-audit",
            .summary = "Audit the native Patch folder in the game root",
            .description =
                "Read only. Audits <game>\\Patch (reskate_cli patch-audit): counts patched assets, EBX, resources and "
                "chunks, mesh/texture/physics resources, ownership headers, added assets and stock replacements, "
                "checks "
                "ownership headers and encoded SHA-1 against the bundle manifests, and lists each replaced stock "
                "asset. "
                "Without a Patch folder every count is 0.\n\nGotchas: it does not look at Mods\\<name> installs or "
                "staging folders, only <game>\\Patch (use 'mod list' for Mods). Integrity problems fail the command "
                "(patch_integrity). Needs oo2core_9_win64.dll. Takes 5-6 s.",
            .params = {game_root_param()},
            .examples = {"game patch-audit", "game patch-audit --json"},
            .long_running = true,
            .run = [](Context &c, const Json &a) -> Json
            {
                fs::path game = checked_game_root(c, a);
                if (!file_exists(game / L"oo2core_9_win64.dll"))
                    throw Error("oodle_missing",
                                "No oo2core_9_win64.dll in " + path_utf8(game),
                                {{"game_root", path_utf8(game)}});
                c.progress(-1, "Auditing " + path_utf8(game / L"Patch"));
                EngineRun run = run_engine(c, {"patch-audit", path_utf8(game)});
                require_ok(run,
                           {{"does not match the bundle manifest", "patch_integrity"},
                            {"non-stock ownership header", "patch_integrity"}});
                Json counts = Json::object();
                Json stock = Json::array();
                Json other = Json::array();
                for (const auto &line : run.out_lines)
                {
                    std::string k, v;
                    if (!split_kv(line, k, v))
                    {
                        other.push_back(line);
                        continue;
                    }
                    if (k == "stock-replacement")
                        stock.push_back(v);
                    else
                        counts[snake(k)] = typed_value(v);
                }
                Json out = {{"game_root", path_utf8(game)},
                            {"patch_dir", path_utf8(game / L"Patch")},
                            {"patch_present", dir_exists(game / L"Patch")},
                            {"counts", counts},
                            {"stock_replacements", stock}};
                if (!other.empty())
                    out["lines"] = other;
                return out;
            },
        });

        // ---- game initfs -----------------------------------------------------------------------------
        r.add({
            .group = "game",
            .name = "initfs",
            .summary = "Summarise the game's initfs_win32 archive",
            .description =
                "Read only. Reports the file count and total bytes of the game's initfs_win32, the initial file system "
                "archive (reskate_cli initfs-info). Unlike the other game commands it needs only the Data folder, not "
                "Skate.exe. Pass the Skate folder, not the initfs file or a staging folder.",
            .params = {game_root_param()},
            .examples = {"game initfs", "game initfs --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                fs::path game = checked_game_root(c, a, false, true);
                EngineRun run = run_engine(c, {"initfs-info", path_utf8(game)});
                require_ok(run, {});
                Json other = Json::array();
                Json out = kv_object(run.out_lines, &other);
                out["game_root"] = path_utf8(game);
                if (!other.empty())
                    out["lines"] = other;
                return out;
            },
        });

        // ---- asset lua -------------------------------------------------------------------------------
        r.add({
            .group = "asset",
            .name = "lua",
            .summary = "Show the source file, size and line count of a Lua asset",
            .description =
                "Read only. Finds a LuaAsset by its exact EBX name (reskate_cli lua) and reports the embedded source "
                "file name, its size in bytes and its line count. The Lua source text itself is not printed.\n\n"
                "Gotchas: the name is an exact match (lowercased for you), not a search; find names with "
                "'asset find characters/lua/'. A non-Lua asset gives not_a_lua_asset. Takes about 2.7 s (game index "
                "load).",
            .params =
                {
                    {"name",
                     ParamType::String,
                     "Full EBX asset name, e.g. characters/lua/skaterlualoaderasset",
                     true,
                     true},
                    game_root_param(),
                },
            .examples = {"asset lua characters/lua/skaterlualoaderasset",
                         "asset lua characters/lua/luatattoocompositor --json"},
            .run = [](Context &c, const Json &a) -> Json
            {
                std::string name = lower_ascii(a["name"].get<std::string>());
                for (auto &ch : name)
                    if (ch == '\\')
                        ch = '/';
                fs::path game = checked_game_root(c, a);
                EngineRun run = run_engine(c, {"lua", path_utf8(game), name});
                try
                {
                    require_ok(run,
                               {{"Lua asset was not found", "asset_not_found"},
                                {"is not a LuaAsset", "not_a_lua_asset"},
                                {"source text is unavailable", "lua_compiled"}});
                }
                catch (const Error &e)
                {
                    if (e.code != "asset_not_found")
                        throw;
                    throw Error(e.code,
                                "No Lua asset named " + name +
                                    " (exact name needed; search with: studio-plus asset find characters/lua/)",
                                e.details);
                }
                Json other = Json::array();
                Json kv = kv_object(run.out_lines, &other);
                Json out = Json::object();
                for (auto it = kv.begin(); it != kv.end(); ++it)
                {
                    if (it.key() == "resource")
                        out["name"] = it.value();
                    else if (it.key() == "lines")
                        out["line_count"] = it.value();
                    else
                        out[it.key()] = it.value();
                }
                if (!other.empty())
                    out["other_lines"] = other;
                return out;
            },
        });
    }
} // namespace studio
