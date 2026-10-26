#include <hs/render_data/scene_hlod.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/core/sha256.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace hs::render_data
{
namespace
{
constexpr std::uint32_t kMagic = 0x314c4853; // SHL1
constexpr std::size_t kMaxRecords = 1'000'000;
constexpr std::uint64_t kMaxBytes = 512ull * 1024 * 1024;
void Require(bool value, std::string_view reason)
{
    if (!value) throw std::runtime_error("scene HLOD: " + std::string(reason));
}
bool Range(std::uint32_t first, std::uint32_t count, std::size_t size)
{
    return first <= size && count <= size - first;
}
std::uint32_t Count(std::size_t size)
{
    Require(size <= std::numeric_limits<std::uint32_t>::max(), "portable count overflow");
    return static_cast<std::uint32_t>(size);
}
void Id(std::string_view id)
{
    Require(id.size() > 7 && id.size() <= 512 && id.starts_with("render/") &&
            id.find("..") == std::string_view::npos && !id.ends_with('/'), "invalid logical product ID");
    bool separator = true;
    for (const auto character : id)
    {
        if (character == '/') { Require(!separator, "empty ID component"); separator = true; }
        else
        {
            Require((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9') || character == '_' || character == '-' ||
                    character == '.', "invalid ID character");
            separator = false;
        }
    }
}
void Bounds(const GeometryBounds &b)
{
    for (std::size_t lane = 0; lane < 3; ++lane)
        Require(std::isfinite(b.minimum[lane]) && std::isfinite(b.maximum[lane]) &&
                b.minimum[lane] <= b.maximum[lane], "invalid world bounds");
    for (const auto value : b.sphere) Require(std::isfinite(value), "nonfinite world sphere");
    Require(b.sphere[3] >= 0, "negative world radius");
}
void Encloses(const GeometryBounds &outer, const GeometryBounds &inner)
{
    double distance2 = 0;
    for (std::size_t lane = 0; lane < 3; ++lane)
    {
        Require(outer.minimum[lane] <= inner.minimum[lane] &&
                outer.maximum[lane] >= inner.maximum[lane], "world bounds omit child/member/proxy");
        const double delta = double(outer.sphere[lane]) - inner.sphere[lane];
        distance2 += delta * delta;
    }
    Require(std::sqrt(distance2) + double(inner.sphere[3]) <= double(outer.sphere[3]),
            "world sphere omits child/member/proxy");
}
void VertexIn(const GeometryBounds &bounds, const ExpandedVertex &vertex)
{
    double distance2 = 0;
    for (std::size_t lane = 0; lane < 3; ++lane)
    {
        Require(vertex.position[lane] >= bounds.minimum[lane] &&
                vertex.position[lane] <= bounds.maximum[lane], "proxy bounds omit vertex");
        const double delta = double(vertex.position[lane]) - bounds.sphere[lane];
        distance2 += delta * delta;
    }
    Require(std::sqrt(distance2) <= double(bounds.sphere[3]), "proxy sphere omits vertex");
}
void Put(std::span<std::byte> bytes, std::size_t offset, std::uint32_t word)
{
    Require(offset <= bytes.size() && 4 <= bytes.size() - offset, "write range");
    for (std::size_t lane = 0; lane < 4; ++lane) bytes[offset + lane] = std::byte((word >> (lane * 8)) & 255u);
}
std::uint32_t Get(std::span<const std::byte> bytes, std::size_t offset)
{
    Require(offset <= bytes.size() && 4 <= bytes.size() - offset, "read range");
    std::uint32_t word = 0;
    for (std::size_t lane = 0; lane < 4; ++lane)
        word |= std::to_integer<std::uint32_t>(bytes[offset + lane]) << (lane * 8);
    return word;
}
void Put64(std::span<std::byte> b, std::size_t o, std::uint64_t v)
{
    Put(b, o, static_cast<std::uint32_t>(v)); Put(b, o + 4, static_cast<std::uint32_t>(v >> 32));
}
std::uint64_t Get64(std::span<const std::byte> b, std::size_t o)
{
    return Get(b, o) | (std::uint64_t(Get(b, o + 4)) << 32);
}
void Float(std::span<std::byte> b, std::size_t o, float v) { Put(b, o, std::bit_cast<std::uint32_t>(v)); }
float Float(std::span<const std::byte> b, std::size_t o) { return std::bit_cast<float>(Get(b, o)); }
void WriteBounds(std::span<std::byte> b, std::size_t o, const GeometryBounds &v)
{
    for (std::size_t i = 0; i < 3; ++i) { Float(b, o + i * 4, v.minimum[i]); Float(b, o + 12 + i * 4, v.maximum[i]); }
    for (std::size_t i = 0; i < 4; ++i) Float(b, o + 24 + i * 4, v.sphere[i]);
}
GeometryBounds ReadBounds(std::span<const std::byte> b, std::size_t o)
{
    GeometryBounds v;
    for (std::size_t i = 0; i < 3; ++i) { v.minimum[i] = Float(b, o + i * 4); v.maximum[i] = Float(b, o + 12 + i * 4); }
    for (std::size_t i = 0; i < 4; ++i) v.sphere[i] = Float(b, o + 24 + i * 4);
    return v;
}
std::uint64_t Size(std::span<const std::uint32_t> counts)
{
    return kSceneHlodHeaderStride + std::uint64_t(counts[0]) * kSceneHlodMemberStride +
        std::uint64_t(counts[1]) * kSceneHlodGroupStride + std::uint64_t(counts[2]) * kSceneHlodProxyStride +
        std::uint64_t(counts[3]) * kSceneHlodPageStride + (std::uint64_t(counts[4]) + counts[5]) * 4 +
        std::uint64_t(counts[6]) * 8 + counts[7];
}
void HashWord(Sha256Stream &digest, std::uint32_t value)
{
    std::array<std::byte, 4> bytes; Put(bytes, 0, value); digest.Add(bytes);
}
} // namespace

std::array<std::byte, 32> ComputeSceneHlodLayoutDigest(
    std::uint32_t layout_abi, std::span<const SceneHlodMember> members)
{
    std::vector<const SceneHlodMember *> canonical;
    canonical.reserve(members.size());
    for (const auto &member : members) canonical.push_back(&member);
    std::ranges::sort(canonical, {}, [](const auto *member) { return member->stable_id; });
    Sha256Stream digest;
    HashWord(digest, layout_abi); HashWord(digest, Count(members.size()));
    for (const auto *member : canonical)
    {
        for (const auto id : {member->stable_id, member->object_id})
        { HashWord(digest, static_cast<std::uint32_t>(id)); HashWord(digest, static_cast<std::uint32_t>(id >> 32)); }
        HashWord(digest, member->mesh_semantic); HashWord(digest, member->variant); HashWord(digest, member->tint_rgba);
        for (const auto value : {member->position.x, member->position.y, member->position.z, member->yaw,
                                member->scale.x, member->scale.y, member->scale.z})
            HashWord(digest, std::bit_cast<std::uint32_t>(value == 0.0f ? 0.0f : value));
    }
    return digest.Finish();
}

void ValidateSceneHlod(const SceneHlod &s)
{
    Require(s.abi == kSceneHlodAbi && s.layout_abi != 0 && s.error_method_abi == kGeometryErrorMethodAbi &&
            s.scene_asset_id.value != 0, "ABI/scene identity");
    for (const auto size : {s.members.size(), s.groups.size(), s.proxies.size(), s.pages.size(),
                           s.roots.size(), s.children.size(), s.dependencies.size()})
        Require(size <= kMaxRecords, "allocation cap");
    std::unordered_set<std::string> dependencies;
    for (const auto &id : s.dependencies) { Id(id); Require(dependencies.insert(id).second, "duplicate dependency"); }
    std::unordered_set<std::uint64_t> stable_ids;
    for (const auto &m : s.members)
    {
        Require(m.stable_id && m.object_id && m.asset_id.value && stable_ids.insert(m.stable_id).second &&
                m.geometry_dependency < s.dependencies.size(), "member identity/dependency");
        Require(MakeAssetId(s.dependencies[m.geometry_dependency]).value == m.asset_id.value,
                "member asset disagrees with source dependency");
        for (const auto value : {m.position.x, m.position.y, m.position.z, m.yaw, m.scale.x, m.scale.y, m.scale.z})
            Require(std::isfinite(value), "nonfinite member transform");
        Require(m.scale.x != 0 && m.scale.y != 0 && m.scale.z != 0, "singular member transform");
        Bounds(m.bounds);
    }
    Require(ComputeSceneHlodLayoutDigest(s.layout_abi, s.members) == s.layout_digest, "layout digest mismatch");
    std::unordered_set<std::string> page_ids;
    for (const auto &p : s.pages)
    {
        Id(p.logical_id);
        Require(p.logical_id.starts_with("render/hlod/") && p.logical_id.ends_with(".rpage") &&
                page_ids.insert(p.logical_id).second && !(p.flags & ~kSceneHlodPagePinned) &&
                !(p.geometry_flags & ~kGeometryFlagTwoSided) && p.vertex_count > 0 &&
                p.vertex_count <= kMaxGeometryVertices && p.index_count > 0 &&
                p.index_count <= kMaxGeometryIndices && p.index_count % 3 == 0 && p.meshlet_count > 0 &&
                p.meshlet_count <= p.index_count / 3, "page directory");
    }
    for (const auto &p : s.proxies)
    {
        Require(p.page_index < s.pages.size() && p.material_dependency < s.dependencies.size() &&
                p.index_count > 0 && p.first_index % 3 == 0 && p.index_count % 3 == 0 &&
                Range(p.first_index, p.index_count, s.pages[p.page_index].index_count) && p.meshlet_count > 0 &&
                Range(p.first_meshlet, p.meshlet_count, s.pages[p.page_index].meshlet_count) &&
                p.atlas_selector <= 65535, "proxy ranges/material");
        Bounds(p.bounds);
    }
    std::vector<std::uint32_t> parents(s.groups.size()), proxy_owners(s.proxies.size()), member_owners(s.members.size());
    std::vector<bool> child_slots(s.children.size());
    std::map<std::uint64_t, std::uint32_t> object_leaf;
    for (std::size_t id = 0; id < s.groups.size(); ++id)
    {
        const auto &g = s.groups[id];
        Require((g.parent == kNoSceneHlodGroup || (g.parent < s.groups.size() && g.parent != id)) &&
                g.member_count > 0 && Range(g.first_member, g.member_count, s.members.size()) &&
                Range(g.first_child, g.child_count, s.children.size()) &&
                Range(g.first_proxy, g.proxy_count, s.proxies.size()) &&
                std::isfinite(g.world_error) && g.world_error >= 0, "group ranges/error");
        Bounds(g.bounds);
        for (std::uint32_t p = g.first_proxy; p < g.first_proxy + g.proxy_count; ++p)
        {
            Require(++proxy_owners[p] == 1, "overlapping proxy ownership"); Encloses(g.bounds, s.proxies[p].bounds);
            // Scene aggregates are optional replacements. Exact original mesh
            // roots retain coverage while any aggregate page is absent.
        }
        if (g.child_count == 0)
        {
            Require(g.proxy_count == 0 && g.world_error == 0, "leaves retain exact original objects");
            for (std::uint32_t m = g.first_member; m < g.first_member + g.member_count; ++m)
            {
                Require(++member_owners[m] == 1, "overlapping leaf membership"); Encloses(g.bounds, s.members[m].bounds);
                const auto [where, inserted] = object_leaf.emplace(s.members[m].object_id, Count(id));
                Require(inserted || where->second == id, "object split across leaves");
            }
        }
        else
        {
            Require(g.child_count >= 2 && g.proxy_count > 0, "parent lacks complete proxy replacement");
            std::uint32_t next = g.first_member;
            for (std::uint32_t c = g.first_child; c < g.first_child + g.child_count; ++c)
            {
                Require(!child_slots[c], "overlapping child directory"); child_slots[c] = true;
                const auto child = s.children[c]; Require(child < s.groups.size(), "child ID range");
                const auto &cg = s.groups[child];
                Require(cg.parent == id && ++parents[child] == 1 && cg.first_member == next &&
                        cg.member_count <= g.first_member + g.member_count - next && cg.world_error <= g.world_error,
                        "child partition/parent/error mismatch");
                Encloses(g.bounds, cg.bounds); next += cg.member_count;
            }
            Require(next == g.first_member + g.member_count, "incomplete parent replacement");
        }
    }
    std::vector<bool> visited(s.groups.size());
    std::uint32_t next_member = 0;
    for (const auto root : s.roots)
    {
        Require(root < s.groups.size() && s.groups[root].parent == kNoSceneHlodGroup &&
                s.groups[root].first_member == next_member, "root interval/order");
        next_member += s.groups[root].member_count;
        std::vector<std::uint32_t> pending{root};
        while (!pending.empty())
        {
            const auto id = pending.back(); pending.pop_back();
            Require(!visited[id], "cycle or duplicate hierarchy path"); visited[id] = true;
            const auto &g = s.groups[id];
            for (std::uint32_t c = 0; c < g.child_count; ++c) pending.push_back(s.children[g.first_child + c]);
        }
    }
    Require(next_member == s.members.size() && std::ranges::all_of(visited, [](bool v) { return v; }) &&
            std::ranges::all_of(child_slots, [](bool v) { return v; }) &&
            std::ranges::all_of(member_owners, [](auto v) { return v == 1; }) &&
            std::ranges::all_of(proxy_owners, [](auto v) { return v == 1; }), "unreachable/unowned components");
    for (std::size_t page = 0; page < s.pages.size(); ++page)
    {
        std::vector<const SceneHlodProxy *> ranges;
        for (const auto &p : s.proxies) if (p.page_index == page) ranges.push_back(&p);
        std::ranges::sort(ranges, {}, [](auto *p) { return p->first_index; });
        std::uint32_t index = 0, meshlet = 0;
        for (const auto *p : ranges)
        { Require(p->first_index == index && p->first_meshlet == meshlet, "page range gap/overlap"); index += p->index_count; meshlet += p->meshlet_count; }
        Require(index == s.pages[page].index_count && meshlet == s.pages[page].meshlet_count, "incomplete page coverage");
    }
}

namespace
{
void ValidatePageDirectory(const SceneHlodPage &directory, const GeometryData &page)
{
    Require(page.flags == directory.geometry_flags && page.vertices.size() == directory.vertex_count &&
            page.indices.size() == directory.index_count &&
            page.meshlets.size() == directory.meshlet_count, "page directory mismatch");
}
void ValidateProxyPagePayload(const SceneHlodProxy &p, const GeometryData &page)
{
    Require(Range(p.first_index, p.index_count, page.indices.size()) &&
            Range(p.first_meshlet, p.meshlet_count, page.meshlets.size()), "payload range mismatch");
    for (std::uint32_t i = p.first_index; i < p.first_index + p.index_count; ++i)
    {
        const auto &vertex = page.vertices[page.indices[i]];
        Require(vertex.material_index == p.atlas_selector, "proxy atlas mismatch"); VertexIn(p.bounds, vertex);
    }
    for (std::uint32_t m = p.first_meshlet; m < p.first_meshlet + p.meshlet_count; ++m)
    {
        const auto &meshlet = page.meshlets[m];
        Require(page.submeshes[meshlet.submesh_index].material_index == p.atlas_selector, "meshlet atlas mismatch");
        for (std::uint32_t t = meshlet.first_triangle; t < meshlet.first_triangle + meshlet.triangle_count; ++t)
        {
            const auto index = page.canonical_primitive_remap[t] * 3;
            Require(index >= p.first_index && index < p.first_index + p.index_count, "proxy meshlet parity mismatch");
        }
    }
}
} // namespace

void ValidateSceneHlodPage(const SceneHlod &s, std::uint32_t id, const GeometryData &page)
{
    Require(id < s.pages.size(), "page ID range"); ValidateGeometryData(page);
    ValidatePageDirectory(s.pages[id], page);
    for (const auto &proxy : s.proxies)
        if (proxy.page_index == id) ValidateProxyPagePayload(proxy, page);
}
void ValidateSceneHlodPages(const SceneHlod &s, std::span<const GeometryData> pages)
{
    ValidateSceneHlod(s); Require(pages.size() == s.pages.size(), "page component count");
    for (std::size_t page = 0; page < pages.size(); ++page) ValidateSceneHlodPage(s, Count(page), pages[page]);
}

AdmittedSceneHlod::AdmittedSceneHlod(std::span<const std::byte> bytes)
    : scene_(DecodeSceneHlod(bytes)), page_proxies_(scene_.pages.size())
{
    for (std::size_t index = 0; index < scene_.proxies.size(); ++index)
        page_proxies_[scene_.proxies[index].page_index].push_back(Count(index));
}

void AdmittedSceneHlod::ValidatePage(std::uint32_t page_index,
    const PreparedGeometryPage &page) const
{
    Require(page_index < scene_.pages.size(), "page ID range");
    ValidatePageDirectory(scene_.pages[page_index], page.geometry);
    for (const auto proxy_index : page_proxies_[page_index])
        ValidateProxyPagePayload(scene_.proxies[proxy_index], page.geometry);
}

std::vector<std::byte> EncodeSceneHlod(const SceneHlod &s)
{
    ValidateSceneHlod(s);
    std::string strings;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> dependency_text, page_text;
    const auto append = [&](std::string_view id, auto &directory) {
        directory.emplace_back(Count(strings.size()), Count(id.size())); strings += id;
    };
    for (const auto &id : s.dependencies) append(id, dependency_text);
    for (const auto &p : s.pages) append(p.logical_id, page_text);
    const std::array counts{Count(s.members.size()), Count(s.groups.size()), Count(s.proxies.size()),
        Count(s.pages.size()), Count(s.roots.size()), Count(s.children.size()), Count(s.dependencies.size()), Count(strings.size())};
    const auto size = Size(counts); Require(size <= kMaxBytes, "encoded allocation cap");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size)); std::span<std::byte> b = bytes;
    Put(b, 0, kMagic); Put(b, 4, s.abi); Put(b, 8, s.layout_abi); Put(b, 12, s.error_method_abi);
    Put64(b, 16, s.scene_asset_id.value); std::ranges::copy(s.layout_digest, b.begin() + 24);
    for (std::size_t i = 0; i < counts.size(); ++i) Put(b, 56 + i * 4, counts[i]);
    Put(b, 88, kSceneHlodMemberStride); Put(b, 92, kSceneHlodGroupStride);
    Put(b, 96, kSceneHlodProxyStride); Put(b, 100, kSceneHlodPageStride); Put64(b, 104, size);
    std::size_t o = kSceneHlodHeaderStride;
    for (const auto &m : s.members)
    {
        Put64(b, o, m.stable_id); Put64(b, o + 8, m.object_id); Put64(b, o + 16, m.asset_id.value);
        Put(b, o + 24, m.geometry_dependency); Put(b, o + 28, m.mesh_semantic); Put(b, o + 32, m.variant); Put(b, o + 36, m.tint_rgba);
        Float(b, o + 40, m.position.x); Float(b, o + 44, m.position.y); Float(b, o + 48, m.position.z);
        Float(b, o + 52, m.scale.x); Float(b, o + 56, m.scale.y); Float(b, o + 60, m.scale.z); Float(b, o + 64, m.yaw);
        WriteBounds(b, o + 68, m.bounds); o += kSceneHlodMemberStride;
    }
    for (const auto &g : s.groups)
    {
        const std::array words{g.parent, g.first_child, g.child_count, g.first_member, g.member_count, g.first_proxy, g.proxy_count};
        for (std::size_t i = 0; i < words.size(); ++i) Put(b, o + i * 4, words[i]);
        WriteBounds(b, o + 28, g.bounds); Float(b, o + 68, g.world_error); o += kSceneHlodGroupStride;
    }
    for (const auto &p : s.proxies)
    {
        const std::array words{p.page_index, p.first_index, p.index_count, p.first_meshlet, p.meshlet_count,
            p.material_dependency, p.mesh_semantic, p.atlas_selector, p.tint_rgba, p.variant};
        for (std::size_t i = 0; i < words.size(); ++i) Put(b, o + i * 4, words[i]);
        WriteBounds(b, o + 40, p.bounds); o += kSceneHlodProxyStride;
    }
    for (std::size_t i = 0; i < s.pages.size(); ++i)
    {
        const auto &p = s.pages[i];
        const std::array words{page_text[i].first, page_text[i].second, p.flags, p.geometry_flags, p.vertex_count, p.index_count, p.meshlet_count};
        for (std::size_t j = 0; j < words.size(); ++j) Put(b, o + j * 4, words[j]);
        o += kSceneHlodPageStride;
    }
    for (const auto *ids : {&s.roots, &s.children}) for (const auto id : *ids) { Put(b, o, id); o += 4; }
    for (const auto [first, length] : dependency_text) { Put(b, o, first); Put(b, o + 4, length); o += 8; }
    for (const auto character : strings) b[o++] = std::byte(static_cast<unsigned char>(character));
    Require(o == bytes.size(), "encoded size disagreement"); return bytes;
}

