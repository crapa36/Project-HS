#pragma once

#include "renderer_translucency_diagnostics_math.hpp"
#include "render_graph.hpp"
#include "view_state.hpp"
#include <hs/core/graphics_settings.hpp>
#include <hs/renderer/translucency_diagnostics.hpp>

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
class RendererPipelineCache;
struct AllocationResource;
constexpr std::uint32_t kLayeredTranslucencyMaxLayers = 4;
// Native root parameter 36: CBV, contiguous SRVs, then the counter UAV.
constexpr std::uint32_t kLayeredTranslucencySrvCount = HS_TRANSLUCENCY_SRV_COUNT;
constexpr std::uint32_t kLayeredTranslucencyUavOffset = HS_TRANSLUCENCY_UAV_TABLE_OFFSET;
constexpr std::uint32_t kLayeredTranslucencyTableSize = HS_TRANSLUCENCY_TABLE_SIZE;
constexpr std::uint32_t kLayeredTranslucencyPhaseCount = HS_TRANSLUCENCY_PHASE_COUNT;
constexpr std::uint32_t kLayeredTranslucencyDescriptorsPerFrame =
    kLayeredTranslucencyTableSize * kLayeredTranslucencyPhaseCount;
// Keep the historical semantic value in diagnostic logs; Guide is not a pass.
enum class LayeredTranslucencyPhase : std::uint8_t { Front = 0, Exit = 1, Tail = 2, Composite = 4 };
struct LayeredTranslucencyGuide { ResourceVersion version; ID3D12Resource *resource{}; };
struct LayeredTranslucencySceneRead { ResourceVersion version; Access access{Access::ShaderRead}; };
using LayeredTranslucencyDraw = std::function<void(ID3D12GraphicsCommandList *, ID3D12PipelineState *,
    LayeredTranslucencyPhase, std::uint32_t, D3D12_GPU_DESCRIPTOR_HANDLE)>;
struct TranslucencyDiagnosticsContributorInput
{
    RendererTranslucencyDiagnosticsContributor source;
    ResourceVersion arguments;
    ID3D12Resource *argument_resource{};
    std::uint64_t argument_offset{}; // root constant + indexed arguments, exactly24 bytes
};
struct TranslucencyDiagnosticsDrawCalls { std::uint32_t execute_indirect{}, draw_indexed{}, dispatch_mesh{}; };
using TranslucencyDiagnosticsDraw = std::function<TranslucencyDiagnosticsDrawCalls(ID3D12GraphicsCommandList *, ID3D12PipelineState *,
    LayeredTranslucencyPhase, std::uint32_t, D3D12_GPU_DESCRIPTOR_HANDLE, std::uint32_t contributor, std::uint64_t diagnostic_uav)>;
