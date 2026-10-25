#include <hs/core/shipped_content_owner.hpp>
#include <hs/core/sha256.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
struct TestDirectory
{
    std::filesystem::path path;
    TestDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 32; ++attempt)
        {
            auto candidate = std::filesystem::temp_directory_path() /
                ("hs-shipped-content-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) { path = std::move(candidate); return; }
        }
        throw std::runtime_error("cannot create shipped-content test directory");
    }
    ~TestDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
void Write(const std::filesystem::path &path, std::span<const std::byte> bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    Check(bool(file), "cannot create shipped-content fixture");
    if (!bytes.empty()) file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    Check(bool(file), "cannot write shipped-content fixture");
}
} // namespace

void TestShippedContentOwner()
{
    static_assert(!std::is_copy_constructible_v<hs::ShippedContentOwner>);
    static_assert(std::is_nothrow_move_constructible_v<hs::ShippedContentOwner>);
    static_assert(std::is_nothrow_move_assignable_v<hs::ShippedContentOwner>);
    TestDirectory directory;
    const auto path = directory.path / "content.bin";
    std::vector<std::byte> expected(4097);
    for (std::size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<std::byte>((i * 37 + i / 251) & 255);
    Write(path, expected);

    hs::ShippedContentOwner owner;
    Check(owner.Size() == 0 && !owner.ReadExact(0, {}).Succeeded(), "unopened owner must reject reads");
    Check(hs::ShippedContentOwner::Open(path, owner).Succeeded(), "content open failed");
    Check(owner.Size() == expected.size(), "content size differs from the opened file");
    std::vector<std::byte> actual(expected.size());
    Check(owner.ReadExact(0, actual).Succeeded() && actual == expected, "full content bytes differ");
    const auto expected_digest = hs::Sha256(expected);
    Check(owner.ReadVerified(0, actual, expected_digest).Succeeded() && actual == expected,
          "verified read must return the expected bytes");
    auto wrong_digest = expected_digest;
    wrong_digest[0] ^= std::byte{1};
    Check(!owner.ReadVerified(0, actual, wrong_digest).Succeeded(), "wrong chunk digest must fail");
    std::array<std::byte, 97> slice{};
    Check(owner.ReadExact(2011, slice).Succeeded() &&
              std::equal(slice.begin(), slice.end(), expected.begin() + 2011), "offset read differs");
    Check(owner.ReadExact(owner.Size(), {}).Succeeded(), "empty read at EOF must succeed");
    Check(owner.ReadVerified(owner.Size(), {}, hs::Sha256({})).Succeeded(), "empty verified chunk must match empty digest");
    Check(!owner.ReadVerified(owner.Size(), {}, expected_digest).Succeeded(), "empty chunk must still verify its digest");
    Check(!owner.ReadExact(owner.Size() + 1, {}).Succeeded(), "empty read beyond EOF must fail");
    Check(!owner.ReadExact(owner.Size(), slice).Succeeded(), "nonempty read at EOF must fail");
    Check(!owner.ReadExact(std::numeric_limits<std::uint64_t>::max(), slice).Succeeded(), "overflowing read range must fail");
    Check(!hs::ShippedContentOwner::Open(directory.path / "missing.bin", owner).Succeeded(), "missing file must fail");
    Check(!hs::ShippedContentOwner::Open(directory.path, owner).Succeeded(), "directory must not open as content");
    Check(owner.Size() == expected.size() && owner.ReadExact(0, actual).Succeeded() && actual == expected,
          "failed reopen must preserve the previous owner");
    const auto corrupt_path = directory.path / "corrupt.bin";
    auto corrupt = expected;
    corrupt[2048] ^= std::byte{1};
    Write(corrupt_path, corrupt);
    hs::ShippedContentOwner corrupt_owner;
    Check(hs::ShippedContentOwner::Open(corrupt_path, corrupt_owner).Succeeded() &&
          !corrupt_owner.ReadVerified(0, actual, expected_digest).Succeeded(),
          "same-size chunk corruption must fail before consumption");

    hs::ShippedContentOwner moved(std::move(owner));
    Check(owner.Size() == 0 && !owner.ReadExact(0, {}).Succeeded(), "moved-from owner must be empty");
    const auto empty_path = directory.path / "empty.bin";
    Write(empty_path, {});
    Check(hs::ShippedContentOwner::Open(empty_path, owner).Succeeded() && owner.Size() == 0 &&
              owner.ReadExact(0, {}).Succeeded(), "empty file must support empty exact reads");
    owner = std::move(moved);
    Check(moved.Size() == 0 && owner.ReadExact(0, actual).Succeeded() && actual == expected,
          "move assignment must replace the prior file and retain exact bytes");

    std::atomic<bool> reads_ok{true};
    {
        std::vector<std::jthread> readers;
        for (std::size_t worker = 0; worker < 4; ++worker)
            readers.emplace_back([&, worker] {
                std::array<std::byte, 113> buffer{};
                for (std::size_t iteration = 0; iteration < 64; ++iteration)
                {
                    const auto offset = (worker * 701 + iteration * 127) % (expected.size() - buffer.size() + 1);
                    const auto digest = hs::Sha256(std::span(expected).subspan(offset, buffer.size()));
                    if (!owner.ReadVerified(offset, buffer, digest).Succeeded() ||
                        !std::equal(buffer.begin(), buffer.end(), expected.begin() + offset))
                        reads_ok.store(false, std::memory_order_relaxed);
                }
            });
    }
    Check(reads_ok.load(std::memory_order_relaxed), "concurrent offset reads must not share a seek cursor");

#ifdef _WIN32
    // An open owner excludes in-place writers, but permits atomic replacement.
    std::ofstream writer(path, std::ios::binary | std::ios::in | std::ios::out);
    Check(!writer.is_open(), "opened shipped content must deny write sharing");
#endif
    std::filesystem::rename(path, directory.path / "old-content.bin");
    const std::array replacement{std::byte{0x12}, std::byte{0x34}};
    Write(path, replacement);
    Check(owner.ReadExact(0, actual).Succeeded() && actual == expected,
          "replacing a pathname must not change an existing owner's content");
    Check(hs::ShippedContentOwner::Open(path, owner).Succeeded() && owner.Size() == replacement.size(),
          "successful reopen must adopt the newly published file");
    std::array<std::byte, 2> replaced{};
    Check(owner.ReadExact(0, replaced).Succeeded() && replaced == replacement, "reopened content bytes differ");
#ifndef _WIN32
    // A noncooperating POSIX writer cannot be excluded by open(). If it breaks
    // immutable publishing, report a short read instead of returning success.
    std::filesystem::resize_file(path, 1);
    Check(!owner.ReadExact(0, replaced).Succeeded(), "truncated backing file must report a short read");
#endif
}
