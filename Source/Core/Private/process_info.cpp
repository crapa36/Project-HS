#include <hs/core/process_info.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Psapi.h>

#include <array>

namespace hs
{
namespace
{

[[nodiscard]] std::filesystem::path ResolveExecutablePath()
{
    std::array<wchar_t, 32'768> path{};
    const auto length =
        GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size())
    {
        return {};
    }
    return std::filesystem::path(path.data());
}

} // namespace

std::filesystem::path CurrentExecutablePath()
{
    return ResolveExecutablePath();
}

std::filesystem::path CurrentExecutableDirectory()
{
    return ResolveExecutablePath().parent_path();
}

std::uint64_t CurrentProcessWorkingSetBytes() noexcept
{
    PROCESS_MEMORY_COUNTERS counters{.cb = sizeof(counters)};
    return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))
               ? static_cast<std::uint64_t>(counters.WorkingSetSize)
               : 0;
}

} // namespace hs
