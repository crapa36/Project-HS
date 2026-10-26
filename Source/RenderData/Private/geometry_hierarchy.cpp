#include <hs/render_data/geometry_hierarchy.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace hs::render_data
{
namespace
{
constexpr std::uint32_t kMagic = 0x31494847u; // GHI1
constexpr std::uint32_t kMaxPageIdBytes = 512;

void Require(bool condition, std::string_view reason)
{
    if (!condition) throw std::runtime_error("geometry hierarchy: " + std::string(reason));
}

bool Range(std::uint32_t first, std::uint32_t count, std::size_t size)
{
    return first <= size && count <= size - first;
}

std::uint32_t Count(std::size_t count)
{
    Require(count <= std::numeric_limits<std::uint32_t>::max(), "count exceeds portable ABI");
    return static_cast<std::uint32_t>(count);
}

void ValidateBounds(const GeometryBounds &bounds)
{
    for (std::size_t lane = 0; lane < 3; ++lane)
        Require(std::isfinite(bounds.minimum[lane]) && std::isfinite(bounds.maximum[lane]) &&
                bounds.minimum[lane] <= bounds.maximum[lane], "invalid bounds interval");
    for (const auto value : bounds.sphere) Require(std::isfinite(value), "nonfinite bounds sphere");
    Require(bounds.sphere[3] >= 0.0f, "negative bounds radius");
}

void Encloses(const GeometryBounds &outer, const GeometryBounds &inner)
{
    for (std::size_t lane = 0; lane < 3; ++lane)
        Require(outer.minimum[lane] <= inner.minimum[lane] &&
                outer.maximum[lane] >= inner.maximum[lane], "group excludes representation bounds");
}

void EnclosesChildSphere(const GeometryBounds &outer, const GeometryBounds &inner)
{
    double squared_distance = 0.0;
    for (std::size_t lane = 0; lane < 3; ++lane)
    {
        const double delta = double(outer.sphere[lane]) - inner.sphere[lane];
        squared_distance += delta * delta;
    }
    Require(std::sqrt(squared_distance) + double(inner.sphere[3]) <= double(outer.sphere[3]),
            "parent sphere excludes child sphere");
}

void Encloses(const GeometryBounds &bounds, const ExpandedVertex &vertex)
{
    double squared_distance = 0.0;
    for (std::size_t lane = 0; lane < 3; ++lane)
    {
        Require(vertex.position[lane] >= bounds.minimum[lane] &&
                vertex.position[lane] <= bounds.maximum[lane], "bounds exclude vertex");
        const double delta = double(vertex.position[lane]) - bounds.sphere[lane];
        squared_distance += delta * delta;
    }
    Require(std::sqrt(squared_distance) <= double(bounds.sphere[3]), "sphere excludes vertex");
}

void ValidateHierarchyId(std::string_view id, std::string_view suffix)
{
    Require(id.size() <= kMaxPageIdBytes && id.starts_with("render/hierarchy/") &&
            id.ends_with(suffix) && id.size() > std::string_view("render/hierarchy/").size() + suffix.size() &&
            id.find("..") == std::string_view::npos,
            "invalid hierarchy page ID");
    bool separator = true;
    for (const char character : id)
    {
        if (character == '/')
        {
            Require(!separator, "empty page ID component");
            separator = true;
        }
        else
        {
            Require((character >= 'a' && character <= 'z') ||
                    (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9') || character == '_' ||
                    character == '-' || character == '.', "invalid page ID character");
            separator = false;
        }
    }
    Require(!separator, "empty page ID suffix");
}

void Put(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value)
{
    Require(offset <= bytes.size() && 4 <= bytes.size() - offset, "write range");
    for (std::size_t lane = 0; lane < 4; ++lane)
        bytes[offset + lane] = std::byte((value >> (lane * 8)) & 255u);
}

std::uint32_t Get(std::span<const std::byte> bytes, std::size_t offset)
{
    Require(offset <= bytes.size() && 4 <= bytes.size() - offset, "read range");
    std::uint32_t value = 0;
    for (std::size_t lane = 0; lane < 4; ++lane)
        value |= std::to_integer<std::uint32_t>(bytes[offset + lane]) << (lane * 8);
    return value;
}

void WriteBounds(std::span<std::byte> bytes, std::size_t offset, const GeometryBounds &bounds)
{
    for (std::size_t lane = 0; lane < 3; ++lane)
    {
        Put(bytes, offset + lane * 4, std::bit_cast<std::uint32_t>(bounds.minimum[lane]));
        Put(bytes, offset + 12 + lane * 4, std::bit_cast<std::uint32_t>(bounds.maximum[lane]));
    }
    for (std::size_t lane = 0; lane < 4; ++lane)
        Put(bytes, offset + 24 + lane * 4, std::bit_cast<std::uint32_t>(bounds.sphere[lane]));
}

GeometryBounds ReadBounds(std::span<const std::byte> bytes, std::size_t offset)
{
    GeometryBounds bounds;
    for (std::size_t lane = 0; lane < 3; ++lane)
    {
        bounds.minimum[lane] = std::bit_cast<float>(Get(bytes, offset + lane * 4));
        bounds.maximum[lane] = std::bit_cast<float>(Get(bytes, offset + 12 + lane * 4));
    }
    for (std::size_t lane = 0; lane < 4; ++lane)
        bounds.sphere[lane] = std::bit_cast<float>(Get(bytes, offset + 24 + lane * 4));
    return bounds;
}

void Reserved(std::span<const std::byte> bytes, std::size_t first, std::size_t end)
{
    for (auto offset = first; offset < end; offset += 4)
        Require(Get(bytes, offset) == 0, "nonzero reserved field");
}

std::uint64_t EncodedSize(std::uint32_t groups, std::uint32_t clusters,
    std::uint32_t pages, std::uint32_t roots, std::uint32_t children,
    std::uint32_t occurrences, std::uint32_t strings)
{
    return std::uint64_t(kGeometryHierarchyHeaderStride) +
        std::uint64_t(groups) * kGeometryHierarchyGroupStride +
        std::uint64_t(clusters) * kGeometryHierarchyClusterStride +
        std::uint64_t(pages) * kGeometryHierarchyPageStride +
        (std::uint64_t(roots) + children + occurrences) * 4 + strings;
}

void ReferenceDependency(std::uint16_t index, std::span<const std::string> dependencies,
    std::string_view main_product_id)
{
    Require(index != kNoGeometryDependency && index < dependencies.size(), "hierarchy dependency index range");
    const auto &id = dependencies[index];
    ValidateHierarchyId(id, ".ghbin");
    if (!main_product_id.empty())
    {
        Require(main_product_id.starts_with("render/") && main_product_id.ends_with(".rpkg") &&
                main_product_id.size() > 12, "invalid hierarchy main product ID");
        const auto stem = main_product_id.substr(7, main_product_id.size() - 12);
        Require(id == "render/hierarchy/" + std::string(stem) + ".ghbin",
                "hierarchy reference does not match main product");
    }
}

void ValidatePageDirectory(const GeometryHierarchy &hierarchy, const GeometryHierarchyPage &directory,
    const GeometryData &page)
{
    Require(page.flags == hierarchy.geometry_flags && page.vertices.size() == directory.vertex_count &&
            page.indices.size() == directory.index_count && page.meshlets.size() == directory.meshlet_count,
            "geometry page disagrees with hierarchy directory");
}

void ValidateClusterPayload(const GeometryRefinementGroup &group,
    const GeometryHierarchyCluster &cluster, const GeometryData &page)
{
    Require(cluster.material_partition == group.material_partition &&
            cluster.material_selector == group.material_selector, "page material partition disagreement");
    for (std::uint32_t offset = 0; offset < cluster.index_count; ++offset)
    {
        const auto &vertex = page.vertices[page.indices[cluster.first_index + offset]];
        Require(vertex.material_index == cluster.material_selector, "cluster crosses authored material selector");
        Encloses(cluster.bounds, vertex);
        Encloses(group.bounds, vertex);
    }
    for (std::uint32_t offset = 0; offset < cluster.meshlet_count; ++offset)
    {
        const auto &meshlet = page.meshlets[cluster.first_meshlet + offset];
        Require(page.submeshes[meshlet.submesh_index].material_index == cluster.material_selector,
                "meshlet material disagrees with hierarchy cluster");
        for (std::uint32_t triangle = 0; triangle < meshlet.triangle_count; ++triangle)
        {
            const auto primitive = page.canonical_primitive_remap[meshlet.first_triangle + triangle];
            Require(primitive >= cluster.first_index / 3 &&
                    primitive - cluster.first_index / 3 < cluster.index_count / 3,
                    "mesh and indexed cluster primitive intervals disagree");
        }
    }
}

void ValidatePagePayload(const GeometryHierarchy &hierarchy, std::uint32_t page_index,
    const GeometryData &page)
{
    Require(page_index < hierarchy.pages.size(), "page index exceeds hierarchy directory");
    ValidateGeometryData(page);
    ValidatePageDirectory(hierarchy, hierarchy.pages[page_index], page);
    for (const auto &group : hierarchy.groups)
        for (std::uint32_t cluster_offset = 0; cluster_offset < group.cluster_count; ++cluster_offset)
        {
            const auto &cluster = hierarchy.clusters[group.first_cluster + cluster_offset];
            if (cluster.page_index != page_index) continue;
            ValidateClusterPayload(group, cluster, page);
        }
}
} // namespace

std::vector<std::byte> EncodeGeometryHierarchyReference(std::uint16_t dependency_index,
    std::span<const std::string> dependencies, std::string_view main_product_id)
{
    ReferenceDependency(dependency_index, dependencies, main_product_id);
    std::vector<std::byte> bytes(kGeometryHierarchyReferenceStride);
    Put(bytes, 0, dependency_index);
    return bytes;
}

std::uint16_t DecodeGeometryHierarchyReference(std::span<const std::byte> bytes,
    std::span<const std::string> dependencies, std::string_view main_product_id)
{
    Require(bytes.size() == kGeometryHierarchyReferenceStride, "invalid hierarchy reference size");
    const auto value = Get(bytes, 0);
    Require((value >> 16) == 0, "hierarchy reference reserved field");
    const auto index = static_cast<std::uint16_t>(value);
    ReferenceDependency(index, dependencies, main_product_id);
    return index;
}

void ValidateGeometryHierarchy(const GeometryHierarchy &hierarchy)
{
    Require(hierarchy.abi == kGeometryHierarchyAbi &&
            hierarchy.error_method_abi == kGeometryErrorMethodAbi &&
            !(hierarchy.geometry_flags & ~7u), "unsupported hierarchy ABI or flags");
    const auto expected_domain = hierarchy.geometry_flags &
        (kGeometryFlagSkinned | kGeometryFlagWind)
        ? GeometryHierarchyErrorDomain::ReferencePose : GeometryHierarchyErrorDomain::Static;
    Require(hierarchy.error_domain == expected_domain, "error domain disagrees with deformation flags");
    Require(hierarchy.source_triangle_count > 0 &&
            hierarchy.source_triangle_count <= kMaxGeometryIndices / 3 &&
            hierarchy.source_primitive_order.size() == hierarchy.source_triangle_count,
            "invalid source occurrence count");
    Require(!hierarchy.groups.empty() &&
            hierarchy.groups.size() <= std::uint64_t(hierarchy.source_triangle_count) * 2 &&
            !hierarchy.clusters.empty() && hierarchy.clusters.size() <= kMaxGeometryIndices &&
            !hierarchy.pages.empty() && hierarchy.pages.size() <= hierarchy.clusters.size() &&
            !hierarchy.root_groups.empty() && hierarchy.root_groups.size() <= hierarchy.groups.size() &&
            hierarchy.child_groups.size() == hierarchy.groups.size() - hierarchy.root_groups.size(),
            "invalid hierarchy counts");
    std::vector<bool> occurrences(hierarchy.source_triangle_count);
    for (const auto occurrence : hierarchy.source_primitive_order)
    {
        Require(occurrence < occurrences.size() && !occurrences[occurrence],
                "source occurrence permutation is not exact");
        occurrences[occurrence] = true;
    }

    std::unordered_set<std::string> page_ids;
    for (const auto &page : hierarchy.pages)
    {
        ValidateHierarchyId(page.logical_id, ".rpage");
        Require(page_ids.insert(page.logical_id).second, "duplicate logical page ID");
        Require(!(page.flags & ~kGeometryHierarchyPagePinned) && page.vertex_count > 0 &&
                page.vertex_count <= kMaxGeometryVertices && page.index_count > 0 &&
                page.index_count <= kMaxGeometryIndices && page.index_count % 3 == 0 &&
                page.vertex_count <= page.index_count && page.meshlet_count > 0 &&
                page.meshlet_count <= page.index_count / 3, "invalid page directory record");
    }

    std::vector<std::uint32_t> owners(hierarchy.clusters.size(), kNoGeometryHierarchyGroup);
    std::vector<bool> child_slots(hierarchy.child_groups.size());
    for (std::size_t id = 0; id < hierarchy.groups.size(); ++id)
    {
        const auto &group = hierarchy.groups[id];
        ValidateBounds(group.bounds);
        Require(std::isfinite(group.geometric_error) && group.geometric_error >= 0.0f &&
                group.material_partition <= 65535 && group.material_selector <= 65535 && group.cluster_count > 0 &&
                Range(group.first_cluster, group.cluster_count, hierarchy.clusters.size()) &&
                group.source_primitive_count > 0 && Range(group.first_source_primitive,
                    group.source_primitive_count, hierarchy.source_primitive_order.size()) &&
                (group.parent == kNoGeometryHierarchyGroup || group.parent < hierarchy.groups.size()),
                "invalid refinement group");
        std::uint64_t representation_triangles = 0;
        for (std::uint32_t offset = 0; offset < group.cluster_count; ++offset)
        {
            const auto cluster_id = group.first_cluster + offset;
            Require(owners[cluster_id] == kNoGeometryHierarchyGroup, "cluster has multiple group owners");
            owners[cluster_id] = static_cast<std::uint32_t>(id);
            const auto &cluster = hierarchy.clusters[cluster_id];
            Require(cluster.material_partition == group.material_partition &&
                    cluster.material_selector == group.material_selector &&
                    cluster.geometric_error == group.geometric_error,
                    "cluster disagrees with its complete group representation");
            Encloses(group.bounds, cluster.bounds);
            representation_triangles += cluster.index_count / 3;
        }
        if (group.child_count == 0)
        {
            Require(group.first_child == 0 && group.geometric_error == 0.0f &&
                    representation_triangles == group.source_primitive_count,
                    "leaf representation does not cover its source occurrences");
            continue;
        }
        Require(group.child_count >= 2 && Range(group.first_child, group.child_count,
                hierarchy.child_groups.size()), "invalid immediate child range");
        std::uint64_t next_source = group.first_source_primitive;
        for (std::uint32_t offset = 0; offset < group.child_count; ++offset)
        {
            const auto slot = group.first_child + offset;
            Require(!child_slots[slot], "child list ranges overlap");
            child_slots[slot] = true;
            const auto child_id = hierarchy.child_groups[slot];
            Require(child_id < hierarchy.groups.size() && child_id != id, "invalid child group ID");
            const auto &child = hierarchy.groups[child_id];
            Require(child.parent == id && child.material_partition == group.material_partition &&
                    child.material_selector == group.material_selector &&
                    child.first_source_primitive == next_source &&
                    std::isfinite(child.geometric_error) && child.geometric_error <= group.geometric_error,
                    "children do not partition the parent patch or error");
            Encloses(group.bounds, child.bounds);
            EnclosesChildSphere(group.bounds, child.bounds);
            next_source += child.source_primitive_count;
        }
        Require(next_source == std::uint64_t(group.first_source_primitive) + group.source_primitive_count,
                "children omit source occurrences");
    }
    Require(std::ranges::none_of(owners, [](auto owner) { return owner == kNoGeometryHierarchyGroup; }) &&
            std::ranges::all_of(child_slots, [](bool owned) { return owned; }), "unowned hierarchy records");

    std::vector<bool> reached(hierarchy.groups.size());
    std::vector<std::uint32_t> pending;
    std::uint64_t next_root_source = 0;
    for (const auto root : hierarchy.root_groups)
    {
        Require(root < hierarchy.groups.size(), "invalid root ID");
        const auto &group = hierarchy.groups[root];
        Require(group.parent == kNoGeometryHierarchyGroup &&
                group.first_source_primitive == next_root_source, "roots do not partition the source");
        next_root_source += group.source_primitive_count;
        pending.push_back(root);
    }
    Require(next_root_source == hierarchy.source_triangle_count, "root coverage is incomplete");
    while (!pending.empty())
    {
        const auto id = pending.back();
        pending.pop_back();
        Require(!reached[id], "hierarchy is cyclic or has shared children");
        reached[id] = true;
        const auto &group = hierarchy.groups[id];
        for (std::uint32_t child = 0; child < group.child_count; ++child)
            pending.push_back(hierarchy.child_groups[group.first_child + child]);
    }
    Require(std::ranges::all_of(reached, [](bool seen) { return seen; }), "unreachable hierarchy group");

    using Interval = std::pair<std::uint32_t, std::uint32_t>;
    std::vector<std::vector<Interval>> index_ranges(hierarchy.pages.size()), meshlet_ranges(hierarchy.pages.size());
    std::vector<std::uint8_t> page_class(hierarchy.pages.size());
    for (std::size_t id = 0; id < hierarchy.clusters.size(); ++id)
    {
        const auto &cluster = hierarchy.clusters[id];
        ValidateBounds(cluster.bounds);
        Require(cluster.page_index < hierarchy.pages.size() && cluster.index_count > 0 &&
                cluster.first_index % 3 == 0 && cluster.index_count % 3 == 0 &&
                cluster.meshlet_count > 0 && std::isfinite(cluster.geometric_error) &&
                cluster.geometric_error >= 0.0f, "invalid hierarchy cluster");
        const auto &page = hierarchy.pages[cluster.page_index];
        Require(Range(cluster.first_index, cluster.index_count, page.index_count) &&
                Range(cluster.first_meshlet, cluster.meshlet_count, page.meshlet_count), "cluster exceeds page");
        index_ranges[cluster.page_index].emplace_back(cluster.first_index, cluster.index_count);
        meshlet_ranges[cluster.page_index].emplace_back(cluster.first_meshlet, cluster.meshlet_count);
        page_class[cluster.page_index] = static_cast<std::uint8_t>(page_class[cluster.page_index] |
            (hierarchy.groups[owners[id]].parent == kNoGeometryHierarchyGroup ? 1u : 2u));
    }
    const auto covers = [](auto &ranges, std::uint32_t size) {
        std::ranges::sort(ranges);
        std::uint64_t cursor = 0;
        for (const auto &[first, count] : ranges)
        {
            Require(first == cursor, "page cluster intervals have a gap or overlap");
            cursor += count;
        }
        Require(cursor == size, "page representation coverage is incomplete");
    };
    for (std::size_t page = 0; page < hierarchy.pages.size(); ++page)
    {
        Require(page_class[page] == 1u || page_class[page] == 2u, "root and nonroot pages must be separate");
        Require((hierarchy.pages[page].flags == kGeometryHierarchyPagePinned) == (page_class[page] == 1u),
                "root representation pinning disagrees with page directory");
        covers(index_ranges[page], hierarchy.pages[page].index_count);
        covers(meshlet_ranges[page], hierarchy.pages[page].meshlet_count);
    }
}

void ValidateGeometryHierarchyPages(const GeometryHierarchy &hierarchy,
    std::span<const GeometryData> pages, std::span<const ExpandedVertex> source)
{
    ValidateGeometryHierarchy(hierarchy);
    Require(pages.size() == hierarchy.pages.size(), "page payload count disagrees with directory");
    std::vector<std::vector<std::byte>> encoded_vertices;
    if (!source.empty())
    {
        Require(source.size() == std::uint64_t(hierarchy.source_triangle_count) * 3,
                "original source occurrence count disagrees with hierarchy");
        encoded_vertices.reserve(pages.size());
    }
    for (std::size_t index = 0; index < pages.size(); ++index)
    {
        const auto &page = pages[index];
        ValidatePagePayload(hierarchy, Count(index), page);
        if (!source.empty()) encoded_vertices.push_back(EncodeExpandedVertices(page.vertices));
    }
    if (source.empty()) return;
    const auto source_bytes = EncodeExpandedVertices(source);
    for (const auto &group : hierarchy.groups)
    {
        for (std::uint32_t primitive = 0; primitive < group.source_primitive_count; ++primitive)
        {
            const auto occurrence = hierarchy.source_primitive_order[group.first_source_primitive + primitive];
            for (std::uint32_t lane = 0; lane < 3; ++lane)
                Encloses(group.bounds, source[std::size_t(occurrence) * 3 + lane]);
        }
        if (group.child_count != 0) continue;
        auto occurrence_offset = group.first_source_primitive;
        for (std::uint32_t cluster_offset = 0; cluster_offset < group.cluster_count; ++cluster_offset)
        {
            const auto &cluster = hierarchy.clusters[group.first_cluster + cluster_offset];
            const auto &page = pages[cluster.page_index];
            const auto &page_bytes = encoded_vertices[cluster.page_index];
            for (std::uint32_t index = 0; index < cluster.index_count; index += 3)
            {
                const auto occurrence = hierarchy.source_primitive_order[occurrence_offset++];
                for (std::uint32_t lane = 0; lane < 3; ++lane)
                {
                    const auto source_offset = (std::size_t(occurrence) * 3 + lane) * kExpandedVertexStride;
                    const auto page_offset = std::size_t(page.indices[cluster.first_index + index + lane]) * kExpandedVertexStride;
                    Require(std::equal(source_bytes.begin() + source_offset,
                        source_bytes.begin() + source_offset + kExpandedVertexStride,
                        page_bytes.begin() + page_offset), "leaf triangle occurrence payload changed");
                }
            }
        }
    }
}

void ValidateGeometryHierarchyPage(const GeometryHierarchy &hierarchy,
    std::uint32_t page_index, const GeometryData &page)
{
    ValidateGeometryHierarchy(hierarchy);
    ValidatePagePayload(hierarchy, page_index, page);
}

AdmittedGeometryHierarchy::AdmittedGeometryHierarchy(std::span<const std::byte> bytes)
    : hierarchy_(DecodeGeometryHierarchy(bytes)), page_clusters_(hierarchy_.pages.size())
{
    for (std::size_t group_index = 0; group_index < hierarchy_.groups.size(); ++group_index)
    {
        const auto &group = hierarchy_.groups[group_index];
        for (std::uint32_t offset = 0; offset < group.cluster_count; ++offset)
        {
            const auto cluster_index = group.first_cluster + offset;
            page_clusters_[hierarchy_.clusters[cluster_index].page_index].emplace_back(
                Count(group_index), cluster_index);
        }
    }
}

void AdmittedGeometryHierarchy::ValidatePage(std::uint32_t page_index,
    const PreparedGeometryPage &page) const
{
    Require(page_index < hierarchy_.pages.size(), "page index exceeds hierarchy directory");
    ValidatePageDirectory(hierarchy_, hierarchy_.pages[page_index], page.geometry);
    for (const auto &[group_index, cluster_index] : page_clusters_[page_index])
        ValidateClusterPayload(hierarchy_.groups[group_index], hierarchy_.clusters[cluster_index],
            page.geometry);
}

std::vector<std::byte> EncodeGeometryHierarchy(const GeometryHierarchy &hierarchy)
{
    ValidateGeometryHierarchy(hierarchy);
    std::uint64_t string_size = 0;
    for (const auto &page : hierarchy.pages) string_size += page.logical_id.size();
    Require(string_size <= std::numeric_limits<std::uint32_t>::max(), "page IDs exceed portable ABI");
    const auto groups = Count(hierarchy.groups.size()), clusters = Count(hierarchy.clusters.size());
    const auto pages = Count(hierarchy.pages.size()), roots = Count(hierarchy.root_groups.size());
    const auto children = Count(hierarchy.child_groups.size()), occurrences = Count(hierarchy.source_primitive_order.size());
    const auto strings = static_cast<std::uint32_t>(string_size);
    const auto total = EncodedSize(groups, clusters, pages, roots, children, occurrences, strings);
    Require(total <= std::numeric_limits<std::size_t>::max(), "hierarchy payload exceeds addressable memory");
    std::vector<std::byte> bytes(static_cast<std::size_t>(total));
    const std::array header{kMagic, hierarchy.abi, hierarchy.geometry_flags, hierarchy.error_method_abi,
        static_cast<std::uint32_t>(hierarchy.error_domain), hierarchy.source_triangle_count,
        groups, clusters, pages, roots, children, occurrences, strings};
    for (std::size_t index = 0; index < header.size(); ++index) Put(bytes, index * 4, header[index]);
    std::size_t cursor = kGeometryHierarchyHeaderStride;
    for (const auto &group : hierarchy.groups)
    {
        const std::array fields{group.parent, group.first_child, group.child_count, group.first_cluster,
            group.cluster_count, group.first_source_primitive, group.source_primitive_count, group.material_partition};
        for (std::size_t index = 0; index < fields.size(); ++index) Put(bytes, cursor + index * 4, fields[index]);
        WriteBounds(bytes, cursor + 32, group.bounds);
        Put(bytes, cursor + 72, std::bit_cast<std::uint32_t>(group.geometric_error));
        Put(bytes, cursor + 76, group.material_selector);
        cursor += kGeometryHierarchyGroupStride;
    }
    for (const auto &cluster : hierarchy.clusters)
    {
        const std::array fields{cluster.page_index, cluster.first_index, cluster.index_count,
            cluster.first_meshlet, cluster.meshlet_count, cluster.material_partition};
        for (std::size_t index = 0; index < fields.size(); ++index) Put(bytes, cursor + index * 4, fields[index]);
        WriteBounds(bytes, cursor + 24, cluster.bounds);
        Put(bytes, cursor + 64, std::bit_cast<std::uint32_t>(cluster.geometric_error));
        Put(bytes, cursor + 68, cluster.material_selector);
        cursor += kGeometryHierarchyClusterStride;
    }
    std::uint32_t string_offset = 0;
    for (const auto &page : hierarchy.pages)
    {
        const std::array fields{string_offset, Count(page.logical_id.size()), page.flags,
            page.vertex_count, page.index_count, page.meshlet_count};
        for (std::size_t index = 0; index < fields.size(); ++index) Put(bytes, cursor + index * 4, fields[index]);
        string_offset += Count(page.logical_id.size());
        cursor += kGeometryHierarchyPageStride;
    }
    for (const auto *values : {&hierarchy.root_groups, &hierarchy.child_groups, &hierarchy.source_primitive_order})
        for (const auto value : *values) { Put(bytes, cursor, value); cursor += 4; }
    for (const auto &page : hierarchy.pages)
        for (const auto character : page.logical_id) bytes[cursor++] = std::byte(static_cast<unsigned char>(character));
    Require(cursor == bytes.size(), "encoded hierarchy size disagrees with header");
    return bytes;
}

GeometryHierarchy DecodeGeometryHierarchy(std::span<const std::byte> bytes)
{
    Require(bytes.size() >= kGeometryHierarchyHeaderStride && Get(bytes, 0) == kMagic,
            "invalid hierarchy header");
    Reserved(bytes, 52, 64);
    GeometryHierarchy hierarchy;
    hierarchy.abi = Get(bytes, 4);
    hierarchy.geometry_flags = Get(bytes, 8);
    hierarchy.error_method_abi = Get(bytes, 12);
    hierarchy.error_domain = static_cast<GeometryHierarchyErrorDomain>(Get(bytes, 16));
    hierarchy.source_triangle_count = Get(bytes, 20);
    const auto groups = Get(bytes, 24), clusters = Get(bytes, 28), pages = Get(bytes, 32);
    const auto roots = Get(bytes, 36), children = Get(bytes, 40), occurrences = Get(bytes, 44), strings = Get(bytes, 48);
    Require(hierarchy.source_triangle_count > 0 && hierarchy.source_triangle_count <= kMaxGeometryIndices / 3 &&
            groups > 0 && groups <= std::uint64_t(hierarchy.source_triangle_count) * 2 &&
            clusters > 0 && clusters <= kMaxGeometryIndices && pages > 0 && pages <= clusters &&
            roots > 0 && roots <= groups && children == groups - roots &&
            occurrences == hierarchy.source_triangle_count && strings <= std::uint64_t(pages) * kMaxPageIdBytes &&
            EncodedSize(groups, clusters, pages, roots, children, occurrences, strings) == bytes.size(),
            "invalid hierarchy section sizes");
    hierarchy.groups.resize(groups);
    hierarchy.clusters.resize(clusters);
    hierarchy.pages.resize(pages);
    std::size_t cursor = kGeometryHierarchyHeaderStride;
    for (auto &group : hierarchy.groups)
    {
        group.parent = Get(bytes, cursor); group.first_child = Get(bytes, cursor + 4);
        group.child_count = Get(bytes, cursor + 8); group.first_cluster = Get(bytes, cursor + 12);
        group.cluster_count = Get(bytes, cursor + 16); group.first_source_primitive = Get(bytes, cursor + 20);
        group.source_primitive_count = Get(bytes, cursor + 24); group.material_partition = Get(bytes, cursor + 28);
        group.bounds = ReadBounds(bytes, cursor + 32);
        group.geometric_error = std::bit_cast<float>(Get(bytes, cursor + 72));
        group.material_selector = Get(bytes, cursor + 76);
        Reserved(bytes, cursor + 80, cursor + kGeometryHierarchyGroupStride);
        cursor += kGeometryHierarchyGroupStride;
    }
    for (auto &cluster : hierarchy.clusters)
    {
        cluster.page_index = Get(bytes, cursor); cluster.first_index = Get(bytes, cursor + 4);
        cluster.index_count = Get(bytes, cursor + 8); cluster.first_meshlet = Get(bytes, cursor + 12);
        cluster.meshlet_count = Get(bytes, cursor + 16); cluster.material_partition = Get(bytes, cursor + 20);
        cluster.bounds = ReadBounds(bytes, cursor + 24);
        cluster.geometric_error = std::bit_cast<float>(Get(bytes, cursor + 64));
        cluster.material_selector = Get(bytes, cursor + 68);
        Reserved(bytes, cursor + 72, cursor + kGeometryHierarchyClusterStride);
        cursor += kGeometryHierarchyClusterStride;
    }
    const auto string_begin = static_cast<std::size_t>(EncodedSize(groups, clusters, pages, roots, children, occurrences, 0));
    std::uint32_t next_string = 0;
    for (auto &page : hierarchy.pages)
    {
        const auto offset = Get(bytes, cursor), length = Get(bytes, cursor + 4);
        Require(offset == next_string && length > 0 && length <= kMaxPageIdBytes && Range(offset, length, strings),
                "invalid logical page string range");
        page.logical_id.assign(reinterpret_cast<const char *>(bytes.data() + string_begin + offset), length);
        page.flags = Get(bytes, cursor + 8); page.vertex_count = Get(bytes, cursor + 12);
        page.index_count = Get(bytes, cursor + 16); page.meshlet_count = Get(bytes, cursor + 20);
        Reserved(bytes, cursor + 24, cursor + kGeometryHierarchyPageStride);
        cursor += kGeometryHierarchyPageStride;
        next_string += length;
    }
    Require(next_string == strings, "unowned page ID bytes");
    for (const auto [values, count] : {std::pair{&hierarchy.root_groups, roots},
            std::pair{&hierarchy.child_groups, children}, std::pair{&hierarchy.source_primitive_order, occurrences}})
    {
        values->resize(count);
        for (auto &value : *values) { value = Get(bytes, cursor); cursor += 4; }
    }
    Require(cursor == string_begin, "hierarchy section cursor disagrees with header");
    ValidateGeometryHierarchy(hierarchy);
    return hierarchy;
}
} // namespace hs::render_data
