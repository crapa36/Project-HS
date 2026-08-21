#pragma once

#include <cstdint>
#include <filesystem>

namespace hs
{

[[nodiscard]] std::filesystem::path CurrentExecutablePath();
[[nodiscard]] std::filesystem::path CurrentExecutableDirectory();
[[nodiscard]] std::uint64_t CurrentProcessWorkingSetBytes() noexcept;

} // namespace hs
