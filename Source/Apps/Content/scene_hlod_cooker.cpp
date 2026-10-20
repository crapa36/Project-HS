#include "scene_hlod_cooker.hpp"
#include "geometry_cooker.hpp"

#include <hs/jobs/task_system.hpp>

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <exception>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace hs::geometry_content
{
namespace
{
namespace rd = render_data;
constexpr std::size_t kAttributeCount = 18;
void Require(bool condition, const char *reason)
{
    if (!condition) throw std::runtime_error(std::string("scene HLOD cooker: ") + reason);
}
std::uint32_t Count(std::size_t value)
{
    Require(value <= std::numeric_limits<std::uint32_t>::max(), "portable count overflow");
    return static_cast<std::uint32_t>(value);
}
float Upper(double value)
{
    Require(std::isfinite(value) && value >= 0 && value <= std::numeric_limits<float>::max(), "world bound overflow");
    auto result = static_cast<float>(value);
    if (double(result) < value) result = std::nextafter(result, std::numeric_limits<float>::infinity());
    Require(std::isfinite(result), "world rounding overflow"); return result;
}
float Lower(double value)
{
    Require(std::isfinite(value) && value >= -std::numeric_limits<float>::max() &&
            value <= std::numeric_limits<float>::max(), "world bound overflow");
    auto result = static_cast<float>(value);
    if (double(result) > value) result = std::nextafter(result, -std::numeric_limits<float>::infinity());
    Require(std::isfinite(result), "world rounding overflow"); return result;
}
float Ceiling(double value)
{
    Require(std::isfinite(value) && value >= -std::numeric_limits<float>::max() &&
            value <= std::numeric_limits<float>::max(), "world bound overflow");
    auto result = static_cast<float>(value);
    if (double(result) < value) result = std::nextafter(result, std::numeric_limits<float>::infinity());
    Require(std::isfinite(result), "world rounding overflow"); return result;
}
rd::GeometryBounds Bounds(std::span<const rd::ExpandedVertex> vertices)
{
    Require(!vertices.empty(), "empty world geometry");
    rd::GeometryBounds bounds; bounds.minimum = bounds.maximum = vertices.front().position;
    for (const auto &vertex : vertices)
        for (std::size_t lane = 0; lane < 3; ++lane)
        {
            Require(std::isfinite(vertex.position[lane]), "nonfinite baked position");
            bounds.minimum[lane] = std::min(bounds.minimum[lane], vertex.position[lane]);
            bounds.maximum[lane] = std::max(bounds.maximum[lane], vertex.position[lane]);
        }
    for (std::size_t lane = 0; lane < 3; ++lane)
        bounds.sphere[lane] = static_cast<float>((double(bounds.minimum[lane]) + bounds.maximum[lane]) * 0.5);
    double radius = 0;
    for (const auto &vertex : vertices)
    {
        double squared = 0;
        for (std::size_t lane = 0; lane < 3; ++lane)
        { const double delta = double(vertex.position[lane]) - bounds.sphere[lane]; squared += delta * delta; }
        radius = std::max(radius, std::sqrt(squared));
    }
    bounds.sphere[3] = Upper(radius);
    if (radius > 0) bounds.sphere[3] = std::nextafter(bounds.sphere[3], std::numeric_limits<float>::infinity());
    return bounds;
}
rd::GeometryBounds Union(std::span<const rd::GeometryBounds> input)
{
    Require(!input.empty(), "empty bound union"); auto b = input.front();
    for (const auto &inner : input)
        for (std::size_t lane = 0; lane < 3; ++lane)
        { b.minimum[lane] = std::min(b.minimum[lane], inner.minimum[lane]); b.maximum[lane] = std::max(b.maximum[lane], inner.maximum[lane]); }
    for (std::size_t lane = 0; lane < 3; ++lane)
        b.sphere[lane] = static_cast<float>((double(b.minimum[lane]) + b.maximum[lane]) * 0.5);
    double radius = 0;
    for (const auto &inner : input)
    {
        double squared = 0;
        for (std::size_t lane = 0; lane < 3; ++lane)
        { const double delta = double(b.sphere[lane]) - inner.sphere[lane]; squared += delta * delta; }
        radius = std::max(radius, std::sqrt(squared) + double(inner.sphere[3]));
    }
    b.sphere[3] = Upper(radius);
    if (radius > 0) b.sphere[3] = std::nextafter(b.sphere[3], std::numeric_limits<float>::infinity());
    Require(std::isfinite(b.sphere[3]), "union sphere overflow"); return b;
}
std::array<float, 3> Normalize(std::array<float, 3> vector)
{
    double squared = 0; for (const auto value : vector) squared += double(value) * value;
    Require(std::isfinite(squared) && squared > 0, "invalid transformed normal/tangent");
    const auto inverse = 1.0 / std::sqrt(squared);
    for (auto &value : vector) value = static_cast<float>(double(value) * inverse);
    return vector;
}
std::array<float, 3> TransformPosition(std::array<float, 3> position, const RenderInstance &instance)
{
    const auto sine = std::sin(instance.yaw), cosine = std::cos(instance.yaw);
    const std::array scale{instance.scale.x, instance.scale.y, instance.scale.z};
    const std::array translation{instance.position.x, instance.position.y, instance.position.z};
    for (std::size_t lane = 0; lane < 3; ++lane) position[lane] *= scale[lane];
    position = std::array{position[0] * cosine + position[2] * sine,
                          position[1], -position[0] * sine + position[2] * cosine};
    for (std::size_t lane = 0; lane < 3; ++lane) position[lane] += translation[lane];
    return position;
}
rd::ExpandedVertex Bake(rd::ExpandedVertex v, const RenderInstance &instance)
{
    const auto sine = std::sin(instance.yaw), cosine = std::cos(instance.yaw);
    const auto rotate = [&](std::array<float, 3> p) {
        return std::array{p[0] * cosine + p[2] * sine, p[1], -p[0] * sine + p[2] * cosine};
    };
    const std::array scale{instance.scale.x, instance.scale.y, instance.scale.z};
    v.position = TransformPosition(v.position, instance);
    for (std::size_t lane = 0; lane < 3; ++lane) v.normal[lane] /= scale[lane];
    v.normal = Normalize(rotate(v.normal));
    // Tangents transform covariantly; reflection also flips handedness.
    std::array tangent{v.tangent[0] * scale[0], v.tangent[1] * scale[1], v.tangent[2] * scale[2]};
    tangent = Normalize(rotate(tangent));
    for (std::size_t lane = 0; lane < 3; ++lane) v.tangent[lane] = tangent[lane];
    if (double(scale[0]) * scale[1] * scale[2] < 0) v.tangent[3] = -v.tangent[3];
    return v;
}
rd::GeometryBounds TransformBounds(const rd::GeometryBounds &source, const RenderInstance &instance)
{
    std::array<std::array<float, 3>, 8> corners{};
    std::array<double, 3> minimum{std::numeric_limits<double>::infinity(),
                                  std::numeric_limits<double>::infinity(),
                                  std::numeric_limits<double>::infinity()};
    std::array<double, 3> maximum{-std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity()};
    for (std::size_t mask = 0; mask < corners.size(); ++mask)
    {
        const std::array<float, 3> corner{
            (mask & 1) ? source.maximum[0] : source.minimum[0],
            (mask & 2) ? source.maximum[1] : source.minimum[1],
            (mask & 4) ? source.maximum[2] : source.minimum[2]};
        corners[mask] = TransformPosition(corner, instance);
        for (std::size_t lane = 0; lane < 3; ++lane)
        {
            Require(std::isfinite(corners[mask][lane]), "nonfinite transformed source bounds");
            minimum[lane] = std::min(minimum[lane], double(corners[mask][lane]));
            maximum[lane] = std::max(maximum[lane], double(corners[mask][lane]));
        }
    }
    rd::GeometryBounds result;
    for (std::size_t lane = 0; lane < 3; ++lane)
    {
        result.minimum[lane] = Lower(minimum[lane]);
        result.maximum[lane] = Ceiling(maximum[lane]);
        if (result.minimum[lane] > -std::numeric_limits<float>::max())
            result.minimum[lane] = std::nextafter(result.minimum[lane], -std::numeric_limits<float>::infinity());
        if (result.maximum[lane] < std::numeric_limits<float>::max())
            result.maximum[lane] = std::nextafter(result.maximum[lane], std::numeric_limits<float>::infinity());
        result.sphere[lane] = static_cast<float>((double(result.minimum[lane]) + result.maximum[lane]) * 0.5);
    }
    double radius = 0;
    for (std::size_t mask = 0; mask < corners.size(); ++mask)
    {
        const std::array<float, 3> corner{
            (mask & 1) ? result.maximum[0] : result.minimum[0],
            (mask & 2) ? result.maximum[1] : result.minimum[1],
            (mask & 4) ? result.maximum[2] : result.minimum[2]};
        double squared = 0;
        for (std::size_t lane = 0; lane < 3; ++lane)
        {
            const auto delta = double(corner[lane]) - double(result.sphere[lane]);
            squared += delta * delta;
        }
        Require(std::isfinite(squared), "transformed source sphere overflow");
        radius = std::max(radius, std::sqrt(squared));
    }
    result.sphere[3] = Upper(radius);
    if (radius > 0) result.sphere[3] = std::nextafter(result.sphere[3], std::numeric_limits<float>::infinity());
    return result;
}
struct Key
{
    std::string material;
    std::uint32_t semantic{}, selector{}, tint{}, variant{}, flags{};
    auto operator<=>(const Key &) const = default;
};
using Representation = std::map<Key, std::vector<rd::ExpandedVertex>>;
struct BakedMember
{
    rd::SceneHlodMember record;
    const SceneHlodCookSource *source{};
    RenderInstance instance{};
};
struct Object
{
    std::uint64_t id{};
    std::vector<BakedMember> members;
    rd::GeometryBounds bounds;
};
bool Append(Representation &target, const Representation &source)
{
    for (const auto &[key, vertices] : source)
    {
        auto &output = target[key];
        if (vertices.size() > rd::kMaxGeometryIndices - output.size()) return false;
        output.insert(output.end(), vertices.begin(), vertices.end());
    }
    return true;
}
std::vector<rd::ExpandedVertex> Simplify(std::span<const rd::ExpandedVertex> expanded,
                                       float ratio)
{
    Require(!expanded.empty() && expanded.size() % 3 == 0 && expanded.size() <= rd::kMaxGeometryIndices,
            "invalid expanded simplification input");
    const auto encoded = rd::EncodeExpandedVertices(expanded);
    std::vector<unsigned> remap(expanded.size());
    const auto unique_count = meshopt_generateVertexRemap(remap.data(), nullptr, expanded.size(),
        encoded.data(), expanded.size(), rd::kExpandedVertexStride);
    Require(unique_count > 0, "empty simplification vertex remap");
    std::vector<std::byte> unique(unique_count * rd::kExpandedVertexStride);
    meshopt_remapVertexBuffer(unique.data(), encoded.data(), expanded.size(), rd::kExpandedVertexStride,
                              remap.data());
    std::vector<unsigned> indices(expanded.size());
    meshopt_remapIndexBuffer(indices.data(), nullptr, expanded.size(), remap.data());
    const auto vertices = rd::DecodeExpandedVertices(unique);
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, kAttributeCount>> attributes;
    positions.reserve(vertices.size()); attributes.reserve(vertices.size());
    for (const auto &vertex : vertices)
    {
        positions.push_back(vertex.position);
        std::array<float, kAttributeCount> a{}; std::size_t lane = 0;
        for (const auto value : vertex.normal) a[lane++] = value;
        for (const auto value : vertex.bone_indices) a[lane++] = static_cast<float>(value);
        for (const auto value : vertex.bone_weights) a[lane++] = value;
        for (const auto value : vertex.uv) a[lane++] = value;
        for (const auto value : vertex.tangent) a[lane++] = value;
        a[lane++] = static_cast<float>(vertex.material_index);
        Require(lane == kAttributeCount, "attribute ABI mismatch"); attributes.push_back(a);
    }
    std::array<float, kAttributeCount> weights; weights.fill(1.0f);
    const auto target = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(double(expanded.size() / 3) * ratio))) * 3;
    std::vector<unsigned> simplified(indices.size());
    const auto count = meshopt_simplifyWithAttributes(simplified.data(), indices.data(), indices.size(),
        positions.front().data(), positions.size(), sizeof(positions.front()), attributes.front().data(),
        sizeof(attributes.front()), weights.data(), weights.size(), nullptr, target, std::numeric_limits<float>::max(),
        meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute | meshopt_SimplifyLockBorder, nullptr);
    Require(count <= indices.size() && count % 3 == 0, "invalid simplification output");
    if (count == 0 || count == indices.size()) return {expanded.begin(), expanded.end()};
    std::vector<rd::ExpandedVertex> output; output.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    { Require(simplified[i] < vertices.size(), "simplification index range"); output.push_back(vertices[simplified[i]]); }
    return output;
}
void BakeInto(Representation &target, const BakedMember &member)
{
    Require(member.source && member.source->geometry, "missing source for leaf bake");
    const auto &source = *member.source;
    const auto &geometry = *source.geometry;
    const auto &record = member.record;
    const bool reflected = double(member.instance.scale.x) * member.instance.scale.y * member.instance.scale.z < 0;
    for (std::size_t first = 0; first < geometry.indices.size(); first += 3)
    {
        const auto selector = geometry.vertices[geometry.indices[first]].material_index;
        Require(selector < source.material_products.size() && !source.material_products[selector].empty(),
                "missing actual material identity");
        const Key key{source.material_products[selector], record.mesh_semantic, selector, record.tint_rgba,
                      record.variant, geometry.flags};
        auto &vertices = target[key];
        Require(vertices.size() <= rd::kMaxGeometryIndices - 3, "leaf representation exceeds geometry page ABI");
        for (std::size_t lane = 0; lane < 3; ++lane)
        {
            const auto oriented_lane = reflected && lane > 0 ? 3 - lane : lane;
            auto vertex = Bake(geometry.vertices[geometry.indices[first + oriented_lane]], member.instance);
            Require(vertex.material_index == selector, "source triangle crosses selector boundary");
            vertices.push_back(vertex);
        }
    }
}
std::uint64_t TriangleCount(const Representation &representation)
{
    std::uint64_t count = 0; for (const auto &[key, vertices] : representation) { (void)key; count += vertices.size() / 3; }
    return count;
}
struct PackedFragment
{
    const Key *key{};
    std::span<const rd::ExpandedVertex> vertices;
    std::uint32_t page{};
};
struct PackedPlan
{
    std::vector<PackedFragment> fragments;
    std::uint32_t page_count{};
};
PackedPlan PlanPages(const Representation &representation, std::uint32_t triangle_limit)
{
    struct OpenPage { std::uint32_t id{}, triangles{}; };
    std::map<std::uint32_t, OpenPage> open;
    PackedPlan plan;
    for (const auto &[key, vertices] : representation)
    {
        for (std::size_t first = 0; first < vertices.size();)
        {
            auto found = open.find(key.flags);
            if (found == open.end() || found->second.triangles == triangle_limit)
                found = open.insert_or_assign(key.flags, OpenPage{plan.page_count++, 0}).first;
            auto &page = found->second;
            const auto triangles = std::min<std::size_t>(triangle_limit - page.triangles,
                                                        (vertices.size() - first) / 3);
            plan.fragments.push_back({&key, std::span(vertices).subspan(first, triangles * 3), page.id});
            page.triangles += Count(triangles);
            first += triangles * 3;
        }
    }
    return plan;
}
void AppendPage(rd::GeometryData &page, const rd::GeometryData &fragment)
{
    const auto vertex_base = Count(page.vertices.size()), index_base = Count(page.indices.size());
    const auto meshlet_base = Count(page.meshlets.size()), ref_base = Count(page.meshlet_vertex_refs.size());
    const auto triangle_base = Count(page.meshlet_triangles.size());
    if (page.vertices.empty()) page.flags = fragment.flags;
    Require(page.flags == fragment.flags, "packed page mixes geometry flag domains");
    page.vertices.insert(page.vertices.end(), fragment.vertices.begin(), fragment.vertices.end());
    for (const auto index : fragment.indices) page.indices.push_back(index + vertex_base);
    std::vector<std::uint32_t> submesh_map;
    for (auto run : fragment.submeshes)
    {
        run.first_index += index_base; run.first_meshlet += meshlet_base;
        if (!page.submeshes.empty() && page.submeshes.back().material_index == run.material_index)
        {
            page.submeshes.back().index_count += run.index_count;
            page.submeshes.back().meshlet_count += run.meshlet_count;
        }
        else page.submeshes.push_back(run);
        submesh_map.push_back(Count(page.submeshes.size() - 1));
    }
    for (auto meshlet : fragment.meshlets)
    {
        meshlet.first_vertex_ref += ref_base; meshlet.first_triangle += triangle_base;
        meshlet.submesh_index = submesh_map.at(meshlet.submesh_index);
        page.meshlets.push_back(meshlet);
    }
    for (const auto ref : fragment.meshlet_vertex_refs) page.meshlet_vertex_refs.push_back(ref + vertex_base);
    page.meshlet_triangles.insert(page.meshlet_triangles.end(), fragment.meshlet_triangles.begin(), fragment.meshlet_triangles.end());
    for (const auto primitive : fragment.canonical_primitive_remap)
        page.canonical_primitive_remap.push_back(primitive + index_base / 3);
}
class Builder
{
public:
    Builder(CookedSceneHlod &output, std::vector<Object> objects, std::string_view prefix, SceneHlodCookOptions options)
        : output_(output), objects_(std::move(objects)), prefix_(prefix), options_(options),
          tasks_(options.error_worker_count == 0 ? hs::TaskSystem::DefaultWorkerCount()
                                                  : options.error_worker_count) {}
    void Run()
    {
        output_.scene.roots = Build(0, objects_.size());
        for (auto &page : output_.scene.pages) page.flags &= ~rd::kSceneHlodPagePinned;
        for (auto &representation : representations_) representation.clear();
    }
private:
    std::uint32_t Dependency(const std::string &id)
    {
        auto &ids = output_.scene.dependencies;
        const auto found = std::ranges::find(ids, id);
        if (found != ids.end()) return Count(static_cast<std::size_t>(found - ids.begin()));
        ids.push_back(id); return Count(ids.size() - 1);
    }
    std::vector<std::uint32_t> Build(std::size_t begin, std::size_t end)
    {
        Require(begin < end, "empty spatial group");
        if (end - begin <= options_.objects_per_leaf)
        {
            rd::SceneHlodGroup group; group.first_member = Count(output_.scene.members.size());
            Representation original; std::vector<rd::GeometryBounds> bounds;
            for (auto index = begin; index < end; ++index)
                for (auto &member : objects_[index].members)
                {
                    output_.scene.members.push_back(member.record);
                    BakeInto(original, member);
                    bounds.push_back(member.record.bounds);
                }
            group.member_count = Count(output_.scene.members.size()) - group.first_member; group.bounds = Union(bounds);
            const auto id = Count(output_.scene.groups.size()); output_.scene.groups.push_back(group);
            representations_.push_back(std::move(original)); return {id};
        }
        std::vector<rd::GeometryBounds> object_bounds;
        for (auto i = begin; i < end; ++i) object_bounds.push_back(objects_[i].bounds);
        const auto all_bounds = Union(object_bounds);
        std::size_t axis = 0;
        for (std::size_t lane = 1; lane < 3; ++lane)
            if (all_bounds.maximum[lane] - all_bounds.minimum[lane] > all_bounds.maximum[axis] - all_bounds.minimum[axis]) axis = lane;
        std::sort(objects_.begin() + begin, objects_.begin() + end, [axis](const auto &a, const auto &b) {
            return std::pair{a.bounds.sphere[axis], a.id} < std::pair{b.bounds.sphere[axis], b.id};
        });
        const auto partitions = std::min<std::size_t>(options_.children_per_group, end - begin);
        std::vector<std::uint32_t> children;
        for (std::size_t partition = 0; partition < partitions; ++partition)
        {
            auto branch = Build(begin + (end - begin) * partition / partitions,
                                begin + (end - begin) * (partition + 1) / partitions);
            children.insert(children.end(), branch.begin(), branch.end());
        }
        Representation input;
        for (const auto child : children) if (!Append(input, representations_[child])) return children;
        Representation coarse;
        for (const auto &[key, vertices] : input) coarse[key] = Simplify(vertices, options_.reduction_ratio);
        // A retained duplicate is never published as a coarser object group.
        if (TriangleCount(coarse) >= TriangleCount(input)) return children;
        // Never publish a representation that requires an unbounded atomic
        // residency set. Children remain complete roots when this parent does
        // not fit; do this before the costly geometric-error qualification.
        if (PlanPages(coarse, options_.max_page_triangles).page_count > options_.max_group_pages)
            return children;
        rd::SceneHlodGroup group; group.first_member = output_.scene.groups[children.front()].first_member;
        const auto &last = output_.scene.groups[children.back()];
        group.member_count = last.first_member + last.member_count - group.first_member;
        double inherited_error{};
        for (const auto child : children)
            inherited_error = std::max(inherited_error, double(output_.scene.groups[child].world_error));
        struct ErrorRow
        {
            std::span<const rd::ExpandedVertex> fine;
            std::span<const rd::ExpandedVertex> coarse;
            GeometryErrorBounds measured{};
            std::exception_ptr exception;
        };
        std::vector<ErrorRow> rows;
        rows.reserve(coarse.size());
        for (const auto &[key, vertices] : coarse)
            rows.push_back({input.at(key), vertices});
        if (!rows.empty())
        {
            hs::TaskGroup tasks;
            tasks_.ParallelFor(tasks, {"scene HLOD geometric error"}, rows.size(), 1,
                [&rows, error_budget = options_.error_refinement_budget](std::size_t begin, std::size_t end) {
                    for (std::size_t index = begin; index < end; ++index)
                    {
                        auto &row = rows[index];
                        try
                        {
                            row.measured = MeasureGeometryError(
                                row.fine, row.coarse, {}, error_budget);
                        }
                        catch (...)
                        {
                            row.exception = std::current_exception();
                        }
                    }
                });
            const auto wait = tasks_.Wait(tasks);
            Require(wait.Succeeded(), "geometric error worker failure");
        }
        for (const auto &row : rows)
        {
            if (row.exception) std::rethrow_exception(row.exception);
            const auto &measured = row.measured;
            Require(std::isfinite(measured.lower) && measured.lower >= 0 && measured.upper >= measured.lower,
                    "invalid qualified world error");
            // Symmetric Hausdorff distance obeys the triangle inequality.
            // Measure the immediate child surfaces once, then include their
            // already-qualified original-to-child bound for this entire group.
            group.world_error = std::max(group.world_error, Upper(measured.upper + inherited_error));
        }
        std::vector<rd::GeometryBounds> bounds;
        for (const auto child : children)
        {
            bounds.push_back(output_.scene.groups[child].bounds);
            group.world_error = std::max(group.world_error, output_.scene.groups[child].world_error);
        }
        for (const auto &[key, vertices] : coarse) { (void)key; bounds.push_back(Bounds(vertices)); }
        group.bounds = Union(bounds);
        group.first_child = Count(output_.scene.children.size()); group.child_count = Count(children.size());
        const auto id = Count(output_.scene.groups.size());
        for (const auto child : children) output_.scene.groups[child].parent = id;
        output_.scene.children.insert(output_.scene.children.end(), children.begin(), children.end());
        output_.scene.groups.push_back(group); representations_.push_back(std::move(coarse));
        Pages(id);
        for (const auto child : children) representations_[child].clear();
        return {id};
    }
    void Pages(std::uint32_t id)
    {
        auto &group = output_.scene.groups[id]; group.first_proxy = Count(output_.scene.proxies.size());
        const auto plan = PlanPages(representations_[id], options_.max_page_triangles);
        const auto base = Count(output_.pages.size());
        output_.pages.resize(base + plan.page_count);
        output_.scene.pages.resize(base + plan.page_count);
        for (const auto &entry : plan.fragments)
        {
            const auto &key = *entry.key;
            const auto material = Dependency(key.material);
            const auto fragment = CookGeometry(entry.vertices, key.flags);
            auto &page = output_.pages[base + entry.page];
            rd::SceneHlodProxy proxy;
            proxy.page_index = base + entry.page; proxy.first_index = Count(page.indices.size());
            proxy.index_count = Count(fragment.indices.size()); proxy.first_meshlet = Count(page.meshlets.size());
            proxy.meshlet_count = Count(fragment.meshlets.size()); proxy.material_dependency = material;
            proxy.mesh_semantic = key.semantic; proxy.atlas_selector = key.selector; proxy.tint_rgba = key.tint;
            proxy.variant = key.variant; proxy.bounds = fragment.bounds;
            AppendPage(page, fragment);
            output_.scene.proxies.push_back(proxy);
        }
        for (std::uint32_t local = 0; local < plan.page_count; ++local)
        {
            auto &page = output_.pages[base + local];
            page.bounds = Bounds(page.vertices);
            auto &directory = output_.scene.pages[base + local];
            directory.logical_id = prefix_ + "/group_" + std::to_string(id) + "/page_" + std::to_string(base + local) + ".rpage";
            directory.geometry_flags = page.flags; directory.vertex_count = Count(page.vertices.size());
            directory.index_count = Count(page.indices.size()); directory.meshlet_count = Count(page.meshlets.size());
        }
        group.proxy_count = Count(output_.scene.proxies.size()) - group.first_proxy;
        // Page spheres can differ from the full partition's sphere. Include
        // their exact conservative bounds before publishing the hierarchy.
        std::vector<rd::GeometryBounds> bounds{group.bounds};
        for (std::uint32_t p = group.first_proxy; p < group.first_proxy + group.proxy_count; ++p)
            bounds.push_back(output_.scene.proxies[p].bounds);
        group.bounds = Union(bounds);
    }
    CookedSceneHlod &output_;
    std::vector<Object> objects_;
    std::string prefix_;
    SceneHlodCookOptions options_;
    hs::TaskSystem tasks_;
    std::vector<Representation> representations_;
};
} // namespace

