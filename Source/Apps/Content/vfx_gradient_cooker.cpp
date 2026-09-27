#include "vfx_gradient_cooker.hpp"
#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <vector>

namespace hs::content
{
nlohmann::json CookVfxGradientDefaults(const nlohmann::json &spec,
    const std::filesystem::path &source_dds, const std::filesystem::path &output_dds)
{
    using Json = nlohmann::json;
    const auto &libraries = spec.at("libraries");
    const auto &catalog = libraries.at("texture_catalog").at("tex.vfx.gradient_lut");
    Json rows = catalog.at("row_map");
    std::ifstream input(source_dds, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open gradient LUT: " + source_dds.string());
    std::vector<char> bytes{std::istreambuf_iterator<char>(input), {}};
    const auto word = [&](std::size_t offset) {
        std::uint32_t result{};
        if (offset + sizeof(result) > bytes.size()) throw std::runtime_error("Truncated gradient DDS");
        std::memcpy(&result, bytes.data() + offset, sizeof(result));
        return result;
    };
    if (word(0) != 0x20534444 || word(4) != 124 || word(84) != 0x30315844 ||
        word(128) != 10 || word(132) != 3 || word(140) != 1 || word(28) != 1 ||
        word(16) != 256 || word(12) != rows.size() || bytes.size() != 148 + rows.size() * 2048)
        throw std::runtime_error("Gradient source must be complete linear RGBA16F 256 x row_count DDS");
    std::set<std::pair<std::string, std::string>> pairs;
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const auto &row = rows[i];
        if (row.at("row") != i || !pairs.emplace(row.at("style"), row.at("gradient")).second)
            throw std::runtime_error("Duplicate or unordered gradient row map");
    }
    std::set<std::pair<std::string, std::string>> missing;
    const auto default_gradient = spec.at("defaults").at("component").at("gradient").get<std::string>();
    for (const auto &effect : spec.at("effects"))
        for (const auto &element : effect.at("visual_elements"))
            for (const auto &output : element.at("outputs"))
            {
                const std::pair<std::string, std::string> pair{
                    effect.at("visual").at("style"), output.value("gradient", default_gradient)};
                if (!pairs.contains(pair)) missing.insert(pair);
            }
    for (const auto &[style, gradient] : missing)
    {
        const auto &stops = libraries.at("gradient_library").at(gradient).at("stops");
        const auto &colors = libraries.at("style_families").at(style).at("colors");
        if (stops.size() < 2 || stops.front()[0] != 0.0 || stops.back()[0] != 1.0)
            throw std::runtime_error("Gradient requires ordered endpoints 0 and 1");
        const auto color = [&](const Json &stop) {
            const auto hex = colors.at(stop[1].get<std::string>()).at("srgb_hex").get<std::string>();
            if (hex.size() != 7 || hex.front() != '#') throw std::runtime_error("Invalid sRGB palette");
            std::array<float, 4> value{};
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const float s = std::stoul(hex.substr(1 + channel * 2, 2), nullptr, 16) / 255.0f;
                value[channel] = s <= .04045f ? s / 12.92f : std::pow((s + .055f) / 1.055f, 2.4f);
            }
            value[3] = stop[2].get<float>();
            return value;
        };
        for (std::uint32_t sample = 0; sample < 256; ++sample)
        {
            const float t = sample / 255.0f;
            std::size_t end = 1;
            while (end + 1 < stops.size() && stops[end][0].get<float>() < t) ++end;
            const float start_t = stops[end - 1][0].get<float>(), end_t = stops[end][0].get<float>();
            if (!(end_t > start_t)) throw std::runtime_error("Unordered gradient stops");
            const auto first = color(stops[end - 1]), last = color(stops[end]);
            const float fraction = std::clamp((t - start_t) / (end_t - start_t), 0.0f, 1.0f);
            for (std::size_t channel = 0; channel < 4; ++channel)
            {
                const auto half = DirectX::PackedVector::XMConvertFloatToHalf(std::lerp(first[channel], last[channel], fraction));
                const auto *begin = reinterpret_cast<const char *>(&half);
                bytes.insert(bytes.end(), begin, begin + sizeof(half));
            }
        }
        rows.push_back({{"row", rows.size()}, {"style", style}, {"gradient", gradient}});
    }
    const auto height = static_cast<std::uint32_t>(rows.size());
    std::memcpy(bytes.data() + 12, &height, sizeof(height));
    std::filesystem::create_directories(output_dds.parent_path());
    std::ofstream output(output_dds, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("Cannot write cooked gradient DDS");
    return rows;
}
}
