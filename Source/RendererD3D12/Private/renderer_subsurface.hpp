#pragma once

#include "render_graph.hpp"
#include "view_state.hpp"

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
// b0/space13; camera-relative reconstruction uses row-vector HLSL convention.
// Bases must select the same immutable material table as the surface producer.
struct alignas(16) SubsurfaceConstants
{
    DirectX::XMFLOAT4X4 inverse_view_projection{};
    DirectX::XMFLOAT4 render_size{};
    DirectX::XMUINT4 material_parameter_bases[4]{};
    DirectX::XMUINT4 hero_material_parameter_bases[2]{};
    DirectX::XMUINT4 quality_options{}; // x: maximum half-kernel samples.
};
static_assert(sizeof(SubsurfaceConstants) == 192);
static_assert(offsetof(SubsurfaceConstants, material_parameter_bases) == 80);
static_assert(offsetof(SubsurfaceConstants, hero_material_parameter_bases) == 144);
static_assert(offsetof(SubsurfaceConstants, quality_options) == 176);

struct SubsurfaceGuide { ResourceVersion version; ID3D12Resource *resource{}; };
struct SubsurfaceInputs
{
    SubsurfaceGuide depth, normal, metadata, diffuse, hdr, material_parameters;
    ViewExtent render_active;
    std::uint32_t frame_slot{};
    CompletionSet completed;
};
struct SubsurfaceOutputs
{
    ResourceVersion corrected;
    ID3D12Resource *corrected_resource{};
    D3D12_CPU_DESCRIPTOR_HANDLE corrected_srv{};
};
struct SubsurfaceLayout
{
    bool valid{}, enabled{};
    ViewExtent capacity{};
    std::array<ResourceVersion, 6> guides{};
    std::array<TextureHandle, 2> textures{};
    std::array<ResourceVersion, 2> written{};
    std::array<std::uint32_t, 2> passes{};
};
struct SubsurfaceTargets;
class SubsurfaceCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<SubsurfaceTargets> targets_;
    friend class SubsurfaceSystem;
};

// Native two-pass RGB screen-space diffusion. Caller selects enabled from
// resident authored capability, retains all imported resources, waits the frame
// slot, and completes every attempted graph submission, including failure.
class SubsurfaceSystem
{
  public:
    using ShaderReader = std::function<Result(std::string_view, std::vector<std::byte> &)>;
    SubsurfaceSystem();
    ~SubsurfaceSystem();
    SubsurfaceSystem(const SubsurfaceSystem &) = delete;
    SubsurfaceSystem &operator=(const SubsurfaceSystem &) = delete;
    [[nodiscard]] Result Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator,
        ViewExtent render_capacity, bool enabled, std::uint32_t frame_slot_count,
        const ShaderReader &read_shader, SubsurfaceCandidate &candidate) const;
    [[nodiscard]] Result Commit(SubsurfaceCandidate &&candidate, const CompletionSet &completed);
    [[nodiscard]] Result Register(RenderGraphBuilder &graph, const SubsurfaceInputs &inputs,
        SubsurfaceConstants constants, SubsurfaceOutputs &outputs, SubsurfaceLayout *layout = nullptr);
    [[nodiscard]] Result BindRetained(RenderGraphBuilder &, const SubsurfaceInputs &,
        SubsurfaceConstants, const SubsurfaceLayout &, SubsurfaceOutputs &);
    [[nodiscard]] bool MatchesLayout(const SubsurfaceInputs &, const SubsurfaceLayout &) const noexcept;
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &graph,
        const CompletionSet &completion, bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // Called only after all old submissions have reliably completed.
    void SwapState(SubsurfaceSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    [[nodiscard]] Result RegisterFrame(RenderGraphBuilder &, const SubsurfaceInputs &,
        SubsurfaceConstants, SubsurfaceOutputs &, SubsurfaceLayout *, const SubsurfaceLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
