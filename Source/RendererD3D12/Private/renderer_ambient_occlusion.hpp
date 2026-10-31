#pragma once
#include <hs/render_data/quality_profiles.hpp>

#include "render_graph.hpp"
#include "view_state.hpp"

#include <hs/core/graphics_settings.hpp>
#include <DirectXMath.h>
#include <d3d12.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace hs
{
struct AllocationResource;
// Mirrors b0/space12 in ambient_occlusion.hlsl. Matrices use row-vector HLSL
// convention. The first inverse transforms jittered NDC directly into view
// position; world_to_view rotates world normals and light directions.
struct alignas(16) AmbientOcclusionConstants
{
    DirectX::XMFLOAT4X4 inverse_projection{};
    DirectX::XMFLOAT4X4 world_to_view{};
    DirectX::XMFLOAT4X4 projection{};
    DirectX::XMFLOAT4 render_size{};
    DirectX::XMFLOAT4 ao_size{};
    DirectX::XMFLOAT4 history_size{};
    DirectX::XMFLOAT4 previous_depth_parameters{}; // P33,P34,P43,P44.
    DirectX::XMFLOAT4 jitter_uv{}; // Current xy, previous zw; logical viewport UV.
    DirectX::XMFLOAT4 gtao_parameters{1.0f, 1.0f, 0.8f, 0.0f}; // Radius, strength, falloff start, 0=general, 1=fast, 2=prepared inverse-W.
    DirectX::XMFLOAT4 filter_parameters{0.03f, 0.85f, 0.9f, 0.02f}; // Depth, normal, history, bilateral.
    DirectX::XMFLOAT4 contact_parameters{0.75f, 0.1f, 0.02f, 0.0f}; // Reach, thickness, bias in m.
    DirectX::XMFLOAT4 toward_light{}; // Normalized world direction toward sun.
    DirectX::XMUINT4 sample_counts{}; // Slices, steps per side, contact steps, filter radius.
    DirectX::XMUINT4 frame_info{}; // History valid, frame index, AO enabled, contact enabled.
};
static_assert(sizeof(AmbientOcclusionConstants) == 368);
static_assert(offsetof(AmbientOcclusionConstants, render_size) == 192);
static_assert(offsetof(AmbientOcclusionConstants, sample_counts) == 336);

struct AmbientOcclusionGuide
{
    ResourceVersion version;
    ID3D12Resource *resource{};
};

struct AmbientOcclusionInputs
{
    AmbientOcclusionGuide depth, normal, metadata, motion, surface_history;
    ViewExtent render_active;
    std::uint32_t frame_slot{};
    std::uint64_t frame_index{};
    CompletionSet completed;
    bool reset_history{};
    AmbientOcclusionGuide adaptive_density; // Actual R8 tile8 density, only for the selected adaptive GTAO consumer.
    QueueHint queue{QueueHint::Direct}; // Only the qualified Main caller selects Compute.
};

struct AmbientOcclusionOutputs
{
    ResourceVersion ao, contact;
    ID3D12Resource *ao_resource{}, *contact_resource{};
    D3D12_CPU_DESCRIPTOR_HANDLE ao_srv{}, contact_srv{};
    bool adaptive_density_used{};
};
struct AmbientOcclusionLayout
{
    bool valid{}, ao{}, contact{}, adaptive{}, history{}, prepared_inverse_w{};
    ViewExtent render_capacity{},ao_capacity{};
    QueueHint queue{};
    std::array<ResourceVersion, 6> guides{};
    std::array<TextureHandle, 7> textures{};
    std::array<std::uint32_t, 5> passes{};
    ResourceVersion raw, history_output, metadata, resolved, contact_output;
};

struct AmbientOcclusionTargets;
class AmbientOcclusionCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<AmbientOcclusionTargets> targets_;
    friend class AmbientOcclusionSystem;
};

// Concrete native service. The caller retains all imported guide resources,
// waits the frame slot before Register, and calls CompleteSubmission after every
// attempted graph submission (including failure). No SDK/provider abstraction.
class AmbientOcclusionSystem
{
  public:
    using ShaderReader = std::function<Result(std::string_view, std::vector<std::byte> &)>;
    AmbientOcclusionSystem();
    ~AmbientOcclusionSystem();
    AmbientOcclusionSystem(const AmbientOcclusionSystem &) = delete;
    AmbientOcclusionSystem &operator=(const AmbientOcclusionSystem &) = delete;

    [[nodiscard]] Result Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator,
        ViewExtent render_capacity, GraphicsQuality quality, const render_data::QualityProfile &, bool ao_enabled,
        bool contact_enabled, std::uint32_t frame_slot_count, const ShaderReader &read_shader,
        AmbientOcclusionCandidate &candidate, bool adaptive_compute=false) const;
    // Completion must cover every previous use before publishing replacement
    // storage. Preparation never alters the current targets or history.
    [[nodiscard]] Result Commit(AmbientOcclusionCandidate &&candidate,
                                const CompletionSet &completed);
    [[nodiscard]] Result Register(RenderGraphBuilder &graph, const AmbientOcclusionInputs &inputs,
        AmbientOcclusionConstants constants, AmbientOcclusionOutputs &outputs,
        AmbientOcclusionLayout *layout = nullptr);
    [[nodiscard]] Result BindRetained(RenderGraphBuilder &, const AmbientOcclusionInputs &,
        AmbientOcclusionConstants, const AmbientOcclusionLayout &, AmbientOcclusionOutputs &);
    [[nodiscard]] bool MatchesLayout(const AmbientOcclusionInputs &,const AmbientOcclusionLayout &)const noexcept;
    [[nodiscard]] bool MatchesLayout(const AmbientOcclusionInputs &, const AmbientOcclusionConstants &,
        const AmbientOcclusionLayout &) const noexcept;
    [[nodiscard]] bool MatchesShape(const AmbientOcclusionLayout &, bool reset_history, QueueHint) const noexcept;
    [[nodiscard]] bool MatchesShape(const AmbientOcclusionLayout &, bool reset_history, QueueHint,
        ViewExtent active, const DirectX::XMFLOAT4X4 &inverse_projection) const noexcept;
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &graph,
        const CompletionSet &completion, bool success);
    void ResetHistory() noexcept;
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    [[nodiscard]] bool AdaptiveComputeApplied() const noexcept;
    [[nodiscard]] std::array<const AllocationResource *, 5> DiagnosticResources() const noexcept;

    // Called only after all old submissions have reliably completed.
    void SwapState(AmbientOcclusionSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    [[nodiscard]] Result RegisterFrame(RenderGraphBuilder &, const AmbientOcclusionInputs &,
        AmbientOcclusionConstants, AmbientOcclusionOutputs &,
        AmbientOcclusionLayout *, const AmbientOcclusionLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
