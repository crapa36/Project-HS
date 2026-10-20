#pragma once
#include <FidelityFX/host/ffx_interface.h>
#include <FidelityFX/host/ffx_brixelizer_raw.h>
#include <d3d12.h>
#include <hs/core/result.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace hs::brixelizer_native
{
using ShaderReader=std::function<Result(std::string_view,std::vector<std::byte> &)>;
struct BackendSlots;
struct BackendSlotsDelete { void operator()(BackendSlots *) const noexcept; };
using BackendSlotsOwner=std::unique_ptr<BackendSlots,BackendSlotsDelete>;
[[nodiscard]] Result CreateSlots(FfxInterface &,FfxBrixelizerRawContext *,std::uint32_t frame_slots,BackendSlotsOwner &);
// Caller has waited this exact slot. BeginFrame is called once per submitted
// graph and BeginBatch once before each official update/dispatch invocation.
[[nodiscard]] Result BeginFrame(BackendSlots &,std::uint32_t slot);
[[nodiscard]] Result BeginBatch(BackendSlots &);
[[nodiscard]] Result BatchResult(const BackendSlots &);
void EndBatch(BackendSlots &) noexcept;
[[nodiscard]] std::uint64_t BackendGpuBytes(const BackendSlots &) noexcept;
[[nodiscard]] std::uint64_t NativeResourceBytes(const FfxInterface &) noexcept;
class ShaderReadScope
{
  public:
    explicit ShaderReadScope(const ShaderReader &);
    ~ShaderReadScope();
  private:
    const ShaderReader *previous_{};
};
// Installs the selected-Generation blob callback. The pinned native table owns
// all reflection/binding metadata; selected bytes must equal its actual DXIL.
void RequireSelectedShaderProducts(FfxInterface &) noexcept;
} // namespace hs::brixelizer_native
