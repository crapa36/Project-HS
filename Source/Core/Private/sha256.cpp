#include <hs/core/sha256.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#endif

namespace hs
{
namespace
{
constexpr std::array<std::uint32_t, 64> kSha256Constants{
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

void ShaBlock(std::array<std::uint32_t, 8> &state,
              const std::array<std::byte, 64> &block)
{
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16; ++i)
        for (std::size_t j = 0; j < 4; ++j)
            words[i] = (words[i] << 8) |
                std::to_integer<std::uint32_t>(block[i * 4 + j]);
    for (std::size_t i = 16; i < 64; ++i)
    {
        const auto s0 = std::rotr(words[i - 15], 7) ^ std::rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
        const auto s1 = std::rotr(words[i - 2], 17) ^ std::rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    auto [a,b,c,d,e,f,g,h] = state;
    for (std::size_t i = 0; i < 64; ++i)
    {
        const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto t1 = h + s1 + ((e & f) ^ (~e & g)) + kSha256Constants[i] + words[i];
        const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}

} // namespace

void Sha256Stream::Add(std::span<const std::byte> bytes)
{
    if (finished_)
        throw std::logic_error("SHA-256 stream has finished");
    if (bytes.size() > std::numeric_limits<std::uint64_t>::max() / 8 - size_)
        throw std::length_error("SHA-256 input too large");
    size_ += bytes.size();
    while (!bytes.empty())
    {
        const auto count = std::min(bytes.size(), block_.size() - buffered_);
        std::copy_n(bytes.data(), count, block_.data() + buffered_);
        buffered_ += count;
        bytes = bytes.subspan(count);
        if (buffered_ == block_.size())
        {
            ShaBlock(state_, block_);
            buffered_ = 0;
        }
    }
}

std::array<std::byte, 32> Sha256Stream::Finish()
{
    if (finished_)
        throw std::logic_error("SHA-256 stream has finished");
    finished_ = true;
    std::fill(block_.begin() + buffered_, block_.end(), std::byte{0});
    block_[buffered_] = std::byte{0x80};
    if (buffered_ >= 56)
    {
        ShaBlock(state_, block_);
        block_.fill(std::byte{0});
    }
    const auto bit_count = size_ * 8;
    for (std::size_t i = 0; i < 8; ++i)
        block_[56 + i] = std::byte((bit_count >> ((7 - i) * 8)) & 0xff);
    ShaBlock(state_, block_);
    std::array<std::byte, 32> digest{};
    for (std::size_t i = 0; i < state_.size(); ++i)
        for (std::size_t j = 0; j < 4; ++j)
            digest[i * 4 + j] = std::byte((state_[i] >> ((3 - j) * 8)) & 0xff);
    return digest;
}

std::array<std::byte, 32> Sha256(std::span<const std::byte> bytes)
{
#ifdef _WIN32
    if (bytes.size() > std::numeric_limits<std::uint64_t>::max() / 8)
        throw std::length_error("SHA-256 input too large");
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("cannot initialize SHA-256 hash");
    struct HashOwner
    {
        BCRYPT_HASH_HANDLE handle;
        ~HashOwner() { BCryptDestroyHash(handle); }
    } owner{hash};
    while (!bytes.empty())
    {
        const auto count = static_cast<ULONG>(std::min<std::size_t>(
            bytes.size(), std::numeric_limits<ULONG>::max()));
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(
                const_cast<std::byte *>(bytes.data())), count, 0) < 0)
            throw std::runtime_error("cannot hash SHA-256 input");
        bytes = bytes.subspan(count);
    }
    std::array<std::byte, 32> digest{};
    if (BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.data()),
                         static_cast<ULONG>(digest.size()), 0) < 0)
        throw std::runtime_error("cannot finish SHA-256 hash");
    return digest;
#else
    Sha256Stream stream;
    stream.Add(bytes);
    return stream.Finish();
#endif
}
} // namespace hs
