#include "geometry_hierarchy_cooker.hpp"
#include "geometry_cooker.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace hs::geometry_content
{
namespace
{
namespace rd = render_data;
constexpr std::size_t kAttributeCount = 18;

void Require(bool condition, const char *reason)
{
    if (!condition) throw std::runtime_error(std::string("geometry hierarchy cooker: ") + reason);
}

std::uint32_t Count(std::size_t value)
{
    Require(value <= std::numeric_limits<std::uint32_t>::max(), "count exceeds portable ABI");
    return static_cast<std::uint32_t>(value);
}

float UpperFloat(double value)
{
    Require(std::isfinite(value) && value >= 0.0 &&
            value <= std::numeric_limits<float>::max(), "nonfinite or unrepresentable bound");
    auto result = static_cast<float>(value);
    if (double(result) < value) result = std::nextafter(result, std::numeric_limits<float>::infinity());
    Require(std::isfinite(result), "outward rounding overflow");
    return result;
}

rd::GeometryBounds Bounds(std::span<const rd::ExpandedVertex> vertices)
{
    Require(!vertices.empty(), "empty bounds input");
    rd::GeometryBounds result;
    result.minimum = result.maximum = vertices.front().position;
    for (const auto &vertex : vertices)
        for (std::size_t lane = 0; lane < 3; ++lane)
        {
            Require(std::isfinite(vertex.position[lane]), "nonfinite position");
            result.minimum[lane] = std::min(result.minimum[lane], vertex.position[lane]);
            result.maximum[lane] = std::max(result.maximum[lane], vertex.position[lane]);
        }
    for (std::size_t lane = 0; lane < 3; ++lane)
        result.sphere[lane] = static_cast<float>(
            (double(result.minimum[lane]) + result.maximum[lane]) * 0.5);
    double radius = 0.0;
    for (const auto &vertex : vertices)
    {
        double squared_distance = 0.0;
        for (std::size_t lane = 0; lane < 3; ++lane)
        {
            const double delta = double(vertex.position[lane]) - result.sphere[lane];
            squared_distance += delta * delta;
        }
        radius = std::max(radius, std::sqrt(squared_distance));
    }
    result.sphere[3] = UpperFloat(radius);
    if (radius > 0.0)
        result.sphere[3] = std::nextafter(result.sphere[3], std::numeric_limits<float>::infinity());
    Require(std::isfinite(result.sphere[3]), "outward sphere radius overflow");
    return result;
}

std::vector<rd::ExpandedVertex> Expand(std::span<const rd::ExpandedVertex> vertices,
    std::span<const unsigned> indices)
{
    std::vector<rd::ExpandedVertex> result;
    result.reserve(indices.size());
    for (const auto index : indices)
    {
        Require(index < vertices.size(), "representation index exceeds source vertex buffer");
        result.push_back(vertices[index]);
    }
    return result;
}

std::vector<rd::ExpandedVertex> SourcePatch(std::span<const rd::ExpandedVertex> source,
    std::span<const std::uint32_t> primitives)
{
    std::vector<rd::ExpandedVertex> result;
    result.reserve(primitives.size() * 3);
    for (const auto primitive : primitives)
    {
        const auto first = std::size_t(primitive) * 3;
        Require(first < source.size() && 3 <= source.size() - first, "source occurrence exceeds input");
        result.insert(result.end(), source.begin() + first, source.begin() + first + 3);
    }
    return result;
}

struct WorkGroup
{
    std::uint32_t parent{rd::kNoGeometryHierarchyGroup};
    std::vector<std::uint32_t> children;
    std::vector<unsigned> representation;
    // Leaves keep this aligned with their representation triangle order. Other
    // groups use it only for their exact original patch and stable ordering.
    std::vector<std::uint32_t> source_primitives;
    std::vector<std::uint32_t> published_leaf_primitives;
    std::uint32_t first_occurrence{};
    std::uint16_t material_index{};
    std::uint32_t material_partition{};
    rd::GeometryBounds bounds;
    float geometric_error{};
};

struct Partition
{
    std::vector<std::uint32_t> children;
    std::vector<unsigned> indices;
};

std::vector<Partition> PartitionGroups(std::span<const std::uint32_t> frontier,
    const std::vector<WorkGroup> &groups, std::span<const std::array<float, 3>> positions,
    std::uint32_t target_group_size)
{
    std::vector<unsigned> indices, counts, assignments(frontier.size());
    for (const auto id : frontier)
    {
        const auto &representation = groups[id].representation;
        counts.push_back(Count(representation.size()));
        indices.insert(indices.end(), representation.begin(), representation.end());
    }
    // Complete child representations are indivisible partition elements. This
    // is the strict tree variant of meshoptimizer's cluster LOD grouping.
    const auto partition_count = meshopt_partitionClusters(assignments.data(), indices.data(),
        indices.size(), counts.data(), frontier.size(), positions.front().data(), positions.size(),
        sizeof(positions.front()), target_group_size);
    Require(partition_count > 0 && partition_count <= frontier.size(), "invalid partitioner output");
    std::vector<Partition> partitions(partition_count);
    for (std::size_t child = 0; child < frontier.size(); ++child)
    {
        Require(assignments[child] < partitions.size(), "partition ID exceeds output");
        partitions[assignments[child]].children.push_back(frontier[child]);
    }
    for (auto &partition : partitions)
    {
        Require(!partition.children.empty(), "empty partitioner output");
        std::ranges::sort(partition.children, [&](auto left, auto right) {
            return std::pair{groups[left].first_occurrence, left} <
                   std::pair{groups[right].first_occurrence, right};
        });
        for (const auto child : partition.children)
            partition.indices.insert(partition.indices.end(), groups[child].representation.begin(),
                groups[child].representation.end());
    }
    std::ranges::sort(partitions, [&](const auto &left, const auto &right) {
        const auto l = left.children.front(), r = right.children.front();
        return std::pair{groups[l].first_occurrence, l} < std::pair{groups[r].first_occurrence, r};
    });
    return partitions;
}

std::vector<unsigned char> BoundaryLocks(const std::vector<Partition> &partitions,
    std::span<const unsigned> position_remap)
{
    const auto unassigned = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> owner(position_remap.size(), unassigned);
    std::vector<bool> shared(position_remap.size());
    for (std::size_t partition = 0; partition < partitions.size(); ++partition)
        for (const auto vertex : partitions[partition].indices)
        {
            const auto position = position_remap[vertex];
            if (owner[position] == unassigned) owner[position] = Count(partition);
            else if (owner[position] != partition) shared[position] = true;
        }
    std::vector<unsigned char> locks(position_remap.size());
    for (std::size_t vertex = 0; vertex < locks.size(); ++vertex)
        if (shared[position_remap[vertex]]) locks[vertex] = meshopt_SimplifyVertex_Lock;
    return locks;
}

std::vector<std::array<float, kAttributeCount>> Attributes(std::span<const rd::ExpandedVertex> vertices)
{
    std::vector<std::array<float, kAttributeCount>> result;
    result.reserve(vertices.size());
    for (const auto &vertex : vertices)
    {
        std::array<float, kAttributeCount> attributes{};
        std::size_t cursor = 0;
        for (const auto value : vertex.normal) attributes[cursor++] = value;
        for (const auto value : vertex.bone_indices) attributes[cursor++] = static_cast<float>(value);
        for (const auto value : vertex.bone_weights) attributes[cursor++] = value;
        for (const auto value : vertex.uv) attributes[cursor++] = value;
        for (const auto value : vertex.tangent) attributes[cursor++] = value;
        attributes[cursor++] = static_cast<float>(vertex.material_index);
        Require(cursor == kAttributeCount, "attribute layout disagrees with expanded vertex");
        result.push_back(attributes);
    }
    return result;
}

WorkGroup MakeParent(const Partition &partition, const std::vector<WorkGroup> &groups,
    std::span<const rd::ExpandedVertex> vertices, std::span<const rd::ExpandedVertex> source,
    std::span<const std::array<float, 3>> positions,
    std::span<const std::array<float, kAttributeCount>> attributes,
    std::span<const unsigned char> locks, std::span<const std::uint16_t> material_mapping,
    float reduction_ratio, GeometryErrorChecks error_checks)
{
    WorkGroup parent;
    parent.children = partition.children;
    parent.material_index = groups[partition.children.front()].material_index;
    parent.material_partition = groups[partition.children.front()].material_partition;
    parent.first_occurrence = groups[partition.children.front()].first_occurrence;
    for (const auto child : parent.children)
    {
        const auto &group = groups[child];
        Require(group.material_index == parent.material_index &&
                group.material_partition == parent.material_partition, "partition crosses a material boundary");
        parent.source_primitives.insert(parent.source_primitives.end(), group.source_primitives.begin(),
            group.source_primitives.end());
        parent.geometric_error = std::max(parent.geometric_error, group.geometric_error);
    }
    const auto patch = SourcePatch(source, parent.source_primitives);
    parent.bounds = Bounds(patch);
    double hierarchy_radius = parent.bounds.sphere[3];
    for (const auto child : parent.children)
    {
        double squared_distance = 0.0;
        for (std::size_t lane = 0; lane < 3; ++lane)
        {
            const double delta = double(parent.bounds.sphere[lane]) - groups[child].bounds.sphere[lane];
            squared_distance += delta * delta;
        }
        hierarchy_radius = std::max(hierarchy_radius,
            std::sqrt(squared_distance) + double(groups[child].bounds.sphere[3]));
    }
    parent.bounds.sphere[3] = UpperFloat(hierarchy_radius);
    if (hierarchy_radius > 0.0)
        parent.bounds.sphere[3] = std::nextafter(parent.bounds.sphere[3], std::numeric_limits<float>::infinity());
    Require(std::isfinite(parent.bounds.sphere[3]), "outward hierarchy sphere overflow");
    const auto triangles = partition.indices.size() / 3;
    const auto target_triangles = std::max<std::size_t>(1,
        static_cast<std::size_t>(std::floor(double(triangles) * reduction_ratio)));
    parent.representation.resize(partition.indices.size());
    std::array<float, kAttributeCount> weights;
    weights.fill(1.0f);
    const auto count = meshopt_simplifyWithAttributes(parent.representation.data(), partition.indices.data(),
        partition.indices.size(), positions.front().data(), positions.size(), sizeof(positions.front()),
        attributes.front().data(), sizeof(attributes.front()), weights.data(), weights.size(), locks.data(),
        target_triangles * 3, std::numeric_limits<float>::max(),
        meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute | meshopt_SimplifyLockBorder, nullptr);
    Require(count <= partition.indices.size() && count % 3 == 0, "invalid simplifier output");
    if (count == 0 || count == partition.indices.size())
        parent.representation = partition.indices; // A complete retained patch, without retry tuning.
    else
        parent.representation.resize(count);
    const auto coarse = Expand(vertices, parent.representation);
    // Measure against original occurrences, not only the previous simplified
    // level. This avoids accumulating an unqualified simplifier estimate.
    const auto measured = MeasureGeometryError(patch, coarse, material_mapping, 4096, error_checks);
    Require(std::isfinite(measured.lower) && measured.lower >= 0.0 && measured.upper >= measured.lower,
            "invalid qualified geometry error");
    parent.geometric_error = std::max(parent.geometric_error, UpperFloat(measured.upper));
    return parent;
}

class PageBuilder
{
public:
    PageBuilder(CookedGeometryHierarchy &output, std::string_view prefix,
        GeometryHierarchyCookOptions options, std::uint32_t flags)
        : output_(output), prefix_(prefix), options_(options), flags_(flags) {}

    void BeginClass(bool pinned)
    {
        Flush();
        pinned_ = pinned;
    }

    void Append(std::uint32_t group_id, WorkGroup &group,
        std::span<const rd::ExpandedVertex> source_vertices)
    {
        const auto expanded = Expand(source_vertices, group.representation);
        const auto geometry = CookGeometry(expanded, flags_);
        auto &record = output_.hierarchy.groups[group_id];
        record.first_cluster = Count(output_.hierarchy.clusters.size());
        for (const auto &meshlet : geometry.meshlets)
        {
            if (!page_.meshlets.empty() && (page_.indices.size() / 3 + meshlet.triangle_count >
                    options_.max_page_triangles || page_.meshlets.size() >= options_.max_page_meshlets))
                Flush();
            if (page_.meshlets.empty())
            {
                page_.flags = flags_;
                page_first_group_ = group_id;
            }
            const auto first_index = Count(page_.indices.size());
            const auto first_meshlet = Count(page_.meshlets.size());
            const auto vertex_base = Count(page_.vertices.size());
            std::vector<rd::ExpandedVertex> cluster_vertices;
            cluster_vertices.reserve(meshlet.vertex_ref_count);
            for (std::uint32_t vertex = 0; vertex < meshlet.vertex_ref_count; ++vertex)
            {
                const auto &value = geometry.vertices[geometry.meshlet_vertex_refs[meshlet.first_vertex_ref + vertex]];
                page_.vertices.push_back(value);
                cluster_vertices.push_back(value);
            }
            const auto material = geometry.submeshes[meshlet.submesh_index].material_index;
            Require(material == group.material_index, "representation crosses authored material selector");
            if (page_.submeshes.empty() || page_.submeshes.back().material_index != material)
                page_.submeshes.push_back({first_index, 0, first_meshlet, 0, material});
            auto &submesh = page_.submeshes.back();
            auto packed_meshlet = meshlet;
            packed_meshlet.first_vertex_ref = Count(page_.meshlet_vertex_refs.size());
            packed_meshlet.first_triangle = Count(page_.meshlet_triangles.size());
            packed_meshlet.submesh_index = Count(page_.submeshes.size() - 1);
            for (std::uint32_t vertex = 0; vertex < meshlet.vertex_ref_count; ++vertex)
                page_.meshlet_vertex_refs.push_back(vertex_base + vertex);
            for (std::uint32_t triangle = 0; triangle < meshlet.triangle_count; ++triangle)
            {
                const auto packed = geometry.meshlet_triangles[meshlet.first_triangle + triangle];
                page_.meshlet_triangles.push_back(packed);
                page_.canonical_primitive_remap.push_back(Count(page_.indices.size() / 3));
                for (std::uint32_t lane = 0; lane < 3; ++lane)
                    page_.indices.push_back(vertex_base + ((packed >> (lane * 8)) & 255u));
                if (group.children.empty())
                {
                    const auto primitive = geometry.canonical_primitive_remap[meshlet.first_triangle + triangle];
                    Require(primitive < group.source_primitives.size(), "leaf remap exceeds occurrence list");
                    group.published_leaf_primitives.push_back(group.source_primitives[primitive]);
                }
            }
            submesh.index_count += meshlet.triangle_count * 3;
            ++submesh.meshlet_count;
            page_.meshlets.push_back(packed_meshlet);
            rd::GeometryHierarchyCluster cluster;
            cluster.page_index = Count(output_.pages.size());
            cluster.first_index = first_index;
            cluster.index_count = meshlet.triangle_count * 3;
            cluster.first_meshlet = first_meshlet;
            cluster.meshlet_count = 1;
            cluster.material_partition = group.material_partition;
            cluster.material_selector = group.material_index;
            cluster.bounds = Bounds(cluster_vertices);
            cluster.geometric_error = group.geometric_error;
            output_.hierarchy.clusters.push_back(cluster);
        }
        record.cluster_count = Count(output_.hierarchy.clusters.size()) - record.first_cluster;
    }

    void Flush()
    {
        if (page_.meshlets.empty()) return;
        page_.bounds = Bounds(page_.vertices);
        rd::GeometryHierarchyPage directory;
        directory.logical_id = prefix_ + "/group_" + std::to_string(page_first_group_) +
            "/page_" + std::to_string(output_.pages.size()) + ".rpage";
        directory.flags = pinned_ ? rd::kGeometryHierarchyPagePinned : 0;
        directory.vertex_count = Count(page_.vertices.size());
        directory.index_count = Count(page_.indices.size());
        directory.meshlet_count = Count(page_.meshlets.size());
        output_.hierarchy.pages.push_back(std::move(directory));
        output_.pages.push_back(std::move(page_));
        page_ = {};
    }

private:
    CookedGeometryHierarchy &output_;
    std::string prefix_;
    GeometryHierarchyCookOptions options_;
    std::uint32_t flags_{};
    rd::GeometryData page_;
    std::uint32_t page_first_group_{};
    bool pinned_{};
};

void AssignCoverage(CookedGeometryHierarchy &output, const std::vector<WorkGroup> &groups)
{
    struct Visit { std::uint32_t id; bool exit; };
    std::vector<Visit> pending;
    for (auto root = output.hierarchy.root_groups.rbegin(); root != output.hierarchy.root_groups.rend(); ++root)
        pending.push_back({*root, false});
    while (!pending.empty())
    {
        const auto visit = pending.back();
        pending.pop_back();
        auto &record = output.hierarchy.groups[visit.id];
        if (visit.exit)
        {
            record.source_primitive_count = Count(output.hierarchy.source_primitive_order.size()) -
                record.first_source_primitive;
            continue;
        }
        record.first_source_primitive = Count(output.hierarchy.source_primitive_order.size());
        const auto &group = groups[visit.id];
        if (group.children.empty())
        {
            Require(group.published_leaf_primitives.size() == group.source_primitives.size(),
                    "leaf page packing omitted occurrence IDs");
            output.hierarchy.source_primitive_order.insert(output.hierarchy.source_primitive_order.end(),
                group.published_leaf_primitives.begin(), group.published_leaf_primitives.end());
            record.source_primitive_count = Count(group.published_leaf_primitives.size());
        }
        else
        {
            pending.push_back({visit.id, true});
            for (auto child = group.children.rbegin(); child != group.children.rend(); ++child)
                pending.push_back({*child, false});
        }
    }
}
} // namespace

CookedGeometryHierarchy CookGeometryHierarchy(std::span<const rd::ExpandedVertex> expanded,
    std::uint32_t flags, std::string_view logical_page_prefix,
    std::span<const std::uint16_t> material_mapping, GeometryHierarchyCookOptions options)
{
    Require(options.target_group_size >= 2 && options.target_group_size <= 64 &&
            std::isfinite(options.reduction_ratio) && options.reduction_ratio > 0.0f &&
            options.reduction_ratio < 1.0f && options.max_page_triangles >= rd::kMaxGeometryMeshletTriangles &&
            options.max_page_triangles <= rd::kMaxGeometryIndices / 3 && options.max_page_meshlets > 0 &&
            options.max_page_meshlets <= rd::kMaxGeometryIndices / 3, "invalid cook options");
    Require(logical_page_prefix.starts_with("render/hierarchy/") && !logical_page_prefix.ends_with('/') &&
            logical_page_prefix.size() <= 448, "invalid logical page prefix");
    const auto geometry = CookGeometry(expanded, flags);
    std::vector<std::array<float, 3>> positions;
    positions.reserve(geometry.vertices.size());
    for (const auto &vertex : geometry.vertices) positions.push_back(vertex.position);
    std::vector<unsigned> position_remap(positions.size());
    meshopt_generatePositionRemap(position_remap.data(), positions.front().data(), positions.size(),
        sizeof(positions.front()));
    const auto attributes = Attributes(geometry.vertices);

    std::vector<WorkGroup> groups;
    std::map<std::uint16_t, std::vector<std::uint32_t>> material_frontiers;
    for (const auto &meshlet : geometry.meshlets)
    {
        WorkGroup leaf;
        leaf.material_index = static_cast<std::uint16_t>(geometry.submeshes[meshlet.submesh_index].material_index);
        Require(material_mapping.empty() || leaf.material_index < material_mapping.size(),
                "material selector exceeds material mapping");
        leaf.material_partition = material_mapping.empty() ? leaf.material_index : material_mapping[leaf.material_index];
        for (std::uint32_t triangle = 0; triangle < meshlet.triangle_count; ++triangle)
        {
            const auto primitive = geometry.canonical_primitive_remap[meshlet.first_triangle + triangle];
            leaf.source_primitives.push_back(primitive);
            for (std::uint32_t lane = 0; lane < 3; ++lane)
                leaf.representation.push_back(geometry.indices[std::size_t(primitive) * 3 + lane]);
        }
        leaf.first_occurrence = *std::ranges::min_element(leaf.source_primitives);
        leaf.bounds = Bounds(SourcePatch(expanded, leaf.source_primitives));
        const auto id = Count(groups.size());
        groups.push_back(std::move(leaf));
        material_frontiers[groups.back().material_index].push_back(id);
    }

    std::vector<std::uint32_t> roots;
    for (auto &[material, frontier] : material_frontiers)
    {
        (void)material;
        while (frontier.size() > 1)
        {
            const auto partitions = PartitionGroups(frontier, groups, positions, options.target_group_size);
            if (partitions.size() == frontier.size()) break;
            const auto locks = BoundaryLocks(partitions, position_remap);
            std::vector<std::uint32_t> next;
            next.reserve(partitions.size());
            for (const auto &partition : partitions)
            {
                if (partition.children.size() == 1)
                {
                    next.push_back(partition.children.front());
                    continue;
                }
                auto parent = MakeParent(partition, groups, geometry.vertices, expanded, positions,
                    attributes, locks, material_mapping, options.reduction_ratio, options.error_checks);
                const auto id = Count(groups.size());
                for (const auto child : parent.children)
                {
                    Require(groups[child].parent == rd::kNoGeometryHierarchyGroup, "child already has a parent");
                    groups[child].parent = id;
                }
                groups.push_back(std::move(parent));
                next.push_back(id);
            }
            frontier = std::move(next);
        }
        roots.insert(roots.end(), frontier.begin(), frontier.end());
    }
    std::ranges::sort(roots, [&](auto left, auto right) {
        return std::pair{groups[left].first_occurrence, left} < std::pair{groups[right].first_occurrence, right};
    });

    CookedGeometryHierarchy output;
    auto &hierarchy = output.hierarchy;
    hierarchy.geometry_flags = flags;
    hierarchy.error_domain = flags & (rd::kGeometryFlagSkinned | rd::kGeometryFlagWind)
        ? rd::GeometryHierarchyErrorDomain::ReferencePose : rd::GeometryHierarchyErrorDomain::Static;
    hierarchy.source_triangle_count = Count(expanded.size() / 3);
    hierarchy.root_groups = std::move(roots);
    hierarchy.groups.resize(groups.size());
    for (std::size_t id = 0; id < groups.size(); ++id)
    {
        const auto &group = groups[id];
        auto &record = hierarchy.groups[id];
        record.parent = group.parent;
        record.material_partition = group.material_partition;
        record.material_selector = group.material_index;
        record.bounds = group.bounds;
        record.geometric_error = group.geometric_error;
        if (!group.children.empty())
        {
            record.first_child = Count(hierarchy.child_groups.size());
            record.child_count = Count(group.children.size());
            hierarchy.child_groups.insert(hierarchy.child_groups.end(), group.children.begin(), group.children.end());
        }
    }
    PageBuilder pages(output, logical_page_prefix, options, flags);
    pages.BeginClass(true);
    for (const auto root : hierarchy.root_groups) pages.Append(root, groups[root], geometry.vertices);
    pages.BeginClass(false);
    for (std::size_t id = 0; id < groups.size(); ++id)
        if (groups[id].parent != rd::kNoGeometryHierarchyGroup)
            pages.Append(Count(id), groups[id], geometry.vertices);
    pages.Flush();
    AssignCoverage(output, groups);
    rd::ValidateGeometryHierarchyPages(hierarchy, output.pages, expanded);
    return output;
}
} // namespace hs::geometry_content
