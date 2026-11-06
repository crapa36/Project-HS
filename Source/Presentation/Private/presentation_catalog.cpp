#include <hs/presentation/presentation_catalog_io.hpp>

#include <hs/core/cooked_format.hpp>
#include <hs/core/canonical_bytes.hpp>
#include <hs/core/canonical_reader.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace hs
{

std::uint64_t PresentationCatalogSchemaHash() noexcept
{
    return Fnv1a64("project_hs_presentation_catalog_wire_v3");
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

std::vector<std::byte> EncodePresentationCatalog(const PresentationCatalog &catalog)
{
    return CanonicalPresentationCatalogBytes(catalog);
}

Result DecodePresentationCatalog(std::span<const std::byte> bytes, PresentationCatalog &catalog)
{
    try
    {
        CanonicalReader in(bytes, "project_hs.presentation_catalog", 1);
        PresentationCatalog decoded{};
        in.Fields(decoded.camera.yaw_degrees, decoded.camera.pitch_degrees,
                  decoded.camera.vertical_fov_degrees, decoded.camera.distance_m,
                  decoded.camera.minimum_distance_percent, decoded.camera.maximum_distance_percent);
        const auto &camera = decoded.camera;
        if (!std::isfinite(camera.yaw_degrees) || !std::isfinite(camera.pitch_degrees) ||
            !std::isfinite(camera.vertical_fov_degrees) || !std::isfinite(camera.distance_m) ||
            camera.vertical_fov_degrees <= 0 || camera.vertical_fov_degrees >= 180 ||
            camera.distance_m <= 0 || camera.minimum_distance_percent == 0 ||
            camera.maximum_distance_percent < camera.minimum_distance_percent)
            throw std::runtime_error("Invalid presentation camera.");
        const auto strings = [&](auto &rows) {
            if (in.Count() != rows.size()) throw std::runtime_error("Presentation row count mismatch.");
            for (auto &row : rows)
            {
                const auto text = in.String();
                if (text.size() >= row.size() || text.find('\0') != std::string_view::npos)
                    throw std::runtime_error("Invalid presentation string.");
                std::copy(text.begin(), text.end(), row.begin());
            }
        };
        strings(decoded.relic_names);
        strings(decoded.relic_rules);
        in.Finish();
        catalog = decoded;
        return Result::Success();
    }
    catch (const std::runtime_error &error)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_presentation", error.what());
    }
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
    if (auto result = DecodePresentationCatalog(payload, catalog); !result) return result;
    if (content_hash)
        *content_hash = header.source_hash;
    return Result::Success();
}

} // namespace hs
