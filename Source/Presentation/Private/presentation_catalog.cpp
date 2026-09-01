#include <hs/presentation/presentation_catalog.hpp>

#include <hs/core/cooked_format.hpp>

#include <cstring>
#include <vector>

namespace hs
{

std::uint64_t PresentationCatalogSchemaHash() noexcept
{
    return Fnv1a64("project_hs_presentation_catalog_v2");
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
