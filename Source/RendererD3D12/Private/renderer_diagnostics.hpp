#pragma once

#include <hs/core/result.hpp>

#include <Windows.h>

#include <string_view>

namespace hs
{

[[nodiscard]] Result HResultFailure(std::string_view operation, HRESULT result);

} // namespace hs
