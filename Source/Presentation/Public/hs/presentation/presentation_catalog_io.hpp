#pragma once

#include <hs/core/result.hpp>
#include <hs/presentation/presentation_catalog.hpp>

#include <cstdint>
#include <filesystem>

namespace hs
{
[[nodiscard]] std::uint64_t PresentationCatalogSchemaHash() noexcept;
[[nodiscard]] Result LoadPresentationCatalog(const std::filesystem::path &path,
                                             PresentationCatalog &catalog,
                                             std::uint64_t *content_hash = nullptr);
} // namespace hs
