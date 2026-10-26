#include <hs/core/shipped_content_owner.hpp>
#include <hs/core/sha256.hpp>

#include <algorithm>
#include <limits>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hs
{
namespace
{
Result IoFailure(const char *operation, unsigned long code)
{
    return Result::Failure(ErrorCode::TaskFailed, "ShippedContentOwner",
                           std::string(operation) + " failed (native error " + std::to_string(code) + ")");
}
Result ShortRead()
{
    return Result::Failure(ErrorCode::TaskFailed, "ShippedContentOwner", "unexpected end of content file");
}
Result CorruptRead()
{
    return Result::Failure(ErrorCode::InvalidArgument, "ShippedContentOwner", "content chunk digest mismatch");
}
#ifdef _WIN32
struct Event
{
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Event() { if (handle) CloseHandle(handle); }
};
#endif
} // namespace

struct ShippedContentOwner::Impl
{
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    ~Impl() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
#else
    int file = -1;
    ~Impl() { if (file >= 0) close(file); }
#endif
    std::uint64_t size{};
    ContentFileIdentity identity{};
};

ShippedContentOwner::ShippedContentOwner() noexcept = default;
ShippedContentOwner::~ShippedContentOwner() = default;
ShippedContentOwner::ShippedContentOwner(ShippedContentOwner &&) noexcept = default;
ShippedContentOwner &ShippedContentOwner::operator=(ShippedContentOwner &&) noexcept = default;

Result ShippedContentOwner::Open(const std::filesystem::path &path, ShippedContentOwner &output)
{
    auto candidate = std::make_unique<Impl>();
#ifdef _WIN32
    candidate->file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (candidate->file == INVALID_HANDLE_VALUE) return IoFailure("CreateFileW", GetLastError());
    if (GetFileType(candidate->file) != FILE_TYPE_DISK)
        return Result::Failure(ErrorCode::InvalidArgument, "ShippedContentOwner", "content must be a regular file");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(candidate->file, &info))
        return IoFailure("GetFileInformationByHandle", GetLastError());
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return Result::Failure(ErrorCode::InvalidArgument, "ShippedContentOwner", "content must be a regular file");
    candidate->size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (candidate->size > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
        return Result::Failure(ErrorCode::InvalidArgument, "ShippedContentOwner", "content file size exceeds native range");
    candidate->identity = {info.dwVolumeSerialNumber,
                          (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
#else
    // POSIX has no mandatory equivalent of Windows' write-sharing exclusion.
    // The immutable-publish contract is required here; pread never moves a
    // shared file cursor, and a replaced pathname does not change this inode.
    candidate->file = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (candidate->file < 0) return IoFailure("open", static_cast<unsigned long>(errno));
    struct stat info{};
    if (fstat(candidate->file, &info) != 0) return IoFailure("fstat", static_cast<unsigned long>(errno));
    if (!S_ISREG(info.st_mode) || info.st_size < 0)
        return Result::Failure(ErrorCode::InvalidArgument, "ShippedContentOwner", "content must be a regular file");
    candidate->size = static_cast<std::uint64_t>(info.st_size);
    candidate->identity = {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino)};
#endif
    output.impl_ = std::move(candidate);
    return Result::Success();
}

std::uint64_t ShippedContentOwner::Size() const noexcept { return impl_ ? impl_->size : 0; }

std::optional<ContentFileIdentity> ShippedContentOwner::Identity() const noexcept
{
    return impl_ ? std::optional{impl_->identity} : std::nullopt;
}

Result ShippedContentOwner::ReadExact(std::uint64_t offset, std::span<std::byte> output) const
{
    return Read(offset, output, nullptr);
}

Result ShippedContentOwner::ReadVerified(std::uint64_t offset, std::span<std::byte> output,
                                        const std::array<std::byte, 32> &expected_digest) const
{
    return Read(offset, output, &expected_digest);
}

Result ShippedContentOwner::Read(std::uint64_t offset, std::span<std::byte> output,
                                const std::array<std::byte, 32> *expected_digest) const
{
    if (!impl_)
        return Result::Failure(ErrorCode::InvalidState, "ShippedContentOwner", "content file is not open");
    if (offset > impl_->size || output.size() > impl_->size - offset)
        return Result::Failure(ErrorCode::InvalidArgument, "ShippedContentOwner", "read exceeds content file bounds");
    if (output.empty())
        return expected_digest && Sha256(output) != *expected_digest ? CorruptRead() : Result::Success();
    Sha256Stream digest;
#ifdef _WIN32
    Event event;
    if (!event.handle) return IoFailure("CreateEventW", GetLastError());
#endif
    std::size_t done = 0;
    while (done < output.size())
    {
        // Bounded requests fit both DWORD and POSIX ssize_t on supported hosts.
        const auto count = std::min<std::size_t>(output.size() - done, 1u << 30);
        const auto position = offset + done;
#ifdef _WIN32
        if (!ResetEvent(event.handle)) return IoFailure("ResetEvent", GetLastError());
        OVERLAPPED request{};
        request.Offset = static_cast<DWORD>(position);
        request.OffsetHigh = static_cast<DWORD>(position >> 32);
        request.hEvent = event.handle;
        DWORD transferred{};
        if (!ReadFile(impl_->file, output.data() + done, static_cast<DWORD>(count), &transferred, &request))
        {
            const auto error = GetLastError();
            if (error == ERROR_HANDLE_EOF) return ShortRead();
            if (error != ERROR_IO_PENDING) return IoFailure("ReadFile", error);
        }
        if (!GetOverlappedResult(impl_->file, &request, &transferred, TRUE))
        {
            const auto error = GetLastError();
            if (error == ERROR_HANDLE_EOF) return ShortRead();
            return IoFailure("GetOverlappedResult", error);
        }
        if (transferred == 0) return ShortRead();
        if (expected_digest) digest.Add(output.subspan(done, transferred));
        done += transferred;
#else
        if (position > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
            return Result::Failure(ErrorCode::InvalidArgument, "ShippedContentOwner", "read offset exceeds native range");
        const auto transferred = pread(impl_->file, output.data() + done, count, static_cast<off_t>(position));
        if (transferred < 0)
        {
            if (errno == EINTR) continue;
            return IoFailure("pread", static_cast<unsigned long>(errno));
        }
        if (transferred == 0) return ShortRead();
        if (expected_digest) digest.Add(output.subspan(done, static_cast<std::size_t>(transferred)));
        done += static_cast<std::size_t>(transferred);
#endif
    }
    return expected_digest && digest.Finish() != *expected_digest ? CorruptRead() : Result::Success();
}
} // namespace hs
