#pragma once

#include "renderer_local_probe.hpp"
#include <hs/render_data/geometry_package.hpp>
#include <hs/render_data/material_schema.hpp>
#include <hs/renderer/renderer.hpp>
#include <D3D12MemAlloc.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hs
{
namespace renderer_detail { struct FrameConstants; }
class RendererPipelineCache;

// One complete conventional page per authored environment/terrain material.
// Use an actual complete resident source/fallback page, never a selected main
// camera cluster/HLOD cut. Retain both native resource and allocator leases.
struct LocalProbeCapturePage
{
    Microsoft::WRL::ComPtr<ID3D12Resource> vertices, indices;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> vertex_allocation, index_allocation;
    ResourceVersion vertex_version, index_version;
    D3D12_VERTEX_BUFFER_VIEW vertex_view{};
    D3D12_INDEX_BUFFER_VIEW index_view{};
    std::uint32_t first_index{}, index_count{}, generation{}, page_slot{};
    std::int32_t base_vertex{};
    render_data::GeometryBounds bounds; // Exact complete page.metadata.bounds.
    bool complete{};
    std::array<std::byte, 32> source_digest{}; // Actual decoded complete geometry page bytes.
};

// Value-only revision facts from the geometry catalog. They let a sample-ready
// probe check source ownership without constructing native capture pages/leases.
struct LocalProbePageRevision
{
    std::uint32_t page_slot{}, generation{}, index_count{};
    std::array<std::byte, 32> source_digest{};
    render_data::GeometryBounds bounds;
};
struct LocalProbeTextureFact
{
    std::string_view logical_id;
    std::uint32_t sampled_mip{};
    bool ready{};
};

struct LocalProbeRuntimeFrameInputs
{
    const RendererConfig *config{};
    RendererPipelineCache *pipeline_cache{};
    std::shared_ptr<const StaticRenderScene> scene;
    std::span<const LightView> lights; // Same authoritative static lights bound below.
    std::uint32_t static_instance_generation{};
    std::uint64_t material_parameter_revision{}, pipeline_revision{};
    float foliage_density{};
    const renderer_detail::FrameConstants *frame_constants{}; // Copied at Register.
    std::span<const LocalProbePageRevision> page_revisions;
    std::span<const LocalProbeTextureFact> texture_facts;
    bool texture_facts_valid{};
    Microsoft::WRL::ComPtr<ID3D12Resource> static_instances;
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> static_allocation;
    ResourceVersion static_version;
    // b0 lives inside root table32. Each face gets a private immutable copy of
    // the frame descriptor prefix with its own CBV; never rewrite the main b0.
    // Bank6 is a separate immutable static-shadow camera/constants/table slot.
    // frame_srv_heap retains the original nonvisible CPU descriptor owner.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> frame_srv_heap, sampler_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE frame_srv_base{}; // First texture descriptor of this frame.
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_table{};
    // Complete geometry catalog order, followed by terrain. Material ordinals are separate.
    std::span<const LocalProbeCapturePage> pages;
    // All actual texture/material/light/shadow/environment/LTC/LUT/frame binding
    // reads. Native vertex/index/static table and private constants are appended.
    std::span<const LocalProbeSceneRead> scene_reads;
    // Bind the existing common root signature, descriptor heaps/tables and all
    // shared material/texture/light/shadow SRVs. Restore the main view bindings
    // and viewport/targets after each capture, including a callback error.
    std::function<void(ID3D12GraphicsCommandList *)> bind_graphics, restore_graphics;
    // Additional owners for resources read through the shared binding tables.
    std::vector<std::shared_ptr<const void>> binding_owners;
    CompletionSet completed;
    std::uint32_t max_cube_views{6};
    std::function<void(PassBuilder &)> declare_material_sampling, declare_shadow_sampling;
};

struct LocalProbeCaptureState;
struct LocalProbeRuntimeFramePlan
{
    const LocalProbeRuntimeFrameInputs *source{};
    render_data::SceneProbeRevisions revisions;
    std::shared_ptr<const StaticRenderScene> scene;
    std::uint64_t static_instance_generation{},material_parameter_revision{},pipeline_revision{};
    std::size_t light_count{},page_count{},scene_read_count{};
    std::uint32_t max_cube_views{};
    std::array<std::byte,32> capture_identity{};
    bool capture_identity_valid{},capture_sources_ready{};
    bool active{},capture{},shadow{};
};
struct LocalProbeRuntimeLayout
{
    LocalProbeLayout service;
    ResourceHandle private_constants{},private_shadow{};
    ResourceVersion shadow_output{};
    std::uint32_t shadow_pass_index{};
    std::vector<LocalProbeSceneRead> scene_reads;
    std::vector<LocalProbeSceneRead> shadow_reads;
    bool active{},capture{},shadow{},shadow_sampling{};
};
class LocalProbeRuntimeCandidate
{
  private:
    LocalProbeCandidate service_;
    std::shared_ptr<LocalProbeCaptureState> capture_;
    bool prepared_{};
    friend class LocalProbeRuntime;
};

// Concrete native adapter. Root owns its Impl member and lifecycle/frame hooks;
// this adapter owns actual static capture commands, PSOs and immutable uploads.
class LocalProbeRuntime
{
  public:
    [[nodiscard]] Result PrepareCaptureMaterialOrdinals(const std::shared_ptr<const StaticRenderScene> &,
        float foliage_density);
    [[nodiscard]] std::span<const std::uint32_t> CachedCaptureMaterialOrdinals(
        const std::shared_ptr<const StaticRenderScene> &, float foliage_density) const noexcept;
    [[nodiscard]] bool CaptureIdentityPending(const CompletionSet &) const noexcept;
    LocalProbeRuntime();
    ~LocalProbeRuntime();
    LocalProbeRuntime(const LocalProbeRuntime &) = delete;
    LocalProbeRuntime &operator=(const LocalProbeRuntime &) = delete;
    [[nodiscard]] Result Prepare(ID3D12Device *, D3D12MA::Allocator *, ID3D12RootSignature *,
        const RendererConfig &, std::string_view scene_generation,
        LocalProbeRuntimeCandidate &, RendererPipelineCache *) const;
    [[nodiscard]] Result Commit(LocalProbeRuntimeCandidate &&, const CompletionSet &completed);
    [[nodiscard]] Result CurrentRevisions(const LocalProbeRuntimeFrameInputs &,
        render_data::SceneProbeRevisions &) const;
    // Current-revision decision only. Capture bindings, pipelines and service
    // preparation are deferred until PrepareFrameRecordData receives them.
    [[nodiscard]] Result PrepareFrameDecisionData(const LocalProbeRuntimeFrameInputs &,
        LocalProbeRuntimeFramePlan &) const;
    [[nodiscard]] Result Register(RenderGraphBuilder &, const LocalProbeRuntimeFrameInputs &, LocalProbeOutputs &);
    [[nodiscard]] Result PrepareFrameRecordData(const LocalProbeRuntimeFrameInputs &,
        LocalProbeRuntimeFramePlan &);
    [[nodiscard]] bool MatchesLayout(const LocalProbeRuntimeFrameInputs &,
        const LocalProbeRuntimeFramePlan &,const LocalProbeRuntimeLayout &) const;
    [[nodiscard]] Result Register(RenderGraphBuilder &,const LocalProbeRuntimeFrameInputs &,
        const LocalProbeRuntimeFramePlan &,LocalProbeOutputs &,LocalProbeRuntimeLayout *);
    [[nodiscard]] Result BindRetained(RenderGraphBuilder &,const LocalProbeRuntimeFrameInputs &,
        const LocalProbeRuntimeFramePlan &,const LocalProbeRuntimeLayout &,LocalProbeOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &, const CompletionSet &, bool success,
        const std::shared_ptr<const StaticRenderScene> &, render_data::SceneProbeRevisions);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] Result ExportBakedPackage(RenderGraphQueueExecutor &, TransientResourcePool &,
        std::uint32_t frame_slot, BarrierMode, std::vector<std::byte> &package_bytes);
    [[nodiscard]] std::span<const render_data::SceneProbeRecord> Probes() const noexcept;
    [[nodiscard]] render_data::SceneProbeRevisions BaselineRevisions() const noexcept;
    [[nodiscard]] LocalProbeBakeStatus BakeStatus(const CompletionSet &) const noexcept;
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // Called only after all old submissions have reliably completed.
    void SwapState(LocalProbeRuntime &other) noexcept { impl_.swap(other.impl_); }
  private:
    [[nodiscard]] Result RegisterImpl(RenderGraphBuilder &,const LocalProbeRuntimeFrameInputs &,
        const LocalProbeRuntimeFramePlan &,LocalProbeOutputs &,LocalProbeRuntimeLayout *,
        const LocalProbeRuntimeLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
