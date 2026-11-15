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

#define HS_RENDER_BINDING_NATIVE
#include "../../../Content/Shaders/render_binding_contract.hlsli"
#undef HS_RENDER_BINDING_NATIVE

namespace hs
{
inline constexpr std::uint32_t kLightListResourceCount = HS_LIGHT_LIST_SRV_COUNT;
struct LocalShadowLightReference;
// Shared with light_list.hlsli. Directional records precede local records; both
// partitions are sorted by stable identity, independently of submission order.
struct alignas(16) GpuLightRecord
{
    DirectX::XMFLOAT4 position_range{};
    DirectX::XMFLOAT4 direction_cos_outer{};
    DirectX::XMFLOAT4 color_intensity{}; // RGB; W/m^2 Directional, W/sr Point/Spot.
    DirectX::XMFLOAT4 shape_cone{}; // Point/Spot radius^2 and cone.
    DirectX::XMUINT4 identity{}; // Stable ID lo/hi, generation, LightType.
    DirectX::XMUINT4 metadata{}; // Revision lo/hi; Z: requested bit0, (shadow record+1)<<1; W: legacy/primary/transient bits 0/1/3.
};
static_assert(sizeof(GpuLightRecord) == 96);
static_assert(offsetof(GpuLightRecord, shape_cone) == 48);
static_assert(offsetof(GpuLightRecord, identity) == 64);
static_assert(offsetof(GpuLightRecord, metadata) == 80);

// This is both the compute CBV and the first 256 bytes of the t1 raw SRV.
// Graphics consumers need only the two SRVs, without an additional graphics CBV.
struct alignas(16) LightListConstants
{
    DirectX::XMFLOAT4X4 world_to_view{}; // Camera-relative rotation, uploaded transposed.
    DirectX::XMFLOAT4X4 projection{};   // Unjittered, uploaded transposed.
    DirectX::XMFLOAT4X4 inverse_projection{};
    DirectX::XMFLOAT4 camera_origin_near{};
    DirectX::XMFLOAT4 extent_far_log{}; // Active width/height, far meters, log2(far/near).
    DirectX::XMUINT4 grid{}; // Width/height/24 slices, Directional count.
    DirectX::XMUINT4 limits{}; // Total lights, reserved, reserved, cell count.
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
    ResourceVersion lights, grid;
    std::array<ID3D12Resource *, kLightListResourceCount> resources{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kLightListResourceCount> srvs{}; // t0..t1 / space19.
    std::uint32_t light_count{}, directional_count{};
};
struct LightListLayout
{
    bool valid{}, clustered{};
    ViewExtent capacity{};
    std::uint32_t max_lights{};
    std::array<BufferHandle, kLightListResourceCount> resources{};
    std::array<ResourceVersion, kLightListResourceCount> uploaded{};
    std::array<ResourceVersion, kLightListResourceCount> outputs{};
    std::array<std::uint32_t,2> passes{};
};

struct LightListTargets;
struct AllocationResource;
struct LightListDiagnostics
{
    std::array<const AllocationResource *, kLightListResourceCount> resources{};
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
// membership in four uint mask words.
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
        const ShaderReader &read_shader, LightListCandidate &candidate) const;
    [[nodiscard]] Result Commit(LightListCandidate &&candidate, const CompletionSet &completed);
    [[nodiscard]] Result Register(RenderGraphBuilder &graph, const LightListInputs &inputs,
        LightListOutputs &outputs, LightListLayout *layout = nullptr);
    [[nodiscard]] bool MatchesLayout(const LightListLayout &, bool clustered)const noexcept;
    [[nodiscard]] Result BindRetained(RenderGraphBuilder &,const LightListInputs &,
        const LightListLayout &,LightListOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &graph,
        const CompletionSet &completion, bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // Borrowed last successfully submitted slot; capture waits for GPU completion.
    [[nodiscard]] LightListDiagnostics DiagnosticResources() const noexcept;
    // Called only after all old submissions have reliably completed.
  private:
    [[nodiscard]] Result RegisterFrame(RenderGraphBuilder &,const LightListInputs &,LightListOutputs &,
        LightListLayout *,const LightListLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
