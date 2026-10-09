#pragma once

#include "render_graph.hpp"
#include "view_state.hpp"
#include <hs/core/graphics_settings.hpp>

#include <DirectXMath.h>
#include <d3d12.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace hs
{
struct HdrPostSettings
{
    GraphicsMotionBlur motion_blur{GraphicsMotionBlur::Off};
    bool depth_of_field{}, color_grading{};
    float focus_distance_m{10.0f}, focal_length_m{0.05f}, f_number{2.8f};
    float sensor_width_m{0.036f}, max_coc_pixels{12.0f};
    DirectX::XMFLOAT4 grade_lift{0, 0, 0, 0};
    DirectX::XMFLOAT4 grade_contrast{1, 1, 1, 0};
    DirectX::XMFLOAT4 grade_gain{1, 1, 1, 0};
};
// b0/space17; matrix and camera position use the same camera-relative basis.
// Motion UV is previous unjittered UV minus current unjittered UV.
struct alignas(16) HdrPostConstants
{
    DirectX::XMFLOAT4X4 inverse_jittered_view_projection{};
    DirectX::XMFLOAT4 camera_relative_position{};
    DirectX::XMFLOAT4 camera_forward{};
    DirectX::XMFLOAT4 render_size{}, output_size{}, jitter_uv{};
    DirectX::XMFLOAT4 frame_parameters{}; // Actual render dt seconds, history valid, shutter ratio, grade sharpening.
    DirectX::XMFLOAT4 lens{}; // Focus distance, focal length, f-number, sensor width, all metres except f-number.
    DirectX::XMFLOAT4 grade_lift{}, grade_contrast{}, grade_gain{};
    DirectX::XMFLOAT4 effect_parameters{}; // Max CoC radius pixels, log shaper range, reference frame dt, shutter degrees.
    DirectX::XMUINT4 options{}; // Motion taps, transparent guides enabled, LUT edge, reserved.
    DirectX::XMFLOAT4X4 previous_unjittered_view_projection{}; // Absolute-world VP; sky projects directions with w=0.
};
static_assert(sizeof(HdrPostConstants) == 320);
static_assert(offsetof(HdrPostConstants, render_size) == 96);
static_assert(offsetof(HdrPostConstants, previous_unjittered_view_projection) == 256);
struct HdrPostResource { ResourceVersion version; ID3D12Resource *resource{}; };
struct HdrPostInputs
{
    HdrPostResource hdr, depth, motion, surface_history, normal;
    HdrPostResource transparent_depth, transparent_motion, transparent_history, transparent_classification;
    bool transparent_guides_enabled{};
    float sharpening{}; // Grade input only; zero when bloom already sharpened it.
    ViewExtent render_active, output_active;
    std::uint32_t frame_slot{};
    CompletionSet completed;
};
struct HdrPostOutputs
{
    ResourceVersion hdr;
    ID3D12Resource *hdr_resource{};
    D3D12_CPU_DESCRIPTOR_HANDLE hdr_srv{};
};
struct HdrPostTargets;
class HdrPostCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
  private:
    std::shared_ptr<HdrPostTargets> targets_;
    friend class HdrPostSystem;
};
class HdrPostSystem
{
  public:
    using ShaderReader = std::function<Result(std::string_view, std::vector<std::byte> &)>;
    HdrPostSystem();
    ~HdrPostSystem();
    HdrPostSystem(const HdrPostSystem &) = delete;
    HdrPostSystem &operator=(const HdrPostSystem &) = delete;
    [[nodiscard]] Result Prepare(ID3D12Device *, D3D12MA::Allocator *, ViewExtent render_capacity,
        ViewExtent output_capacity, const HdrPostSettings &, std::uint32_t frame_slot_count,
        const ShaderReader &, HdrPostCandidate &) const;
    [[nodiscard]] Result Commit(HdrPostCandidate &&, const CompletionSet &completed);
    // Pre-exposed output-resolution HDR. These passes never write temporal history.
    [[nodiscard]] Result RegisterMotionAndDoF(RenderGraphBuilder &, const HdrPostInputs &,
        HdrPostConstants, HdrPostOutputs &);
    // Invoke after bloom, on the same graph. Grade-only can be the first registration.
    [[nodiscard]] Result RegisterGrade(RenderGraphBuilder &, const HdrPostInputs &, HdrPostOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &, const CompletionSet &, bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // Called only after all old submissions have reliably completed.
    void SwapState(HdrPostSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
