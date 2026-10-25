#pragma once
#include "d3d12_resources.hpp"
#include "view_state.hpp"
#include <hs/renderer/vfx_screen_input.hpp>
#include <DirectXMath.h>
#include <functional>
#include <memory>
#include <string_view>

namespace hs
{
class RendererPipelineCache;
struct ScreenOverlayTargets;
class ScreenOverlayCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<ScreenOverlayTargets> targets_;
    bool prepared_{};
    friend class ScreenOverlaySystem;
};
struct ScreenOverlayInputs
{
    VfxScreenInputs commands;
    ResourceVersion gradient;
    ID3D12Resource *gradient_resource{};
    ViewExtent render_active, output_active;
    DirectX::XMFLOAT3 camera_right{}, camera_up{};
    float intensity{};
    std::uint32_t frame_slot{};
    CompletionSet completed;
};
struct ScreenOverlayOutputs
{
    ResourceVersion reactive, upload;
    ID3D12Resource *reactive_resource{};
    D3D12_CPU_DESCRIPTOR_HANDLE reactive_srv{};
    bool active{};
};
// The analytic alpha guide is generated before reconstruction. Color is drawn
// after scene postprocessing and before UI, outside every temporal history.
class ScreenOverlaySystem
{
  public:
    using ShaderReader=std::function<Result(std::string_view,std::vector<std::byte>&)>;
    [[nodiscard]] Result Prepare(ID3D12Device *,D3D12MA::Allocator *,ViewExtent,
        std::uint32_t,const ShaderReader &,bool enabled,RendererPipelineCache &,ScreenOverlayCandidate &) const;
    [[nodiscard]] Result Commit(ScreenOverlayCandidate &&,const CompletionSet &);
    [[nodiscard]] Result RegisterMask(RenderGraphBuilder &,const ScreenOverlayInputs &,ScreenOverlayOutputs &);
    void Draw(ID3D12GraphicsCommandList *,std::uint32_t,D3D12_CPU_DESCRIPTOR_HANDLE) const;
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    void SwapState(ScreenOverlaySystem &other) noexcept { targets_.swap(other.targets_); }
  private:
    std::shared_ptr<ScreenOverlayTargets> targets_;
    const RenderGraphBuilder *pending_graph_{};
    ResourceHandle pending_mask_,pending_upload_;
    std::uint32_t pending_slot_{};
};
}
