#pragma once

#include "render_graph.hpp"
#include "renderer_sdk_frame.hpp"
#include "view_state.hpp"

#include <DirectXMath.h>
#include <d3d12.h>
#include <array>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hs
{
struct AllocationResource;

// b0/space13. Absolute-world inverse VP supplies sky rays; previous VP projects
// them as directions (w=0), so camera translation does not move infinitely far sky.
struct alignas(16) ReconstructionGuidesConstants
{
    DirectX::XMFLOAT4X4 inverse_unjittered_view_projection{};
    DirectX::XMFLOAT4X4 previous_unjittered_view_projection{};
    DirectX::XMFLOAT4 camera_origin{};
    DirectX::XMFLOAT4 render_size{};
    DirectX::XMFLOAT4 output_size{};
    DirectX::XMFLOAT4 jitter_uv{};
    DirectX::XMFLOAT4 reactive_parameters{1.0f, 1.0f, 0.0f, 0.0f}; // Coverage and color-change gain.
    DirectX::XMUINT4 frame_info{}; // Previous view valid, transparent guides, composition output, unjittered RPM.
};
static_assert(sizeof(ReconstructionGuidesConstants) == 224);
static_assert(offsetof(ReconstructionGuidesConstants, camera_origin) == 128);

struct ReconstructionGuideResource
{
    ResourceVersion version;
    ID3D12Resource *resource{};
};
struct ReconstructionGuidesInputs
{
    ReconstructionGuideResource depth, motion, metadata, surface_history;
    ReconstructionGuideResource composite_hdr, opaque_hdr, exposure, transparency_revealage;
    ViewExtent render_active;
    std::uint32_t frame_slot{};
    CompletionSet completed;
    bool reset_history{};
    ReconstructionGuideResource transparent_depth, transparent_motion, transparent_history;
    ReconstructionGuideResource transparent_classification, transparent_metadata, transparent_overflow;
    ReconstructionGuideResource transparent_transport;
    bool transparent_guides_enabled{};
    ReconstructionGuideResource screen_overlay_alpha;
};
struct ReconstructionGuidesOutputs
{
    ResourceVersion pre_exposed, raw_output, motion, responsive, exposure, depth, composition;
    ID3D12Resource *pre_exposed_resource{};
    D3D12_CPU_DESCRIPTOR_HANDLE pre_exposed_srv{};
    ID3D12Resource *depth_resource{}, *motion_resource{};
    std::shared_ptr<const void> owner;
};
struct ReconstructionGuidesLayout
{
    std::uint64_t target_generation{};
    std::array<ResourceHandle,7> imports{};
    std::array<ResourceVersion,7> outputs{};
    std::array<ResourceVersion,16> inputs{};
    std::array<std::uint32_t,3> pass_indices{}; // 49, 9, 51
    std::string provider_name;
    bool external{}, transparent_guides{}, screen_overlay{}, composition{};
};
struct ReconstructionGuidesTargets;
class ReconstructionGuidesCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<ReconstructionGuidesTargets> targets_;
    friend class ReconstructionGuidesSystem;
};
// Root owns native context initialization and GPU-drained shutdown. This system
// owns native input guides, output textures, pipeline products and descriptor
// leases; pass 9 calls the supplied adapter on its initialization thread.
class ReconstructionGuidesSystem
{
  public:
    using ShaderReader = std::function<Result(std::string_view, std::vector<std::byte> &)>;
    using ExecuteReconstruction = std::function<Result(ID3D12GraphicsCommandList *, const SdkReconstructionFrame &)>;
    using RegisterExternalReconstruction = std::function<Result(PassBuilder &, const SdkReconstructionFrame &,
        std::uint32_t, std::span<const ExternalResourceAccess>, std::vector<std::shared_ptr<const void>>)>;
    ReconstructionGuidesSystem();
    ~ReconstructionGuidesSystem();
    ReconstructionGuidesSystem(const ReconstructionGuidesSystem &) = delete;
    ReconstructionGuidesSystem &operator=(const ReconstructionGuidesSystem &) = delete;
    [[nodiscard]] Result Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator,
        ViewExtent render_capacity, ViewExtent output_extent, std::uint32_t frame_slot_count,
        const ShaderReader &read_shader, ReconstructionGuidesCandidate &candidate,
        bool composition_guide = false) const;
    [[nodiscard]] Result Commit(ReconstructionGuidesCandidate &&candidate,
                                const CompletionSet &completed);
    [[nodiscard]] Result Register(RenderGraphBuilder &graph, const ReconstructionGuidesInputs &inputs,
        ReconstructionGuidesConstants constants, SdkReconstructionFrame frame,
        ExecuteReconstruction execute, std::string_view provider_name,
        ReconstructionGuidesOutputs &outputs, RegisterExternalReconstruction external = {},
        ReconstructionGuidesLayout *layout=nullptr);
    [[nodiscard]] bool MatchesLayout(const ReconstructionGuidesInputs &,bool external,
        std::string_view provider_name,const ReconstructionGuidesLayout &) const noexcept;
    // external_bind must call the provider's BindExternal, which supplies the
    // current queue lease and callback without declaring graph boundaries.
    [[nodiscard]] Result BindRetained(RenderGraphBuilder &,const ReconstructionGuidesInputs &,
        ReconstructionGuidesConstants,SdkReconstructionFrame,ExecuteReconstruction,
        std::string_view provider_name,ReconstructionGuidesOutputs &,
        const ReconstructionGuidesLayout &,RegisterExternalReconstruction external_bind={});
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &graph,
        const CompletionSet &completion, bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // Retains the current output-resolution pre-exposed HDR allocation and its
    // graph completion/state lease for post-submit capture publication.
    [[nodiscard]] AllocationResource RetainPreExposedOutput() const;
    // Called only after all old submissions have reliably completed.
    void SwapState(ReconstructionGuidesSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    [[nodiscard]] Result RegisterImpl(RenderGraphBuilder &,const ReconstructionGuidesInputs &,
        ReconstructionGuidesConstants,SdkReconstructionFrame,ExecuteReconstruction,
        std::string_view,ReconstructionGuidesOutputs &,RegisterExternalReconstruction,
        ReconstructionGuidesLayout *,const ReconstructionGuidesLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
