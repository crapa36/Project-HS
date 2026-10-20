#include <hs/asset_manifest/generation_lock.hpp>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hs
{
struct GenerationFileLock::State
{
    bool locked{};
#ifdef _WIN32
    HANDLE handle{INVALID_HANDLE_VALUE};
    // Beyond manifest contents: coordination must not block ordinary JSON I/O.
    OVERLAPPED range{};
    State() { range.OffsetHigh = 1; }
    ~State()
    {
        if (locked) UnlockFileEx(handle, 0, 1, 0, &range);
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
#else
    int handle{-1};
    ~State()
    {
        if (locked) flock(handle, LOCK_UN);
        if (handle >= 0) close(handle);
    }
#endif
};

GenerationFileLock::GenerationFileLock() noexcept = default;
GenerationFileLock::~GenerationFileLock() = default;
GenerationFileLock::GenerationFileLock(GenerationFileLock &&) noexcept = default;
GenerationFileLock &GenerationFileLock::operator=(GenerationFileLock &&) noexcept = default;
bool GenerationFileLock::OwnsLock() const noexcept { return state_ && state_->locked; }

GenerationFileLock::GenerationFileLock(const std::filesystem::path &path, Mode mode, bool create)
    : state_(std::make_unique<State>())
{
#ifdef _WIN32
    state_->handle = CreateFileW(path.c_str(), GENERIC_READ | (create ? GENERIC_WRITE : 0),
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (state_->handle == INVALID_HANDLE_VALUE)
        throw std::system_error(GetLastError(), std::system_category(), "open Generation lock");
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(state_->handle, &information))
        throw std::system_error(GetLastError(), std::system_category(), "inspect Generation lock");
    if (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Generation lock must be a regular non-reparse file");
    DWORD flags = mode == Mode::Shared ? 0 : LOCKFILE_EXCLUSIVE_LOCK;
    if (mode == Mode::TryExclusive) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    if (!LockFileEx(state_->handle, flags, 0, 1, 0, &state_->range))
    {
        const auto error = GetLastError();
        if (mode == Mode::TryExclusive && error == ERROR_LOCK_VIOLATION) return;
        throw std::system_error(error, std::system_category(), "lock Generation file");
    }
#else
    state_->handle = open(path.c_str(), (create ? O_RDWR | O_CREAT : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (state_->handle < 0) throw std::system_error(errno, std::generic_category(), "open Generation lock");
    struct stat information{};
    if (fstat(state_->handle, &information) != 0)
        throw std::system_error(errno, std::generic_category(), "inspect Generation lock");
    if (!S_ISREG(information.st_mode)) throw std::runtime_error("Generation lock must be regular");
    int flags = mode == Mode::Shared ? LOCK_SH : LOCK_EX;
    if (mode == Mode::TryExclusive) flags |= LOCK_NB;
    int result;
    do { result = flock(state_->handle, flags); } while (result != 0 && errno == EINTR);
    if (result != 0)
    {
        if (mode == Mode::TryExclusive && (errno == EWOULDBLOCK || errno == EAGAIN)) return;
        throw std::system_error(errno, std::generic_category(), "lock Generation file");
    }
#endif
    state_->locked = true;
    // GC/repair may have retired the path between open and lock acquisition.
    // Never protect an old inode while resolving products from its replacement.
    if (mode == Mode::Shared)
    {
#ifdef _WIN32
        const auto current = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (current == INVALID_HANDLE_VALUE)
            throw std::system_error(GetLastError(), std::system_category(), "reopen Generation manifest");
        BY_HANDLE_FILE_INFORMATION current_information{};
        const bool inspected = GetFileInformationByHandle(current, &current_information) != 0;
        const auto error = GetLastError();
        CloseHandle(current);
        if (!inspected) throw std::system_error(error, std::system_category(), "reinspect Generation manifest");
        if (information.dwVolumeSerialNumber != current_information.dwVolumeSerialNumber ||
            information.nFileIndexHigh != current_information.nFileIndexHigh ||
            information.nFileIndexLow != current_information.nFileIndexLow)
            throw std::runtime_error("Generation manifest retired during selection");
#else
        struct stat current_information{};
        if (lstat(path.c_str(), &current_information) != 0)
            throw std::system_error(errno, std::generic_category(), "reinspect Generation manifest");
        if (information.st_dev != current_information.st_dev || information.st_ino != current_information.st_ino)
            throw std::runtime_error("Generation manifest retired during selection");
#endif
    }
}
} // namespace hs
