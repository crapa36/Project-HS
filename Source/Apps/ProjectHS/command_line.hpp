#pragma once

#include <hs/runtime/application.hpp>

#include <span>
#include <string_view>

namespace hs
{

// args contains command-line arguments after the executable name.
[[nodiscard]] ApplicationConfig ParseProjectHsArguments(
    std::span<const std::string_view> args);

} // namespace hs
