#pragma once

#include "renderer_virtual_shadow.hpp"

#include <string>

namespace hs
{
inline constexpr std::uint32_t kLocalShadowPassBit = 0x40000000u;
inline constexpr std::uint32_t kLocalShadowDescriptorCount = 5;
inline constexpr std::uint32_t kLocalShadowGuard = 2;
inline constexpr std::uint32_t kLocalShadowMaximumSamples = 4;
inline constexpr std::uint32_t kLocalShadowMaximumFaces = 6 * kLocalShadowMaximumSamples;

struct LocalShadowProfile
{
    bool enabled{};
    std::uint64_t byte_budget{}; // Both depth banks, all frame buffers and native descriptor heaps.
    std::uint32_t maximum_lights{}, face_resolution{}, max_casters{}, max_draw_groups{};
    std::uint32_t filter_samples{};
    std::uint32_t command_capacity{}, frame_slots{3};
    std::uint32_t area_samples{1}; // Actual authored-plane samples: 1, 2 or 4. The physical tile pool stays maximum_lights*6.
    float normal_bias_m{0.002f}, depth_bias{0.0001f};
};
struct alignas(16) LocalShadowMetadata
{
    DirectX::XMUINT4 counts{}; // Fixed record count, selected count, tile count, enabled.
    DirectX::XMFLOAT4 bank{}; // Width, height, columns, tile extent including guard.
    DirectX::XMFLOAT4 options{}; // Inner resolution, guard, depth bias, normal bias metres.
    DirectX::XMUINT4 frame{}; // Render ID lo/hi, overflow light count, unsupported light count.
};
struct alignas(16) GpuLocalShadowRecord
{
    DirectX::XMUINT4 identity{}; // Full stable ID lo/hi, generation, LightType.
    DirectX::XMUINT4 revision{}; // Full light revision lo/hi, sample count, face count.
    DirectX::XMFLOAT4 position_range{}, direction_cos_outer{}, parameters{};
    // Absolute-world, untransposed DirectX row-vector matrices. HLSL uses row_major.
    std::array<DirectX::XMFLOAT4X4,kLocalShadowMaximumFaces> world_to_clip{};
    std::array<DirectX::XMFLOAT4,kLocalShadowMaximumFaces> atlas_tiles{}; // Actual, possibly noncontiguous tile origin XY, inner resolution, guard.
    std::array<DirectX::XMFLOAT4,kLocalShadowMaximumSamples> sample_positions{}; // Absolute authored-plane point, W reserved.
    DirectX::XMFLOAT4 emitter_normal_two_sided{}; // Unit emission normal; W is the authored two-sided flag.
    DirectX::XMUINT4 state{}; // Published face mask, selected, reserved, reserved.
};
static_assert(sizeof(LocalShadowMetadata)==64);
// Above the native 2048B structured-element limit: t1 is a raw SRV, with exact
// byte offsets decoded by local_shadow.hlsli, never a structured stride2096.
static_assert(sizeof(GpuLocalShadowRecord)==2096);
static_assert(offsetof(GpuLocalShadowRecord,world_to_clip)==80);
static_assert(offsetof(GpuLocalShadowRecord,atlas_tiles)==1616);
static_assert(offsetof(GpuLocalShadowRecord,sample_positions)==2000);
static_assert(offsetof(GpuLocalShadowRecord,emitter_normal_two_sided)==2064);
static_assert(offsetof(GpuLocalShadowRecord,state)==2080);

struct LocalShadowStatus
{
    bool enabled{};
    std::uint32_t requested_lights{}, selected_lights{}, unsupported_lights{}, budget_lights{}, overflow_lights{};
    std::uint64_t allocated_bytes{};
    std::string reason;
};
struct LocalShadowLightReference
{
    std::uint64_t stable_id{}, revision{};
    std::uint32_t generation{}, record_index{};
    LightType type{};
};
struct LocalShadowOutputs
{
    ResourceVersion metadata, records, static_bank, dynamic_bank, draw_records;
    ResourceVersion indexed_commands, mesh_commands, command_counts;
    std::array<ID3D12Resource *,5> resources{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE,5> srvs{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE,2> depth_views{};
    ID3D12Resource *indexed_arguments{}, *mesh_arguments{}, *counts_resource{};
    ViewExtent bank_extent;
    std::uint32_t descriptor_count{};
    // Same pending graph/cache snapshot as records; valid until CompleteSubmission.
    std::span<const LocalShadowLightReference> light_records;
};
struct LocalShadowInputs
{
    std::span<const LightView> lights;
    DirectX::XMFLOAT3 camera_position{};
    std::uint64_t rendered_frame_id{};
    std::uint32_t frame_slot{};
    CompletionSet completed;
    bool reset_cache{};
    // Same original leaf superset, geometry records, current SkinCache and common
    // opacity bindings as VirtualShadow. Removed casters retain previous bounds.
    ShadowCasterRange casters;
    std::span<const VirtualShadowDrawGroup> draw_groups;
    std::span<const VirtualShadowNativeRead> native_reads;
    std::vector<std::shared_ptr<const void>> owners; // Placed geometry/material backing, if not renderer-owned.
    // Same 24B indexed / 16B mesh signatures and root6 DWORD0 as VirtualShadow.
    // The local pass bit selects the projection; no new geometry allocation.
    std::function<Result(RenderPassContext &,const LocalShadowOutputs &,
        std::span<const VirtualShadowDrawBatch>)> draw;
    std::function<void(PassBuilder &)> declare_material_sampling;
};
struct LocalShadowLayout
{
    std::uint64_t storage_generation{};
    std::array<ResourceHandle,9> imports{}; // upload, six frame buffers, two banks
    std::array<ResourceVersion,8> outputs{}; // six buffers, static and dynamic banks
    std::array<std::uint32_t,4> pass_indices{}; // 220..223
    std::vector<VirtualShadowNativeRead> native_reads;
    bool initialized{}, material_sampling{};
    std::array<bool,2> sampled_layers{};
};
struct LocalShadowFramePlanData;
struct LocalShadowFramePlan
{
    std::array<bool,2> has_batches{};
  private:
    std::shared_ptr<LocalShadowFramePlanData> data;
    friend class LocalShadowSystem;
};
struct LocalShadowStorage;
class LocalShadowCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<LocalShadowStorage> storage_;
    bool prepared_{};
    friend class LocalShadowSystem;
};
// Passes220..223. Each selected Point owns six guarded faces, Spot one actual
// cone projection. Rectangle/ellipse Disk own 1/2/4 actual plane samples, each
// with six cube faces admitted together from the same fixed physical tile pool.
// Unknown/unpublished/overflow faces resolve to unshadowed
// lighting; status reports that bounded fallback. Static depth is invalidated
// by old AND new caster bounds/revisions; dynamic depth is replaced each frame.
// All output reads must be declared by opaque, transparent and world consumers.
class LocalShadowSystem
{
  public:
    using ShaderReader=VirtualShadowSystem::ShaderReader;
    LocalShadowSystem();
    ~LocalShadowSystem();
    LocalShadowSystem(const LocalShadowSystem &)=delete;
    LocalShadowSystem &operator=(const LocalShadowSystem &)=delete;
    [[nodiscard]] Result Prepare(ID3D12Device *,D3D12MA::Allocator *,const LocalShadowProfile &,
        const ShaderReader &,LocalShadowCandidate &,RendererPipelineCache *cache=nullptr) const;
    [[nodiscard]] Result Commit(LocalShadowCandidate &&,const CompletionSet &completed);
    [[nodiscard]] Result PrepareFrameRecordData(const LocalShadowInputs &,LocalShadowFramePlan &);
    [[nodiscard]] Result Register(RenderGraphBuilder &,const LocalShadowInputs &,
        LocalShadowOutputs &,LocalShadowLayout *layout=nullptr);
    [[nodiscard]] Result Register(RenderGraphBuilder &,const LocalShadowInputs &,
        LocalShadowFramePlan &,LocalShadowOutputs &,LocalShadowLayout *layout=nullptr);
    [[nodiscard]] Result BindRetained(RenderGraphBuilder &,const LocalShadowInputs &,
        LocalShadowFramePlan &,const LocalShadowLayout &,LocalShadowOutputs &);
    [[nodiscard]] bool MatchesLayout(const LocalShadowInputs &,
        const LocalShadowFramePlan &,const LocalShadowLayout &) const noexcept;
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    [[nodiscard]] LocalShadowStatus Status() const;
    // Called only after all old submissions have reliably completed.
    void SwapState(LocalShadowSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    [[nodiscard]] Result RegisterImpl(RenderGraphBuilder &,const LocalShadowInputs &,
        LocalShadowFramePlan &,LocalShadowOutputs &,LocalShadowLayout *,
        const LocalShadowLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
