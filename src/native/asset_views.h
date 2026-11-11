#pragma once
// What the asset browser shows about one asset, read with Studio+'s own decoders: the index entry in
// detail, EBX as JSON, textures as RGBA / PNG / DDS, and Lua source.
#include "native/asset_index.h"

#include <Engine/Resource/texture.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace studio::native
{

    // asset info: the entry, its bundle, ids, related assets and the views that apply.
    Json describe_entry(const AssetIndex &index, const Entry &e, const GameFiles &files);

    // ---------------------------------------------------------------- EBX

    struct EbxOptions
    {
        std::optional<long long> instance; // only this instance
        long long limit = 200;             // most instances
        long long max_array = 512;         // longer arrays end with {"$more": n}
    };
    Json ebx_json(const AssetIndex &index, const Entry &e, const GameFiles &files, const EbxOptions &options);
    // Reads only the EBX import table, without serializing instance fields. Used by catalog scans.
    Json ebx_imports(const AssetIndex &index, const Entry &e, const GameFiles &files);

    // One edit: a field path ("Name", "[2].Transform.trans.x", "Items[0]") and the new value as text.
    struct EbxEdit
    {
        std::string path, value;
    };
    // Applies `edits` to the asset's EBX, writes the result to `output` (written with ReSkate's EBX writer)
    // and reads it back to check every edit landed. Scalar fields only: bool, integers, enums, floats, strings.
    // With `written` the bytes are handed back instead, and `output` is not written.
    Json ebx_set(const AssetIndex &index,
                 const Entry &e,
                 const GameFiles &files,
                 const std::vector<EbxEdit> &edits,
                 const std::filesystem::path &output,
                 std::vector<std::byte> *written = nullptr);

    // ---------------------------------------------------------------- textures

    struct Texture
    {
        const Entry *resource = nullptr; // the Texture resource
        fb::TextureHeader header;
        std::vector<std::byte> pixels; // the pixel chunk, every mip and slice
        std::size_t pixels_start = 0;  // where `pixels` starts in the whole chunk, when only a piece is in the game
    };
    // `e` is a TextureAsset EBX or a Texture resource. Reads the header and the pixel chunk.
    Texture read_texture(const AssetIndex &index, const Entry &e, const GameFiles &files);
    Json texture_info(const Texture &texture);
    std::size_t texture_slices(const Texture &texture);
    // Whether the game's data holds this mip (a streamed texture may only have its small ones).
    bool mip_present(const Texture &texture, std::uint32_t mip);
    // One mip and slice as RGBA (straight alpha). Throws Error("texture_format_unsupported") for formats
    // it cannot decode.
    fb::Image decode_mip(const Texture &texture, std::uint32_t mip, std::uint32_t slice);
    std::vector<unsigned char> encode_png(const fb::Image &image);
    // The whole texture (all mips and slices) as a DDS file with a DX10 header.
    std::vector<unsigned char> encode_dds(const Texture &texture);

    // ---------------------------------------------------------------- Lua

    struct LuaSource
    {
        std::string name;   // the LuaScript resource
        std::string source; // the text
        std::string file;   // the file name stored with it, if any
    };
    // `e` is a LuaScript resource, or an EBX asset with a LuaScript resource of the same name.
    LuaSource read_lua(const AssetIndex &index, const Entry &e, const GameFiles &files);

    void write_file(const std::filesystem::path &path, const void *data, std::size_t size);

} // namespace studio::native
