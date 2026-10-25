#include <hs/render_data/static_shadow_archive.hpp>
#include <meshoptimizer.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace hs::render_data
{
namespace
{
constexpr std::array<std::byte, 8> kMagic{
    std::byte{'H'}, std::byte{'S'}, std::byte{'S'}, std::byte{'H'},
    std::byte{'A'}, std::byte{'R'}, std::byte{'C'}, std::byte{'1'}};
constexpr std::uint32_t kRecordBytes = 48;
constexpr std::size_t kRawBytes = kStaticShadowPageTexels * 4;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

void Require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(std::string("static shadow archive: ") + message);
}
void Put(std::span<std::byte> bytes, std::size_t at, std::uint64_t value, unsigned width = 4)
{
    for (unsigned i = 0; i < width; ++i) bytes[at + i] = std::byte((value >> (i * 8)) & 255);
}
std::uint64_t Get(std::span<const std::byte> bytes, std::size_t at, unsigned width = 4)
{
    std::uint64_t value{};
    for (unsigned i = 0; i < width; ++i) value |= std::uint64_t(std::to_integer<unsigned>(bytes[at + i])) << (i * 8);
    return value;
}
void PutFloat(std::span<std::byte> bytes, std::size_t at, float value)
{
    Put(bytes, at, std::bit_cast<std::uint32_t>(value));
}
float GetFloat(std::span<const std::byte> bytes, std::size_t at)
{
    return std::bit_cast<float>(static_cast<std::uint32_t>(Get(bytes, at)));
}
bool Zero(std::span<const std::byte> bytes)
{
    return std::ranges::all_of(bytes, [](std::byte value) { return value == std::byte{}; });
}
double Dot(Float3 a, Float3 b)
{
    return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z;
}
std::uint64_t ValidateDescription(const StaticShadowArchiveDescription &d)
{
    for (const auto axis : {d.light_right, d.light_up, d.light_forward})
        Require(std::isfinite(axis.x) && std::isfinite(axis.y) && std::isfinite(axis.z) &&
                std::abs(Dot(axis, axis) - 1.0) <= 1e-4, "basis must be finite and normalized");
    Require(std::abs(Dot(d.light_right, d.light_up)) <= 1e-4 &&
            std::abs(Dot(d.light_right, d.light_forward)) <= 1e-4 &&
            std::abs(Dot(d.light_up, d.light_forward)) <= 1e-4, "basis must be orthogonal");
    const double depth_span = double(d.depth_maximum) - d.depth_minimum;
    Require(std::isfinite(d.depth_minimum) && std::isfinite(d.depth_maximum) && depth_span > 0 &&
            depth_span <= std::numeric_limits<float>::max() &&
            std::isfinite(1.0f / static_cast<float>(depth_span)), "invalid fixed depth span");
    Require(!Zero(d.layout_digest), "missing layout identity");
    std::uint64_t count{};
    for (const auto &level : d.levels)
    {
        Require(std::isfinite(level.page_world_size) && level.page_world_size > 0 &&
                ((level.width == 0) == (level.height == 0)), "invalid level extent");
        if (!level.width) continue;
        const auto end_x = std::int64_t(level.minimum_x) + level.width;
        const auto end_y = std::int64_t(level.minimum_y) + level.height;
        Require(end_x - 1 <= INT32_MAX && end_y - 1 <= INT32_MAX, "level coordinate overflow");
        for (const auto coordinate : {std::int64_t(level.minimum_x), std::int64_t(level.minimum_y), end_x, end_y})
            Require(std::abs(double(coordinate) * level.page_world_size) <= std::numeric_limits<float>::max(),
                    "level world span overflow");
        const auto level_count = std::uint64_t(level.width) * level.height;
        Require(level_count <= UINT32_MAX - count, "page count overflow");
        count += level_count;
    }
    Require(count <= (std::numeric_limits<std::size_t>::max() - kStaticShadowArchiveHeaderBytes) / kRecordBytes,
            "index byte count overflow");
    return count;
}
void ValidatePage(const StaticShadowArchivePage &page, std::uint64_t payload_bytes)
{
    if (!page.bytes || page.bytes == UINT32_MAX)
        Require(page.offset == 0 && Zero(page.digest), "clear or missing page must have zero offset and digest");
    else
        Require(page.bytes >= 2 && page.bytes <= kRawBytes + 1 && !Zero(page.digest) &&
                page.offset <= payload_bytes && page.bytes <= payload_bytes - page.offset,
                "invalid encoded page range or digest");
}
void ValidateDepth(float value)
{
    Require(std::isfinite(value) && value >= 0 && value <= 1, "invalid depth texel");
}
} // namespace

