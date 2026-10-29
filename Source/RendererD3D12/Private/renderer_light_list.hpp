#pragma once

#include "render_graph.hpp"
#include "view_state.hpp"
#include <hs/render_data/render_snapshot_values.hpp>
#include <hs/core/graphics_settings.hpp>
#include <DirectXMath.h>
#include <d3d12.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace hs
{
struct LocalShadowLightReference;
// Shared with light_list.hlsli. Directional records precede local records; both
// partitions are sorted by stable identity, independently of submission order.
struct alignas(16) GpuLightRecord
{
    DirectX::XMFLOAT4 position_range{};
    DirectX::XMFLOAT4 direction_cos_outer{};
    DirectX::XMFLOAT4 color_intensity{}; // RGB; W/m^2 Directional, W/sr Point/Spot, W/(m^2 sr) area.
    DirectX::XMFLOAT4 shape_cone{}; // Point/Spot radius^2, cone; area: area m^2, bound radius m,0,0.
    DirectX::XMUINT4 identity{}; // Stable ID lo/hi, generation, LightType.
    DirectX::XMUINT4 metadata{}; // Revision lo/hi; Z: requested bit0, (shadow record+1)<<1; W: legacy/primary/two-sided/transient bits0..3.
    DirectX::XMFLOAT4 area_tangent_half_width{}; // Unit tangent; Rectangle half width or Disk radius-X.
    DirectX::XMFLOAT4 area_bitangent_half_height{}; // Unit cross(direction,tangent); half height or radius-Y.
};
static_assert(sizeof(GpuLightRecord) == 128);
static_assert(offsetof(GpuLightRecord, shape_cone) == 48);
static_assert(offsetof(GpuLightRecord, identity) == 64);
static_assert(offsetof(GpuLightRecord, metadata) == 80);
static_assert(offsetof(GpuLightRecord, area_tangent_half_width) == 96);
static_assert(offsetof(GpuLightRecord, area_bitangent_half_height) == 112);

// This is both the compute CBV and the first 256 bytes of the t1 raw SRV.
// Graphics consumers need only the four SRVs, without an additional graphics CBV.
struct alignas(16) LightListConstants
{
    DirectX::XMFLOAT4X4 world_to_view{}; // Camera-relative rotation, uploaded transposed.
    DirectX::XMFLOAT4X4 projection{};   // Unjittered, uploaded transposed.
    DirectX::XMFLOAT4X4 inverse_projection{};
    DirectX::XMFLOAT4 camera_origin_near{};
    DirectX::XMFLOAT4 extent_far_log{}; // Active width/height, far meters, log2(far/near).
    DirectX::XMUINT4 grid{}; // Width/height/24 slices, Directional count.
    DirectX::XMUINT4 limits{}; // Total lights, inline capacity, per-cell spill capacity, cell count.
};
static_assert(sizeof(LightListConstants) == 256);
static_assert(offsetof(LightListConstants, camera_origin_near) == 192);
static_assert(offsetof(LightListConstants, extent_far_log) == 208);
static_assert(offsetof(LightListConstants, grid) == 224);
static_assert(offsetof(LightListConstants, limits) == 240);

struct LightListInputs
{
    std::span<const LightView> lights;
    std::span<const LocalShadowLightReference> local_shadow_records; // Same graph's freshly prepared shadow slots.
    ViewExtent render_active;
    std::uint32_t frame_slot{};
    CompletionSet completed;
    // DirectX row-vector matrices before transpose. View translation is replaced
    // by explicit camera_origin to keep absolute world light positions stable.
    DirectX::XMFLOAT4X4 unjittered_view{}, unjittered_projection{};
    DirectX::XMFLOAT3 camera_origin{};
    float near_plane{}, far_plane{}; // Physical positive depth bounds, not reversed-Z arguments.
    // Exact identities of transient records in lights. Capture excludes these
    // records after sorting; regular scene consumers retain all authored lights.
    std::span<const std::uint64_t> transient_light_ids;
};
struct LightListOutputs
{
    ResourceVersion lights, ranges, inline_indices, spill_indices;
    std::array<ID3D12Resource *, 4> resources{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> srvs{}; // t0..t3 / space19, in this order.
    std::uint32_t light_count{}, directional_count{};
};
struct LightListLayout
{
    bool valid{};
    ViewExtent capacity{};
    std::uint32_t max_lights{},inline_capacity{},spill_capacity{};
    std::array<BufferHandle,4> resources{};
    std::array<ResourceVersion,2> uploaded{};
    std::array<ResourceVersion,4> outputs{};
    std::array<std::uint32_t,2> passes{};
};

struct LightListTargets;
struct AllocationResource;
struct LightListDiagnostics
{
    std::array<const AllocationResource *, 4> resources{};
    ViewExtent capacity{};
    std::uint32_t frame_slot_count{}, frame_slot{};
    std::uint64_t allocated_bytes{}, upload_allocated_bytes{};
};
class LightListCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<LightListTargets> targets_;
    friend class LightListSystem;
};

// At most128 authoritative lights. Each finite cell stores complete ordered
// membership in four uint mask words; inline/spill counts are diagnostics only.
// Invalid cells and off-frustum consumers retain the exact world query.
class LightListSystem
{
  public:
    using ShaderReader = std::function<Result(std::string_view, std::vector<std::byte> &)>;
    LightListSystem();
    ~LightListSystem();
    LightListSystem(const LightListSystem &) = delete;
    LightListSystem &operator=(const LightListSystem &) = delete;
    [[nodiscard]] Result Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator,
        ViewExtent render_capacity, std::uint32_t max_lights, std::uint32_t frame_slot_count,
        const ShaderReader &read_shader, LightListCandidate &candidate,
        std::uint32_t cluster_capacity) const;
    [[nodiscard]] Result Commit(LightListCandidate &&candidate, const CompletionSet &completed);
    [[nodiscard]] Result Register(RenderGraphBuilder &graph, const LightListInputs &inputs,
        LightListOutputs &outputs, LightListLayout *layout = nullptr);
    [[nodiscard]] bool MatchesLayout(const LightListLayout &)const noexcept;
    [[nodiscard]] Result BindRetained(RenderGraphBuilder &,const LightListInputs &,
        const LightListLayout &,LightListOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &graph,
        const CompletionSet &completion, bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // Borrowed last successfully submitted slot; capture waits for GPU completion.
    [[nodiscard]] LightListDiagnostics DiagnosticResources() const noexcept;
    // Called only after all old submissions have reliably completed.
    void SwapState(LightListSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    [[nodiscard]] Result RegisterFrame(RenderGraphBuilder &,const LightListInputs &,LightListOutputs &,
        LightListLayout *,const LightListLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
