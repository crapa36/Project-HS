#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/texture_package.hpp>
#include <hs/core/cooked_format.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace hs::render_data
{
namespace
{
void Require(bool condition, std::string_view reason);

constexpr std::array<char, 4> kParameterMagic{'M', 'P', 'A', 'R'};
constexpr std::uint16_t kParameterPayloadVersion = 1;
constexpr std::size_t kParameterPayloadHeader = 8;

bool ValidParameterId(std::string_view id)
{
    if (id.empty() || id.size() > 32 ||
        !((id.front() >= 'a' && id.front() <= 'z') || id.front() == '_')) return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

bool ValidParameterType(MaterialParameterType type)
{
    const auto value = static_cast<std::uint8_t>(type);
    return value >= static_cast<std::uint8_t>(MaterialParameterType::Bool) &&
           value <= static_cast<std::uint8_t>(MaterialParameterType::Float4);
}

std::size_t ParameterComponents(MaterialParameterType type)
{
    switch (type)
    {
    case MaterialParameterType::Bool: case MaterialParameterType::Uint:
    case MaterialParameterType::Float: return 1;
    case MaterialParameterType::Float2: return 2;
    case MaterialParameterType::Float3: return 3;
    case MaterialParameterType::Float4: return 4;
    default: Require(false, "invalid material parameter type"); return 0;
    }
}

void PutParameter(std::vector<std::byte> &bytes, std::size_t offset,
                  std::uint32_t value, std::size_t width)
{
    Require(offset + width <= bytes.size(), "material parameter payload overflow");
    for (std::size_t i = 0; i < width; ++i)
        bytes[offset + i] = std::byte((value >> (i * 8)) & 0xff);
}

std::uint32_t GetParameter(std::span<const std::byte> bytes, std::size_t offset,
                           std::size_t width)
{
    Require(offset + width <= bytes.size(), "truncated material parameter payload");
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < width; ++i)
        value |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (i * 8);
    return value;
}

void ValidateParameterDefinition(const MaterialParameterDefinition &parameter)
{
    Require(ValidParameterId(parameter.id), "invalid material parameter ID");
    Require(ValidParameterType(parameter.type), "invalid material parameter type");
    const auto count = ParameterComponents(parameter.type);
    for (std::size_t lane = count; lane < parameter.default_words.size(); ++lane)
        Require(parameter.default_words[lane] == 0, "unused material parameter lane is nonzero");
    if (parameter.type == MaterialParameterType::Bool)
    {
        Require(parameter.default_words[0] <= 1 && parameter.uint_min == 0 &&
                parameter.uint_max == std::numeric_limits<std::uint32_t>::max(),
                "invalid bool material parameter default or range");
    }
    else if (parameter.type == MaterialParameterType::Uint)
    {
        Require(parameter.uint_min <= parameter.uint_max &&
                parameter.default_words[0] >= parameter.uint_min &&
                parameter.default_words[0] <= parameter.uint_max,
                "invalid uint material parameter range or default");
    }
    else
    {
        Require(parameter.uint_min == 0 &&
                parameter.uint_max == std::numeric_limits<std::uint32_t>::max(),
                "non-uint material parameter has a uint range");
        for (std::size_t lane = 0; lane < count; ++lane)
            Require(std::isfinite(std::bit_cast<float>(parameter.default_words[lane])),
                    "material parameter default is nonfinite");
    }
}

std::vector<std::byte> ParameterLayoutBytes(
    std::span<const MaterialParameterDefinition> parameters)
{
    ValidateMaterialParameters(parameters);
    std::vector<std::byte> bytes;
    bytes.reserve(4 + parameters.size() * 44);
    const auto append = [&](std::uint32_t value, std::size_t width) {
        const auto offset = bytes.size(); bytes.resize(offset + width); PutParameter(bytes, offset, value, width);
    };
    append(static_cast<std::uint32_t>(parameters.size()), 2);
    append(1, 2); // identity encoding version
    for (std::size_t ordinal = 0; ordinal < parameters.size(); ++ordinal)
    {
        const auto &parameter = parameters[ordinal];
        append(static_cast<std::uint32_t>(parameter.id.size()), 1);
        append(static_cast<std::uint32_t>(parameter.type), 1);
        append(static_cast<std::uint32_t>(ordinal), 2);
        append(parameter.uint_min, 4);
        append(parameter.uint_max, 4);
        bytes.insert(bytes.end(), reinterpret_cast<const std::byte *>(parameter.id.data()),
                     reinterpret_cast<const std::byte *>(parameter.id.data() + parameter.id.size()));
    }
    return bytes;
}

std::string HexBytes(std::span<const std::byte> bytes)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes)
    {
        const auto value = std::to_integer<unsigned>(byte);
        result.push_back(hex[value >> 4]);
        result.push_back(hex[value & 0xf]);
    }
    return result;
}

std::string HexDigest(std::span<const std::byte> bytes)
{
    const auto digest = Sha256(bytes);
    return HexBytes(digest);
}

constexpr std::array<std::array<std::string_view, 6>, 4> kEnvironmentTextures{{
    {"environment/environment_rock_basecolor.dds",
     "environment/environment_rock_surface.dds",
     "environment/environment_rock_normal.dds",
     "environment/environment_rock_detail.dds", "", ""},
    {"environment/environment_bark_basecolor.dds",
     "environment/environment_bark_surface.dds",
     "environment/environment_bark_normal.dds",
     "environment/environment_bark_detail.dds", "", ""},
    {"environment/environment_leaf_basecolor.dds",
     "environment/environment_leaf_roughness.dds",
     "environment/environment_leaf_normal.dds", "", "", ""},
    {"environment/environment_grass_basecolor.dds",
     "environment/environment_grass_roughness.dds",
     "environment/environment_grass_normal.dds", "", "", ""}}};

void Require(bool condition, std::string_view reason)
{
    if (!condition) throw std::runtime_error("environment material: " + std::string(reason));
}

void Put(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value,
         std::size_t width)
{
    for (std::size_t i = 0; i < width; ++i)
        bytes[offset + i] = std::byte((value >> (i * 8)) & 0xff);
}

std::uint32_t Get(std::span<const std::byte> bytes, std::size_t offset,
                  std::size_t width)
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < width; ++i)
        value |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (i * 8);
    return value;
}

