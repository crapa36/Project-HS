#pragma once
#include <hs/render_data/quality_profiles.hpp>

#include "renderer_light_list.hpp"
#include "renderer_virtual_shadow.hpp"
#include "renderer_local_shadow.hpp"
#include <hs/core/graphics_settings.hpp>
#include <hs/render_data/vfx_volume_input.hpp>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string_view>
#include <vector>

namespace hs
{
#define HS_VM_NATIVE
#include "../../../Content/Shaders/volumetrics_abi.hlsli"
#undef HS_VM_NATIVE
using VolumetricShaderBinder = std::function<Result(ID3D12GraphicsCommandList *)>;
using VolumetricFrameTableBuilder = std::function<Result(D3D12_CPU_DESCRIPTOR_HANDLE, std::uint32_t, VolumetricShaderBinder &)>;
using VolumetricShaderReader = std::function<Result(std::string_view, std::vector<std::byte> &)>;
// Volume descriptor block, space24: b0 at0, t0..13 at1..14, u0..8 at15..23,
// hero metadata t14 at24, actual STBN Texture2DArray t15 at25,
// authored RGBA8 Texture3D t16 at26; local SH and records t2/t4 space23 at27/28.
inline constexpr std::uint32_t kVolumetricDescriptorCount=VmExternalGradient;
// Compact per-frame table for the volume-owned root signature. The first 29
// descriptors retain the suffix layout above; remaining entries are imported
// frame inputs used by the volume shaders.
inline constexpr std::uint32_t kVolumetricFrameDescriptorCount=VmExternalLocalShadow+VmExternalCountLocalShadow;
inline constexpr std::uint32_t kVolumetricGradientOffset=VmExternalGradient;
inline constexpr std::uint32_t kVolumetricLightOffset=VmExternalLight;
inline constexpr std::uint32_t kVolumetricShadowOffset=VmExternalShadow;
inline constexpr std::uint32_t kVolumetricShadowRootOffset=VmExternalShadowRoot;
inline constexpr std::uint32_t kVolumetricLocalShadowOffset=VmExternalLocalShadow;
enum class VolumetricShape : std::uint32_t { Global, Box, Sphere };
struct alignas(16) VolumetricVolume
{
    // Absolute world -> canonical local, DirectX row-vector, not transposed.
    // Box occupies [-1,1]^3; Sphere has unit radius; Global ignores transform.
    DirectX::XMFLOAT4X4 world_to_local{};
    DirectX::XMFLOAT4 density_extinction{1.0f,0.01f,0.0f,0.0f}; // density, extinction m^-1, HG g, reserved.
    DirectX::XMFLOAT4 scattering{0.009f,0.009f,0.009f,0.0f}; // RGB m^-1 before density; <= extinction.
    DirectX::XMUINT4 identity{}; // Stable ID lo/hi, generation, VolumetricShape.
};
static_assert(sizeof(VolumetricVolume)==112);
static_assert(offsetof(VolumetricVolume,density_extinction)==64);
struct alignas(16) VolumetricHeroVolume
{
    DirectX::XMFLOAT4X4 world_to_local{};
    DirectX::XMFLOAT4 optical{}; // density*alpha*live knot envelope, extinction m^-1, g, actual seconds; LUT alpha applies on GPU.
    DirectX::XMFLOAT4 albedo{},emission{}; // RGB scattering albedo/source age; RGB raw HDR emission/actual gradient row.
    DirectX::XMUINT4 sampling{}; // Exact steps, noise kind, stable seed, shape.
    DirectX::XMUINT4 effect_source{}; // Effect ID lo/hi, source stable ID, source index.
    DirectX::XMUINT4 owner_event{}; // Persistent owner lo/hi, event sequence lo/hi (one domain only).
    DirectX::XMUINT4 episode_output{}; // Actual episode lo/hi, output index, effect handle.
};
static_assert(sizeof(VolumetricHeroVolume)==176);
struct VolumetricHeroState
{
    VfxVolumeIdentity identity;
    VolumetricHeroVolume gpu;
    std::array<float,16> local_to_world;
    std::array<float,6> bounds;
    std::array<std::uint64_t,3> revisions;
    bool operator==(const VolumetricHeroState &other) const
    {
        return identity==other.identity && std::memcmp(&gpu,&other.gpu,sizeof(gpu))==0 &&
            local_to_world==other.local_to_world && bounds==other.bounds && revisions==other.revisions;
    }
};
class PreparedVfxHeroVolumes
{
  public:
    [[nodiscard]] bool Matches(VfxVolumeInputs inputs) const noexcept
    { return admitted_ && source_.data()==inputs.data() && source_.size()==inputs.size(); }
    [[nodiscard]] const std::vector<VolumetricHeroState> &Records() const noexcept { return records_; }
  private:
    VfxVolumeInputs source_;
    std::vector<VolumetricHeroState> records_;
    bool admitted_{};
    friend Result PrepareVfxHeroVolumes(VfxVolumeInputs, std::uint32_t, PreparedVfxHeroVolumes &);
};
[[nodiscard]] Result PrepareVfxHeroVolumes(VfxVolumeInputs, std::uint32_t gradient_rows, PreparedVfxHeroVolumes &);
[[nodiscard]] Result ValidateVfxHeroVolumes(VfxVolumeInputs, std::uint32_t gradient_rows);
struct VolumetricCamera
{
    // Camera-relative row-major row-vector transforms; translation is expressed
    // only by origin. Physical positive near/far are independent of reversed Z.
    DirectX::XMFLOAT4X4 inverse_view_projection{},view_projection{},world_to_view{};
    DirectX::XMFLOAT3 origin{};
    float near_plane{},far_plane{};
};
struct VolumetricRevisions
{
    std::uint64_t scene{},light{},volume{},dynamic_scene{},probe_material{},probe_environment{};
    bool operator==(const VolumetricRevisions &) const = default;
};
struct VolumetricGuide
{
    ResourceVersion version{};
    ID3D12Resource *resource{};
};
struct VolumetricCompactShadow
{
    VolumetricGuide atlas;
    std::array<DirectX::XMFLOAT4X4,3> view_projection{}; // Absolute world, row-vector matrices.
    std::array<DirectX::XMFLOAT4,3> atlas_scale_offset{};
    DirectX::XMUINT4 identity{}; // Exact Directional ID lo/hi, generation, enabled.
    float receiver_bias{}; // Reversed-Z depth bias.
};
struct VolumetricInputs
{
    VolumetricGuide depth,hdr; // Actual raw scene-linear opaque HDR, before pre-exposure.
    LightListOutputs lights;
    VirtualShadowOutputs shadows; // If enabled, every one of its five reads is declared.
    LocalShadowOutputs local_shadows; // Actual world-query Point/Spot atlas, all five resources or absent.
    VolumetricGuide probe_irradiance, probe_records;
    bool probe_ready{};
    VolumetricCompactShadow compact_shadow;
    std::span<const VolumetricVolume> volumes;
    VfxVolumeInputs hero_volumes;
    const PreparedVfxHeroVolumes *prepared_heroes{};
    // Resident authored VFX STBN: exactly R8_UNORM, Texture2DArray 64x64x64,
    // one mip and one sample. Required/read/retained only with active heroes.
    VolumetricGuide hero_blue_noise;
    // t1/space3 binding, Texture2D RGBA16_FLOAT width256, one
    // mip/layer/sample. Its actual row count validates every active output.
    VolumetricGuide hero_gradient;
    // Authored High-tier FBM source: exact RGBA8 64x64x64 Texture3D, one
    // mip and one sample. Required/read/retained only with active heroes.
    VolumetricGuide hero_noise_volume;
    VolumetricCamera camera,previous_camera; // Previous must match the successful source camera to admit history.
    ViewExtent render_active;
    std::uint32_t frame_slot{};
    std::uint64_t frame_index{};
    CompletionSet completed;
    VolumetricRevisions revisions;
    bool reset_history{};
    VolumetricFrameTableBuilder make_bindings; // Builds the compact volume table and retains input table leases.
};
struct VolumetricTexture
{
    ResourceVersion version{};
    ID3D12Resource *resource{};
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};
};
struct VolumetricOutputs
{
    // Boundary textures have slices+1 planes: plane0 is L=0,T=1 at near.
    VolumetricTexture scattering,transmittance,resolved,filtered_scattering,column_depth;
    ResourceVersion constants;
    ID3D12Resource *constant_resource{};
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor_suffix{};
    std::uint32_t descriptor_count{kVolumetricDescriptorCount};
    ViewExtent froxel_active,froxel_capacity;
    std::uint32_t slices{};
};
struct VolumetricLayout
{
    // Immutable graph declarations only. Native owners, descriptors and callbacks
    // are supplied afresh for each submission.
    std::uint64_t storage_generation{};
    std::array<ResourceVersion, 24> input_versions{};
    std::array<ResourceHandle, 14> imports{};
    std::array<ResourceVersion, 6> output_versions{};
    std::array<std::uint32_t, 7> pass_index{}; // 170..175, optional 176
    bool history{}, hero{}, shadows{}, local_shadows{}, compact_shadow{};
    bool root_static{}, root_dynamic{}, probe_ready{};
};
// Minimal values needed to compare graph topology and volumetric history.
// Native graph bindings remain in VolumetricInputs and are checked at bind time.
struct VolumetricShapeInput
{
    std::uint32_t frame_slot{};
    std::size_t volume_count{};
    VfxVolumeInputs hero_volumes;
    const PreparedVfxHeroVolumes *prepared_heroes{};
    bool reset_history{};
    std::uint64_t frame_index{};
    VolumetricRevisions revisions{};
    const VolumetricCamera *previous_camera{};
    ID3D12Resource *hero_blue_noise{}, *hero_gradient{}, *hero_noise_volume{};
    bool shadows{}, local_shadows{}, compact_shadow{}, root_static{}, root_dynamic{};
};
struct VolumetricStorage;
class RendererPipelineCache;
class VolumetricCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<VolumetricStorage> storage_;
    friend class VolumetricSystem;
};
class VolumetricSystem
{
  public:
    VolumetricSystem();
    ~VolumetricSystem();
    VolumetricSystem(const VolumetricSystem &)=delete;
    VolumetricSystem &operator=(const VolumetricSystem &)=delete;
    // Off performs no native allocations, shader reads, or PSO creation.
    [[nodiscard]] Result Prepare(ID3D12Device *,D3D12MA::Allocator *,
        GraphicsOptionalQuality,const render_data::QualityProfile &,ViewExtent capacity,std::uint32_t frame_slots,std::uint32_t max_volumes,
        const VolumetricShaderReader &,VolumetricCandidate &,RendererPipelineCache *cache=nullptr,
        bool hero_enabled=false) const;
    [[nodiscard]] Result Commit(VolumetricCandidate &&,const CompletionSet &completed);
    [[nodiscard]] Result Register(RenderGraphBuilder &,const VolumetricInputs &,VolumetricOutputs &,
        VolumetricLayout *layout=nullptr);
    [[nodiscard]] Result BindRetained(RenderGraphBuilder &,const VolumetricInputs &,
        const VolumetricLayout &,VolumetricOutputs &);
    [[nodiscard]] bool MatchesLayout(const VolumetricInputs &,const VolumetricLayout &) const noexcept;
    // Pre-restore shape comparison excludes graph versions, checked at binding.
    [[nodiscard]] bool MatchesShape(const VolumetricShapeInput &,
        const VolumetricLayout &,bool probe_ready) const noexcept;
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool success);
    void ResetHistory() noexcept;
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    void SwapState(VolumetricSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    [[nodiscard]] Result RegisterImpl(RenderGraphBuilder &,const VolumetricInputs &,
        VolumetricOutputs &,VolumetricLayout *,const VolumetricLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
