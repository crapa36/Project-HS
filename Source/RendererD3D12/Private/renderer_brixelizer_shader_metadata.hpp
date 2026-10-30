#pragma once
#include <FidelityFX/host/ffx_types.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace hs::brixelizer_native::detail
{
// Defined only by the generated metadata source in hs_brixelizer_native.
[[nodiscard]] const FfxShaderBlob *FindShaderMetadata(std::uint32_t effect,std::uint32_t pass,
    std::uint32_t permutation,std::array<std::byte,32> &digest) noexcept;
}
