#include <hs/presentation/presentation_catalog_io.hpp>

#include <hs/core/cooked_format.hpp>
#include <hs/core/canonical_bytes.hpp>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

namespace hs
{

std::uint64_t PresentationCatalogSchemaHash() noexcept
{
    return Fnv1a64("project_hs_presentation_catalog_v2");
}

std::vector<std::byte> CanonicalPresentationCatalogBytes(const PresentationCatalog &catalog)
{
    CanonicalBytes bytes("project_hs.presentation_catalog", 1);
    bytes.Fields(catalog.camera.yaw_degrees, catalog.camera.pitch_degrees,
                 catalog.camera.vertical_fov_degrees, catalog.camera.distance_m,
                 catalog.camera.minimum_distance_percent, catalog.camera.maximum_distance_percent);
    bytes.Sequence(catalog.relic_names, [](CanonicalBytes &output, const auto &name) {
        const auto end = std::ranges::find(name, '\0');
        output.String(std::string_view(name.data(), static_cast<std::size_t>(end - name.begin())));
    });
    bytes.Sequence(catalog.relic_rules, [](CanonicalBytes &output, const auto &rule) {
        const auto end = std::ranges::find(rule, '\0');
        output.String(std::string_view(rule.data(), static_cast<std::size_t>(end - rule.begin())));
    });
    return std::move(bytes).Take();
}

Result LoadPresentationCatalog(const std::filesystem::path &path,
                               PresentationCatalog &catalog,
                               std::uint64_t *content_hash)
{
    CookedHeader header;
    std::vector<std::byte> payload;
    if (auto result = ReadCookedPayload(path, PresentationCatalogSchemaHash(), header,
                                        payload);
        !result)
        return result;
    if (payload.size() != sizeof(PresentationCatalog))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_presentation",
                               "Cooked presentation catalog has an unexpected size.");
    std::memcpy(&catalog, payload.data(), sizeof(catalog));
    if (content_hash)
        *content_hash = header.source_hash;
    return Result::Success();
}

} // namespace hs
