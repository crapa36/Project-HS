#include "renderer_diagnostics.hpp"

#include <cstdint>
#include <format>

namespace hs
{

Result HResultFailure(std::string_view operation, HRESULT result)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                           std::format("{} failed: 0x{:08X}", operation,
                                       static_cast<std::uint32_t>(result)));
}

} // namespace hs