CookedSceneHlod CookSceneHlod(AssetId scene_asset_id, std::uint32_t layout_abi,
    const std::array<std::byte, 32> &layout_digest, std::span<const SceneHlodCookMember> members,
    std::span<const SceneHlodCookSource> sources, std::string_view prefix, SceneHlodCookOptions options)
{
    Require(scene_asset_id.value && layout_abi && !members.empty() && members.size() <= 1'000'000 &&
            options.objects_per_leaf > 0 && options.objects_per_leaf <= 64 &&
            options.children_per_group >= 2 && options.children_per_group <= 16 &&
            std::isfinite(options.reduction_ratio) && options.reduction_ratio > 0 && options.reduction_ratio < 1 &&
            options.max_page_triangles > 0 && options.max_page_triangles <= rd::kMaxGeometryIndices / 3 &&
            options.max_group_pages > 0 && options.max_group_pages <= 128 && options.error_worker_count <= 8,
            "invalid explicit scene/options");
    Require(prefix.starts_with("render/hlod/") && !prefix.ends_with('/') && prefix.size() <= 448 &&
            prefix.find("..") == std::string_view::npos, "invalid page prefix");
    CookedSceneHlod output; output.scene.scene_asset_id = scene_asset_id;
    output.scene.layout_abi = layout_abi; output.scene.layout_digest = layout_digest;
    std::map<std::uint64_t, const SceneHlodCookSource *> source_assets;
    std::map<std::uint64_t, std::uint32_t> source_dependencies;
    for (const auto &source : sources)
    {
        Require(source.asset_id.value && source.geometry && !source.material_products.empty() &&
                MakeAssetId(source.logical_id).value == source.asset_id.value && source_assets.emplace(source.asset_id.value, &source).second, "invalid/duplicate source identity");
        rd::ValidateGeometryData(*source.geometry);
        Require(!(source.geometry->flags & (rd::kGeometryFlagSkinned | rd::kGeometryFlagWind)), "deformed source cannot be baked");
    }
    std::unordered_set<std::uint64_t> used_sources;
    for (const auto &member : members) used_sources.insert(member.instance.asset_id.value);
    for (const auto &[asset, source] : source_assets)
    {
        if (!used_sources.contains(asset)) continue;
        source_dependencies[asset] = Count(output.scene.dependencies.size());
        output.scene.dependencies.push_back(source->logical_id);
    }
    std::map<std::uint64_t, Object> objects;
    std::unordered_set<std::uint64_t> stable_ids;
    for (const auto &member : members)
    {
        const auto &instance = member.instance;
        Require(instance.stable_id && member.object_id && stable_ids.insert(instance.stable_id).second &&
                instance.mesh != RenderMesh::Grass && instance.mesh != RenderMesh::Ground &&
                source_assets.contains(instance.asset_id.value), "invalid eligible static member/source");
        for (const auto value : {instance.position.x, instance.position.y, instance.position.z, instance.yaw,
                                instance.scale.x, instance.scale.y, instance.scale.z})
            Require(std::isfinite(value), "nonfinite member transform");
        Require(instance.scale.x != 0 && instance.scale.y != 0 && instance.scale.z != 0, "singular member transform");
        const auto &source = *source_assets.at(instance.asset_id.value);
        const auto &geometry = *source.geometry;
        BakedMember baked; baked.source = &source; baked.instance = instance; auto &record = baked.record;
        record.stable_id = instance.stable_id; record.object_id = member.object_id; record.asset_id = instance.asset_id;
        record.geometry_dependency = source_dependencies.at(source.asset_id.value); record.mesh_semantic = static_cast<std::uint32_t>(instance.mesh);
        record.variant = instance.environment_variant; record.tint_rgba = instance.color_rgba;
        record.position = instance.position; record.scale = instance.scale; record.yaw = instance.yaw;
        record.bounds = TransformBounds(geometry.bounds, instance);
        auto &object = objects[member.object_id]; object.id = member.object_id; object.members.push_back(std::move(baked));
    }
    std::vector<Object> spatial;
    for (auto &[id, object] : objects)
    {
        (void)id;
        std::ranges::sort(object.members, {}, [](const auto &member) { return member.record.stable_id; });
        std::vector<rd::GeometryBounds> bounds;
        for (const auto &member : object.members) bounds.push_back(member.record.bounds);
        object.bounds = Union(bounds); spatial.push_back(std::move(object));
    }
    Builder(output, std::move(spatial), prefix, options).Run();
    // Pages are emitted in postorder, so update ancestor spheres after page
    // packing rather than trusting a different partition sphere center.
    for (auto &group : output.scene.groups)
    {
        if (group.child_count == 0) continue;
        std::vector<rd::GeometryBounds> bounds{group.bounds};
        for (std::uint32_t child = 0; child < group.child_count; ++child)
            bounds.push_back(output.scene.groups[output.scene.children[group.first_child + child]].bounds);
        group.bounds = Union(bounds);
    }
    auto canonical_dependencies = output.scene.dependencies;
    std::ranges::sort(canonical_dependencies);
    const auto dependency_index = [&](std::uint32_t old_index) {
        const auto &id = output.scene.dependencies.at(old_index);
        const auto found = std::ranges::lower_bound(canonical_dependencies, id);
        Require(found != canonical_dependencies.end() && *found == id, "missing canonical scene dependency");
        return Count(static_cast<std::size_t>(found - canonical_dependencies.begin()));
    };
    for (auto &member : output.scene.members)
        member.geometry_dependency = dependency_index(member.geometry_dependency);
    for (auto &proxy : output.scene.proxies)
        proxy.material_dependency = dependency_index(proxy.material_dependency);
    output.scene.dependencies = std::move(canonical_dependencies);
    rd::ValidateSceneHlodPages(output.scene, output.pages); return output;
}
} // namespace hs::geometry_content
