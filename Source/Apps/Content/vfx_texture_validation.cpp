#include "vfx_texture_validation.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

namespace hs::content
{
namespace
{
using Json = nlohmann::json;
std::uint32_t Unsigned(const Json &value)
{
    if ((!value.is_number_unsigned() && !value.is_number_integer()) ||
        value.get<double>() < 0 || value.get<double>() > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("Expected nonnegative 32-bit integer");
    return value.get<std::uint32_t>();
}
std::uint32_t Format(const std::string &name)
{
    if (name == "R8_UNORM") return 61;
    if (name == "R8G8_SNORM") return 51;
    if (name == "RGBA8_UNORM" || name == "R8G8B8A8_UNORM") return 28;
    if (name == "R16_FLOAT") return 54;
    if (name == "R16G16B16A16_FLOAT") return 10;
    if (name == "BC4_UNORM") return 80;
    if (name == "BC5_SNORM") return 84;
    if (name == "BC7_UNORM") return 98;
    throw std::runtime_error("Unsupported or non-linear VFX format: " + name);
}
void Maps(const Json &entry, std::uint32_t slices, std::uint32_t rows)
{
    for (const auto *kind : {"slice_map", "row_map"})
    {
        if (!entry.contains(kind)) continue;
        const bool slice = std::string(kind) == "slice_map";
        const auto &map = entry.at(kind);
        const auto limit = slice ? slices : rows;
        if (!map.is_array() || map.size() != limit) throw std::runtime_error("Incomplete texture map");
        std::set<std::uint32_t> indices;
        std::set<std::string> names;
        for (const auto &item : map)
        {
            const auto index = Unsigned(item.at(slice ? "slice" : "row"));
            Json identity;
            if (slice) identity = item.at("name").get<std::string>();
            else if (item.contains("curve")) identity = item.at("curve").get<std::string>();
            else identity = Json::array({item.at("style").get<std::string>(), item.at("gradient").get<std::string>()});
            if ((identity.is_string() && identity.get<std::string>().empty()) || index >= limit || !indices.insert(index).second ||
                !names.insert(identity.dump()).second) throw std::runtime_error("Invalid or duplicate texture map entry");
        }
    }
    if (entry.contains("frame_count") && Unsigned(entry.at("frame_count")) != slices)
        throw std::runtime_error("Frame count differs from array size");
    if (!entry.contains("clip_map")) return;
    const auto &clips = entry.at("clip_map");
    if (!clips.is_object()) throw std::runtime_error("Clip map must be an object");
    std::set<std::uint32_t> indices;
    std::set<std::uint32_t> occupied;
    for (const auto &clip : clips)
    {
        const auto index = Unsigned(clip.at("clip_index"));
        const auto first = Unsigned(clip.at("first_slice"));
        const auto count = Unsigned(clip.at("frame_count"));
        if (index >= clips.size() || !indices.insert(index).second || count == 0 ||
            first >= slices || count > slices - first || !clip.at("fps").is_number() ||
            !std::isfinite(clip.at("fps").get<double>()) || !(clip.at("fps").get<double>() > 0)) throw std::runtime_error("Invalid texture clip");
        for (auto i = first; i < first + count; ++i)
            if (!occupied.insert(i).second) throw std::runtime_error("Overlapping texture clips");
    }
}
}

void ValidateVfxTextures(const Json &spec, const std::filesystem::path &root)
{
    std::set<std::string> assets;
    for (const auto &[id, entry] : spec.at("libraries").at("texture_catalog").items())
    {
        try
        {
            const auto asset = entry.at("runtime_asset").get<std::string>();
            constexpr auto prefix = "Content/Textures/VFX/";
            const std::filesystem::path filename(asset.substr(std::min(asset.size(), std::char_traits<char>::length(prefix))));
            if (!asset.starts_with(prefix) || filename.empty() || filename.has_parent_path() ||
                filename.extension() != ".dds" || asset.find('\\') != std::string::npos ||
                asset.find(':') != std::string::npos || !assets.insert(asset).second)
                throw std::runtime_error("Invalid or duplicate runtime asset path");
            const auto directory = std::filesystem::weakly_canonical(root / "ContentSource/Textures/VFX");
            const auto path = std::filesystem::weakly_canonical(directory / filename);
            if (path.parent_path() != directory) throw std::runtime_error("Texture path escapes source directory");
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input) throw std::runtime_error("Cannot open source texture: " + path.string());
            const auto length = input.tellg();
            if (length < 148) throw std::runtime_error("Truncated DDS header");
            input.seekg(0);
            std::array<unsigned char, 148> bytes{};
            if (!input.read(reinterpret_cast<char *>(bytes.data()), bytes.size())) throw std::runtime_error("Cannot read DDS header");
            const auto word = [&](std::size_t offset) {
                return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
                    (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
            };
            if (word(0) != 0x20534444 || word(4) != 124 || word(76) != 32 || word(80) != 4 ||
                word(84) != 0x30315844 || (word(8) & 0x1007) != 0x1007 || !(word(108) & 0x1000) ||
                word(136) != 0 || word(144) != 0) throw std::runtime_error("Invalid linear DX10 DDS header");
            const auto format = word(128);
            const auto preferred = Format(entry.at("runtime_format_preferred").get<std::string>());
            const auto development = entry.contains("runtime_format_development") ?
                Format(entry.at("runtime_format_development").get<std::string>()) : preferred;
            if (format != preferred && format != development) throw std::runtime_error("DDS format differs from catalog");
            const auto color = entry.at("color_space").get<std::string>();
            if (color != "linear_data" && color != "signed_linear_data" && color != "linear_hdr" && color != "linear_scalar_data")
                throw std::runtime_error("VFX data must be linear");
            const auto type = entry.value("resource_type", std::string("Texture2D"));
            const bool volume = type == "Texture3D", array = type == "Texture2DArray";
            if (!volume && !array && type != "Texture2D") throw std::runtime_error("Unsupported texture resource type");
            const auto &dimensions = entry.at("dimensions");
            if (!dimensions.is_array() || dimensions.size() != (volume || array ? 3u : 2u)) throw std::runtime_error("Invalid texture dimensions");
            auto width = Unsigned(dimensions[0]), height = Unsigned(dimensions[1]);
            auto depth = volume ? Unsigned(dimensions[2]) : 1u;
            const auto slices = array ? Unsigned(dimensions[2]) : 1u;
            if (!width || !height || !depth || !slices || width > 16384 || height > 16384 || depth > 2048 || slices > 2048 ||
                word(16) != width || word(12) != height || word(24) != (volume ? depth : 0) ||
                word(132) != (volume ? 4u : 3u) || word(140) != slices ||
                word(112) != (volume ? 0x200000u : 0u) || (volume && !(word(8) & 0x800000)))
                throw std::runtime_error("DDS resource dimensions differ from catalog");
            if ((volume || array) && !(word(108) & 8)) throw std::runtime_error("DDS array/volume lacks complex capability");
            const auto policy = entry.at("mips").get<std::string>();
            std::uint32_t mip_count = 1;
            if (policy == "full_chain" || policy == "full_chain_per_slice" || policy == "full_chain_vector_average")
                for (auto axis = std::max({width, height, depth}); axis > 1; axis >>= 1) ++mip_count;
            else if (policy != "none" && policy != "none_baseline") throw std::runtime_error("Unknown mip policy");
            if (word(28) != mip_count || (mip_count > 1 && (!(word(8) & 0x20000) || (word(108) & 0x400008) != 0x400008)))
                throw std::runtime_error("Incomplete DDS mip chain");
            const bool block = format == 80 || format == 84 || format == 98;
            const std::uint64_t unit = format == 80 ? 8 : (block ? 16 : (format == 10 ? 8 : (format == 28 ? 4 : (format == 61 ? 1 : 2))));
            const auto top_pitch = block ? ((std::uint64_t(width) + 3) / 4) * ((height + 3) / 4) * unit : std::uint64_t(width) * unit;
            if (!(word(8) & (block ? 0x80000 : 8)) || word(20) != top_pitch) throw std::runtime_error("Invalid DDS pitch/linear size");
            std::uint64_t expected = 148;
            for (std::uint32_t mip = 0; mip < mip_count; ++mip)
            {
                expected += (block ? ((std::uint64_t(width) + 3) / 4) * ((height + 3) / 4) : std::uint64_t(width) * height) * depth * unit * slices;
                width = std::max(1u, width / 2); height = std::max(1u, height / 2); depth = std::max(1u, depth / 2);
            }
            if (static_cast<std::uint64_t>(length) != expected) throw std::runtime_error("DDS payload is truncated or has trailing bytes");
            Maps(entry, slices, word(12));
        }
        catch (const std::exception &error)
        {
            throw std::runtime_error("VFX texture " + id + ": " + error.what());
        }
    }
}
}
