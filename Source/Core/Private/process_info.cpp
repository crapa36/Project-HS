#include <hs/core/process_info.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Psapi.h>
#elif defined(__linux__)
#include <cstdio>
#include <unistd.h>
#else
#error Unsupported process-info platform
#endif

#include <array>

namespace hs
{
namespace
{

[[nodiscard]] std::filesystem::path ResolveExecutablePath()
{
#if defined(_WIN32)
    std::array<wchar_t, 32'768> path{};
    const auto length =
        GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size())
    {
        return {};
    }
    return std::filesystem::path(path.data());
#else
    std::error_code error;
    auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : path;
#endif
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
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{.cb = sizeof(counters)};
    return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))
               ? static_cast<std::uint64_t>(counters.WorkingSetSize)
               : 0;
#else
    auto *statm = std::fopen("/proc/self/statm", "r");
    if (!statm) return 0;
    unsigned long long size{}, resident{};
    const auto fields = std::fscanf(statm, "%llu %llu", &size, &resident);
    std::fclose(statm);
    const auto page_size = sysconf(_SC_PAGESIZE);
    return fields == 2 && page_size > 0
               ? resident * static_cast<std::uint64_t>(page_size)
               : 0;
#endif
}

} // namespace hs