std::string_view ShaderMaterialName(std::string_view id, std::string_view suffix)
{
    constexpr std::string_view prefix = "render/shader/";
    Require(id.starts_with(prefix) && id.ends_with(suffix), "invalid shader product ID");
    const auto name = id.substr(prefix.size(), id.size() - prefix.size() - suffix.size());
    Require(!name.empty() && name.size() <= 32, "invalid shader material name");
    for (const char c : name)
        Require((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_',
                "invalid shader material name");
    return name;
}
} // namespace

std::vector<std::byte> EncodeMaterialSurfaceLayout(MaterialSurfaceLayout layout)
{
    std::vector<std::byte> bytes(kMaterialSurfaceLayoutStride);
    Put(bytes, 0, 0x314c534du, 4); // MSL1
    Put(bytes, 4, layout.capability_mask, 4);
    Put(bytes, 8, layout.varying_mask, 4);
    (void)DecodeMaterialSurfaceLayout(bytes);
    return bytes;
}

MaterialSurfaceLayout DecodeMaterialSurfaceLayout(std::span<const std::byte> bytes)
{
    Require(bytes.size() == kMaterialSurfaceLayoutStride && Get(bytes, 0, 4) == 0x314c534du &&
            Get(bytes, 12, 4) == 0, "invalid material surface layout record");
    MaterialSurfaceLayout layout{Get(bytes, 4, 4), Get(bytes, 8, 4)};
    Require((layout.capability_mask & ~511u) == 0 && (layout.capability_mask & 65u) != 0 &&
            (layout.varying_mask & ~31u) == 0, "unsupported material surface capabilities");
    Require((layout.capability_mask & 65u) != 65u,
            "material surface has conflicting basic and unlit terminals");
    const auto transport = layout.capability_mask & (16u | 128u | 256u);
    Require(transport == 0 || (transport & (transport - 1u)) == 0,
            "material surface has conflicting thin, subsurface and solid transport");
    Require((layout.capability_mask & 64u) == 0 ||
            (layout.capability_mask & (2u | 4u | 8u | 16u | 128u | 256u)) == 0,
            "unlit material surface cannot contain scattering layers");
    constexpr std::array<std::uint32_t, 5> capabilities{4u, 2u, 8u, 16u, 32u | 64u};
    for (std::size_t bit = 0; bit < capabilities.size(); ++bit)
        Require((layout.varying_mask & (1u << bit)) == 0 ||
                (layout.capability_mask & capabilities[bit]) != 0,
                "varying field has no matching closure capability");
    return layout;
}

void ValidateUniformMaterialSurfaceLayout(MaterialSurfaceLayout layout)
{
    (void)EncodeMaterialSurfaceLayout(layout);
    Require(layout.varying_mask == 0,
            "renderer uniform surface profile cannot consume varying closure sidecars");
}

std::uint32_t PackHeroMaterialDomainOrdinal(HeroMaterialDomain domain, std::uint32_t ordinal)
{
    Require((domain == HeroMaterialDomain::Opaque || domain == HeroMaterialDomain::Translucent) &&
            ordinal <= 65535u, "invalid hero material domain/ordinal");
    return ordinal | (domain == HeroMaterialDomain::Translucent ? 0x80000000u : 0u);
}

HeroMaterialDomain UnpackHeroMaterialDomain(std::uint32_t packed)
{
    Require((packed & 0x7fff0000u) == 0, "reserved hero material binding bits are nonzero");
    return (packed & 0x80000000u) != 0 ? HeroMaterialDomain::Translucent : HeroMaterialDomain::Opaque;
}

std::uint32_t UnpackHeroMaterialOrdinal(std::uint32_t packed)
{
    (void)UnpackHeroMaterialDomain(packed);
    return packed & 0xffffu;
}

std::vector<std::byte> EncodeCharacterMaterialBindings(
    std::span<const CharacterMaterialBinding> bindings, std::span<const std::string> dependencies,
    std::uint32_t material_count)
{
    Require(material_count > 0 && material_count <= 65535u && bindings.size() >= material_count &&
            bindings.size() <= std::uint64_t(material_count) * 2u, "invalid character material binding count");
    std::vector<std::byte> bytes(bindings.size() * kCharacterMaterialBindingStride);
    std::vector<bool> opaque(material_count), translucent(material_count);
    for (std::size_t i = 0; i < bindings.size(); ++i)
    {
        const auto &binding = bindings[i];
        Require(binding.slot < material_count && binding.dependency_index < dependencies.size() &&
                binding.hero_ordinal < kHeroMaterialNames.size(), "character material binding is out of bounds");
        const auto name = kHeroMaterialNames[binding.hero_ordinal];
        Require(dependencies[binding.dependency_index] == "render/material/" + std::string(name) + "/material.rpkg",
                "character material binding dependency disagrees with authored identity");
        Require((binding.domain == HeroMaterialDomain::Translucent) == (name == "slime_body"),
                "character material binding domain disagrees with closure domain");
        auto &seen = binding.domain == HeroMaterialDomain::Translucent ? translucent : opaque;
        Require(!seen[binding.slot], "duplicate character material slot/domain binding");
        seen[binding.slot] = true;
        Put(bytes, i * 8, binding.slot, 2);
        Put(bytes, i * 8 + 2, binding.dependency_index, 2);
        Put(bytes, i * 8 + 4, PackHeroMaterialDomainOrdinal(binding.domain, binding.hero_ordinal), 4);
    }
    Require(std::ranges::all_of(opaque, [](bool value) { return value; }), "missing opaque character material binding");
    for (const auto &binding : bindings)
        if (binding.domain == HeroMaterialDomain::Translucent)
        {
            const auto feature = std::ranges::find_if(bindings, [&](const auto &candidate) {
                return candidate.slot == binding.slot && candidate.domain == HeroMaterialDomain::Opaque;
            });
            Require(feature != bindings.end() && kHeroMaterialNames[feature->hero_ordinal] == "slime_features",
                    "solid jelly body requires its opaque feature binding");
        }
        else if (kHeroMaterialNames[binding.hero_ordinal] == "slime_features")
            Require(translucent[binding.slot], "jelly feature binding is missing its transparent body");
    return bytes;
}

std::vector<CharacterMaterialBinding> DecodeCharacterMaterialBindings(
    std::span<const std::byte> bytes, std::span<const std::string> dependencies,
    std::uint32_t material_count)
{
    Require(bytes.size() % kCharacterMaterialBindingStride == 0, "invalid character material binding stride");
    std::vector<CharacterMaterialBinding> result;
    for (std::size_t offset = 0; offset < bytes.size(); offset += kCharacterMaterialBindingStride)
    {
        const auto packed = Get(bytes, offset + 4, 4);
        result.push_back({static_cast<std::uint16_t>(Get(bytes, offset, 2)),
            static_cast<std::uint16_t>(Get(bytes, offset + 2, 2)),
            UnpackHeroMaterialOrdinal(packed), UnpackHeroMaterialDomain(packed)});
    }
    (void)EncodeCharacterMaterialBindings(result, dependencies, material_count);
    return result;
}