std::vector<std::byte> BuildStaticShadowArchiveIndex(const StaticShadowArchiveIndex &index)
{
    const auto count = ValidateDescription(index.description);
    Require(index.pages.size() == count, "directory extent mismatch");
    for (const auto &page : index.pages) ValidatePage(page, index.payload_bytes);
    std::vector<std::byte> bytes(kStaticShadowArchiveHeaderBytes + static_cast<std::size_t>(count) * kRecordBytes);
    std::ranges::copy(kMagic, bytes.begin());
    Put(bytes, 8, 1); Put(bytes, 12, kStaticShadowArchiveHeaderBytes);
    Put(bytes, 16, kStaticShadowPageInner); Put(bytes, 20, kStaticShadowPageGuard);
    Put(bytes, 24, index.description.levels.size()); Put(bytes, 28, kRecordBytes);
    Put(bytes, 32, index.payload_bytes, 8); Put(bytes, 40, count, 8);
    std::size_t at = 48;
    for (const auto axis : {index.description.light_right, index.description.light_up, index.description.light_forward})
        for (const float value : {axis.x, axis.y, axis.z}) { PutFloat(bytes, at, value); at += 4; }
    PutFloat(bytes, 84, index.description.depth_minimum); PutFloat(bytes, 88, index.description.depth_maximum);
    at = 96;
    for (const auto &level : index.description.levels)
    {
        PutFloat(bytes, at, level.page_world_size);
        Put(bytes, at + 4, std::bit_cast<std::uint32_t>(level.minimum_x));
        Put(bytes, at + 8, std::bit_cast<std::uint32_t>(level.minimum_y));
        Put(bytes, at + 12, level.width); Put(bytes, at + 16, level.height);
        at += 20;
    }
    std::ranges::copy(index.description.layout_digest, bytes.begin() + 156);
    at = kStaticShadowArchiveHeaderBytes;
    for (const auto &page : index.pages)
    {
        Put(bytes, at, page.offset, 8); Put(bytes, at + 8, page.bytes);
        std::ranges::copy(page.digest, bytes.begin() + at + 16);
        at += kRecordBytes;
    }
    return bytes;
}

