#pragma once

#include <hs/render_data/geometry_hierarchy.hpp>
#include <hs/renderer/renderer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace hs
{
inline constexpr std::uint32_t kClusterGroupReferencePose = 1;
inline constexpr std::uint32_t kClusterCandidateFineOnlyDeformationBound = 1;
inline constexpr std::uint32_t kClusterCutFailureCapacity = 1;
inline constexpr std::uint32_t kClusterCutFailureCatalog = 2;
inline constexpr std::uint32_t kClusterCutFailureMissingPage = 4;
inline constexpr std::uint32_t kClusterCutFailureErrorDomain = 8;
inline constexpr std::uint32_t kClusterCutFailureAncestorRange = 16;

// Shared with cluster_refinement.hlsl. Indices are global table indices after
// PrepareGpu rebases a catalog. Content generation tags are nonzero and cannot
// be reused while any old frame/resource using that tag is still alive.
struct alignas(16) GpuClusterGroup
{
    std::uint32_t parent{}, first_child{}, child_count{}, first_cluster{};
    std::uint32_t cluster_count{}, material_partition{}, flags{}, material_selector{};
    std::array<float, 4> sphere{};
    std::array<float, 4> minimum_error{};
    std::array<float, 4> maximum_reserved{};
    std::uint32_t catalog_index{}, generation{}, ancestor_steps{}, next_subtree{UINT32_MAX};
};
struct alignas(16) GpuClusterRecord
{
    std::uint32_t page_slot{}, first_index{}, index_count{}, first_meshlet{};
    std::uint32_t meshlet_count{}, material_selector{}, group_index{}, material_partition{};
};
struct alignas(16) GpuClusterPage
{
    std::uint32_t generation{}, resident{}, physical_page_slot{UINT32_MAX}, page_version{};
    std::uint32_t triangle_offset{}, remap_offset{}, draw_flags{}, reserved{};
};
struct alignas(16) GpuClusterCandidate
{
    std::uint32_t instance_index{}, instance_generation{}, catalog_index{}, view_mask{};
    std::uint32_t first_group{}, group_count{}, mesh_id{}, policy_flags{};
    float deformation_radius{}; // proven conservative world-space displacement radius
    std::uint32_t mesh_flags{}, first_root{}, reserved{}; // existing StaticDrawRef flags are preserved
};
struct alignas(16) GpuClusterCutStatus
{
    std::uint32_t required_records_low{}, required_records_high{};
    std::uint32_t required_commands_low{}, required_commands_high{};
    std::uint32_t failure_flags{}, committed_records{}, candidate_count{}, reserved{};
    std::uint32_t frame_id_low{}, frame_id_high{}, content_generation{}, catalog_index{};
};
struct alignas(16) GpuClusterRefinementConstants
{
    std::uint32_t candidate_count{}, group_count{}, cluster_count{}, child_count{};
    std::uint32_t page_count{}, output_capacity{}, geometry_draw_base{}, generation{};
    std::uint32_t prefix_stage{}, prefix_block_count{}, max_ancestor_steps{}, geometry_frontend_flags{};
    std::uint32_t cluster_ref_base{}, raster_record_base{}, material_word{}, reserved{};
    std::array<float, 4> camera_origin_near{};
    std::array<float, 4> view_right{}, view_up{}, view_forward{};
    std::array<float, 4> projection_scale{}; // output pixels: perspective camera, three orthographic cascades
    std::array<float, 4> error_tolerance{};
    std::array<std::array<std::array<float, 4>, 6>, 4> frustum_planes{};
    std::uint32_t history_instance_capacity{}, history_words_per_instance{};
    std::uint32_t previous_history_instance_capacity{}, previous_history_valid{};
    std::uint32_t frame_id_low{}, frame_id_high{}, catalog_cluster_base{}, catalog_cluster_count{};
};
static_assert(sizeof(GpuClusterGroup) == 96);
static_assert(sizeof(GpuClusterRecord) == 32);
static_assert(sizeof(GpuClusterPage) == 32);
static_assert(sizeof(GpuClusterCandidate) == 48);
static_assert(sizeof(GpuClusterCutStatus) == 48);
static_assert(sizeof(GpuClusterRefinementConstants) == 576);

struct ClusterGpuTables
{
    std::vector<GpuClusterGroup> groups;
    std::vector<std::uint32_t> children;
    std::vector<GpuClusterRecord> clusters;
    std::vector<GpuClusterPage> pages;
    std::uint32_t max_ancestor_steps{};
};
struct ClusterCatalogPage
{
    std::string logical_id;
    std::filesystem::path path;
    std::string digest;
    bool pinned{};
};
using ClusterCatalogRead = std::function<Result(const std::filesystem::path &, std::vector<std::byte> &)>;

// Owns one of the fourteen environment material catalogs. The selected
// Generation is immutable: Load reads only GHI1, while page paths/digests are
// retained for the existing nonblocking native streaming path.
class EnvironmentClusterCatalog
{
public:
    [[nodiscard]] Result Load(const RendererConfig &config,
        const render_data::PackageView &main_package, std::string_view main_product_id,
        const ClusterCatalogRead &read);
    [[nodiscard]] Result DecodePage(std::uint32_t page_index,
        std::span<const std::byte> bytes, render_data::GeometryData &output) const;
    [[nodiscard]] Result PrepareGpu(std::uint32_t catalog_ordinal, std::uint32_t generation_tag,
        std::uint32_t group_base, std::uint32_t child_base, std::uint32_t cluster_base,
        std::uint32_t page_base, std::uint32_t material_base, ClusterGpuTables &output) const;
    void Reset() noexcept;
    [[nodiscard]] const render_data::GeometryHierarchy &Hierarchy() const noexcept { return hierarchy_; }
    [[nodiscard]] std::span<const ClusterCatalogPage> Pages() const noexcept { return pages_; }
    [[nodiscard]] std::span<const std::uint32_t> PinnedPages() const noexcept { return pinned_pages_; }
    [[nodiscard]] std::string_view Generation() const noexcept { return generation_; }
    [[nodiscard]] std::string_view LogicalId() const noexcept { return logical_id_; }
    [[nodiscard]] std::uint32_t MaxAncestorSteps() const noexcept { return max_ancestor_steps_; }
    [[nodiscard]] std::uint32_t FindPage(std::string_view logical_id) const;

private:
    render_data::GeometryHierarchy hierarchy_;
    std::vector<ClusterCatalogPage> pages_;
    std::vector<std::uint32_t> pinned_pages_;
    std::unordered_map<std::string, std::uint32_t> page_indices_;
    std::string generation_, logical_id_, source_id_;
    std::uint32_t max_ancestor_steps_{};
};

// Dispatch contract: clear status/demand/count buffers, CountCuts, UAV barrier,
// PrefixCuts stages 0/1/2 with UAV barriers, then EmitCuts and transition records,
// refs, both argument arrays, and four command counts for indirect consumption.
// For N elements, dispatch X=min(ceil(N/64),65535), Y=ceil(ceil(N/64)/65535),
// Z=1. Count/Emit N=candidate_count; prefix0/2 N=max(candidate_count,
// output_capacity); prefix1 dispatches one group. The shader flattens the2D
// dispatch and handles padding without wrapping the32-bit candidate address.
// Counts/offsets are uint2 per candidate; block sums/offsets are uint2 per64.
// Emit writes matching existing64B GeometryDrawRecord and16B StaticDrawRef,
// indexed24B and mesh16B commands for each of four views. GeometryDrawBase must
// reserve record0 and fit the existing30-bit packed pass field. Any status fault
// zeros all four command counts; failed stage2 also clears their command arrays.
// Tables, candidates and residency are immutable from Count through Emit.
} // namespace hs
