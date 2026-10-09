#pragma once

#include <hs/runtime/application.hpp>

#include <span>
#include <string_view>

namespace hs
{

struct SettingsDocument;

// args contains command-line arguments after the executable name.
[[nodiscard]] ApplicationConfig ParseProjectHsArguments(
    std::span<const std::string_view> args,
    const SettingsDocument *saved_settings = nullptr);

} // namespace hs
