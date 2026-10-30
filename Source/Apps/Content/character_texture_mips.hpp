#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace hs::content
{
enum class CharacterTextureSemantic { Color, NormalRoughness, LinearData };
using CharacterTexel = std::array<float, 4>;
struct CharacterMip
{
    std::uint32_t width{}, height{};
    std::vector<CharacterTexel> pixels;
};
struct CharacterAlphaPolicy
{
    float cutoff{};
    // Slime alpha also carries continuous body opacity. Coverage correction
    // may change boundary samples, never the homogeneous translucent body.
    bool preserve_translucent{};
};

// Apply only to mip0 AFTER building the original normal mip chain. Current
// shaders normalize TBN*N, so removing positive source magnitude preserves the
// point-sampled direction. It does change bilinear weights and mip0/1 blends;
// that filtering difference requires separate optical qualification.
inline void CanonicalizeCharacterNormalBase(CharacterMip &base)
{
    if (!base.width || !base.height || std::uint64_t(base.width) * base.height != base.pixels.size())
        throw std::runtime_error("character normal: invalid canonicalization layout");
    for (auto &pixel : base.pixels)
    {
        std::array<double, 3> normal{};
        double length_squared{};
        for (std::size_t channel = 0; channel < 4; ++channel)
            if (!std::isfinite(pixel[channel]) || pixel[channel] < 0 || pixel[channel] > 1)
                throw std::runtime_error("character normal: nonfinite/out-of-range canonicalization source");
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            normal[channel] = double(pixel[channel]) * 2.0 - 1.0;
            length_squared += normal[channel] * normal[channel];
        }
        if (!(length_squared > 0))
            throw std::runtime_error("character normal: zero source direction cannot be canonicalized");
        const double inverse_length = 1.0 / std::sqrt(length_squared);
        for (std::size_t channel = 0; channel < 3; ++channel)
            pixel[channel] = static_cast<float>(normal[channel] * inverse_length * 0.5 + 0.5);
    }
}

[[nodiscard]] inline CharacterMip CharacterConstantSlice(std::uint32_t width, std::uint32_t height,
    std::array<std::uint8_t, 4> channels)
{
    if (!width || width > 16384 || !height || height > 16384)
        throw std::runtime_error("character texture: invalid placeholder extent");
    CharacterTexel value;
    for (std::size_t channel = 0; channel < value.size(); ++channel) value[channel] = channels[channel] / 255.0f;
    return {width, height, std::vector<CharacterTexel>(std::size_t(width) * height, value)};
}

inline double CharacterSrgbToLinear(double value)
{
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}
inline double CharacterLinearToSrgb(double value)
{
    return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}
inline std::uint32_t CharacterFullMipCount(std::uint32_t width, std::uint32_t height)
{
    if (!width || !height) throw std::runtime_error("character texture: zero extent");
    std::uint32_t count = 1;
    for (auto edge = std::max(width, height); edge > 1; edge >>= 1) ++count;
    return count;
}

// The coverage target is rounded to the nearest representable texel count.
// Alpha-zero remains zero. For mixed translucent/opaque slime textures, only
// filter footprints containing an opaque sample may be promoted to opaque.
inline void PreserveCharacterAlphaCoverage(CharacterMip &mip, double base_coverage,
    CharacterAlphaPolicy policy, std::span<const bool> opaque_support)
{
    if (!(policy.cutoff > 0 && policy.cutoff <= 1)) return;
    const auto target = static_cast<std::size_t>(std::llround(base_coverage * mip.pixels.size()));
    if (std::ranges::count_if(mip.pixels, [&](const auto &pixel) { return pixel[3] >= policy.cutoff; }) ==
        static_cast<std::ptrdiff_t>(target)) return;
    std::vector<std::size_t> candidates;
    for (std::size_t index = 0; index < mip.pixels.size(); ++index)
        if (mip.pixels[index][3] > 0 && (!policy.preserve_translucent || opaque_support[index])) candidates.push_back(index);
    std::ranges::sort(candidates, [&](auto left, auto right) {
        return mip.pixels[left][3] != mip.pixels[right][3]
            ? mip.pixels[left][3] > mip.pixels[right][3] : left < right;
    });
    const auto selected = std::min(target, candidates.size());
    // Sorting and a fixed index tie-break make equal-alpha coverage deterministic.
    for (std::size_t rank = 0; rank < candidates.size(); ++rank)
    {
        auto &alpha = mip.pixels[candidates[rank]][3];
        if (rank < selected) alpha = std::max(alpha, policy.preserve_translucent ? 1.0f :
            std::min(1.0f, policy.cutoff + 1.0f / 255.0f));
        else if (alpha >= policy.cutoff) alpha = std::max(0.0f, policy.cutoff - 1.0f / 255.0f);
    }
}

