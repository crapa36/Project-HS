#include "renderer_cluster_catalog.hpp"
#include <hs/core/cooked_format.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace hs
{
namespace
{
namespace rd = render_data;
void Require(bool condition, const char *reason)
{
    if (!condition) throw std::runtime_error(reason);
}
Result Failure(std::string message)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
}
std::string Digest(std::span<const std::byte> bytes)
{
    constexpr char hex[] = "0123456789abcdef";
    const auto digest = rd::Sha256(bytes);
    std::string result = "sha256:";
    for (const auto value : digest)
    {
        const auto byte = std::to_integer<unsigned>(value);
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}
std::string ExpectedDigest(const RendererConfig &config, const std::string &id)
{
    const auto found = config.product_digests.find(id);
    Require(found != config.product_digests.end() && found->second.size() == 71 &&
            found->second.starts_with("sha256:"), "selected Generation omits product digest");
    for (std::size_t index = 7; index < found->second.size(); ++index)
        Require((found->second[index] >= '0' && found->second[index] <= '9') ||
                (found->second[index] >= 'a' && found->second[index] <= 'f'), "invalid Generation product digest");
    return found->second;
}
std::vector<std::uint32_t> Depths(const rd::GeometryHierarchy &hierarchy, std::uint32_t &maximum)
{
    // Decode already proves rooted acyclic coverage. Compute the exact traversal
    // budget; an ancestor walk never silently stops at a fixed stack/depth limit.
    std::vector<std::uint32_t> result(hierarchy.groups.size());
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pending;
    for (const auto root : hierarchy.root_groups) pending.emplace_back(root, 1);
    maximum = 0;
    while (!pending.empty())
    {
        const auto [id, depth] = pending.back();
        pending.pop_back();
        Require(id < result.size() && result[id] == 0 && depth > 0 && depth <= hierarchy.groups.size(),
                "invalid hierarchy depth or coverage");
        result[id] = depth;
        maximum = std::max(maximum, depth);
        const auto &group = hierarchy.groups[id];
        for (std::uint32_t child = 0; child < group.child_count; ++child)
            pending.emplace_back(hierarchy.child_groups[group.first_child + child], depth + 1);
    }
    Require(std::ranges::all_of(result, [](auto depth) { return depth != 0; }), "unreachable cluster group");
    return result;
}
void RebasedRange(std::uint32_t base, std::size_t count)
{
    Require(count <= std::numeric_limits<std::uint32_t>::max() - base, "GPU cluster table range overflow");
}
} // namespace

Result EnvironmentClusterCatalog::Load(const RendererConfig &config,
    const rd::PackageView &main_package, std::string_view main_product_id, const ClusterCatalogRead &read)
{
    try
    {
        Require(!config.content_generation_id.empty() && static_cast<bool>(read), "cluster catalog requires a selected Generation and reader");
        const auto reference = std::ranges::find(main_package.chunks, rd::ChunkType::GeometryHierarchyReference, &rd::ChunkView::type);
        Require(main_package.chunks.size() == 7 && reference != main_package.chunks.end() &&
                reference->required && reference->version == 1 && reference->count == 1 &&
                reference->stride == rd::kGeometryHierarchyReferenceStride, "LOD0 hierarchy reference is missing or invalid");
        const auto dependency = rd::DecodeGeometryHierarchyReference(reference->bytes, main_package.dependencies, main_product_id);
        const auto &logical_id = main_package.dependencies[dependency];
        if (generation_ == config.content_generation_id && logical_id_ == logical_id) return Result::Success();
        const auto metadata_chunk = std::ranges::find(main_package.chunks, rd::ChunkType::GeometryMetadata, &rd::ChunkView::type);
        Require(metadata_chunk != main_package.chunks.end() && metadata_chunk->required &&
                metadata_chunk->version == 1 && metadata_chunk->count == 1 &&
                metadata_chunk->stride == rd::kGeometryMetadataStride, "cluster catalog main geometry metadata is invalid");
        const auto metadata = rd::DecodeGeometryMetadata(metadata_chunk->bytes, main_package.dependencies, main_product_id);
        Require(metadata.lod == 0, "hierarchy reference belongs to an authored fallback LOD");
        const auto stem = main_product_id.substr(7, main_product_id.size() - 12);
        const auto source_id = "environment/environment_" + std::string(stem) + ".meshbin";
        Require(std::ranges::find(main_package.dependencies, source_id) != main_package.dependencies.end(), "cluster catalog omits original mesh dependency");
        std::filesystem::path hierarchy_path;
        if (auto resolved = config.ResolveProduct(logical_id, hierarchy_path); !resolved) return resolved;
        const auto expected_digest = ExpectedDigest(config, logical_id);
        std::vector<std::byte> bytes;
        if (auto loaded = read(hierarchy_path, bytes); !loaded) return loaded;
        Require(Digest(bytes) == expected_digest, "hierarchy bytes differ from selected Generation digest");
        EnvironmentClusterCatalog prepared;
        prepared.hierarchy_ = rd::DecodeGeometryHierarchy(bytes);
        Require(prepared.hierarchy_.geometry_flags == metadata.flags &&
                prepared.hierarchy_.source_triangle_count == metadata.index_count / 3,
                "hierarchy flags/source count differ from main LOD0 header");
        Require((metadata.flags & (rd::kGeometryFlagWind | rd::kGeometryFlagSkinned)) == 0 ||
                prepared.hierarchy_.error_domain == rd::GeometryHierarchyErrorDomain::ReferencePose,
                "deforming hierarchy cannot claim a qualified static geometric-error domain");
        Require(std::ranges::all_of(prepared.hierarchy_.groups, [&](const auto &group) {
            return group.material_partition == 0 && group.material_selector <= metadata.max_material_index;
        }), "environment hierarchy crosses its actual material or authored selector range");
        (void)Depths(prepared.hierarchy_, prepared.max_ancestor_steps_);
        prepared.generation_ = config.content_generation_id;
        prepared.logical_id_ = logical_id;
        prepared.source_id_ = source_id;
        const auto page_prefix = logical_id.substr(0, logical_id.size() - 6) + "/";
        for (std::size_t index = 0; index < prepared.hierarchy_.pages.size(); ++index)
        {
            const auto &page = prepared.hierarchy_.pages[index];
            Require(page.logical_id.starts_with(page_prefix), "hierarchy page belongs to another catalog");
            ClusterCatalogPage directory;
            directory.logical_id = page.logical_id;
            if (auto resolved = config.ResolveProduct(page.logical_id, directory.path); !resolved) return resolved;
            directory.digest = ExpectedDigest(config, page.logical_id); // No page payload I/O during catalog load.
            directory.pinned = (page.flags & rd::kGeometryHierarchyPagePinned) != 0;
            const auto local_index = static_cast<std::uint32_t>(index);
            Require(prepared.page_indices_.emplace(page.logical_id, local_index).second, "duplicate cluster page ID");
            if (directory.pinned) prepared.pinned_pages_.push_back(local_index);
            prepared.pages_.push_back(std::move(directory));
        }
        Require(!prepared.pinned_pages_.empty(), "cluster hierarchy has no pinned root pages");
        *this = std::move(prepared);
        return Result::Success();
    }
    catch (const std::exception &error)
    {
        return Failure("Invalid cluster catalog for " + std::string(main_product_id) + ": " + error.what());
    }
}

Result EnvironmentClusterCatalog::DecodePage(std::uint32_t page_index, std::span<const std::byte> bytes,
    rd::GeometryData &output) const
{
    try
    {
        Require(!generation_.empty() && page_index < pages_.size(), "cluster page has no catalog/generation");
        const auto &page = pages_[page_index];
        Require(Digest(bytes) == page.digest, "cluster page bytes differ from selected Generation digest");
        const auto package = rd::ReadPackage(bytes, MakeAssetId(page.logical_id));
        Require(package.dependencies == std::vector<std::string>{source_id_}, "cluster page source dependency mismatch");
        auto geometry = rd::DecodeGeometryPagePayload(bytes, MakeAssetId(page.logical_id), hierarchy_.geometry_flags);
        rd::ValidateGeometryHierarchyPage(hierarchy_, page_index, geometry);
        output = std::move(geometry);
        return Result::Success();
    }
    catch (const std::exception &error) { return Failure("Invalid streamed cluster page: " + std::string(error.what())); }
}

Result EnvironmentClusterCatalog::PrepareGpu(std::uint32_t catalog_ordinal, std::uint32_t generation_tag,
    std::uint32_t group_base, std::uint32_t child_base, std::uint32_t cluster_base,
    std::uint32_t page_base, std::uint32_t material_base, ClusterGpuTables &output) const
{
    try
    {
        Require(!generation_.empty() && catalog_ordinal < 14 && generation_tag != 0, "GPU catalog identity is invalid");
        rd::ValidateGeometryHierarchy(hierarchy_);
        RebasedRange(group_base, hierarchy_.groups.size()); RebasedRange(child_base, hierarchy_.child_groups.size());
        RebasedRange(cluster_base, hierarchy_.clusters.size()); RebasedRange(page_base, hierarchy_.pages.size());
        ClusterGpuTables prepared;
        const auto depths = Depths(hierarchy_, prepared.max_ancestor_steps);
        prepared.groups.reserve(hierarchy_.groups.size());
        prepared.clusters.resize(hierarchy_.clusters.size());
        prepared.pages.resize(hierarchy_.pages.size());
        for (std::size_t id = 0; id < hierarchy_.groups.size(); ++id)
        {
            const auto &group = hierarchy_.groups[id];
            Require(group.material_partition <= UINT32_MAX - material_base, "GPU material partition overflow");
            GpuClusterGroup record;
            record.parent = group.parent == rd::kNoGeometryHierarchyGroup ? UINT32_MAX : group_base + group.parent;
            record.first_child = group.child_count ? child_base + group.first_child : 0;
            record.child_count = group.child_count; record.first_cluster = cluster_base + group.first_cluster;
            record.cluster_count = group.cluster_count; record.material_partition = material_base + group.material_partition;
            record.flags = hierarchy_.error_domain == rd::GeometryHierarchyErrorDomain::ReferencePose ? kClusterGroupReferencePose : 0;
            record.material_selector = group.material_selector; record.sphere = group.bounds.sphere;
            record.minimum_error = {group.bounds.minimum[0], group.bounds.minimum[1], group.bounds.minimum[2], group.geometric_error};
            record.maximum_reserved = {group.bounds.maximum[0], group.bounds.maximum[1], group.bounds.maximum[2], 0};
            record.catalog_index = catalog_ordinal; record.generation = generation_tag; record.ancestor_steps = depths[id];
            prepared.groups.push_back(record);
            for (std::uint32_t offset = 0; offset < group.cluster_count; ++offset)
            {
                const auto cluster_id = group.first_cluster + offset;
                const auto &cluster = hierarchy_.clusters[cluster_id];
                prepared.clusters[cluster_id] = {page_base + cluster.page_index, cluster.first_index,
                    cluster.index_count, cluster.first_meshlet, cluster.meshlet_count, cluster.material_selector,
                    group_base + static_cast<std::uint32_t>(id), material_base + cluster.material_partition};
            }
        }
        for (const auto child : hierarchy_.child_groups) prepared.children.push_back(group_base + child);
        for (auto &page : prepared.pages) page.generation = generation_tag;
        output = std::move(prepared);
        return Result::Success();
    }
    catch (const std::exception &error) { return Failure("Cannot prepare GPU cluster catalog: " + std::string(error.what())); }
}

void EnvironmentClusterCatalog::Reset() noexcept
{
    hierarchy_ = {}; pages_.clear(); pinned_pages_.clear(); page_indices_.clear();
    generation_.clear(); logical_id_.clear(); source_id_.clear(); max_ancestor_steps_ = 0;
}
std::uint32_t EnvironmentClusterCatalog::FindPage(std::string_view logical_id) const
{
    const auto found = page_indices_.find(std::string(logical_id));
    return found == page_indices_.end() ? UINT32_MAX : found->second;
}
} // namespace hs
