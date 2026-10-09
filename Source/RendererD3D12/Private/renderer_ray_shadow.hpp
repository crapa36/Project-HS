#pragma once

#include "renderer_ray_ambient_occlusion.hpp"
#include "renderer_local_shadow.hpp"

namespace hs
{
inline constexpr std::uint32_t kRayShadowOwnDescriptorCount=25;
inline constexpr std::uint32_t kRayShadowExportCount=6;
inline constexpr std::uint32_t kRayShadowMaximumLights=4;
// Copy the first existing21 descriptors to space20, and the second25 to
// space28 at the renderer-derived suffix offset. Do not overwrite intermediate
// canonical ranges; they include volumetrics, transport, NRD and local shadows.
using RayShadowTableBuilder=std::function<Result(D3D12_CPU_DESCRIPTOR_HANDLE,
    D3D12_CPU_DESCRIPTOR_HANDLE,RayShaderBinder &)>;
struct RayShadowProfile
{
    bool enabled{};
    std::uint64_t byte_budget{};
    ViewExtent render_capacity;
    GraphicsQuality quality{GraphicsQuality::Medium};
    render_data::QualityProfile quality_profile;
    std::uint32_t maximum_lights{4}, frame_slots{3};
    float maximum_trace_distance_m{60.0f};
    RayDenoiserRequest denoiser;
};
struct alignas(16) RayShadowMetadata
{
    DirectX::XMFLOAT4X4 world_to_clip{}; // Absolute unjittered row-vector matrix.
    DirectX::XMFLOAT4 render_size{}, jitter_uv{};
    DirectX::XMUINT4 counts{}; // Selected records, enabled, asuint(projection._33), asuint(projection._43).
};
struct alignas(16) GpuRayShadowRecord
{
    DirectX::XMUINT4 identity{}, revision{}; // Full light identity and revision.
    DirectX::XMFLOAT4 position_range{}, direction_cos_outer{}, emitter{};
    DirectX::XMUINT4 state{}; // Output array layer, published, native SIGMA, reserved.
};
struct alignas(16) RayShadowConstants
{
    DirectX::XMFLOAT4X4 inverse_view_projection{}, world_to_view{}, projection{};
    DirectX::XMFLOAT4 render_size{}, trace_size{}, previous_trace_size{}, previous_depth_parameters{};
    DirectX::XMFLOAT4 jitter_uv{}, world_origin{}, light_position_range{}, light_direction_cos{}, emitter{};
    DirectX::XMFLOAT4 area_tangent_half_width{}, area_bitangent_half_height{};
    DirectX::XMUINT4 identity{}, frame{}, control{}, budget{};
};
static_assert(sizeof(RayShadowMetadata)==112);
static_assert(sizeof(GpuRayShadowRecord)==96);
static_assert(sizeof(RayShadowConstants)==432);
struct RayShadowGuide {ResourceVersion version;ID3D12Resource *resource{};};
struct RayShadowInputs
{
    RaySceneOutputs scene;
    RayGeometryOutputs geometry;
    std::span<const RayShaderRead> reads;
    std::span<const LightView> lights;
    RayShadowGuide depth,normal,metadata,motion,surface_history,blue_noise; // Actual resident64x64x64 R8 STBN.
    RayShadowTableBuilder make_bindings;
    SdkCamera camera;
    SdkFrameIdentity identity;
    ViewExtent render_active;
    std::uint32_t frame_slot{};
    CompletionSet completed;
    bool reset_history{};
    std::vector<std::shared_ptr<const void>> owners;
};
struct RayShadowOutputs
{
    ResourceVersion metadata,records,visibility,confidence,depth,normal;
    std::array<ID3D12Resource *,kRayShadowExportCount> resources{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE,kRayShadowExportCount> srvs{};
    std::uint32_t descriptor_count{};
};
struct RayShadowStatus
{
    bool enabled{}, native_sigma{};
    std::uint32_t requested_lights{},selected_lights{},unsupported_lights{},budget_lights{},rays_per_pixel{};
    ReconstructionAvailability availability{ReconstructionAvailability::Unavailable};
    std::uint64_t allocated_bytes{};
    std::string reason;
};
struct RayShadowStorage;
class RayShadowCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<RayShadowStorage> storage_;
    bool prepared_{};
    friend class RayShadowSystem;
};
// Trace/temporal/resolve passes230..241 (three per selected light), metadata upload
// and output initialization242, publication243;
// SIGMA pass domains10000+contextSlot*1000. Context identity is stable ID+generation,
// never the sorted array position. A changed emitter revision resets its history.
// The R16F visibility/confidence arrays are OPAQUE alternatives to same-light
// raster visibility. Transparent/froxel/secondary consumers use world ShadowData.
class RayShadowSystem
{
  public:
    RayShadowSystem();
    ~RayShadowSystem();
    RayShadowSystem(const RayShadowSystem &)=delete;
    RayShadowSystem &operator=(const RayShadowSystem &)=delete;
    [[nodiscard]] Result Prepare(ID3D12Device *,D3D12MA::Allocator *,ID3D12RootSignature *,
        const RayShadowProfile &,const RayShaderReader &,RayShadowCandidate &) const;
    [[nodiscard]] Result Commit(RayShadowCandidate &&,const CompletionSet &);
    [[nodiscard]] Result Register(RenderGraphBuilder &,const RayShadowInputs &,RayShadowOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    void ResetHistory() noexcept;
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    [[nodiscard]] RayShadowStatus Status() const;
    // Called only after all old submissions have reliably completed.
    void SwapState(RayShadowSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
