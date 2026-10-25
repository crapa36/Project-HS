#pragma once
#include <hs/render_data/quality_profiles.hpp>

#include "renderer_light_list.hpp"
#include "renderer_virtual_shadow.hpp"
#include "renderer_local_shadow.hpp"
#include "renderer_scene_geometry.hpp"
#include <hs/core/graphics_settings.hpp>
#include <hs/renderer/vfx_volume_input.hpp>

namespace hs
{
// Table32 suffix, space24: b0 at0, t0..13 at1..14, u0..8 at15..23,
// hero metadata t14 at24, actual STBN Texture2DArray t15 at25,
// authored RGBA8 Texture3D t16 at26.
// The frame-table builder supplies existing space19/21/27 light/shadow ranges.
inline constexpr std::uint32_t kVolumetricDescriptorCount=27;
// Pure authored-input admission, shared by frame preflight and graph registration.
[[nodiscard]] Result ValidateVfxHeroVolumes(VfxVolumeInputs, std::uint32_t gradient_rows);
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
    std::uint64_t scene{},light{},volume{},dynamic_scene{};
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
struct alignas(16) VolumetricConstants
{
    DirectX::XMFLOAT4X4 inverse_view_projection{},world_to_view{},previous_view_projection{},previous_world_to_view{};
    std::array<DirectX::XMFLOAT4X4,3> shadow_view_projection{};
    std::array<DirectX::XMFLOAT4,3> shadow_atlas_scale_offset{};
    DirectX::XMFLOAT4 render_size{},froxel_size{},capacity{},previous_render_size{},previous_froxel_size{};
    DirectX::XMFLOAT4 origin_near{},previous_origin_near{},depth_range{}; // far, log2(far/near), previous far/log.
    DirectX::XMFLOAT4 filter_parameters{0.85f,0.03f,0.0f,0.0f}; // history blend, relative depth tolerance.
    DirectX::XMFLOAT4 shadow_parameters{}; // Atlas inverse width/height, reversed-Z receiver bias, reserved.
    DirectX::XMUINT4 shadow_identity{},frame_info{},counts{}; // frame lo/hi, history valid, area samples; volumes, slices.
    DirectX::XMUINT4 hero_info{}; // Active hero count, admitted, actual local shadow resources enabled, reserved.
};
static_assert(sizeof(VolumetricConstants)==720);
static_assert(offsetof(VolumetricConstants,render_size)==496);
struct VolumetricInputs
{
    VolumetricGuide depth,hdr; // Actual raw scene-linear opaque HDR, before pre-exposure.
    LightListOutputs lights;
    VirtualShadowOutputs shadows; // If enabled, every one of its five reads is declared.
    LocalShadowOutputs local_shadows; // Actual world-query Point/Spot atlas, all five resources or absent.
    VolumetricCompactShadow compact_shadow;
    std::span<const VolumetricVolume> volumes;
    VfxVolumeInputs hero_volumes;
    // Resident authored VFX STBN: exactly R8_UNORM, Texture2DArray 64x64x64,
    // one mip and one sample. Required/read/retained only with active heroes.
    VolumetricGuide hero_blue_noise;
    // Existing t1/space3 binding, Texture2D RGBA16_FLOAT width256, one
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
    SceneFrameTableBuilder make_bindings; // Copies27 descriptors; retains frame/light/shadow table leases.
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
    [[nodiscard]] Result Prepare(ID3D12Device *,D3D12MA::Allocator *,ID3D12RootSignature *,
        GraphicsOptionalQuality,const render_data::QualityProfile &,ViewExtent capacity,std::uint32_t frame_slots,std::uint32_t max_volumes,
        const SceneShaderReader &,VolumetricCandidate &,RendererPipelineCache *cache=nullptr,
        bool hero_enabled=false) const;
    [[nodiscard]] Result Commit(VolumetricCandidate &&,const CompletionSet &completed);
    [[nodiscard]] Result Register(RenderGraphBuilder &,const VolumetricInputs &,VolumetricOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool success);
    void ResetHistory() noexcept;
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    void SwapState(VolumetricSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
