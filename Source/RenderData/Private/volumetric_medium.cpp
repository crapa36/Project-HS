#include <hs/render_data/volumetric_medium.hpp>
#include <bit>
#include <cmath>
#include <set>
#include <stdexcept>
namespace hs::render_data
{
namespace
{
void Require(bool valid) { if (!valid) throw std::runtime_error("Invalid volumetric medium package."); }
void Word(std::vector<std::byte> &bytes, std::uint32_t word)
{ for (unsigned i = 0; i != 4; ++i) bytes.push_back(std::byte((word >> (i * 8)) & 255)); }
std::uint32_t Word(std::span<const std::byte> bytes, std::size_t &offset)
{
    Require(offset + 4 <= bytes.size()); std::uint32_t word{};
    for (unsigned i = 0; i != 4; ++i) word |= std::uint32_t(bytes[offset++]) << (i * 8);
    return word;
}
}
void ValidateVolumetricMedia(std::span<const VolumetricMedium> media)
{
    Require(media.size() <= 256); std::set<std::uint64_t> ids;
    for (const auto &medium : media)
    {
        Require(medium.stable_id && medium.generation && medium.shape <= 2 && ids.insert(medium.stable_id).second);
        for (const auto scalar : medium.world_to_local) Require(std::isfinite(scalar));
        for (const auto scalar : medium.density_extinction) Require(std::isfinite(scalar));
        for (const auto scalar : medium.scattering) Require(std::isfinite(scalar));
        const auto &m = medium.world_to_local;
        const double determinant = double(m[0]) * (double(m[5]) * m[10] - double(m[6]) * m[9]) -
            double(m[1]) * (double(m[4]) * m[10] - double(m[6]) * m[8]) +
            double(m[2]) * (double(m[4]) * m[9] - double(m[5]) * m[8]);
        Require(std::isfinite(determinant) && determinant != 0 && m[3] == 0 && m[7] == 0 && m[11] == 0 && m[15] == 1);
        Require(medium.density_extinction[0] >= 0 && medium.density_extinction[1] >= 0 &&
            std::abs(medium.density_extinction[2]) < 1 && medium.density_extinction[3] == 0 && medium.scattering[3] == 0);
        for (unsigned i = 0; i != 3; ++i) Require(medium.scattering[i] >= 0 && medium.scattering[i] <= medium.density_extinction[1]);
    }
}
std::vector<std::byte> EncodeVolumetricMedia(std::span<const VolumetricMedium> media)
{
    ValidateVolumetricMedia(media); std::vector<std::byte> bytes; bytes.reserve(media.size() * kVolumetricMediumStride);
    for (const auto &medium : media)
    {
        for (const auto scalar : medium.world_to_local) Word(bytes, std::bit_cast<std::uint32_t>(scalar));
        for (const auto scalar : medium.density_extinction) Word(bytes, std::bit_cast<std::uint32_t>(scalar));
        for (const auto scalar : medium.scattering) Word(bytes, std::bit_cast<std::uint32_t>(scalar));
        Word(bytes, std::uint32_t(medium.stable_id)); Word(bytes, std::uint32_t(medium.stable_id >> 32));
        Word(bytes, medium.generation); Word(bytes, medium.shape);
    }
    return bytes;
}
std::vector<VolumetricMedium> DecodeVolumetricMedia(std::span<const std::byte> bytes)
{
    Require(bytes.size() % kVolumetricMediumStride == 0 && bytes.size() / kVolumetricMediumStride <= 256);
    std::vector<VolumetricMedium> media(bytes.size() / kVolumetricMediumStride); std::size_t offset{};
    for (auto &medium : media)
    {
        for (auto &scalar : medium.world_to_local) scalar = std::bit_cast<float>(Word(bytes, offset));
        for (auto &scalar : medium.density_extinction) scalar = std::bit_cast<float>(Word(bytes, offset));
        for (auto &scalar : medium.scattering) scalar = std::bit_cast<float>(Word(bytes, offset));
        const auto low = Word(bytes, offset); const auto high = Word(bytes, offset);
        medium.stable_id = low | (std::uint64_t(high) << 32); medium.generation = Word(bytes, offset); medium.shape = Word(bytes, offset);
    }
    ValidateVolumetricMedia(media); return media;
}
}
