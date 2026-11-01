#pragma once

#include "renderer_scene_geometry.hpp"
#include "renderer_ambient_occlusion.hpp"
#include "renderer_sdk_frame.hpp"
#include <hs/render_data/texture_descriptor.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
struct BrixelizerSceneRun
{
    std::size_t job_begin{},job_count{},instance_begin{},instance_count{};
};
struct BrixelizerCurrentSceneRun
{
    std::size_t job_begin{},job_count{},instance_begin{},instance_count{};
};
class BrixelizerGeometryLookup;
// A const shared section is the semantic owner of immutable GI rows and their
// original scene order. Graph resource versions remain frame-current bindings.
class BrixelizerImmutableSceneSection
{
  public:
    BrixelizerImmutableSceneSection(std::vector<SoftwareSceneGeometry> geometries,
        std::vector<BrixelizerSceneInstance> instances,std::vector<std::size_t> leaf_jobs,
        std::vector<BrixelizerSceneRun> runs,
        std::shared_ptr<const BrixelizerGeometryLookup> geometry_lookup={})
        : geometries_(std::move(geometries)),instances_(std::move(instances)),
          leaf_jobs_(std::move(leaf_jobs)),runs_(std::move(runs)),
          geometry_lookup_(std::move(geometry_lookup)) {}
    [[nodiscard]] std::span<const SoftwareSceneGeometry> Geometries() const noexcept { return geometries_; }
    [[nodiscard]] std::span<const BrixelizerSceneInstance> Instances() const noexcept { return instances_; }
    [[nodiscard]] std::span<const std::size_t> LeafJobs() const noexcept { return leaf_jobs_; }
    [[nodiscard]] std::span<const BrixelizerSceneRun> Runs() const noexcept { return runs_; }
    [[nodiscard]] const BrixelizerGeometryLookup *GeometryLookup() const noexcept { return geometry_lookup_.get(); }
  private:
    std::vector<SoftwareSceneGeometry> geometries_;
    std::vector<BrixelizerSceneInstance> instances_;
    std::vector<std::size_t> leaf_jobs_;
    std::vector<BrixelizerSceneRun> runs_;
    std::shared_ptr<const BrixelizerGeometryLookup> geometry_lookup_;
};
struct BrixelizerGeometryVersions { ResourceVersion vertex{},index{}; };
// The immutable prefix is borrowed; only its frame-current graph versions vary.
struct BrixelizerGeometryRange
{
    std::span<const SoftwareSceneGeometry> prefix,current;
    std::span<const BrixelizerGeometryVersions> prefix_versions;
    BrixelizerGeometryRange &operator=(std::span<const SoftwareSceneGeometry> raw) noexcept
    {prefix={};current=raw;prefix_versions={};return *this;}
    [[nodiscard]] std::size_t size() const noexcept { return prefix.size()+current.size(); }
    [[nodiscard]] const SoftwareSceneGeometry &operator[](std::size_t index) const noexcept
    {
        return index<prefix.size()?prefix[index]:current[index-prefix.size()];
    }
    [[nodiscard]] BrixelizerGeometryVersions VersionsAt(std::size_t index) const noexcept
    {
        if(index<prefix.size()&&!prefix_versions.empty())return prefix_versions[index];
        const auto &geometry=(*this)[index];
        return {geometry.vertex_version,geometry.index_version};
    }
};
struct BrixelizerSceneInputs
{
    BrixelizerGeometryRange geometries; // immutable prefix, then current SkinCache / procedural leaves
    std::span<const BrixelizerSceneInstance> current_instances;
    std::span<const BrixelizerCurrentSceneRun> current_runs;
    std::shared_ptr<const BrixelizerImmutableSceneSection> immutable_section;
    std::size_t static_job_base{};
    DirectX::XMFLOAT3 sdf_center{};
    std::uint32_t frame_slot{};
    std::uint64_t frame_index{},scene_revision{};
    CompletionSet completed{};
    // Actual AllocationResource/heap/Generation owners, not just COM references.
    std::vector<std::shared_ptr<const void>> owners;
    // Validated homogeneous exclusions need no per-submesh instance storage.
    std::uint32_t excluded_masked_count{},excluded_thin_count{},excluded_translucent_count{};
};
struct BrixelizerSceneOutputs { ResourceVersion sdf_atlas{}; std::uint32_t instance_count{}; };
struct BrixelizerFrameRecordData;
class BrixelizerGiSystem;
class BrixelizerPreparedScene
{
  public:
    BrixelizerPreparedScene()=default;
    ~BrixelizerPreparedScene();
    BrixelizerPreparedScene(const BrixelizerPreparedScene&)=delete;
    BrixelizerPreparedScene &operator=(const BrixelizerPreparedScene&)=delete;
    BrixelizerPreparedScene(BrixelizerPreparedScene&& other) noexcept { MoveFrom(std::move(other)); }
    BrixelizerPreparedScene &operator=(BrixelizerPreparedScene&& other) noexcept
    { if(this!=&other){Reset();MoveFrom(std::move(other));}return *this; }
    [[nodiscard]] const BrixelizerFrameRecordData &Record() const noexcept { return *record_; }
    [[nodiscard]] explicit operator bool() const noexcept { return owner_!=nullptr; }
  private:
    const BrixelizerFrameRecordData *record_{};
    BrixelizerGeometryRange geometries_;
    std::shared_ptr<const BrixelizerImmutableSceneSection> immutable_section_;
    BrixelizerGiSystem *owner_{};
    std::uint32_t frame_slot_{};
    std::uint64_t frame_index_{},scene_revision_{};
    void Reset() noexcept;
    void MoveFrom(BrixelizerPreparedScene &&other) noexcept
    {
        record_=std::exchange(other.record_,nullptr);geometries_=other.geometries_;other.geometries_=BrixelizerGeometryRange{};
        immutable_section_=std::move(other.immutable_section_);frame_slot_=other.frame_slot_;
        frame_index_=other.frame_index_;scene_revision_=other.scene_revision_;owner_=std::exchange(other.owner_,nullptr);
    }
    friend class BrixelizerGiSystem;
};
// Ephemeral current-frame CPU/SDK preparation. The SDK BakeUpdate and native
// identity mutations happen once before graph plan selection; no native owner
// pointer or baked SDK descriptor is retained in a graph layout.
struct BrixelizerFrameRecordData
{
    std::uint32_t frame_slot{}, static_instances{}, dynamic_instances{}, updated_cascade{};
    std::uint64_t frame_index{}, scene_revision{}, scratch_width{};
    std::uint32_t geometry_count{}, instance_count{}, physical_cascades{};
    ViewExtent extent{};
    bool history_valid{}, noise_upload{};
    std::vector<SceneGeometryReadToken> geometry_reads;
    std::vector<SceneGeometryReadToken> unique_geometry_reads;
    bool operator==(const BrixelizerFrameRecordData &) const noexcept = default;
};
inline constexpr std::uint32_t kBrixelizerGraphScreenCount=16;
struct BrixelizerGraphLayout
{
    BrixelizerFrameRecordData record;
    std::vector<ResourceHandle> imports;
    std::vector<ResourceVersion> sdf_versions,sdf_read_versions;
    std::array<ResourceVersion,kBrixelizerGraphScreenCount> initial_screens{};
    std::array<ResourceVersion,kBrixelizerGraphScreenCount> screens{};
    std::array<ResourceVersion,16> noise_versions{};
    ResourceVersion constant_version{};
    std::array<std::uint32_t,10> passes{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,
        UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    std::vector<SceneGeometryReadToken> cache_reads,lighting_reads;
    std::array<ResourceVersion,12> lighting_guides{};
    ResourceVersion environment_cube{},noise_override{},composite_hdr{};
    std::uint32_t sidecar_count{};
    bool cache_injection{},lighting{},composite{},valid{};
};
[[nodiscard]] bool MatchesBrixelizerGraphLayout(const BrixelizerFrameRecordData &,
    const BrixelizerGraphLayout &) noexcept;
struct BrixelizerCacheInjectionInputs
{
    std::span<const SceneShaderRead> reads; // one canonical set of deferred probe/shadow/light dependencies
    SceneShaderBinder bind_graphics; // actual deferred light/material/shadow tables
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
    std::span<const SceneShaderRead> reads; // actual common closure/frame/material/LUT backing
    SceneFrameTableBuilder make_bindings;
    SceneShaderBinder restore_bindings;
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
    SceneFrameTableBuilder make_bindings;
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
    // Arena/200-enemy batches require over 7M references and 10K bricks.
    std::uint32_t max_references{8u<<20},triangle_swap_bytes{64u<<20},max_bricks_per_bake{16u<<10};
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
        const SceneShaderReader &,std::span<const render_data::TextureDescriptor>,BrixelizerCandidate &) const;
    [[nodiscard]] Result Commit(BrixelizerCandidate &&,const CompletionSet &);
    [[nodiscard]] Result PrepareFrameRecordData(BrixelizerSceneInputs &&,BrixelizerPreparedScene &);
    [[nodiscard]] Result RegisterScene(RenderGraphBuilder &,BrixelizerPreparedScene &&,BrixelizerSceneOutputs &,
        BrixelizerGraphLayout *capture=nullptr,const BrixelizerGraphLayout *retained=nullptr);
    [[nodiscard]] Result RegisterCacheInjection(RenderGraphBuilder &,const BrixelizerCacheInjectionInputs &,AmbientOcclusionGuide &);
    [[nodiscard]] Result RegisterLighting(RenderGraphBuilder &,const BrixelizerLightingInputs &,
        BrixelizerConstants,BrixelizerLightingOutputs &);
    [[nodiscard]] Result RegisterComposite(RenderGraphBuilder &,const BrixelizerCompositeInputs &,BrixelizerTexture &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool success);
    void AbandonPreparedFrame() noexcept;
    // SDK has no reset flag. Before frame preparation, drained GI recreation
    // retains SDF unless a scene/origin discontinuity requires rebuilding it.
    [[nodiscard]] Result ResetHistoryAfterCompletion(const CompletionSet &,ViewExtent fixed_render_extent,bool rebuild_sdf=false);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] BrixelizerStatus Status() const;
    // Native history can outlive the view's validity flag after a cut.
    [[nodiscard]] std::optional<std::uint64_t> HistoryLightRevision() const noexcept;
    // Exact finest SDF voxel configured on the active native context; zero
    // means no usable context and requires LOD0 at its consumers.
    [[nodiscard]] float FinestVoxelSizeMeters() const noexcept;
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    [[nodiscard]] CompletionSet RequiredCompletion() const noexcept;
    void SwapState(BrixelizerGiSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
