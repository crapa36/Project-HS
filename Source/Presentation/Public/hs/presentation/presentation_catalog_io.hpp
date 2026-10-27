#pragma once

#include <hs/core/result.hpp>
#include <hs/presentation/presentation_catalog.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace hs
{
[[nodiscard]] std::uint64_t PresentationCatalogSchemaHash() noexcept;
[[nodiscard]] std::vector<std::byte> CanonicalPresentationCatalogBytes(const PresentationCatalog &catalog);
[[nodiscard]] Result LoadPresentationCatalog(const std::filesystem::path &path,
                                             PresentationCatalog &catalog,
                                             std::uint64_t *content_hash = nullptr);
} // namespace hs