struct LayeredTranslucencyInputs
{
    LayeredTranslucencyGuide opaque_depth, hdr, accumulation, revealage;
    D3D12_CPU_DESCRIPTOR_HANDLE accumulation_rtv{}, revealage_rtv{}, opaque_readonly_dsv{};
    ViewExtent render_active;
    // Current-frame conservative HDR write rectangle. Empty selects a full copy.
    D3D12_RECT composition_rect{};
    // A sparse CPU contributor set may justify same-frame GPU occupancy queries.
    bool use_occupancy_queries{};
    bool single_layer_water{};
    std::uint32_t frame_slot{};
    CompletionSet completed;
    // Reserved suffix of the current native frame SRV heap. Root owns its lease.
    ID3D12DescriptorHeap *shader_heap{};
    D3D12_CPU_DESCRIPTOR_HANDLE table_cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE table_gpu{};
    std::uint32_t descriptor_stride{};
    std::span<const LayeredTranslucencySceneRead> peel_reads;
    std::span<const LayeredTranslucencySceneRead> scene_reads;
    LayeredTranslucencyDraw draw;
    // Optional original CPU descriptor owner with the same logical layout as shader_heap.
    ID3D12DescriptorHeap *descriptor_source_heap{};
    // Used only by a capture_translucency_diagnostics target. Sources are copied at registration.
    std::span<const TranslucencyDiagnosticsContributorInput> work_contributors;
    TranslucencyDiagnosticsDraw work_draw;
    std::uint64_t submitted_frame{}, tick{};
    std::uint32_t view_id{};
    ID3D12CommandQueue *direct_queue{};
    std::uint64_t direct_timestamp_frequency{};
};
struct LayeredTranslucencyOutputs
{
    ResourceVersion accumulation, revealage;
    ResourceVersion depth, motion, history, classification, metadata, unpeeled_fragment_count, transport;
    ID3D12Resource *depth_resource{}, *motion_resource{}, *history_resource{};
    ID3D12Resource *classification_resource{}, *metadata_resource{}, *unpeeled_fragment_count_resource{}, *transport_resource{};
    // RG16F transport = {solid transmission present, animated emissive present}.
    D3D12_CPU_DESCRIPTOR_HANDLE depth_srv{}, motion_srv{}, history_srv{}, classification_srv{}, metadata_srv{}, unpeeled_fragment_count_srv{}, transport_srv{};
    ResourceVersion corrected;
    ID3D12Resource *corrected_resource{};
    D3D12_CPU_DESCRIPTOR_HANDLE corrected_srv{};
};
struct TranslucencyDiagnosticsRetainedLayout
{
    static constexpr std::size_t kMaxContributors = kTranslucencyDiagnosticsContributors;
    std::uint32_t contributor_count{}, argument_copy_pass{};
    BufferHandle zero_upload{}, arguments_readback{};
    std::array<ResourceVersion, kMaxContributors> argument_versions{};
    std::array<BufferHandle, kLayeredTranslucencyPhaseCount> native_readback{},
        shader_gpu{}, shader_readback{};
    std::array<std::uint32_t, kLayeredTranslucencyPhaseCount> shader_initialize_pass{},
        shader_copy_pass{};
};
struct LayeredTranslucencyLayout
{
    std::uint64_t target_generation{};
    std::uint32_t layer_count{};
    bool capture_work{}, occupancy{}, single_layer_water{};
    std::array<ResourceVersion, 4> inputs{};
    ResourceVersion background{};
    std::vector<LayeredTranslucencySceneRead> peel_reads, scene_reads;
    std::array<TextureHandle, kLayeredTranslucencyMaxLayers> front{}, identity{};
    TextureHandle exit{};
    std::array<TextureHandle, 2> color{};
    std::array<TextureHandle, 6> guides{};
    TextureHandle unpeeled_fragment_count{};
    std::array<BufferHandle, kLayeredTranslucencyMaxLayers> occupancy_results{};
    std::array<std::uint32_t, kLayeredTranslucencyMaxLayers> front_pass{}, copy_pass{}, composite_pass{};
    std::uint32_t exit_pass{};
    std::uint32_t unpeeled_fragment_count_clear_pass{}, tail_pass{};
    ResourceVersion unpeeled_fragment_count_version{}, accumulation_version{}, revealage_version{}, corrected_version{};
    TranslucencyDiagnosticsRetainedLayout work{};
};
struct LayeredTranslucencyTargets;
class LayeredTranslucencyCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<LayeredTranslucencyTargets> targets_;
    friend class TranslucencySystem;
};
class TranslucencySystem
{
  public:
    using ShaderReader = std::function<Result(std::string_view, std::vector<std::byte> &)>;
    TranslucencySystem();
    ~TranslucencySystem();
    TranslucencySystem(const TranslucencySystem &) = delete;
    TranslucencySystem &operator=(const TranslucencySystem &) = delete;
    [[nodiscard]] Result Prepare(ID3D12Device *, D3D12MA::Allocator *, ID3D12RootSignature *,
        ViewExtent capacity, std::uint32_t layer_count, bool enabled, std::uint32_t frame_slot_count,
        const ShaderReader &, RendererPipelineCache &, LayeredTranslucencyCandidate &, bool capture_translucency_diagnostics = false, TranslucencyDiagnosticsMode mode = TranslucencyDiagnosticsMode::Full) const;
    [[nodiscard]] Result Commit(LayeredTranslucencyCandidate &&, const CompletionSet &completed);
    // Root registers the regular VFX OIT first, excluding the hero bodies.
    // These passes append only the unpeeled hero fragments to that OIT tail.
    [[nodiscard]] Result RegisterLayers(RenderGraphBuilder &, const LayeredTranslucencyInputs &,
        LayeredTranslucencyOutputs &, LayeredTranslucencyLayout *layout = nullptr);
    [[nodiscard]] Result BindRetainedLayers(RenderGraphBuilder &, const LayeredTranslucencyInputs &,
        const LayeredTranslucencyLayout &, LayeredTranslucencyOutputs &);
    // Call after the existing distortion/OIT composite has produced background.
    // Initializes both targets from the active background, then copies the
    // qualified write rectangle for later layers before replacing fronts.
    // The final draw emits four guides; depth/identity reuse the peeled front.
    [[nodiscard]] Result RegisterComposition(RenderGraphBuilder &, LayeredTranslucencyGuide background,
        LayeredTranslucencyOutputs &, LayeredTranslucencyLayout *layout = nullptr);
    [[nodiscard]] Result BindRetainedComposition(RenderGraphBuilder &, LayeredTranslucencyGuide background,
        const LayeredTranslucencyLayout &, LayeredTranslucencyOutputs &);
    // Pure topology check for selecting a retained plan before RestorePlan.
    [[nodiscard]] bool MatchesShape(const LayeredTranslucencyLayout &, bool use_occupancy_queries,
        std::size_t diagnostic_contributor_count, bool single_layer_water = false) const noexcept;
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &, const CompletionSet &, bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    [[nodiscard]] Result ConsumeCompletedWork(const CompletionSet &, std::vector<RendererTranslucencyDiagnosticsSample> &);
    [[nodiscard]] RendererTranslucencyDiagnosticsStatus WorkStatus() const;
#if defined(HS_DEVELOPMENT_TOOLS)
    [[nodiscard]] std::array<const AllocationResource *, 8> DiagnosticResources() const noexcept;
#endif
    // Called only after all old submissions have reliably completed.
    void SwapState(TranslucencySystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    [[nodiscard]] Result RegisterLayersImpl(RenderGraphBuilder &, const LayeredTranslucencyInputs &,
        LayeredTranslucencyOutputs &, LayeredTranslucencyLayout *capture,
        const LayeredTranslucencyLayout *retained);
    [[nodiscard]] Result RegisterCompositionImpl(RenderGraphBuilder &, LayeredTranslucencyGuide,
        LayeredTranslucencyOutputs &, LayeredTranslucencyLayout *capture,
        const LayeredTranslucencyLayout *retained);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