SceneHlod DecodeSceneHlod(std::span<const std::byte> b)
{
    Require(b.size() >= kSceneHlodHeaderStride && b.size() <= kMaxBytes && Get(b, 0) == kMagic, "header/magic");
    Require(Get(b, 88) == kSceneHlodMemberStride && Get(b, 92) == kSceneHlodGroupStride &&
            Get(b, 96) == kSceneHlodProxyStride && Get(b, 100) == kSceneHlodPageStride, "record stride");
    for (std::size_t o = 112; o < 128; o += 4) Require(Get(b, o) == 0, "reserved header");
    std::array<std::uint32_t, 8> counts;
    for (std::size_t i = 0; i < counts.size(); ++i)
    { counts[i] = Get(b, 56 + i * 4); Require(i == 7 || counts[i] <= kMaxRecords, "decoded allocation cap"); }
    Require(Size(counts) == b.size() && Get64(b, 104) == b.size(), "checked component size");
    const auto strings = b.last(counts[7]);
    const auto read_id = [&](std::uint32_t first, std::uint32_t length) {
        Require(Range(first, length, strings.size()) && length <= 512, "string range");
        return std::string(reinterpret_cast<const char *>(strings.data() + first), length);
    };
    SceneHlod s; s.abi = Get(b, 4); s.layout_abi = Get(b, 8); s.error_method_abi = Get(b, 12);
    s.scene_asset_id = {Get64(b, 16)}; std::ranges::copy(b.subspan(24, 32), s.layout_digest.begin());
    std::size_t o = kSceneHlodHeaderStride;
    s.members.reserve(counts[0]);
    for (std::uint32_t i = 0; i < counts[0]; ++i)
    {
        SceneHlodMember m; m.stable_id = Get64(b, o); m.object_id = Get64(b, o + 8); m.asset_id = {Get64(b, o + 16)};
        m.geometry_dependency = Get(b, o + 24); m.mesh_semantic = Get(b, o + 28); m.variant = Get(b, o + 32); m.tint_rgba = Get(b, o + 36);
        m.position = {Float(b, o + 40), Float(b, o + 44), Float(b, o + 48)};
        m.scale = {Float(b, o + 52), Float(b, o + 56), Float(b, o + 60)}; m.yaw = Float(b, o + 64);
        m.bounds = ReadBounds(b, o + 68); Require(Get(b, o + 108) == 0, "reserved member");
        s.members.push_back(m); o += kSceneHlodMemberStride;
    }
    s.groups.reserve(counts[1]);
    for (std::uint32_t i = 0; i < counts[1]; ++i)
    {
        SceneHlodGroup g; g.parent = Get(b, o); g.first_child = Get(b, o + 4); g.child_count = Get(b, o + 8);
        g.first_member = Get(b, o + 12); g.member_count = Get(b, o + 16); g.first_proxy = Get(b, o + 20); g.proxy_count = Get(b, o + 24);
        g.bounds = ReadBounds(b, o + 28); g.world_error = Float(b, o + 68);
        Require(Get(b, o + 72) == 0 && Get(b, o + 76) == 0, "reserved group"); s.groups.push_back(g); o += kSceneHlodGroupStride;
    }
    s.proxies.reserve(counts[2]);
    for (std::uint32_t i = 0; i < counts[2]; ++i)
    {
        SceneHlodProxy p; p.page_index = Get(b, o); p.first_index = Get(b, o + 4); p.index_count = Get(b, o + 8);
        p.first_meshlet = Get(b, o + 12); p.meshlet_count = Get(b, o + 16); p.material_dependency = Get(b, o + 20);
        p.mesh_semantic = Get(b, o + 24); p.atlas_selector = Get(b, o + 28); p.tint_rgba = Get(b, o + 32); p.variant = Get(b, o + 36);
        p.bounds = ReadBounds(b, o + 40); s.proxies.push_back(p); o += kSceneHlodProxyStride;
    }
    s.pages.reserve(counts[3]);
    for (std::uint32_t i = 0; i < counts[3]; ++i)
    {
        SceneHlodPage p; p.logical_id = read_id(Get(b, o), Get(b, o + 4)); p.flags = Get(b, o + 8);
        p.geometry_flags = Get(b, o + 12); p.vertex_count = Get(b, o + 16); p.index_count = Get(b, o + 20); p.meshlet_count = Get(b, o + 24);
        Require(Get(b, o + 28) == 0, "reserved page"); s.pages.push_back(std::move(p)); o += kSceneHlodPageStride;
    }
    s.roots.reserve(counts[4]); s.children.reserve(counts[5]);
    for (auto [ids, count] : {std::pair{&s.roots, counts[4]}, std::pair{&s.children, counts[5]}})
        for (std::uint32_t i = 0; i < count; ++i) { ids->push_back(Get(b, o)); o += 4; }
    s.dependencies.reserve(counts[6]);
    for (std::uint32_t i = 0; i < counts[6]; ++i)
    { s.dependencies.push_back(read_id(Get(b, o), Get(b, o + 4))); o += 8; }
    Require(o + counts[7] == b.size(), "decoded size disagreement"); ValidateSceneHlod(s); return s;
}
} // namespace hs::render_data
