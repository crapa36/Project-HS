#pragma once

#include "renderer_ray_geometry.hpp"
#include "renderer_ambient_occlusion.hpp"
#include "renderer_sdk_frame.hpp"
#include <array>
#include <string>

namespace hs
{
// Main table32 suffix: b0, t0..14, u0..8, all space31. Native SDK root
// signatures/heaps are independent and are restored with restore_bindings.
inline constexpr std::uint32_t kBrixelizerDescriptorCount=25;
struct BrixelizerNativeSupport
{
    bool compiled{},supported{};
    std::string_view reason;
};
[[nodiscard]] BrixelizerNativeSupport QueryBrixelizerNativeSupport(ID3D12Device *) noexcept;
struct alignas(16) BrixelizerConstants
{
    DirectX::XMFLOAT4X4 inverse_view_projection{};
    DirectX::XMFLOAT4 render_size{},jitter_uv{},world_origin{};
    DirectX::XMFLOAT4 cache_parameters{10000.0f,0.0f,1.0f,0.0f};
    DirectX::XMUINT4 frame_info{}; // frame index, history valid, sidecar planes, reserved
};
static_assert(sizeof(BrixelizerConstants)==144);
enum class BrixelizerGeometryDomain : std::uint8_t
{
    Opaque,
    AuthoredCoverage, // actual coverage triangles in supplied native ranges
    UnsupportedMasked,
    UnsupportedThin,
    Translucent
};
struct BrixelizerSceneInstance
{
    std::uint32_t stable_gpu_scene_slot{},generation{},geometry_index{},material_id{};
    std::uint64_t material_revision{},deformation_revision{};
    DirectX::XMFLOAT4X4 world{}; // DirectX row-vector affine local-to-world
    DirectX::XMFLOAT3 bounds_min{},bounds_max{}; // conservative ABSOLUTE world AABB
    BrixelizerGeometryDomain domain{BrixelizerGeometryDomain::Opaque};
};
struct BrixelizerSceneInputs
{
    std::span<const RaySceneGeometry> geometries; // original leaf / current SkinCache / procedural
    std::span<const BrixelizerSceneInstance> instances; // all offscreen eligible instances too
    DirectX::XMFLOAT3 sdf_center{};
    std::uint32_t frame_slot{};
    std::uint64_t frame_index{},scene_revision{};
    CompletionSet completed{};
    // Actual AllocationResource/heap/Generation owners, not just COM references.
    std::vector<std::shared_ptr<const void>> owners;
};
struct BrixelizerSceneOutputs { ResourceVersion sdf_atlas{}; std::uint32_t instance_count{}; };
struct BrixelizerCacheInjectionInputs
{
    std::span<const RayShaderRead> reads; // one canonical set of deferred probe/shadow/light dependencies
    RayShaderBinder bind_graphics; // actual deferred light/material/shadow tables
    ViewExtent render_active{};
    std::uint32_t frame_slot{};
    std::vector<std::shared_ptr<const void>> owners;
};
struct BrixelizerLightingInputs
{
    AmbientOcclusionGuide depth,normal,metadata,motion,base,material;
    std::array<AmbientOcclusionGuide,2> sidecars{};
    // Independent unexposed DirectDiffuse+Emission, exact fallback and SSS ingress.
    AmbientOcclusionGuide cache_injection,fallback_diffuse,environment_specular,subsurface_diffuse;
    // Native environment cube; noise is an optional genuine128x128 override.
    // Otherwise selected Generation owns16 pinned-sampler RGBA16F textures.
    AmbientOcclusionGuide environment_cube,noise;
    std::span<const RayShaderRead> reads; // actual common closure/frame/material/LUT backing
    RayFrameTableBuilder make_bindings;
    RayShaderBinder restore_bindings;
    ViewExtent render_active{};
    SdkCamera camera{};
    std::uint32_t frame_slot{},sidecar_plane_count{};
    // Discontinuous lighting binding/profile epoch. Moving or flickering
    // lights retain this epoch and update the actual direct cache each frame.
    std::uint64_t frame_index{},light_revision{};
    CompletionSet completed{};
    std::vector<std::shared_ptr<const void>> owners;
};
struct BrixelizerTexture
{
    ResourceVersion version{};
    ID3D12Resource *resource{};
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};
};
struct BrixelizerLightingOutputs
{
    // Remodulated scene-linear diffuse replacement/delta, SSS ingress and
    // environment-specular replacement. Alpha records material capability.
    BrixelizerTexture diffuse,delta,adjusted_subsurface,specular;
    BrixelizerTexture raw_diffuse,raw_specular,cache_export;
};
struct BrixelizerCompositeInputs
{
    AmbientOcclusionGuide hdr;
    RayFrameTableBuilder make_bindings;
    ViewExtent render_active{};
    std::uint32_t frame_slot{};
    std::vector<std::shared_ptr<const void>> owners;
};
struct BrixelizerSettings
{
    bool enabled{};
    GraphicsQuality quality{GraphicsQuality::Medium};
    std::uint32_t internal_resolution_percent{};
    std::uint32_t logical_cascades{}; // static+dynamic+merged => 12 physical
    float first_voxel_size_m{0.125f};
    std::uint32_t max_references{1u<<20},triangle_swap_bytes{64u<<20},max_bricks_per_bake{4096};
    std::uint64_t memory_budget_bytes{1024ull<<20}; // includes old+candidate during Prepare
};
struct BrixelizerStatus
{
    bool compiled{},enabled{},native_ready{},faulted{};
    std::uint32_t static_instances{},dynamic_instances{},unsupported_masked{},unsupported_thin{},excluded_translucent{};
    std::uint32_t physical_cascades{};
    std::uint64_t gpu_bytes{},cpu_bytes{},scratch_bytes{},scene_revision{},light_revision{};
    std::string reason;
};
struct BrixelizerStorage;
class BrixelizerCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<BrixelizerStorage> storage_;
    friend class BrixelizerGiSystem;
};
class BrixelizerGiSystem
{
  public:
    BrixelizerGiSystem();
    ~BrixelizerGiSystem();
    BrixelizerGiSystem(const BrixelizerGiSystem&)=delete;
    BrixelizerGiSystem &operator=(const BrixelizerGiSystem&)=delete;
    [[nodiscard]] Result Prepare(ID3D12Device *,D3D12MA::Allocator *,ID3D12RootSignature *,
        ViewExtent fixed_render_extent,std::uint32_t frame_slots,const BrixelizerSettings &,
        const RayShaderReader &,BrixelizerCandidate &) const;
    [[nodiscard]] Result Commit(BrixelizerCandidate &&,const CompletionSet &);
    [[nodiscard]] Result RegisterScene(RenderGraphBuilder &,const BrixelizerSceneInputs &,BrixelizerSceneOutputs &);
    [[nodiscard]] Result RegisterCacheInjection(RenderGraphBuilder &,const BrixelizerCacheInjectionInputs &,AmbientOcclusionGuide &);
    [[nodiscard]] Result RegisterLighting(RenderGraphBuilder &,const BrixelizerLightingInputs &,
        BrixelizerConstants,BrixelizerLightingOutputs &);
    [[nodiscard]] Result RegisterComposite(RenderGraphBuilder &,const BrixelizerCompositeInputs &,BrixelizerTexture &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool success);
    // SDK has no reset flag. Drained GI recreation retains SDF on a pure extent
    // change; camera cut/origin discontinuity requests rebuild_sdf=true.
    [[nodiscard]] Result ResetHistoryAfterCompletion(const CompletionSet &,ViewExtent fixed_render_extent,bool rebuild_sdf=false);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] BrixelizerStatus Status() const;
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    [[nodiscard]] CompletionSet RequiredCompletion() const noexcept;
    void SwapState(BrixelizerGiSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
