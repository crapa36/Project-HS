#pragma once

#include "render_graph.hpp"
#include <d3d12.h>
#include <DirectXMath.h>
#include <functional>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace hs
{
// CPU inputs for native software SDF geometry.
struct SoftwareSceneGeometry
{
    std::uint64_t stable_key{}, generation{}, current_revision{};
    ID3D12Resource *vertices{}, *indices{};
    ResourceVersion vertex_version{}, index_version{};
    std::uint64_t vertex_byte_offset{}, index_byte_offset{};
    std::uint32_t vertex_stride{}, vertex_count{}, index_count{};
    DXGI_FORMAT index_format{DXGI_FORMAT_R32_UINT};
    bool opaque{true}, dynamic{}, force_rebuild{};
};
struct SoftwareSceneInstance
{
    std::uint32_t stable_gpu_scene_slot{}, generation{}, geometry_index{}, material_id{};
    std::uint64_t mesh_key{}, mesh_generation{};
    DirectX::XMFLOAT4X4 world{};
};

// Append these ranges to the existing compute-visible frame table, space20:
// CBV b0 at0, SRV t3 at1, SRV t14 at2, UAV u0 at3. No new root parameter.
inline constexpr std::uint32_t kSceneGeometryDescriptorCount=4;
enum SceneGeometryFlags : std::uint32_t
{
    SceneGeometryDeformed=1, SceneGeometryCachedSkin=2, SceneGeometryTwoSided=4,
    SceneGeometryBuiltin=16,
    SceneGeometryTransmissive=32
};
// HLSL SceneGeometryRecord stride56. Offsets/counts are byte-address
// representable; indices are R32_UINT canonical indices, relative to the page.
// SceneGeometryTransmissive is for a homogeneous unsupported transport domain;
// mixed hero pages instead resolve each vertex's actual opaque binding ordinal.
struct SceneGeometryRecord
{
    std::uint32_t vertex_descriptor{},index_descriptor{},first_index{},index_count{};
    std::uint32_t vertex_byte_offset{},vertex_stride{76},vertex_count{},position_byte_offset{};
    std::uint32_t stable_gpu_scene_slot{},instance_generation{},skin_slot{},mesh_id{};
    std::uint32_t environment_ordinal{~0u},flags{};
};
static_assert(sizeof(SceneGeometryRecord)==56);
static_assert(offsetof(SceneGeometryRecord,vertex_byte_offset)==16);
static_assert(offsetof(SceneGeometryRecord,stable_gpu_scene_slot)==32);
static_assert(offsetof(SceneGeometryRecord,environment_ordinal)==48);
struct SceneDrawInstance
{
    DirectX::XMFLOAT4 position{},scale{};
    std::uint32_t color{},mesh{};
    float yaw{},padding{};
};
static_assert(sizeof(SceneDrawInstance)==48);
// Immutable logical static rows are owned by the admitted StaticGpuScene cache.
// Frame slots own current rows and the per-frame offsets for this section.
struct SceneGeometryStaticSection
{
    std::vector<SceneGeometryRecord> geometries;
    std::vector<SceneDrawInstance> instances;
    std::vector<std::uint32_t> deformation_rows;
    std::vector<std::uint32_t> deformation_rank_by_row;
};
// Shared with early GI exclusions so unsupported domains retain source validation.
[[nodiscard]] Result ValidateSceneGeometryRecord(const SceneGeometryRecord &, const SceneDrawInstance &);
// Fold the fixed procedural Ground/Dirt placeholder Y shape into a static draw
// transform so immutable native GI can consume the canonical page directly.
// Returns true only when it applied the transform and cleared Deformed.
[[nodiscard]] bool FoldStaticBuiltinTerrainShape(SceneGeometryRecord &,SceneDrawInstance &) noexcept;
[[nodiscard]] Result AppendSceneGeometryStaticRow(SceneGeometryStaticSection &,
    const SceneGeometryRecord &,const SceneDrawInstance &);
// Production row planner used by the waited frame slot and its existing CPU fixture.
// Keeps logical order while returning compact source rows for deformation staging.
[[nodiscard]] Result PrepareSceneGeometryRows(std::span<SceneGeometryRecord>,
    std::span<const SceneDrawInstance>,std::vector<std::uint32_t> &,std::uint64_t &);
[[nodiscard]] Result PrepareSceneGeometrySections(std::span<SceneGeometryRecord>,
    std::span<const SceneDrawInstance>,const SceneGeometryStaticSection &,
    std::vector<std::uint32_t> &,std::vector<std::uint32_t> &,std::uint64_t &);

struct SceneShaderRead
{
    ResourceVersion version{};
    ID3D12Resource *resource{};
    Access access{Access::ComputeRead};
};
// The caller binds the main compute root signature and all referenced tables,
// FrameCB, native skin/material resources and sampler heaps. Its table32 must
// contain the service's descriptor_suffix at the appended space20 ranges.
// The callback runs after the service sets the PSO; it must not replace the PSO.
using SceneShaderBinder=std::function<Result(ID3D12GraphicsCommandList *)>;
using SceneFrameTableBuilder=std::function<Result(D3D12_CPU_DESCRIPTOR_HANDLE,std::uint32_t,SceneShaderBinder &)>;
using SceneShaderReader=std::function<Result(std::string_view,std::vector<std::byte> &)>;
struct SceneGeometryInputs
{
    std::span<const SceneShaderRead> reads; // canonical vertex/index, current skin, FrameCB, material set
    // Called once after all4 descriptors are valid. Copy them into a private
    // frame-table32 suffix and return a binder retaining that table/heap lease.
    SceneFrameTableBuilder make_bindings;
    std::uint32_t frame_slot{};
    CompletionSet completed{};
};
struct SceneGeometryOutputs
{
    ResourceVersion positions{};
    ID3D12Resource *position_resource{};
    std::shared_ptr<const void> owner; // actual service allocations, retained by native SDF registrations
};
struct SceneGeometryReadToken
{
    ResourceVersion version{};
    Access access{};
    bool operator==(const SceneGeometryReadToken &) const noexcept = default;
};
// Graph topology and physical range inputs only. Native resources, binders and
// completion leases are rebuilt from the current frame.
struct SceneGeometryRecordData
{
    std::uint32_t frame_slot{};
    std::uint64_t record_count{}, deformed_position_bytes{}; // record_count is logical; import widths cover compact deformed rows
    std::array<std::uint64_t,6> import_widths{};
    std::vector<SceneGeometryReadToken> reads;
    bool operator==(const SceneGeometryRecordData &) const noexcept = default;
};
struct SceneGeometryGraphLayout
{
    SceneGeometryRecordData record;
    std::array<BufferHandle,6> imports{};
    ResourceVersion records{}, instances{}, positions{};
    std::uint32_t upload_pass{UINT32_MAX}, deform_pass{UINT32_MAX};
    bool valid{};
};
[[nodiscard]] bool MatchesSceneGeometryGraphLayout(const SceneGeometryRecordData &,
    const SceneGeometryGraphLayout &) noexcept;
struct SceneGeometryStorage;
class SceneGeometryPreparedFrame
{
    std::shared_ptr<SceneGeometryStorage> storage_;
    std::shared_ptr<const void> owner_;
    std::uint32_t frame_slot_{};
    bool active_{};
    friend class SceneGeometrySystem;
    SceneGeometryPreparedFrame(std::shared_ptr<SceneGeometryStorage>,std::shared_ptr<const void>,std::uint32_t);
    void Release() noexcept;
  public:
    SceneGeometryPreparedFrame()=default;
    SceneGeometryPreparedFrame(const SceneGeometryPreparedFrame &)=delete;
    SceneGeometryPreparedFrame &operator=(const SceneGeometryPreparedFrame &)=delete;
    SceneGeometryPreparedFrame(SceneGeometryPreparedFrame &&) noexcept;
    SceneGeometryPreparedFrame &operator=(SceneGeometryPreparedFrame &&) noexcept;
    ~SceneGeometryPreparedFrame();
    [[nodiscard]] std::size_t GeometryCount() const noexcept;
    [[nodiscard]] const SceneGeometryRecord &GeometryAt(std::size_t,SceneGeometryRecord &) const noexcept;
    [[nodiscard]] const SceneDrawInstance &InstanceAt(std::size_t) const noexcept;
    [[nodiscard]] ID3D12Resource *PositionResource() const noexcept;
    [[nodiscard]] const SceneGeometryRecordData *RecordData() const noexcept;
    [[nodiscard]] const std::shared_ptr<const void> &Owner() const noexcept { return owner_; }
};
class SceneGeometryCandidate
{
    std::shared_ptr<SceneGeometryStorage> storage_;
    friend class SceneGeometrySystem;
};
class SceneGeometrySystem
{
  public:
    SceneGeometrySystem();
    ~SceneGeometrySystem();
    [[nodiscard]] Result Prepare(ID3D12Device *,D3D12MA::Allocator *,ID3D12RootSignature *,
        bool enabled,std::uint32_t frame_slots,const SceneShaderReader &,SceneGeometryCandidate &) const;
    [[nodiscard]] Result Commit(SceneGeometryCandidate &&,const CompletionSet &);
    [[nodiscard]] Result PrepareFrame(std::vector<SceneGeometryRecord> &&,
        std::vector<SceneDrawInstance> &&,std::shared_ptr<const SceneGeometryStaticSection>,
        std::uint32_t,const CompletionSet &,SceneGeometryPreparedFrame &);
    [[nodiscard]] Result RegisterPrepared(RenderGraphBuilder &,const SceneGeometryInputs &,
        SceneGeometryPreparedFrame &,SceneGeometryOutputs &,
        SceneGeometryGraphLayout *capture=nullptr);
    [[nodiscard]] Result BindRetainedPrepared(RenderGraphBuilder &,const SceneGeometryInputs &,
        SceneGeometryPreparedFrame &,const SceneGeometryGraphLayout &,SceneGeometryOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // Called only after all old submissions have reliably completed.
    void SwapState(SceneGeometrySystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    [[nodiscard]] Result RegisterCore(RenderGraphBuilder &,const SceneGeometryInputs &,
        SceneGeometryPreparedFrame &,
        SceneGeometryOutputs &,SceneGeometryGraphLayout *,const SceneGeometryGraphLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