void ValidateMaterialParameters(std::span<const MaterialParameterDefinition> parameters)
{
    Require(parameters.size() <= 64, "too many material parameter declarations");
    std::unordered_set<std::string> ids;
    for (const auto &parameter : parameters)
    {
        ValidateParameterDefinition(parameter);
        if (parameter.id == "metallic" || parameter.id == "roughness_scale")
        {
            Require(parameter.type == MaterialParameterType::Float,
                    "reserved material parameter must be scalar float");
            const auto value = std::bit_cast<float>(parameter.default_words[0]);
            const auto maximum = parameter.id == "metallic" ? 1.0f : 2.0f;
            Require(std::isfinite(value) && value >= 0.0f && value <= maximum,
                    "reserved material parameter default is out of range");
        }
        Require(ids.insert(parameter.id).second, "duplicate material parameter ID");
    }
}

std::string MaterialParameterLayoutHash(
    std::span<const MaterialParameterDefinition> parameters)
{
    return HexDigest(ParameterLayoutBytes(parameters));
}

std::vector<std::byte> EncodeMaterialParameterLayout(
    std::span<const MaterialParameterDefinition> parameters)
{
    const auto digest = Sha256(ParameterLayoutBytes(parameters));
    return {digest.begin(), digest.end()};
}

std::string DecodeMaterialParameterLayout(std::span<const std::byte> encoded_hash)
{
    Require(encoded_hash.size() == 32, "invalid material parameter layout hash size");
    return HexBytes(encoded_hash);
}

void ValidateMaterialParameterLayout(
    std::span<const std::byte> encoded_hash,
    std::span<const MaterialParameterDefinition> parameters)
{
    Require(encoded_hash.size() == 32, "invalid material parameter layout hash size");
    const auto digest = Sha256(ParameterLayoutBytes(parameters));
    Require(std::equal(digest.begin(), digest.end(), encoded_hash.begin()),
            "material parameter layout identity mismatch");
}

std::vector<std::byte> EncodeMaterialParameters(
    std::span<const MaterialParameterDefinition> parameters)
{
    ValidateMaterialParameters(parameters);
    std::size_t size = kParameterPayloadHeader;
    for (const auto &parameter : parameters) size += 28 + parameter.id.size();
    std::vector<std::byte> bytes(size);
    std::copy(kParameterMagic.begin(), kParameterMagic.end(),
              reinterpret_cast<char *>(bytes.data()));
    PutParameter(bytes, 4, kParameterPayloadVersion, 2);
    PutParameter(bytes, 6, static_cast<std::uint32_t>(parameters.size()), 2);
    std::size_t cursor = kParameterPayloadHeader;
    for (const auto &parameter : parameters)
    {
        PutParameter(bytes, cursor, static_cast<std::uint32_t>(parameter.id.size()), 1);
        PutParameter(bytes, cursor + 1, static_cast<std::uint32_t>(parameter.type), 1);
        PutParameter(bytes, cursor + 2, 0, 2);
        for (std::size_t lane = 0; lane < 4; ++lane)
            PutParameter(bytes, cursor + 4 + lane * 4, parameter.default_words[lane], 4);
        PutParameter(bytes, cursor + 20, parameter.uint_min, 4);
        PutParameter(bytes, cursor + 24, parameter.uint_max, 4);
        cursor += 28;
        std::memcpy(bytes.data() + cursor, parameter.id.data(), parameter.id.size());
        cursor += parameter.id.size();
    }
    (void)DecodeMaterialParameters(bytes);
    return bytes;
}

std::vector<MaterialParameterDefinition> DecodeMaterialParameters(
    std::span<const std::byte> bytes)
{
    Require(bytes.size() >= kParameterPayloadHeader, "invalid material parameter payload size");
    Require(std::equal(kParameterMagic.begin(), kParameterMagic.end(),
                       reinterpret_cast<const char *>(bytes.data())) &&
                GetParameter(bytes, 4, 2) == kParameterPayloadVersion,
            "invalid material parameter payload header");
    const auto count = GetParameter(bytes, 6, 2);
    Require(count <= 64, "too many material parameter declarations");
    std::vector<MaterialParameterDefinition> result;
    result.reserve(count);
    std::size_t cursor = kParameterPayloadHeader;
    for (std::size_t i = 0; i < count; ++i)
    {
        Require(cursor + 28 <= bytes.size(), "truncated material parameter declaration");
        const auto id_length = GetParameter(bytes, cursor, 1);
        Require(id_length > 0 && id_length <= 32 && cursor + 28 + id_length <= bytes.size(),
                "invalid material parameter declaration ID length");
        Require(GetParameter(bytes, cursor + 2, 2) == 0, "material parameter reserved bytes are nonzero");
        MaterialParameterDefinition parameter;
        parameter.id.assign(reinterpret_cast<const char *>(bytes.data() + cursor + 28), id_length);
        parameter.type = static_cast<MaterialParameterType>(GetParameter(bytes, cursor + 1, 1));
        for (std::size_t lane = 0; lane < 4; ++lane)
            parameter.default_words[lane] = GetParameter(bytes, cursor + 4 + lane * 4, 4);
        parameter.uint_min = GetParameter(bytes, cursor + 20, 4);
        parameter.uint_max = GetParameter(bytes, cursor + 24, 4);
        ValidateParameterDefinition(parameter);
        result.push_back(std::move(parameter));
        cursor += 28 + id_length;
    }
    Require(cursor == bytes.size(), "trailing bytes in material parameter payload");
    ValidateMaterialParameters(result);
    return result;
}

std::vector<std::byte> BuildMaterialParameterPackage(
    std::string_view product_id, std::span<const MaterialParameterDefinition> parameters)
{
    const auto payload = EncodeMaterialParameters(parameters);
    const auto layout = EncodeMaterialParameterLayout(parameters);
    const std::array<ChunkSource, 2> chunks{{ChunkSource{
        static_cast<std::uint32_t>(ChunkType::MaterialParameters), 1, true, payload,
        payload.size(), 1},
        ChunkSource{static_cast<std::uint32_t>(ChunkType::MaterialParameterLayout), 1, true,
                    layout, 1, 32}}};
    return BuildPackage(hs::MakeAssetId(product_id), {}, chunks);
}

