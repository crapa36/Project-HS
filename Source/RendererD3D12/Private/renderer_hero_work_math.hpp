#pragma once
#include <array>
#include <cstdint>
#include <limits>

namespace hs
{
inline constexpr std::uint32_t kHeroWorkContributors = 12, kHeroWorkPhases = 14;
inline constexpr std::uint64_t kHeroWorkRowBytes = 136;
inline constexpr std::uint64_t kHeroWorkStatisticsOffset = 16, kHeroWorkOcclusionOffset = 128;
inline constexpr std::uint32_t kHeroWorkShaderMagic = 0x31574848u, kHeroWorkShaderVersion = 1;
inline constexpr std::uint64_t kHeroWorkShaderHeaderBytes = 256, kHeroWorkShaderStorageLimit = 512ull * 1024 * 1024;
struct HeroWorkShaderLayout
{
    std::uint32_t pixels{}, bitmap_words{};
    std::uint64_t contributor_bytes{}, phase_bytes{}, total_bytes{};
};
[[nodiscard]] inline bool HeroWorkMakeShaderLayout(std::uint32_t width, std::uint32_t height,
    std::uint32_t slots, HeroWorkShaderLayout &out) noexcept
{
    const std::uint64_t pixels = std::uint64_t(width) * height;
    if (!width || !height || !slots || pixels > UINT32_MAX) return false;
    const auto words = (pixels + 31) / 32;
    const auto stride = (kHeroWorkShaderHeaderBytes + words * 8 + 255) & ~std::uint64_t(255);
    const auto phase = stride * kHeroWorkContributors;
    // Check before multiplying by arbitrary caller-provided slot count.
    const auto per_slot = phase * (kHeroWorkPhases + 1) + kHeroWorkShaderHeaderBytes * kHeroWorkContributors * kHeroWorkPhases;
    if (per_slot > kHeroWorkShaderStorageLimit / slots) return false;
    out = {static_cast<std::uint32_t>(pixels), static_cast<std::uint32_t>(words), stride, phase, per_slot * slots};
    return true;
}
// Raw timestamps and calibrated intervals are separate availability contracts.
// Unsigned anchors may precede or follow a query; do not subtract as uint64_t.
[[nodiscard]] inline bool HeroWorkCalibrate(std::uint64_t stamp, std::uint64_t gpuAnchor,
    std::uint64_t qpcAnchor, std::uint64_t gpuFrequency, std::uint64_t qpcFrequency,
    std::uint64_t &output) noexcept
{
    output = 0;
    if (!gpuFrequency || !qpcFrequency) return false;
    const long double delta = stamp >= gpuAnchor ? static_cast<long double>(stamp - gpuAnchor)
        : -static_cast<long double>(gpuAnchor - stamp);
    const auto result = static_cast<long double>(qpcAnchor) + delta * qpcFrequency / gpuFrequency;
    if (result < 0 || result >= static_cast<long double>((std::numeric_limits<std::uint64_t>::max)())) return false;
    output = static_cast<std::uint64_t>(result);
    return true;
}
[[nodiscard]] inline bool HeroWorkIndexedArgumentsValid(const std::array<std::uint32_t, 6> &args,
    std::uint32_t geometryIndices, std::uint32_t candidates) noexcept
{
    return args[1] <= geometryIndices && args[3] <= geometryIndices - args[1] && args[2] <= candidates;
}
} // namespace hs
