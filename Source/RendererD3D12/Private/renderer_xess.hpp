#pragma once

#include "reconstruction_plan.hpp"

#include <memory>
#include <string_view>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace hs
{
struct XessReconstructionFlags
{
    bool inverted_depth{};
    bool responsive_pixel_mask{true};
    bool exposure_scale_texture{true};
};

struct XessInputExtent
{
    ViewExtent optimal;
    ViewExtent minimum;
    ViewExtent maximum;
};

struct XessRect
{
    std::uint32_t x{}, y{}, width{}, height{};
};

struct XessReconstructionInputs
{
    // Scene-linear Rec.709 RGBA16F. The SDK output retains the input's radiometric
    // meaning: raw input + exposure texture E returns raw output; pre-exposed
    // input requires the desired model exposure divided by that pre-exposure.
    ID3D12Resource *pre_exposed_color{};
    // Forward or inverted device depth matching initialization. R32_TYPELESS
    // with the engine's D32 view, D32_FLOAT or R32_FLOAT; low-res MV requires it.
    ID3D12Resource *depth{};
    // Undilated RG16F, current -> previous unjittered viewport UV. Include object,
    // camera and rotational sky motion. motion_scale converts UV to output pixels.
    ID3D12Resource *motion{};
    // R8_UNORM/R16_FLOAT/R32_FLOAT, [0,1], in unjittered input coordinates.
    ID3D12Resource *responsive{};
    // 1x1 R32_FLOAT, NON_PIXEL_SHADER_RESOURCE. Contains the input color scaling
    // factor (Ecurrent for raw scene color), not Ecurrent/Eprevious. The SDK
    // restores the inverse factor on output. Required when its init flag is set.
    ID3D12Resource *exposure_scale_texture{};
    // RGBA16F with ALLOW_UNORDERED_ACCESS. Alpha is SDK-owned, not depth history.
    ID3D12Resource *output{};
    XessRect active_rect;
    XessRect output_rect;
    float jitter_pixels[2]{}; // Projection shear X*2/W, -Y*2/H, each [-0.5,0.5].
    bool reset_history{};
    // Used only without an exposure texture: desired model exposure divided by
    // input pre-exposure. No SDK auto exposure is enabled.
    float exposure_scale{1.0f};
    float motion_scale[2]{}; // output width, output height for the UV convention.
};

// Concrete D3D12 XeSS-SR adapter. All methods must run on the initialization
// thread. Execute records into a normal Direct graph pass; the graph owns barriers,
// submission, input/output leases and completion. Do not precede it with engine TAA.
// After Execute, drain the old graph uses before reinitialization or destruction.
class XessReconstruction
{
  public:
    XessReconstruction();
    ~XessReconstruction();
    XessReconstruction(const XessReconstruction &) = delete;
    XessReconstruction &operator=(const XessReconstruction &) = delete;

    // Creates only a device query context. No pipelines, initialization, GPU
    // resources or per-view history. Query qualities, then ResetAfterCompletion.
    [[nodiscard]] Result ProbeDevice(ID3D12Device *device);
    [[nodiscard]] Result Initialize(ID3D12Device *device, ViewExtent output,
                                    const GraphicsModeId &quality,
                                    XessReconstructionFlags flags = {});
    [[nodiscard]] Result QueryInputExtent(const GraphicsModeId &quality, ViewExtent output,
                                         XessInputExtent &extent);
    // Inputs enter NON_PIXEL_SHADER_RESOURCE; output enters UNORDERED_ACCESS.
    // The SDK can bind its own descriptor heap/root signature/PSOs. Every later
    // graph callback must bind its own command-list state as usual.
    [[nodiscard]] Result Execute(ID3D12GraphicsCommandList *commands,
                                 const XessReconstructionInputs &inputs);
    // Caller guarantees that every command list referencing this context has
    // completed (or device teardown has made its work impossible).
    [[nodiscard]] Result ResetAfterCompletion();
    [[nodiscard]] ReconstructionAvailability Availability() const noexcept;
    [[nodiscard]] std::string_view FailureReason() const noexcept;
    [[nodiscard]] std::string_view DriverWarning() const noexcept;
    [[nodiscard]] bool Initialized() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
