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
struct alignas(16) ScreenReflectionsConstants
{
    DirectX::XMFLOAT4X4 inverse_view_projection{}; // Jittered NDC -> camera-relative world.
    DirectX::XMFLOAT4X4 world_to_view{}; // Rotation transforms positions relative to camera.
    DirectX::XMFLOAT4X4 projection{}; // Jittered view -> clip.
    DirectX::XMFLOAT4 render_size{}, trace_size{}, previous_trace_size{};
    DirectX::XMFLOAT4 previous_depth_parameters{}; // Previous P33,P34,P43,P44.
    DirectX::XMFLOAT4 jitter_uv{}; // Current xy, previous zw, logical viewport UV.
    DirectX::XMFLOAT4 trace_parameters{50.0f, 0.15f, 0.03f, 0.75f}; // Distance/thickness/bias m, roughness cutoff.
    DirectX::XMFLOAT4 filter_parameters{0.03f, 0.85f, 0.9f, 0.08f}; // Relative depth, normal cosine, blend, roughness delta.
    DirectX::XMUINT4 budget{}; // Rays, traversal iterations, spatial radius, max hierarchy level.
    DirectX::XMUINT4 frame_info{}; // History valid, frame index, hierarchy count, enabled.
    std::array<DirectX::XMUINT4, 16> hierarchy_levels{}; // Capacity width,height,element offset,reserved.
};
static_assert(sizeof(ScreenReflectionsConstants) == 592);
static_assert(offsetof(ScreenReflectionsConstants, hierarchy_levels) == 336);

struct ScreenReflectionResource
{
    ResourceVersion version;
    ID3D12Resource *resource{};
};
struct ScreenReflectionsInputs
{
    ScreenReflectionResource depth, base, normal, metadata, material, motion, surface_history;
    ScreenReflectionResource opaque_hdr, environment_specular, material_parameters;
    std::array<ScreenReflectionResource, 2> sidecars;
    std::uint32_t sidecar_plane_count{};
    std::uint64_t frame_constants_address{}; // Live FrameConstants b0/space0, held by completed frame slot.
    ViewExtent render_active;
    std::uint32_t frame_slot{};
    std::uint64_t frame_index{};
    CompletionSet completed;
    bool reset_history{};
    ScreenReflectionResource ambient_visibility;
    bool ambient_visibility_enabled{};
};
struct ScreenReflectionsOutputs
{
    ResourceVersion composed;
    ID3D12Resource *composed_resource{};
    D3D12_CPU_DESCRIPTOR_HANDLE composed_srv{};
};
struct ScreenReflectionsLayout
{
    bool valid{}, active{}, history{}, ambient{};
    ViewExtent render_capacity{},trace_capacity{};
    std::uint32_t hierarchy_elements{};
    std::uint32_t sidecars{};
    std::array<ResourceVersion, 13> guides{};
    std::array<ResourceHandle, 7> resources{};
    std::array<std::uint32_t, 3> passes{};
    ResourceVersion hierarchy, raw, current, validation, composed;
};
struct ScreenReflectionsTargets;
class ScreenReflectionsCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<ScreenReflectionsTargets> targets_;
    friend class ScreenReflectionsSystem;
};

// Screen-space reflection service. Initial opaque lighting and its exact existing
// indirect-specular contribution are separate graph inputs, so tracing cannot
// form a lighting feedback cycle or add a second ambient reflection term.
class ScreenReflectionsSystem
{
  public:
    using ShaderReader = std::function<Result(std::string_view, std::vector<std::byte> &)>;
    ScreenReflectionsSystem();
    ~ScreenReflectionsSystem();
    ScreenReflectionsSystem(const ScreenReflectionsSystem &) = delete;
    ScreenReflectionsSystem &operator=(const ScreenReflectionsSystem &) = delete;
    [[nodiscard]] Result Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator,
        ViewExtent render_capacity, GraphicsOptionalQuality quality, const render_data::QualityProfile &, std::uint32_t frame_slot_count,
        const ShaderReader &read_shader, ScreenReflectionsCandidate &candidate) const;
    [[nodiscard]] Result Commit(ScreenReflectionsCandidate &&candidate, const CompletionSet &completed);
    [[nodiscard]] Result Register(RenderGraphBuilder &graph, const ScreenReflectionsInputs &inputs,
        ScreenReflectionsConstants constants, ScreenReflectionsOutputs &outputs,
        ScreenReflectionsLayout *layout = nullptr);
    [[nodiscard]] Result BindRetained(RenderGraphBuilder &, const ScreenReflectionsInputs &,
        ScreenReflectionsConstants, const ScreenReflectionsLayout &, ScreenReflectionsOutputs &);
    [[nodiscard]] bool MatchesLayout(const ScreenReflectionsInputs &,const ScreenReflectionsLayout &)const noexcept;
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &graph,
        const CompletionSet &completion, bool success);
    void ResetHistory() noexcept;
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // Called only after all old submissions have reliably completed.
    void SwapState(ScreenReflectionsSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    [[nodiscard]] Result RegisterFrame(RenderGraphBuilder &, const ScreenReflectionsInputs &,
        ScreenReflectionsConstants, ScreenReflectionsOutputs &,
        ScreenReflectionsLayout *, const ScreenReflectionsLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
