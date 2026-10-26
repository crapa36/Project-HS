#pragma once

#include "render_graph.hpp"

#include <DirectXMath.h>
#include <d3d12.h>

#include <array>
#include <cstddef>
#include <memory>
#include <span>

namespace hs
{
struct LightingProbeStorage;
class LightingProbeCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<LightingProbeStorage> storage_;
    friend class LightingProbeSystem;
};
struct LightingProbeOutputs
{
    ResourceVersion radiance, prefiltered, irradiance;
    ID3D12Resource *radiance_resource{}, *prefiltered_resource{}, *irradiance_resource{};
    // Copy these immutable CPU descriptors into the existing native table's
    // space15 range t0/t1/t2. No additional root parameter is needed.
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE,3> srvs{};
    DirectX::XMFLOAT4 options{}; // cosYaw,sinYaw,prefilter maximum mip,enabled.
};
struct LightingProbeLayout
{
    std::uint64_t storage_generation{};
    std::array<ResourceHandle,4> imports{}; // cubes, SH, optional staging
    std::array<ResourceVersion,3> outputs{};
    std::uint32_t upload_pass_index{};
    bool upload{};
};

// One immutable scene probe. Preparation validates and stages package bytes but
// submits no work. The first registered graph uploads the actual HDR cubes/SH;
// subsequent graphs import their exact native state and completion requirements.
// Renderer consumers must declare reads of the returned ResourceVersions and
// call CompleteSubmission even after partial/failed graph submission. Replacement
// and shutdown require completed leases. Resize does not recreate this probe.
class LightingProbeSystem
{
  public:
    LightingProbeSystem();
    ~LightingProbeSystem();
    LightingProbeSystem(const LightingProbeSystem &)=delete;
    LightingProbeSystem &operator=(const LightingProbeSystem &)=delete;
    [[nodiscard]] Result Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,
        std::span<const std::byte> package_bytes,LightingProbeCandidate &candidate) const;
    [[nodiscard]] Result Commit(LightingProbeCandidate &&candidate,const CompletionSet &completed);
    [[nodiscard]] Result Register(RenderGraphBuilder &graph,const CompletionSet &completed,
        LightingProbeOutputs &outputs,LightingProbeLayout *layout=nullptr);
    [[nodiscard]] bool MatchesLayout(const LightingProbeLayout &) const noexcept;
    [[nodiscard]] Result BindRetained(RenderGraphBuilder &,const CompletionSet &,
        const LightingProbeLayout &,LightingProbeOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &graph,
        const CompletionSet &completion,bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] DirectX::XMFLOAT4 FrameOptions() const noexcept;
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // Called only after all old submissions have reliably completed.
    void SwapState(LightingProbeSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    [[nodiscard]] Result RegisterImpl(RenderGraphBuilder &,const CompletionSet &,
        LightingProbeOutputs &,LightingProbeLayout *,const LightingProbeLayout *);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
