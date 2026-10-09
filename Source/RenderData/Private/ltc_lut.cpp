#include <hs/render_data/ltc_lut.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace hs::render_data
{
namespace
{
constexpr std::uint32_t kMagic = 0x3143544c; // LTC1.
constexpr std::uint64_t kCount = std::uint64_t(kLtcLutEdge) * kLtcLutEdge;
constexpr float kAlphaFloor = 0.002f;
void Require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error("LTC LUT: " + std::string(message));
}
void Put(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value, std::size_t count)
{
    Require(offset <= bytes.size() && count <= bytes.size() - offset, "write bounds");
    for (std::size_t i=0; i<count; ++i) bytes[offset+i]=std::byte((value>>(i*8))&255);
}
std::uint64_t Get(std::span<const std::byte> bytes, std::size_t offset, std::size_t count)
{
    Require(offset <= bytes.size() && count <= bytes.size() - offset, "read bounds");
    std::uint64_t value=0;
    for (std::size_t i=0; i<count; ++i) value|=std::uint64_t(std::to_integer<unsigned>(bytes[offset+i]))<<(i*8);
    return value;
}
void ValidateDescription(LtcLutDescription description)
{
    Require(description.fit_samples>=256 && description.fit_samples<=4096 &&
        std::has_single_bit(description.fit_samples) && description.fit_iterations>=32 &&
        description.fit_iterations<=256, "invalid bounded fit budget");
}
void ValidateTexel(Float4 value, bool matrix)
{
    Require(std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
        std::isfinite(value.w), "nonfinite fit texel");
    if (matrix)
    {
        const double determinant=double(value.x)*value.w-double(value.y)*value.z;
        Require(determinant>0 && std::isfinite(determinant) && value.x>0 &&
            std::abs(value.x)<=1e8 && std::abs(value.y)<=1e8 &&
            std::abs(value.z)<=1e8 && std::abs(value.w)<=1e8, "singular or unrepresentable inverse LTC");
    }
    else Require(value.x>=0 && value.x<=1 && value.y>=0 && value.y<=value.x &&
        value.z==0 && value.w==0, "invalid energy or reserved amplitude components");
}
Float4 ReadTexel(std::span<const std::byte> bytes, std::size_t offset, bool matrix)
{
    Float4 value{std::bit_cast<float>(std::uint32_t(Get(bytes,offset,4))),
        std::bit_cast<float>(std::uint32_t(Get(bytes,offset+4,4))),
        std::bit_cast<float>(std::uint32_t(Get(bytes,offset+8,4))),
        std::bit_cast<float>(std::uint32_t(Get(bytes,offset+12,4)))};
    ValidateTexel(value,matrix); return value;
}
TexturePackageView Texture(std::span<const std::byte> payload,
    const std::vector<std::string> &dependencies, bool matrix)
{
    Require(payload.size()==kCount*16, "LUT payload size mismatch");
    for (std::size_t offset=0;offset<payload.size();offset+=16) (void)ReadTexel(payload,offset,matrix);
    TexturePackageView result;
    result.description={kLtcLutEdge,kLtcLutEdge,1,1,kTextureFormatRgba32Float};
    result.dependencies=dependencies;
    result.subresources.push_back({0,0,kLtcLutEdge,kLtcLutEdge,kLtcLutEdge*16,
        kLtcLutEdge*kLtcLutEdge*16,payload});
    return result;
}
} // namespace
std::vector<std::byte> BuildLtcLutPackage(AssetId asset_id,
    std::span<const std::string> dependencies,LtcLutDescription description,
    std::span<const Float4> matrix,std::span<const Float4> amplitude)
{
    ValidateDescription(description);
    Require(matrix.size()==kCount && amplitude.size()==kCount,"fit texel count mismatch");
    std::vector<std::byte> metadata(kLtcLutMetadataStride), matrices(kCount*16), amplitudes(kCount*16);
    Put(metadata,0,kMagic,4); Put(metadata,4,kLtcLutVersion,4);
    Put(metadata,8,kLtcLutEdge,4); Put(metadata,12,kTextureFormatRgba32Float,4);
    // Roughness map, view map, shared GGX/Smith/Schlick model, cosine/pi normalization.
    for (std::size_t offset:{16,20,24,28}) Put(metadata,offset,1,4);
    Put(metadata,32,std::bit_cast<std::uint32_t>(kAlphaFloor),4);
    Put(metadata,36,description.fit_samples,4); Put(metadata,40,description.fit_iterations,4);
    Put(metadata,44,1,4); // normalized L1 fit objective with deterministic MIS quadrature.
    Put(metadata,48,kCount*16,8); Put(metadata,56,kCount*16,8);
    auto encode=[](std::span<std::byte> bytes,std::span<const Float4> texels,bool isMatrix) {
        for (std::size_t i=0;i<texels.size();++i)
        {
            const auto value=texels[i]; ValidateTexel(value,isMatrix);
            const std::array values{value.x,value.y,value.z,value.w};
            for(std::size_t j=0;j<4;++j) Put(bytes,i*16+j*4,std::bit_cast<std::uint32_t>(values[j]),4);
        }
    };
    encode(matrices,matrix,true); encode(amplitudes,amplitude,false);
    const std::array chunks{
        ChunkSource{std::uint32_t(ChunkType::LtcLutMetadata),1,true,metadata,1,kLtcLutMetadataStride},
        ChunkSource{std::uint32_t(ChunkType::LtcLutMatrix),1,true,matrices,kCount,16},
        ChunkSource{std::uint32_t(ChunkType::LtcLutAmplitude),1,true,amplitudes,kCount,16}};
    return BuildPackage(asset_id,dependencies,chunks);
}
LtcLutPackageView ReadLtcLutPackage(std::span<const std::byte> bytes,AssetId expected_asset_id)
{
    const auto package=ReadPackage(bytes,expected_asset_id);
    Require(Get(bytes,64,4)==3 && package.chunks.size()==3 &&
        package.chunks[0].type==ChunkType::LtcLutMetadata && package.chunks[1].type==ChunkType::LtcLutMatrix &&
        package.chunks[2].type==ChunkType::LtcLutAmplitude && package.chunks[0].required &&
        package.chunks[1].required && package.chunks[2].required,"expected exactly three required LTC chunks");
    const auto metadata=package.chunks[0].bytes;
    Require(metadata.size()==kLtcLutMetadataStride && Get(metadata,0,4)==kMagic &&
        Get(metadata,4,4)==kLtcLutVersion && Get(metadata,8,4)==kLtcLutEdge &&
        Get(metadata,12,4)==kTextureFormatRgba32Float,"unsupported LTC dimensions/format");
    for (std::size_t offset:{16,20,24,28,44}) Require(Get(metadata,offset,4)==1,"unsupported fit convention");
    Require(Get(metadata,32,4)==std::bit_cast<std::uint32_t>(kAlphaFloor) &&
        Get(metadata,48,8)==kCount*16 && Get(metadata,56,8)==kCount*16,"unsupported alpha or payload layout");
    LtcLutPackageView result;
    result.description={std::uint32_t(Get(metadata,36,4)),std::uint32_t(Get(metadata,40,4))};
    ValidateDescription(result.description);
    result.matrix=Texture(package.chunks[1].bytes,package.dependencies,true);
    result.amplitude=Texture(package.chunks[2].bytes,package.dependencies,false);
    return result;
}
} // namespace hs::render_data
