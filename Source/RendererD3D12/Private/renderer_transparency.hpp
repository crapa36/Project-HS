#pragma once

#include "render_graph.hpp"
#include "view_state.hpp"
#include <hs/core/graphics_settings.hpp>
#include <hs/renderer/hero_work.hpp>

#include <d3d12.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace hs
{
class RendererPipelineCache;
constexpr std::uint32_t kHeroTransparencyMaxLayers = 4;
// One immutable table for each front, exit, tail and composite phase; slot9 is reserved.
// Native root parameter36: CBV b0/space14 at0; SRV t0..20/space14 at1;
// UAV u0/space14 at22. The table adds one DWORD to the native root.
constexpr std::uint32_t kHeroTransparencyTableSize = 23;
constexpr std::uint32_t kHeroTransparencyPhaseCount = 14;
constexpr std::uint32_t kHeroTransparencyDescriptorsPerFrame =
    kHeroTransparencyTableSize * kHeroTransparencyPhaseCount;
// Guide retains its numeric diagnostic identity; Composite layer0 now emits its guides.
enum class HeroTransparencyPhase : std::uint8_t { Front, Exit, Tail, Guide, Composite };
struct HeroTransparencyGuide { ResourceVersion version; ID3D12Resource *resource{}; };
struct HeroTransparencySceneRead { ResourceVersion version; Access access{Access::ShaderRead}; };
using HeroTransparencyDraw = std::function<void(ID3D12GraphicsCommandList *, ID3D12PipelineState *,
    HeroTransparencyPhase, std::uint32_t, D3D12_GPU_DESCRIPTOR_HANDLE)>;
struct HeroWorkContributorInput
{
    RendererHeroWorkContributor source;
    ResourceVersion arguments;
    ID3D12Resource *argument_resource{};
    std::uint64_t argument_offset{}; // root constant + indexed arguments, exactly24 bytes
};
struct HeroWorkDrawCalls { std::uint32_t execute_indirect{}, draw_indexed{}, dispatch_mesh{}; };
using HeroWorkDraw = std::function<HeroWorkDrawCalls(ID3D12GraphicsCommandList *, ID3D12PipelineState *,
    HeroTransparencyPhase, std::uint32_t, D3D12_GPU_DESCRIPTOR_HANDLE, std::uint32_t contributor, std::uint64_t diagnostic_uav)>;
struct HeroTransparencyInputs
{
    HeroTransparencyGuide opaque_depth, hdr, accumulation, revealage;
    D3D12_CPU_DESCRIPTOR_HANDLE accumulation_rtv{}, revealage_rtv{}, opaque_readonly_dsv{};
    ViewExtent render_active;
    // Current-frame conservative HDR write rectangle. Empty selects a full copy.
    D3D12_RECT composition_rect{};
    // A sparse CPU contributor set may justify same-frame GPU occupancy queries.
    bool use_occupancy_queries{};
    std::uint32_t frame_slot{};
    CompletionSet completed;
    // Reserved suffix of the current native frame SRV heap. Root owns its lease.
    ID3D12DescriptorHeap *shader_heap{};
    D3D12_CPU_DESCRIPTOR_HANDLE table_cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE table_gpu{};
    std::uint32_t descriptor_stride{};
    std::span<const HeroTransparencySceneRead> peel_reads;
    std::span<const HeroTransparencySceneRead> scene_reads;
    HeroTransparencyDraw draw;
    // Optional original CPU descriptor owner with the same logical layout as shader_heap.
    ID3D12DescriptorHeap *descriptor_source_heap{};
    // Used only by a capture_hero_work target. Sources are copied at registration.
    std::span<const HeroWorkContributorInput> work_contributors;
    HeroWorkDraw work_draw;
    std::uint64_t submitted_frame{}, tick{};
    std::uint32_t view_id{};
    ID3D12CommandQueue *direct_queue{};
    std::uint64_t direct_timestamp_frequency{};
};
struct HeroTransparencyOutputs
{
    ResourceVersion accumulation, revealage;
    ResourceVersion depth, motion, history, classification, metadata, overflow, transport;
    ID3D12Resource *depth_resource{}, *motion_resource{}, *history_resource{};
    ID3D12Resource *classification_resource{}, *metadata_resource{}, *overflow_resource{}, *transport_resource{};
    // RG16F transport = {solid transmission present, animated emissive present}.
    D3D12_CPU_DESCRIPTOR_HANDLE depth_srv{}, motion_srv{}, history_srv{}, classification_srv{}, metadata_srv{}, overflow_srv{}, transport_srv{};
    ResourceVersion corrected;
    ID3D12Resource *corrected_resource{};
    D3D12_CPU_DESCRIPTOR_HANDLE corrected_srv{};
};
struct HeroWorkRetainedLayout
{
    static constexpr std::size_t kMaxContributors = 12;
    std::uint32_t contributor_count{}, argument_copy_pass{};
    BufferHandle zero_upload{}, arguments_readback{};
    std::array<ResourceVersion, kMaxContributors> argument_versions{};
    std::array<BufferHandle, kHeroTransparencyPhaseCount> native_readback{},
        shader_gpu{}, shader_readback{};
    std::array<std::uint32_t, kHeroTransparencyPhaseCount> shader_initialize_pass{},
        shader_copy_pass{};
};
struct HeroTransparencyLayout
{
    std::uint64_t target_generation{};
    std::uint32_t layer_count{};
    bool capture_work{}, occupancy{};
    std::array<ResourceVersion, 4> inputs{};
    ResourceVersion background{};
    std::vector<HeroTransparencySceneRead> peel_reads, scene_reads;
    std::array<TextureHandle, kHeroTransparencyMaxLayers> front{}, identity{}, exit{};
    std::array<TextureHandle, 2> color{};
    std::array<TextureHandle, 6> guides{};
    TextureHandle overflow{};
    std::array<BufferHandle, kHeroTransparencyMaxLayers> occupancy_results{};
    std::array<std::uint32_t, kHeroTransparencyMaxLayers> front_pass{}, exit_pass{},
        copy_pass{}, composite_pass{};
    std::uint32_t overflow_clear_pass{}, tail_pass{};
    std::array<ResourceVersion, 6> guide_versions{};
    ResourceVersion overflow_version{}, accumulation_version{}, revealage_version{}, corrected_version{};
    HeroWorkRetainedLayout work{};
};
struct HeroTransparencyTargets;
class HeroTransparencyCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<HeroTransparencyTargets> targets_;
    friend class HeroTransparencySystem;
};
class HeroTransparencySystem
{
  public:
    using ShaderReader = std::function<Result(std::string_view, std::vector<std::byte> &)>;
    HeroTransparencySystem();
    ~HeroTransparencySystem();
    HeroTransparencySystem(const HeroTransparencySystem &) = delete;
    HeroTransparencySystem &operator=(const HeroTransparencySystem &) = delete;
    [[nodiscard]] Result Prepare(ID3D12Device *, D3D12MA::Allocator *, ID3D12RootSignature *,
        ViewExtent capacity, std::uint32_t layer_count, bool enabled, std::uint32_t frame_slot_count,
        const ShaderReader &, RendererPipelineCache &, HeroTransparencyCandidate &, bool capture_hero_work = false, HeroWorkDiagnosticMode mode = HeroWorkDiagnosticMode::Full) const;
    [[nodiscard]] Result Commit(HeroTransparencyCandidate &&, const CompletionSet &completed);
    // Root registers the regular VFX OIT first, excluding the hero bodies.
    // These passes append only the unpeeled hero fragments to that OIT tail.
    [[nodiscard]] Result RegisterLayers(RenderGraphBuilder &, const HeroTransparencyInputs &,
        HeroTransparencyOutputs &, HeroTransparencyLayout *layout = nullptr);
    [[nodiscard]] Result BindRetainedLayers(RenderGraphBuilder &, const HeroTransparencyInputs &,
        const HeroTransparencyLayout &, HeroTransparencyOutputs &);
    // Call after the existing distortion/OIT composite has produced background.
    // Initializes both targets from the active background, then copies the
    // qualified write rectangle for later layers before replacing fronts.
    // The final nearest-front draw also produces all six reconstruction guides.
    [[nodiscard]] Result RegisterComposition(RenderGraphBuilder &, HeroTransparencyGuide background,
        HeroTransparencyOutputs &, HeroTransparencyLayout *layout = nullptr);
    [[nodiscard]] Result BindRetainedComposition(RenderGraphBuilder &, HeroTransparencyGuide background,
        const HeroTransparencyLayout &, HeroTransparencyOutputs &);
    // Pure topology check for selecting a retained plan before RestorePlan.
    [[nodiscard]] bool MatchesLayout(const HeroTransparencyInputs &,
        HeroTransparencyGuide background, const HeroTransparencyLayout &) const noexcept;
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &, const CompletionSet &, bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    [[nodiscard]] Result ConsumeCompletedWork(const CompletionSet &, std::vector<RendererHeroWorkSample> &);
    [[nodiscard]] RendererHeroWorkStatus WorkStatus() const;
    // Called only after all old submissions have reliably completed.
    void SwapState(HeroTransparencySystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    [[nodiscard]] Result RegisterLayersImpl(RenderGraphBuilder &, const HeroTransparencyInputs &,
        HeroTransparencyOutputs &, HeroTransparencyLayout *capture,
        const HeroTransparencyLayout *retained);
    [[nodiscard]] Result RegisterCompositionImpl(RenderGraphBuilder &, HeroTransparencyGuide,
        HeroTransparencyOutputs &, HeroTransparencyLayout *capture,
        const HeroTransparencyLayout *retained);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
