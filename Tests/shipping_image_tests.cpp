#include <hs/runtime/shipping_content.hpp>
#include <hs/core/shipped_content_owner.hpp>

#include <chrono>
#include <fstream>
#include <stdexcept>
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
                ("hs-shipping-image-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) { path = std::move(candidate); return; }
        }
        throw std::runtime_error("cannot create shipping-image test directory");
    }
    ~TestDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
void Write(const std::filesystem::path &path, std::span<const std::byte> bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    Check(bool(stream), "cannot create shipping-image fixture");
    stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    Check(bool(stream), "cannot write shipping-image fixture");
}
} // namespace

void TestShippingImage()
{
    TestDirectory directory;
    hs::shipping::ShippingImageInfo info{};
    info.relative_path = L"Content/content.hsp";
    std::filesystem::create_directories(directory.path / L"Content");
    // PT05: magic0, format8, layout12, build ID16, file bytes48,
    // fixed header bytes56, reserved60. No native struct layout is serialized.
    constexpr std::array magic{'H', 'S', 'S', 'H', 'I', 'P', '0', '1'};
    for (std::size_t i = 0; i < magic.size(); ++i) info.header[i] = std::byte(magic[i]);
    info.header[8] = std::byte{1}; info.header[12] = std::byte{1};
    for (std::size_t i = 0; i < 32; ++i) info.header[16 + i] = std::byte(i + 1);
    info.header[48] = std::byte{80}; info.header[49] = std::byte{1}; // 336 bytes, little endian.
    info.header[56] = std::byte{64};
    std::vector<std::byte> valid(info.header.begin(), info.header.end());
    valid.resize(336, std::byte{0x5a});
    Write(directory.path / info.relative_path, valid);
    hs::ShippedContentOwner owner;
    Check(hs::shipping::OpenImage(directory.path, info, owner).Succeeded(), "matching shipping image must open");
    const auto preserves_owner = [&] {
        std::vector<std::byte> actual(valid.size());
        Check(owner.Size() == valid.size() && owner.ReadExact(0, actual).Succeeded() && actual == valid,
              "failed image replacement must preserve the previously opened image");
    };
    preserves_owner();
    info.font_offset=64;info.font_bytes=valid.size()-64;
    // Independently calculated SHA-256 of 272 bytes of 0x5a.
    info.font_digest={std::byte{0x53},std::byte{0x15},std::byte{0x63},std::byte{0xb6},std::byte{0x9e},std::byte{0x24},std::byte{0xdd},std::byte{0xc9},std::byte{0x1d},std::byte{0x9c},std::byte{0x8a},std::byte{0x87},std::byte{0xe2},std::byte{0x9e},std::byte{0x51},std::byte{0xae},std::byte{0xc3},std::byte{0x79},std::byte{0xa3},std::byte{0x6e},std::byte{0x25},std::byte{0xa2},std::byte{0x89},std::byte{0x15},std::byte{0x9e},std::byte{0x00},std::byte{0xed},std::byte{0xca},std::byte{0xf5},std::byte{0xde},std::byte{0x3b},std::byte{0x48}};
    std::vector<std::byte> font;
    Check(hs::shipping::ReadFont(owner,info,font).Succeeded() && font==std::vector<std::byte>(valid.begin()+64,valid.end()), "packed font exact range");
    hs::shipping::ShippingRange vfx{info.font_offset, info.font_bytes, info.font_digest};
    std::vector<std::byte> vfx_bytes;
    Check(hs::shipping::ReadRange(owner,vfx,vfx_bytes).Succeeded() && vfx_bytes==font,"packed VFX range verified");
    auto invalid_vfx=vfx;
    invalid_vfx.digest[0]^=std::byte{1};
    Check(!hs::shipping::ReadRange(owner,invalid_vfx,vfx_bytes).Succeeded() && vfx_bytes==font,"invalid VFX digest preserves output");

    {
        const auto original_cwd=std::filesystem::current_path();
        struct RestoreCwd { std::filesystem::path path; ~RestoreCwd(){std::error_code error;std::filesystem::current_path(path,error);} } restore{original_cwd};
        std::filesystem::current_path(directory.path / L"Content");
        hs::ShippedContentOwner from_other_cwd;
        Check(hs::shipping::OpenImage(directory.path,info,from_other_cwd).Succeeded(),"absolute executable root ignores current directory");
        const auto old_font=font;
        auto wrong_digest=info;wrong_digest.font_digest[0]^=std::byte{1};
        Check(!hs::shipping::ReadFont(from_other_cwd,wrong_digest,font).Succeeded() && font==old_font,"font digest mismatch preserves output");
    }
    // Publication replaces the file: an existing handle still owns the old bytes.
    std::filesystem::rename(directory.path / info.relative_path,directory.path / L"retired.bin");
    auto corrupted=valid;corrupted.back()^=std::byte{1};
    Write(directory.path / info.relative_path,corrupted);
    hs::ShippedContentOwner corrupt_owner;
    Check(hs::shipping::OpenImage(directory.path,info,corrupt_owner).Succeeded(),"payload corruption preserves matching fixed header");
    const auto previous_font=font;
    Check(!hs::shipping::ReadFont(corrupt_owner,info,font).Succeeded() && font==previous_font,"same-length font corruption rejected before publication");
    preserves_owner();

    auto candidate_info = info;
    candidate_info.relative_path = L"candidate.bin";
    const auto rejects = [&](std::span<const std::byte> bytes) {
        Write(directory.path / candidate_info.relative_path, bytes);
        Check(!hs::shipping::OpenImage(directory.path, candidate_info, owner).Succeeded(),
              "foreign or malformed shipping image must fail");
        preserves_owner();
    };
    auto wrong = valid;
    wrong[16] ^= std::byte{1};
    rejects(wrong);
    candidate_info.relative_path = L"missing.bin";
    Check(!hs::shipping::OpenImage(directory.path, candidate_info, owner).Succeeded(), "missing shipping image must fail");
    preserves_owner();
}
