#pragma once

#include <hs/core/result.hpp>
#include <hs/game_domain/game_types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace hs
{

inline constexpr std::size_t kRelicNameBytes = 64;
inline constexpr std::size_t kRelicRuleBytes = 512;

struct PresentationCatalog
{
    std::array<std::array<char, kRelicNameBytes>, kRelicCount> relic_names{};
    std::array<std::array<char, kRelicRuleBytes>, kRelicCount> relic_rules{};
};

[[nodiscard]] std::uint64_t PresentationCatalogSchemaHash() noexcept;
[[nodiscard]] Result LoadPresentationCatalog(const std::filesystem::path &path,
                                             PresentationCatalog &catalog,
                                             std::uint64_t *content_hash = nullptr);

} // namespace hs
