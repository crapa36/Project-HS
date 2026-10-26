#pragma once

#include <hs/core/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

namespace hs
{
struct ContentFileIdentity
{
    std::uint64_t volume{};
    std::uint64_t file{};
    bool operator==(const ContentFileIdentity &) const = default;
};

// Owns one opened content file, not its format or decoded domain objects.
// Concurrent reads are supported while this owner remains alive and unmoved.
// Publishers must replace files rather than modify opened content in place.
class ShippedContentOwner
{
  public:
    ShippedContentOwner() noexcept;
    ~ShippedContentOwner();
    ShippedContentOwner(ShippedContentOwner &&) noexcept;
    ShippedContentOwner &operator=(ShippedContentOwner &&) noexcept;
    ShippedContentOwner(const ShippedContentOwner &) = delete;
    ShippedContentOwner &operator=(const ShippedContentOwner &) = delete;

    // Failure preserves output's previously opened file.
    [[nodiscard]] static Result Open(const std::filesystem::path &path, ShippedContentOwner &output);
    [[nodiscard]] std::uint64_t Size() const noexcept;
    // Captured from the opened file; absent for an unopened or moved-from owner.
    [[nodiscard]] std::optional<ContentFileIdentity> Identity() const noexcept;
    // The span may be partially written on an I/O error; only success promises
    // all requested bytes. An empty read at EOF is valid for an open owner.
    [[nodiscard]] Result ReadExact(std::uint64_t offset, std::span<std::byte> output) const;
    // Hashes the same read stream. Only success permits consuming the bytes;
    // failure may leave partially read or unverified bytes in the destination.
    [[nodiscard]] Result ReadVerified(std::uint64_t offset, std::span<std::byte> output,
                                     const std::array<std::byte, 32> &expected_digest) const;

  private:
    [[nodiscard]] Result Read(std::uint64_t offset, std::span<std::byte> output,
                              const std::array<std::byte, 32> *expected_digest) const;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
