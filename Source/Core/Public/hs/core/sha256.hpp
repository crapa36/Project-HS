#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hs
{
class Sha256Stream
{
  public:
#ifdef _WIN32
    Sha256Stream();
    ~Sha256Stream();
    Sha256Stream(const Sha256Stream &);
    Sha256Stream &operator=(const Sha256Stream &);
    Sha256Stream(Sha256Stream &&) noexcept;
    Sha256Stream &operator=(Sha256Stream &&) noexcept;
#endif
    void Add(std::span<const std::byte> bytes);
    [[nodiscard]] std::array<std::byte, 32> Finish();

  private:
#ifdef _WIN32
    void *hash_{};
    std::uint64_t native_size_ = 0;
    // Batch scalar serialization; large spans still go directly to CNG.
    std::array<std::byte, 256> pending_{};
    std::size_t pending_size_{};
    void FlushPending();
#else
    std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::byte, 64> block_{};
    std::uint64_t size_ = 0;
    std::size_t buffered_ = 0;
#endif
    bool finished_ = false;
};

[[nodiscard]] std::array<std::byte, 32> Sha256(std::span<const std::byte> bytes);
} // namespace hs