std::vector<MaterialParameterDefinition> ReadMaterialParameterPackage(
    std::span<const std::byte> bytes, AssetId expected_asset_id)
{
    const auto package = ReadPackage(bytes, expected_asset_id);
    const auto found = std::ranges::find(package.chunks, ChunkType::MaterialParameters,
                                         &ChunkView::type);
    Require(found != package.chunks.end(), "material parameter package has no parameter chunk");
    const auto parameters = DecodeMaterialParameters(found->bytes);
    const auto layout = std::ranges::find(package.chunks, ChunkType::MaterialParameterLayout,
                                          &ChunkView::type);
    Require(layout != package.chunks.end(), "material parameter package has no layout chunk");
    ValidateMaterialParameterLayout(layout->bytes, parameters);
    return parameters;
}

std::uint32_t MaterialSamplerRegister(MaterialSamplerProfile profile)
{
    switch (profile)
    {
    case MaterialSamplerProfile::LinearClamp: return 0;
    case MaterialSamplerProfile::LinearWrap: return 2;
    case MaterialSamplerProfile::EnvironmentAnisotropicWrap: return 3;
    case MaterialSamplerProfile::FoliageAnisotropicClamp: return 4;
    default: Require(false, "unsupported sampler profile"); return 0;
    }
}

namespace
{
bool TextureProductId(std::string_view id)
{
    constexpr std::string_view prefix = "render/texture/environment_";
    constexpr std::string_view suffix = ".rpkg";
    if (!id.starts_with(prefix) || !id.ends_with(suffix) ||
        id.size() <= prefix.size() + suffix.size()) return false;
    const auto token = id.substr(prefix.size(), id.size() - prefix.size() - suffix.size());
    return std::ranges::all_of(token, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

bool DecalTextureProductId(std::string_view id)
{
    constexpr std::string_view prefix = "render/texture/vfx_";
    constexpr std::string_view suffix = "_decal_array.rpkg";
    if (!id.starts_with(prefix) || !id.ends_with(suffix) ||
        id.size() <= prefix.size() + suffix.size()) return false;
    const auto token = id.substr(prefix.size(), id.size() - prefix.size() - suffix.size());
    return std::ranges::all_of(token, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

void ValidateBindingSemantic(std::size_t slot, MaterialTextureSemantic semantic)
{
    Require((slot == 0 && (semantic == MaterialTextureSemantic::BaseColor ||
                           semantic == MaterialTextureSemantic::Gaussian)) ||
        (slot == 1 && (semantic == MaterialTextureSemantic::Surface ||
                       semantic == MaterialTextureSemantic::Roughness)) ||
        (slot == 2 && semantic == MaterialTextureSemantic::Normal) ||
        (slot == 3 && semantic == MaterialTextureSemantic::Detail) ||
        (slot == 4 && semantic == MaterialTextureSemantic::Height) ||
        (slot == 5 && semantic == MaterialTextureSemantic::Histogram),
        "texture semantic disagrees with immutable slot");
}
} // namespace

void ValidateMaterialTextureDefinitions(std::span<const MaterialTextureDefinition> textures)
{
    Require(textures.size() == 3 || textures.size() == 4 || textures.size() == 6,
            "expected three, four or six texture declarations");
    std::unordered_set<std::string> ids;
    for (std::size_t i = 0; i < textures.size(); ++i)
    {
        Require(!textures[i].id.empty() && ids.insert(textures[i].id).second,
                "empty or duplicate texture declaration");
        Require(TextureProductId(textures[i].product_id), "invalid typed texture product ID");
        Require(textures[i].dimension == MaterialTextureDimension::Texture2DArray,
                "unsupported texture dimension");
        ValidateBindingSemantic(i, textures[i].semantic);
    }
}

void ValidateDecalMaterialTextureDefinition(const MaterialTextureDefinition &texture)
{
    Require(ValidParameterId(texture.id) && DecalTextureProductId(texture.product_id) &&
            texture.dimension == MaterialTextureDimension::Texture2DArray &&
            (texture.semantic == MaterialTextureSemantic::DecalStructure ||
             texture.semantic == MaterialTextureSemantic::DecalMask),
            "invalid decal array texture declaration");
}

void ValidateMaterialTexturePackage(const MaterialTextureDefinition &texture,
    const TextureDescription &description, std::uint32_t required_layer)
{
    Require(texture.dimension == MaterialTextureDimension::Texture2DArray &&
            description.width > 0 && description.height > 0 &&
            description.array_size > required_layer && description.mip_count > 0,
            "texture array layer or mip chain is unavailable");
    std::uint32_t full_mips = 1;
    for (auto edge = std::max(description.width, description.height); edge > 1; edge >>= 1)
        ++full_mips;
    Require(description.mip_count <= full_mips, "texture mip chain exceeds dimensions");
    bool compatible = false;
    switch (texture.semantic)
    {
    case MaterialTextureSemantic::BaseColor:
        compatible = description.format == kTextureFormatBc7Srgb ||
                     description.format == kTextureFormatRgba8Srgb; break;
    case MaterialTextureSemantic::Gaussian:
        compatible = description.format == kTextureFormatBc7; break;
    case MaterialTextureSemantic::Surface: case MaterialTextureSemantic::Normal:
    case MaterialTextureSemantic::Detail:
        compatible = description.format == kTextureFormatBc5; break;
    case MaterialTextureSemantic::Roughness:
        compatible = description.format == kTextureFormatBc4 ||
                     description.format == kTextureFormatR8; break;
    case MaterialTextureSemantic::Height:
        compatible = description.format == kTextureFormatBc4; break;
    case MaterialTextureSemantic::Histogram:
        compatible = description.format == kTextureFormatRgba32Float; break;
    case MaterialTextureSemantic::DecalStructure:
        compatible = description.format == kTextureFormatRgba8 ||
                     description.format == kTextureFormatBc7; break;
    case MaterialTextureSemantic::DecalMask:
        compatible = description.format == kTextureFormatR8 ||
                     description.format == kTextureFormatBc4; break;
    default: break;
    }
    Require(compatible, "texture format/channel contract disagrees with semantic");
}

std::vector<std::byte> EncodeMaterialBindings(std::span<const std::string> dependencies,
    std::span<const MaterialTextureDefinition> textures)
{
    ValidateMaterialTextureDefinitions(textures);
    std::vector<std::byte> bytes(kMaterialTextureSlots * kMaterialBindingStride);
    for (std::size_t slot = 0; slot < kMaterialTextureSlots; ++slot)
    {
        std::uint32_t index = 0xffff;
        const auto semantic = slot == 0 ? MaterialTextureSemantic::Gaussian :
            slot == 1 ? MaterialTextureSemantic::Surface :
            slot == 2 ? MaterialTextureSemantic::Normal :
            slot == 3 ? MaterialTextureSemantic::Detail :
            slot == 4 ? MaterialTextureSemantic::Height : MaterialTextureSemantic::Histogram;
        auto encoded_semantic = semantic;
        if (slot < textures.size())
        {
            const auto found = std::ranges::find(dependencies, textures[slot].product_id);
            Require(found != dependencies.end(), "texture package missing from dependencies");
            index = static_cast<std::uint32_t>(found - dependencies.begin());
            Require(index < 0xffff, "too many material dependencies");
            encoded_semantic = textures[slot].semantic;
        }
        const auto offset = slot * kMaterialBindingStride;
        Put(bytes, offset, static_cast<std::uint32_t>(slot), 2);
        Put(bytes, offset + 2, index, 2);
        Put(bytes, offset + 4, static_cast<std::uint32_t>(MaterialTextureDimension::Texture2DArray), 2);
        Put(bytes, offset + 6, static_cast<std::uint32_t>(encoded_semantic), 2);
        Put(bytes, offset + 8, kMaterialShaderBindingAbi, 4);
    }
    (void)DecodeMaterialBindings(bytes, dependencies);
    return bytes;
}

std::vector<MaterialTextureBinding> DecodeMaterialBindings(
    std::span<const std::byte> bytes, std::span<const std::string> dependencies)
{
    Require(bytes.size() == kMaterialTextureSlots * kMaterialBindingStride,
            "invalid material bindings size");
    std::vector<MaterialTextureBinding> result;
    for (std::size_t slot = 0; slot < kMaterialTextureSlots; ++slot)
    {
        const auto offset = slot * kMaterialBindingStride;
        MaterialTextureBinding binding{static_cast<std::uint16_t>(Get(bytes, offset, 2)),
            static_cast<std::uint16_t>(Get(bytes, offset + 2, 2)),
            static_cast<MaterialTextureDimension>(Get(bytes, offset + 4, 2)),
            static_cast<MaterialTextureSemantic>(Get(bytes, offset + 6, 2)),
            Get(bytes, offset + 8, 4)};
        Require(binding.slot == slot && binding.dimension == MaterialTextureDimension::Texture2DArray &&
                binding.shader_binding_abi == kMaterialShaderBindingAbi,
                "material binding slot, dimension or shader ABI mismatch");
        ValidateBindingSemantic(slot, binding.semantic);
        if (binding.dependency_index == 0xffff)
            Require(slot >= 3, "required texture binding is absent");
        else Require(binding.dependency_index < dependencies.size() &&
                     TextureProductId(dependencies[binding.dependency_index]),
                     "material binding references an invalid texture package dependency");
        result.push_back(binding);
    }
    return result;
}

std::vector<std::byte> EncodeMaterialPrograms(
    std::span<const std::string> dependencies,
    std::span<const MaterialProgramSource> programs)
{
    Require(programs.size() == 1 || programs.size() == 2 || programs.size() == 4, "invalid program count");
    std::vector<std::byte> bytes(programs.size() * kMaterialProgramStride);
    for (std::size_t i = 0; i < programs.size(); ++i)
    {
        const auto found = std::ranges::find(dependencies, programs[i].shader_id);
        Require(found != dependencies.end(), "shader is missing from dependency IDs");
        const auto index = static_cast<std::size_t>(found - dependencies.begin());
        Require(index < 65536, "too many material dependencies");
        Put(bytes, i * kMaterialProgramStride, static_cast<std::uint32_t>(index), 2);
        Put(bytes, i * kMaterialProgramStride + 2,
            static_cast<std::uint32_t>(programs[i].kind), 2);
    }
    (void)DecodeMaterialPrograms(bytes, dependencies);
    return bytes;
}

std::vector<MaterialProgram> DecodeMaterialPrograms(
    std::span<const std::byte> bytes, std::span<const std::string> dependencies)
{
    Require(bytes.size() == kMaterialProgramStride ||
            bytes.size() == 2 * kMaterialProgramStride ||
            bytes.size() == 4 * kMaterialProgramStride, "invalid material program size");
    std::vector<MaterialProgram> programs;
    programs.reserve(bytes.size() / kMaterialProgramStride);
    bool opaque = false, masked_scene = false, masked_shadow = false, decal = false;
    std::uint32_t hero_mask{};
    std::string_view material_name;
    for (std::size_t i = 0; i < bytes.size(); i += kMaterialProgramStride)
    {
        const auto index = Get(bytes, i, 2);
        const auto value = Get(bytes, i + 2, 2);
        Require(index < dependencies.size(), "program dependency index out of range");
        const auto id = std::string_view(dependencies[index]);
        MaterialProgramKind kind{};
        if (value == static_cast<std::uint32_t>(MaterialProgramKind::OpaqueScenePS))
        {
            Require(!opaque, "duplicate opaque program");
            material_name = ShaderMaterialName(id, "_scene.dxil");
            opaque = true;
            kind = MaterialProgramKind::OpaqueScenePS;
        }
        else if (value == static_cast<std::uint32_t>(MaterialProgramKind::MaskedScenePS))
        {
            Require(!masked_scene, "duplicate masked scene program");
            const auto name = ShaderMaterialName(id, "_scene.dxil");
            Require(material_name.empty() || material_name == name,
                    "masked programs belong to different materials");
            material_name = name;
            masked_scene = true;
            kind = MaterialProgramKind::MaskedScenePS;
        }
        else if (value == static_cast<std::uint32_t>(MaterialProgramKind::MaskedShadowPS))
        {
            Require(!masked_shadow, "duplicate masked shadow program");
            const auto name = ShaderMaterialName(id, "_shadow.dxil");
            Require(material_name.empty() || material_name == name,
                    "masked programs belong to different materials");
            material_name = name;
            masked_shadow = true;
            kind = MaterialProgramKind::MaskedShadowPS;
        }
        else if (value >= static_cast<std::uint32_t>(MaterialProgramKind::HeroScenePS) &&
                 value <= static_cast<std::uint32_t>(MaterialProgramKind::HeroTranslucentPS))
        {
            constexpr std::array suffixes{"_scene.dxil", "_visibility.dxil", "_resolve.dxil", "_shadow.dxil", "_translucent.dxil"};
            const auto bit = 1u << (value - 4u);
            Require((hero_mask & bit) == 0, "duplicate hero material program");
            const auto name = ShaderMaterialName(id, suffixes[value - 4u]);
            Require(std::ranges::find(kHeroMaterialNames, name) != kHeroMaterialNames.end() &&
                    (material_name.empty() || material_name == name), "hero programs belong to an unknown/different material");
            material_name = name;
            hero_mask |= bit;
            kind = static_cast<MaterialProgramKind>(value);
        }
        else if (value == static_cast<std::uint32_t>(MaterialProgramKind::DecalSurfaceCS))
        {
            Require(!decal && programs.empty(), "duplicate or mixed decal program");
            Require(id == "render/shader/decal_surface_cs.dxil", "unknown decal Surface program");
            decal = true;
            kind = MaterialProgramKind::DecalSurfaceCS;
        }
        else Require(false, "unknown material program kind");
        programs.push_back({static_cast<std::uint16_t>(index), kind});
    }
    Require((decal && programs.size() == 1 && !opaque && !masked_scene && !masked_shadow && hero_mask == 0) ||
            (!decal && hero_mask == 0 && programs.size() == 1 && opaque && !masked_scene && !masked_shadow) ||
            (hero_mask == 0 && programs.size() == 2 && !opaque && masked_scene && masked_shadow) ||
            (!opaque && !masked_scene && !masked_shadow &&
             ((programs.size() == 4 && hero_mask == 15u && material_name != "slime_body") ||
              (programs.size() == 1 && hero_mask == 16u && material_name == "slime_body"))),
            "missing required material program kind");
    return programs;
}

void ValidateDecalReceiverPolicy(const DecalReceiverPolicy &policy)
{
    constexpr auto models = (1u << 1u) | (1u << 2u) | (1u << 3u);
    // The Surface v1 ABI owns Valid, foliage classification and ThinSheet.
    // Model bits and outline-group bits have separate meanings and cannot be
    // used as arbitrary receiver flags.
    constexpr auto flags = 1u | (3u << 8u) | (1u << 10u);
    Require(policy.model_mask != 0 && (policy.model_mask & ~models) == 0 &&
            (policy.required_surface_flags & ~flags) == 0 &&
            (policy.forbidden_surface_flags & ~flags) == 0 &&
            (policy.required_surface_flags & policy.forbidden_surface_flags) == 0 &&
            (policy.required_surface_flags & 1u) != 0 &&
            policy.normal_filter <= DecalReceiverNormalFilter::GroundFacing,
            "decal receivers disagree with SurfaceLayout v1 classifications/flags");
}

DecalMaterialPackageData ReadDecalMaterialPackage(std::span<const std::byte> bytes, std::string_view name)
{
    Require(std::ranges::find(kDecalMaterialNames, name) != kDecalMaterialNames.end(), "unknown decal material name");
    const auto package = ReadPackage(bytes, MakeAssetId("render/material/" + std::string(name) + "/material.rpkg"));
    Require(package.chunks.size() == 3 && std::ranges::all_of(package.chunks,
            [](const auto &chunk) { return chunk.required; }), "decal material requires three mandatory chunks");
    const auto find = [&](ChunkType type) -> const ChunkView & {
        const auto found = std::ranges::find(package.chunks, type, &ChunkView::type);
        Require(found != package.chunks.end(), "decal material is missing a mandatory chunk");
        return *found;
    };
    const auto metadata = find(ChunkType::DecalMaterialMetadata).bytes;
    Require(Get(metadata, 0, 4) == kDecalShaderBindingAbi &&
            Get(metadata, 4, 4) == static_cast<std::uint32_t>(DecalRayPolicy::RasterOnly) &&
            Get(metadata, 26, 2) == 0 && Get(metadata, 28, 4) == 0,
            "decal material shader ABI, ray policy or reserved fields are invalid");
    DecalMaterialPackageData result;
    result.dependencies = package.dependencies;
    result.receivers = {Get(metadata, 8, 4), Get(metadata, 12, 4), Get(metadata, 16, 4),
        static_cast<DecalReceiverNormalFilter>(Get(metadata, 20, 4))};
    ValidateDecalReceiverPolicy(result.receivers);
    result.texture_dependency_index = static_cast<std::uint16_t>(Get(metadata, 24, 2));
    Require(result.texture_dependency_index < package.dependencies.size() &&
            DecalTextureProductId(package.dependencies[result.texture_dependency_index]), "invalid decal texture dependency");
    result.parameter_layout_hash = DecodeMaterialParameterLayout(find(ChunkType::MaterialParameterLayout).bytes);
    const auto programs = DecodeMaterialPrograms(find(ChunkType::MaterialProgram).bytes, package.dependencies);
    Require(programs.size() == 1 && programs.front().kind == MaterialProgramKind::DecalSurfaceCS &&
            package.dependencies[programs.front().dependency_index] == "render/shader/decal_surface_cs.dxil",
            "decal material program identity mismatch");
    result.program = programs.front();
    std::vector<std::string> expected{"render/material/" + std::string(name) + "/parameters.rpkg",
        package.dependencies[result.texture_dependency_index], package.dependencies[result.program.dependency_index]};
    std::ranges::sort(expected);
    Require(package.dependencies == expected, "decal material has missing or undeclared dependencies");
    return result;
}

std::vector<std::byte> BuildDecalMaterialPackage(std::string_view name,
    std::span<const MaterialParameterDefinition> parameters, std::string_view texture_id,
    std::string_view shader_id, const DecalReceiverPolicy &receivers)
{
    ValidateDecalReceiverPolicy(receivers);
    Require(DecalTextureProductId(texture_id), "invalid decal texture product ID");
    std::vector<std::string> dependencies{"render/material/" + std::string(name) + "/parameters.rpkg",
        std::string(texture_id), std::string(shader_id)};
    std::ranges::sort(dependencies);
    const std::array programs{MaterialProgramSource{shader_id, MaterialProgramKind::DecalSurfaceCS}};
    const auto program_bytes = EncodeMaterialPrograms(dependencies, programs);
    const auto layout = EncodeMaterialParameterLayout(parameters);
    std::vector<std::byte> metadata(32);
    Put(metadata, 0, kDecalShaderBindingAbi, 4);
    Put(metadata, 4, static_cast<std::uint32_t>(DecalRayPolicy::RasterOnly), 4);
    Put(metadata, 8, receivers.model_mask, 4);
    Put(metadata, 12, receivers.required_surface_flags, 4);
    Put(metadata, 16, receivers.forbidden_surface_flags, 4);
    Put(metadata, 20, static_cast<std::uint32_t>(receivers.normal_filter), 4);
    Put(metadata, 24, static_cast<std::uint32_t>(std::ranges::find(dependencies, texture_id) - dependencies.begin()), 2);
    const std::array chunks{
        ChunkSource{static_cast<std::uint32_t>(ChunkType::MaterialProgram), 1, true, program_bytes, 1, kMaterialProgramStride},
        ChunkSource{static_cast<std::uint32_t>(ChunkType::MaterialParameterLayout), 1, true, layout, 1, 32},
        ChunkSource{static_cast<std::uint32_t>(ChunkType::DecalMaterialMetadata), 1, true, metadata, 1, 32}};
    const auto package = BuildPackage(MakeAssetId("render/material/" + std::string(name) + "/material.rpkg"), dependencies, chunks);
    (void)ReadDecalMaterialPackage(package, name);
    return package;
}

HeroMaterialPackageData ReadHeroMaterialPackage(std::span<const std::byte> bytes, std::string_view name)
{
    Require(std::ranges::find(kHeroMaterialNames, name) != kHeroMaterialNames.end(), "unknown hero material name");
    const auto id = "render/material/" + std::string(name) + "/material.rpkg";
    const auto package = ReadPackage(bytes, MakeAssetId(id));
    Require(package.chunks.size() == 3 && std::ranges::all_of(package.chunks,
            [](const auto &chunk) { return chunk.required; }), "hero material requires exactly three mandatory chunks");
    const auto find = [&](ChunkType type) -> const ChunkView & {
        const auto found = std::ranges::find(package.chunks, type, &ChunkView::type);
        Require(found != package.chunks.end(), "hero material package is missing a mandatory chunk");
        return *found;
    };
    HeroMaterialPackageData result;
    result.dependencies = package.dependencies;
    result.surface = DecodeMaterialSurfaceLayout(find(ChunkType::MaterialSurfaceLayout).bytes);
    result.parameter_layout_hash = DecodeMaterialParameterLayout(find(ChunkType::MaterialParameterLayout).bytes);
    result.programs = DecodeMaterialPrograms(find(ChunkType::MaterialProgram).bytes, package.dependencies);
    result.domain = name == "slime_body" ? HeroMaterialDomain::Translucent : HeroMaterialDomain::Opaque;
    Require(((result.surface.capability_mask & 256u) != 0) == (result.domain == HeroMaterialDomain::Translucent) &&
            (result.domain != HeroMaterialDomain::Translucent || (result.surface.capability_mask & 128u) == 0),
            "hero material surface capabilities disagree with its native domain");
    const auto parameter_id = "render/material/" + std::string(name) + "/parameters.rpkg";
    Require(std::ranges::find(package.dependencies, parameter_id) != package.dependencies.end(),
            "hero material is missing its parameter package dependency");
    for (const auto &program : result.programs)
    {
        const auto kind = static_cast<std::uint32_t>(program.kind);
        Require(kind >= 4u && kind <= 8u &&
                package.dependencies[program.dependency_index].starts_with("render/shader/" + std::string(name) + "_"),
                "hero material program identity disagrees with its package");
        Require((program.kind == MaterialProgramKind::HeroTranslucentPS) ==
                (result.domain == HeroMaterialDomain::Translucent), "hero program domain disagrees with its package");
    }
    std::vector<std::string> expected_dependencies{parameter_id};
    for (const auto &program : result.programs)
        expected_dependencies.push_back(package.dependencies[program.dependency_index]);
    if (name == "monster_pbr")
        for (const auto *texture : {"monster_basecolor", "monster_emissive", "monster_ram"})
            expected_dependencies.push_back("render/texture/" + std::string(texture) + ".rpkg");
    std::ranges::sort(expected_dependencies);
    Require(package.dependencies == expected_dependencies, "hero material has missing or undeclared native adapter dependencies");
    return result;
}

std::vector<std::byte> BuildHeroMaterialPackage(std::string_view name,
    std::span<const MaterialParameterDefinition> parameters, MaterialSurfaceLayout surface,
    std::span<const MaterialProgramSource> programs)
{
    std::vector<std::string> dependencies{"render/material/" + std::string(name) + "/parameters.rpkg"};
    if (name == "monster_pbr")
        for (const auto *texture : {"monster_basecolor", "monster_emissive", "monster_ram"})
            dependencies.push_back("render/texture/" + std::string(texture) + ".rpkg");
    for (const auto &program : programs) dependencies.emplace_back(program.shader_id);
    std::ranges::sort(dependencies);
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    const auto program_bytes = EncodeMaterialPrograms(dependencies, programs);
    const auto surface_bytes = EncodeMaterialSurfaceLayout(surface);
    const auto layout_bytes = EncodeMaterialParameterLayout(parameters);
    const std::array chunks{
        ChunkSource{static_cast<std::uint32_t>(ChunkType::MaterialProgram), 1, true, program_bytes,
                    programs.size(), kMaterialProgramStride},
        ChunkSource{static_cast<std::uint32_t>(ChunkType::MaterialParameterLayout), 1, true, layout_bytes, 1, 32},
        ChunkSource{static_cast<std::uint32_t>(ChunkType::MaterialSurfaceLayout), 1, true, surface_bytes, 1, kMaterialSurfaceLayoutStride}};
    const auto package = BuildPackage(MakeAssetId("render/material/" + std::string(name) + "/material.rpkg"), dependencies, chunks);
    (void)ReadHeroMaterialPackage(package, name);
    return package;
}

std::vector<std::byte> EncodeMaterialProgram(
    std::span<const std::string> dependencies, std::string_view shader_id)
{
    const std::array source{MaterialProgramSource{
        shader_id, MaterialProgramKind::OpaqueScenePS}};
    return EncodeMaterialPrograms(dependencies, source);
}

MaterialProgram DecodeMaterialProgram(
    std::span<const std::byte> bytes, std::span<const std::string> dependencies)
{
    const auto programs = DecodeMaterialPrograms(bytes, dependencies);
    Require(programs.size() == 1, "expected one opaque material program");
    return programs.front();
}

void ValidateRockMaterial(const RockMaterial &material)
{
    bool known_roles = false;
    for (const auto &roles : kEnvironmentTextures)
    {
        bool matches = true;
        for (std::size_t slot = 0; slot < material.textures.size(); ++slot)
            matches &= material.textures[slot] == roles[slot];
        known_roles |= matches;
    }
    const bool typed = TextureProductId(material.textures[0]);
    if (typed)
    {
        for (std::size_t slot = 0; slot < material.textures.size(); ++slot)
            Require((slot >= 3 && material.textures[slot].empty()) ||
                    TextureProductId(material.textures[slot]), "invalid texture package ID");
        const bool terrain = material.textures[0] ==
            "render/texture/environment_terrain_gaussian.rpkg";
        if (terrain)
            for (std::size_t slot = 0; slot < material.textures.size(); ++slot)
                Require(!material.textures[slot].empty(),
                        "terrain material requires all six texture packages");
        for (std::size_t slot = 4; slot < material.textures.size(); ++slot)
            Require(material.textures[slot].empty() || terrain,
                    "non-terrain material cannot bind terrain texture slots");
    }
    else
    {
        Require(known_roles, "unexpected texture role ID");
        Require(material.textures[4].empty() && material.textures[5].empty(),
                "legacy material contains terrain texture roles");
    }
    Require(std::isfinite(material.metallic) && material.metallic >= 0.0f &&
            material.metallic <= 1.0f && std::isfinite(material.roughness_scale) &&
            material.roughness_scale >= 0.0f && material.roughness_scale <= 2.0f,
            "invalid parameters");
}

EnvironmentMaterialVariant ClassifyEnvironmentMaterial(const RockMaterial &material)
{
    // Version 1 metadata has no alpha bit. Its leaf and grass roles still use
    // the established two-sided 0.5 alpha cutout path.
    const bool foliage = material.textures[0] == kEnvironmentTextures[2][0] ||
                         material.textures[0] == kEnvironmentTextures[3][0];
    return foliage || material.two_sided || material.alpha_mask
        ? EnvironmentMaterialVariant::MaskedTwoSided
        : EnvironmentMaterialVariant::OpaqueSingleSided;
}

std::vector<std::byte> EncodeRockMaterial(
    const RockMaterial &material, std::span<const std::string> dependencies)
{
    ValidateRockMaterial(material);
    std::vector<std::byte> bytes(kMaterialMetadataStride);
    const bool typed = TextureProductId(material.textures[0]);
    Put(bytes, 0, typed ? 4 : 2, 4); // v4 has six typed texture indices
    Put(bytes, 4, 1, 4); // surface domain
    Put(bytes, 8, 1, 4); // metallic-roughness model
    Put(bytes, 12, material.two_sided ? 1 : 0, 4);
    const auto texture_count = typed ? material.textures.size() : std::size_t{4};
    for (std::size_t i = 0; i < texture_count; ++i)
    {
        if (material.textures[i].empty())
        {
            Require(typed ? i >= 3 : i == 3, "required texture may not be absent");
            Put(bytes, 16 + i * 2, 0xffff, 2);
            continue;
        }
        const auto found = std::ranges::find(dependencies, material.textures[i]);
        Require(found != dependencies.end(), "texture is missing from dependency IDs");
        const auto index = static_cast<std::size_t>(found - dependencies.begin());
        Require(index < 65536, "too many material dependencies");
        Put(bytes, 16 + i * 2, static_cast<std::uint32_t>(index), 2);
    }
    if (typed)
    {
        Put(bytes, 28, std::bit_cast<std::uint32_t>(material.metallic), 4);
        Put(bytes, 32, std::bit_cast<std::uint32_t>(material.roughness_scale), 4);
        Put(bytes, 36, material.alpha_mask ? 1 : 0, 4);
    }
    else
    {
        Put(bytes, 24, std::bit_cast<std::uint32_t>(material.metallic), 4);
        Put(bytes, 28, std::bit_cast<std::uint32_t>(material.roughness_scale), 4);
        Put(bytes, 32, material.alpha_mask ? 1 : 0, 4);
    }
    return bytes;
}

RockMaterial DecodeRockMaterial(
    std::span<const std::byte> bytes, std::span<const std::string> dependencies)
{
    const bool historical_size = bytes.size() == kHistoricalMaterialMetadataStride;
    Require(bytes.size() == kMaterialMetadataStride || historical_size,
            "invalid typed metadata record size");
    const auto version = Get(bytes, 0, 4);
    Require((version >= 1 && version <= 4) && Get(bytes, 4, 4) == 1 &&
            Get(bytes, 8, 4) == 1 && Get(bytes, 12, 4) <= 1,
            "invalid typed metadata record");
    RockMaterial material;
    material.two_sided = Get(bytes, 12, 4) != 0;
    const bool v4 = version == 4;
    Require(!(v4 && historical_size), "v4 metadata requires the 40-byte record");
    if (!v4 && !historical_size)
        Require(Get(bytes, 36, 4) == 0, "historical metadata reserved bytes are nonzero");
    Require(v4 ? Get(bytes, 36, 4) <= 1 : Get(bytes, 32, 4) <= (version == 1 ? 0u : 1u),
            "invalid historical or v4 alpha flag");
    material.alpha_mask = v4 ? Get(bytes, 36, 4) != 0 : Get(bytes, 32, 4) != 0;
    const auto texture_count = v4 ? material.textures.size() : std::size_t{4};
    for (std::size_t i = 0; i < texture_count; ++i)
    {
        const auto index = Get(bytes, 16 + i * 2, 2);
        if (index == 0xffff && (i == 3 || v4 && i >= 4))
            continue;
        Require(index < dependencies.size(), "dependency index out of range");
        material.textures[i] = dependencies[index];
    }
    material.metallic = std::bit_cast<float>(Get(bytes, v4 ? 28 : 24, 4));
    material.roughness_scale = std::bit_cast<float>(Get(bytes, v4 ? 32 : 28, 4));
    ValidateRockMaterial(material);
    Require(v4 ? TextureProductId(material.textures[0]) :
                 (version == 3) == TextureProductId(material.textures[0]),
            "metadata version disagrees with typed texture dependency ABI");
    if (version == 2)
    {
        const bool foliage = material.textures[0] == kEnvironmentTextures[2][0] ||
                             material.textures[0] == kEnvironmentTextures[3][0];
        Require(material.two_sided == foliage && material.alpha_mask == foliage,
                "surface flags disagree with texture roles");
    }
    if (version == 1)
        Require(!TextureProductId(material.textures[0]),
                "historical metadata unexpectedly contains typed texture IDs");
    return material;
}
} // namespace hs::render_data