[[nodiscard]] inline std::vector<CharacterMip> BuildCharacterMipChain(CharacterMip base,
    CharacterTextureSemantic semantic, CharacterAlphaPolicy alpha = {})
{
    if (!base.width || !base.height || base.width > 16384 || base.height > 16384 ||
        std::uint64_t(base.width) * base.height != base.pixels.size())
        throw std::runtime_error("character texture: invalid source layout");
    if (!std::isfinite(alpha.cutoff) || alpha.cutoff < 0 || alpha.cutoff > 1)
        throw std::runtime_error("character texture: invalid declared alpha cutoff");
    for (const auto &pixel : base.pixels)
        for (const auto channel : pixel)
            if (!std::isfinite(channel) || channel < 0 || channel > 1)
                throw std::runtime_error("character texture: nonfinite/out-of-range channel");
    std::size_t covered{};
    if (alpha.cutoff > 0)
        for (const auto &pixel : base.pixels) covered += pixel[3] >= alpha.cutoff;
    const auto coverage = double(covered) / base.pixels.size();
    std::vector<CharacterMip> result;
    result.reserve(CharacterFullMipCount(base.width, base.height));
    result.push_back(std::move(base));
    while (result.back().width > 1 || result.back().height > 1)
    {
        const auto &source = result.back();
        CharacterMip next{std::max(1u, source.width / 2), std::max(1u, source.height / 2), {}};
        next.pixels.resize(std::size_t(next.width) * next.height);
        // vector<bool> does not expose contiguous bool storage.
        std::unique_ptr<bool[]> support(new bool[next.pixels.size()]{});
        for (std::uint32_t y = 0; y < next.height; ++y)
            for (std::uint32_t x = 0; x < next.width; ++x)
            {
                const auto x0 = std::uint64_t(x) * source.width / next.width;
                const auto x1 = std::uint64_t(x + 1) * source.width / next.width;
                const auto y0 = std::uint64_t(y) * source.height / next.height;
                const auto y1 = std::uint64_t(y + 1) * source.height / next.height;
                std::array<double, 4> sum{};
                auto &pixel = next.pixels[std::size_t(y) * next.width + x];
                for (auto sy = y0; sy < y1; ++sy)
                    for (auto sx = x0; sx < x1; ++sx)
                    {
                        const auto &value = source.pixels[sy * source.width + sx];
                        for (std::size_t channel = 0; channel < 4; ++channel)
                            sum[channel] += channel < 3 && semantic == CharacterTextureSemantic::Color
                                ? CharacterSrgbToLinear(value[channel])
                                : channel < 3 && semantic == CharacterTextureSemantic::NormalRoughness
                                    ? value[channel] * 2.0 - 1.0 : value[channel];
                        support[std::size_t(y) * next.width + x] |= value[3] >= alpha.cutoff;
                    }
                const double count = (x1 - x0) * (y1 - y0);
                for (auto &channel : sum) channel /= count;
                if (semantic == CharacterTextureSemantic::NormalRoughness)
                {
                    const auto length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
                    if (length > 1e-20) for (std::size_t channel = 0; channel < 3; ++channel) sum[channel] /= length;
                    else { sum[0] = sum[1] = 0; sum[2] = 1; }
                    for (std::size_t channel = 0; channel < 3; ++channel) sum[channel] = sum[channel] * 0.5 + 0.5;
                }
                for (std::size_t channel = 0; channel < 4; ++channel)
                    pixel[channel] = static_cast<float>(channel < 3 && semantic == CharacterTextureSemantic::Color
                        ? CharacterLinearToSrgb(sum[channel]) : sum[channel]);
            }
        PreserveCharacterAlphaCoverage(next, coverage, alpha, {support.get(), next.pixels.size()});
        result.push_back(std::move(next));
    }
    return result;
}
} // namespace hs::content