StaticShadowArchiveIndex ReadStaticShadowArchiveIndex(std::span<const std::byte> bytes)
{
    Require(bytes.size() >= kStaticShadowArchiveHeaderBytes &&
            std::equal(kMagic.begin(), kMagic.end(), bytes.begin()), "invalid header");
    Require(Get(bytes, 8) == 1 && Get(bytes, 12) == kStaticShadowArchiveHeaderBytes &&
            Get(bytes, 16) == kStaticShadowPageInner && Get(bytes, 20) == kStaticShadowPageGuard &&
            Get(bytes, 24) == 3 && Get(bytes, 28) == kRecordBytes, "unsupported archive layout");
    Require(Get(bytes, 92) == 0 && Zero(bytes.subspan(188, 68)), "nonzero reserved header");
    StaticShadowArchiveIndex index;
    auto &d = index.description;
    d.light_right = {GetFloat(bytes, 48), GetFloat(bytes, 52), GetFloat(bytes, 56)};
    d.light_up = {GetFloat(bytes, 60), GetFloat(bytes, 64), GetFloat(bytes, 68)};
    d.light_forward = {GetFloat(bytes, 72), GetFloat(bytes, 76), GetFloat(bytes, 80)};
    d.depth_minimum = GetFloat(bytes, 84); d.depth_maximum = GetFloat(bytes, 88);
    std::size_t at = 96;
    for (auto &level : d.levels)
    {
        level.page_world_size = GetFloat(bytes, at);
        level.minimum_x = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(Get(bytes, at + 4)));
        level.minimum_y = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(Get(bytes, at + 8)));
        level.width = static_cast<std::uint32_t>(Get(bytes, at + 12));
        level.height = static_cast<std::uint32_t>(Get(bytes, at + 16));
        at += 20;
    }
    std::copy_n(bytes.begin() + 156, d.layout_digest.size(), d.layout_digest.begin());
    const auto count = ValidateDescription(d);
    Require(Get(bytes, 40, 8) == count &&
            bytes.size() == kStaticShadowArchiveHeaderBytes + static_cast<std::size_t>(count) * kRecordBytes,
            "directory byte count mismatch");
    index.payload_bytes = Get(bytes, 32, 8);
    index.pages.reserve(static_cast<std::size_t>(count));
    at = kStaticShadowArchiveHeaderBytes;
    for (std::uint64_t i = 0; i < count; ++i)
    {
        Require(Get(bytes, at + 12) == 0, "nonzero reserved page record");
        StaticShadowArchivePage page;
        page.offset = Get(bytes, at, 8); page.bytes = static_cast<std::uint32_t>(Get(bytes, at + 8));
        std::copy_n(bytes.begin() + at + 16, page.digest.size(), page.digest.begin());
        ValidatePage(page, index.payload_bytes);
        index.pages.push_back(page);
        at += kRecordBytes;
    }
    return index;
}

std::vector<std::byte> EncodeStaticShadowPage(std::span<const float> page)
{
    Require(page.size() == kStaticShadowPageTexels, "page texel count mismatch");
    bool clear = true;
    for (const float value : page) { ValidateDepth(value); clear &= std::bit_cast<std::uint32_t>(value) == 0; }
    if (clear) return {};
    std::vector<std::byte> raw(kRawBytes);
    for (std::size_t i = 0; i < page.size(); ++i) PutFloat(raw, i * 4, page[i]);
    std::vector<std::byte> encoded(meshopt_encodeVertexBufferBound(page.size(), 4) + 1);
    const auto size = meshopt_encodeVertexBufferLevel(reinterpret_cast<unsigned char *>(encoded.data() + 1),
        encoded.size() - 1, raw.data(), page.size(), 4, 2, 1);
    Require(size != 0, "page compression failed");
    if (size >= raw.size())
    {
        encoded.resize(raw.size() + 1); encoded[0] = std::byte{0};
        std::ranges::copy(raw, encoded.begin() + 1);
    }
    else
    {
        encoded.resize(size + 1); encoded[0] = std::byte{1};
    }
    return encoded;
}

void DecodeStaticShadowPage(std::span<const std::byte> encoded, std::span<float> page)
{
    Require(page.size() == kStaticShadowPageTexels, "decoded page texel count mismatch");
    if (encoded.empty()) { std::ranges::fill(page, 0.0f); return; }
    Require(encoded.size() <= kRawBytes + 1, "encoded page exceeds raw bound");
    if (encoded[0] == std::byte{0})
    {
        Require(encoded.size() == kRawBytes + 1, "raw page byte count mismatch");
        std::memcpy(page.data(), encoded.data() + 1, kRawBytes);
    }
    else
    {
        Require(encoded[0] == std::byte{1} && encoded.size() > 1, "unknown or empty page codec");
        Require(meshopt_decodeVertexBuffer(page.data(), page.size(), 4,
                    reinterpret_cast<const unsigned char *>(encoded.data() + 1), encoded.size() - 1) == 0,
                "invalid compressed page");
    }
    if constexpr (std::endian::native != std::endian::little)
        for (std::size_t i = 0; i < page.size(); ++i) page[i] = GetFloat(std::as_bytes(page), i * 4);
    for (const float value : page) ValidateDepth(value);
}
} // namespace hs::render_data
