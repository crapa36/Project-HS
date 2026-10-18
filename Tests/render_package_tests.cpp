#include <hs/core/cooked_format.hpp>
#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/material_ir.hpp>
#include <hs/render_data/character_package.hpp>
#include <hs/render_data/render_asset_format.hpp>
#include <hs/render_data/texture_package.hpp>
#include <hs/render_data/quality_profiles.hpp>
#include "../Source/Apps/RenderMaterialShader/material_graph_parser.hpp"
#include "../Source/Apps/AssetPipeline/render_shader_bindings.hpp"
#include <nlohmann/json.hpp>

#include <array>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <span>
#include <string>
#include <vector>
#include <utility>

namespace
{
namespace rd = hs::render_data;
bool dump_numeric_fixtures{};
std::vector<std::string> numeric_fixtures;

void Require(bool condition, const char *reason)
{
    if (!condition) throw std::runtime_error(reason);
}

template <typename Operation>
void Reject(Operation &&operation, const char *reason)
{
    try { operation(); }
    catch (const std::exception &) { return; }
    throw std::runtime_error(reason);
}

template <typename Operation>
void RunNamed(std::string_view name, Operation &&operation)
{
    try { operation(); }
    catch (const std::exception &error)
    {
        throw std::runtime_error(std::string(name) + ": " + error.what());
    }
}

void Put64(std::vector<std::byte> &bytes, std::size_t offset, std::uint64_t value)
{
    for (std::size_t i = 0; i < 8; ++i)
        bytes[offset + i] = std::byte((value >> (8 * i)) & 0xff);
}

void TestExpandedVertexFields()
{
    const rd::ExpandedVertex original{
        {1.0f, -2.0f, 3.5f}, {0.0f, 0.0f, -1.0f},
        {1, 2, 0x1234, 0xffff}, {0.25f, 0.25f, 0.25f, 0.25f},
        {0.5f, 0.75f}, {1.0f, 0.0f, 0.0f, -1.0f}, 0x1234, 0};
    const auto encoded = rd::EncodeExpandedVertices(std::span(&original, 1));
    Require(encoded.size() == rd::kExpandedVertexStride &&
            encoded[0] == std::byte{0x00} && encoded[1] == std::byte{0x00} &&
            encoded[2] == std::byte{0x80} && encoded[3] == std::byte{0x3f} &&
            encoded[20] == std::byte{0x00} && encoded[22] == std::byte{0x80} &&
            encoded[23] == std::byte{0xbf} &&
            encoded[24] == std::byte{0x01} && encoded[25] == std::byte{0x00} &&
            encoded[26] == std::byte{0x02} &&
            encoded[28] == std::byte{0x34} && encoded[29] == std::byte{0x12} &&
            encoded[30] == std::byte{0xff} && encoded[31] == std::byte{0xff} &&
            encoded[32] == std::byte{0x00} && encoded[34] == std::byte{0x80} &&
            encoded[35] == std::byte{0x3e} &&
            encoded[48] == std::byte{0x00} && encoded[51] == std::byte{0x3f} &&
            encoded[72] == std::byte{0x34} && encoded[73] == std::byte{0x12} &&
            encoded[74] == std::byte{0x00} && encoded[75] == std::byte{0x00},
            "expanded vertex field offsets or little-endian bytes changed");
    const auto decoded = rd::DecodeExpandedVertices(encoded);
    Require(decoded.size() == 1 && decoded[0].position == original.position &&
            decoded[0].normal == original.normal &&
            decoded[0].bone_indices == original.bone_indices &&
            decoded[0].bone_weights == original.bone_weights &&
            decoded[0].uv == original.uv &&
            decoded[0].tangent == original.tangent &&
            decoded[0].material_index == original.material_index &&
            decoded[0].reserved == 0, "expanded vertex field roundtrip");

    auto invalid = encoded;
    invalid[74] = std::byte{1};
    Reject([&] { (void)rd::DecodeExpandedVertices(invalid); },
           "nonzero vertex reserved field accepted");
    invalid = encoded;
    invalid[0] = std::byte{0}; invalid[1] = std::byte{0};
    invalid[2] = std::byte{0xc0}; invalid[3] = std::byte{0x7f};
    Reject([&] { (void)rd::DecodeExpandedVertices(invalid); },
           "nonfinite vertex position accepted");
}

std::uint64_t Get64(const std::vector<std::byte> &bytes, std::size_t offset)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i)
        value |= std::uint64_t(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
    return value;
}

void TestPackage()
{
    constexpr std::array<std::byte, 3> abc{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    constexpr std::array<std::uint8_t, 32> expected_hash{
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    const auto hash = rd::Sha256(abc);
    for (std::size_t i = 0; i < hash.size(); ++i)
        Require(std::to_integer<std::uint8_t>(hash[i]) == expected_hash[i], "SHA-256 digest");

    const auto asset = hs::MakeAssetId("render/rock_0.rpkg");
    rd::RockMaterial material{{"environment/environment_rock_basecolor.dds",
                                "environment/environment_rock_surface.dds",
                                "environment/environment_rock_normal.dds",
                                "environment/environment_rock_detail.dds"},
                               0.0f, 1.0f, false};
    const std::vector<std::string> dependencies{
        "environment/environment_rock_0.meshbin",
        "environment/environment_rock_basecolor.dds",
        "environment/environment_rock_detail.dds",
        "environment/environment_rock_normal.dds",
        "environment/environment_rock_surface.dds"};
    const auto metadata = rd::EncodeRockMaterial(material, dependencies);
    const auto decoded_material = rd::DecodeRockMaterial(metadata, dependencies);
    Require(decoded_material.textures == material.textures &&
            !decoded_material.two_sided && !decoded_material.alpha_mask,
            "opaque environment material roundtrip");
    Require(metadata.size() == rd::kMaterialMetadataStride &&
            rd::kMaterialMetadataStride == 40,
            "current material metadata record is not the explicit 40-byte v4 layout");
    auto historical_metadata = metadata;
    historical_metadata.resize(rd::kHistoricalMaterialMetadataStride);
    Require(rd::DecodeRockMaterial(historical_metadata, dependencies).textures == material.textures,
            "historical 36-byte material metadata was not explicitly decoded");
    const std::array historical_chunks{rd::ChunkSource{
        static_cast<std::uint32_t>(rd::ChunkType::MaterialMetadata), 1, true,
        historical_metadata, 1, rd::kHistoricalMaterialMetadataStride}};
    Require(rd::ReadPackage(rd::BuildPackage(asset, dependencies, historical_chunks), asset)
                .chunks.size() == 1,
            "historical material metadata chunk was not explicitly package-readable");
    Require(rd::ClassifyEnvironmentMaterial(decoded_material) ==
                rd::EnvironmentMaterialVariant::OpaqueSingleSided,
            "opaque environment variant");
    rd::RockMaterial foliage{{"environment/environment_leaf_basecolor.dds",
                               "environment/environment_leaf_roughness.dds",
                               "environment/environment_leaf_normal.dds", ""},
                              0.0f, 1.0f, true, true};
    const std::vector<std::string> foliage_dependencies{
        "environment/environment_leaf_basecolor.dds",
        "environment/environment_leaf_normal.dds",
        "environment/environment_leaf_roughness.dds"};
    const auto foliage_metadata = rd::EncodeRockMaterial(foliage, foliage_dependencies);
    const auto decoded_foliage = rd::DecodeRockMaterial(foliage_metadata, foliage_dependencies);
    Require(decoded_foliage.textures == foliage.textures &&
            decoded_foliage.two_sided && decoded_foliage.alpha_mask,
            "masked foliage material roundtrip");
    Require(rd::ClassifyEnvironmentMaterial(decoded_foliage) ==
                rd::EnvironmentMaterialVariant::MaskedTwoSided,
            "masked environment variant");
    auto invalid_foliage = foliage_metadata;
    invalid_foliage[32] = std::byte{0};
    Reject([&] { (void)rd::DecodeRockMaterial(invalid_foliage, foliage_dependencies); },
           "version 2 foliage without alpha mask accepted");
    auto invalid_opaque = metadata;
    invalid_opaque[12] = std::byte{1};
    Reject([&] { (void)rd::DecodeRockMaterial(invalid_opaque, dependencies); },
           "version 2 opaque material with two-sided flag accepted");
    auto legacy_foliage = foliage_metadata;
    legacy_foliage[0] = std::byte{1};
    legacy_foliage[32] = std::byte{0};
    Require(rd::ClassifyEnvironmentMaterial(
                rd::DecodeRockMaterial(legacy_foliage, foliage_dependencies)) ==
                rd::EnvironmentMaterialVariant::MaskedTwoSided,
            "version 1 foliage retains its cutout variant");
    const std::array<std::byte, 3 * rd::kExpandedVertexStride> vertices{};
    const std::array<std::byte, 4> future{std::byte{1}, std::byte{2},
                                           std::byte{3}, std::byte{4}};
    const std::array chunks{
        rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::ExpandedVertices),
                        1, true, vertices, 3, rd::kExpandedVertexStride},
        rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::MaterialMetadata),
                        1, true, metadata, 1, rd::kMaterialMetadataStride}};
    const auto package = rd::BuildPackage(asset, dependencies, chunks);
    const auto view = rd::ReadPackage(package, asset);
    Require(view.asset_id.value == asset.value && view.dependencies == dependencies &&
            view.chunks.size() == 2 && view.chunks[0].bytes.size() == vertices.size() &&
            view.chunks[1].bytes.size() == metadata.size() &&
            view.chunks[0].required && view.chunks[1].required,
            "valid package roundtrip");

    std::vector<rd::ChunkSource> optional_known(chunks.begin(), chunks.end());
    optional_known[1].required = false;
    const auto optional_known_package = rd::BuildPackage(asset, dependencies, optional_known);
    const auto optional_known_view = rd::ReadPackage(optional_known_package, asset);
    Require(optional_known_view.chunks.size() == 2 &&
            optional_known_view.chunks[0].required && !optional_known_view.chunks[1].required,
            "known chunk requirement flag was lost");

    auto truncated = package;
    truncated.pop_back();
    Reject([&] { (void)rd::ReadPackage(truncated, asset); }, "truncated package accepted");

    auto overflow = package;
    Put64(overflow, 56, UINT64_MAX - 7); // directory offset + count*stride
    Reject([&] { (void)rd::ReadPackage(overflow, asset); }, "overflow directory accepted");

    auto wrong_id = package;
    Put64(wrong_id, 16, asset.value + 1);
    Reject([&] { (void)rd::ReadPackage(wrong_id, asset); }, "wrong asset ID accepted");

    auto corrupt = package;
    corrupt.back() ^= std::byte{1};
    Reject([&] { (void)rd::ReadPackage(corrupt, asset); }, "changed chunk accepted");

    std::vector<rd::ChunkSource> optional(chunks.begin(), chunks.end());
    optional.push_back({0x9000, 1, false, future, 4, 1});
    const auto optional_package = rd::BuildPackage(asset, dependencies, optional);
    Require(rd::ReadPackage(optional_package, asset).chunks.size() == 2,
            "unknown optional chunk was not skipped");
    auto compressed_optional = optional_package;
    const auto directory = static_cast<std::size_t>(Get64(compressed_optional, 56));
    const auto entry = directory + 2 * 80;
    Put64(compressed_optional, entry + 16, 2); // future chunk uses an unknown compression.
    compressed_optional.resize(compressed_optional.size() - 2);
    Put64(compressed_optional, 88, compressed_optional.size());
    const auto future_hash = rd::Sha256(std::span<const std::byte>(compressed_optional).last(2));
    std::copy(future_hash.begin(), future_hash.end(), compressed_optional.begin() + entry + 48);
    const auto content_hash = rd::Sha256(std::span<const std::byte>(compressed_optional).subspan(96));
    std::copy(content_hash.begin(), content_hash.end(), compressed_optional.begin() + 24);
    Require(rd::ReadPackage(compressed_optional, asset).chunks.size() == 2,
            "future compressed optional chunk was not skipped");
    optional.back().required = true;
    const auto required_package = rd::BuildPackage(asset, dependencies, optional);
    Reject([&] { (void)rd::ReadPackage(required_package, asset); },
           "unknown required chunk accepted");
}

void TestQualityProfiles()
{
    rd::QualityProfiles table;
    for (std::size_t i = 0; i < table.profiles.size(); ++i)
    {
        auto &p = table.profiles[i];
        p = {4.0f, .35f, 2, .5f, 32, 4, 1024, .5f, 2, 4, 8, 1,
             .5f, 1, 16, 1, 16, 32, 1, 0, 8, 2, 4, 128, 1, 48,
             4, 3, 25, 0, 0, .25f, 12, 20, 2};
        p.geometry_screen_error_px += static_cast<float>(i);
        p.subsurface_half_samples += static_cast<std::uint32_t>(i);
    }
    table.auto_fg_cadence = {30, 40, 8};
    const auto encoded = rd::EncodeQualityProfiles(table);
    const auto decoded = rd::DecodeQualityProfiles(encoded);
    Require(rd::EncodeQualityProfiles(decoded) == encoded,
            "quality profile fields or cadence failed byte-exact roundtrip");
    const auto asset = hs::MakeAssetId(rd::kQualityProfilesProduct);
    const auto package = rd::ReadPackage(encoded, asset);
    const auto rows = std::ranges::find(package.chunks, rd::ChunkType::QualityProfiles, &rd::ChunkView::type);
    Require(rows != package.chunks.end() && rows->version == 2 && rows->count == 4 &&
            rows->stride == 140 && rows->bytes.size() == 560,
            "non-RT quality profiles must use version 2 and 140-byte records");
    auto legacy = encoded;
    const auto directory = static_cast<std::size_t>(Get64(legacy, 56));
    const auto row_index = static_cast<std::size_t>(rows - package.chunks.begin());
    legacy[directory + row_index * 80 + 4] = std::byte{1};
    const auto hash = rd::Sha256(std::span<const std::byte>(legacy).subspan(96));
    std::copy(hash.begin(), hash.end(), legacy.begin() + 24);
    Reject([&] { (void)rd::DecodeQualityProfiles(legacy); },
           "legacy RT quality profile version accepted as current layout");
    std::vector<std::byte> old_rows(4 * 176);
    const std::array old_chunks{rd::ChunkSource{
        static_cast<std::uint32_t>(rd::ChunkType::QualityProfiles), 1, true, old_rows, 4, 176}};
    Reject([&] { (void)rd::BuildPackage(asset, {}, old_chunks); },
           "legacy RT quality profile producer layout accepted");
}

void TestDecalMaterialPackages()
{
    const auto directory = std::filesystem::path(__FILE__).parent_path().parent_path() /
                           "ContentSource/Materials";
    for (const auto name : rd::kDecalMaterialNames)
    {
        std::ifstream file(directory / (std::string(name) + ".json"));
        Require(file.is_open(), "authored decal material source missing");
        const std::string source{std::istreambuf_iterator<char>(file), {}};
        const auto graph = hs::material_content::ParseDecalMaterialGraph(name, source);
        const auto encoded = rd::BuildDecalMaterialPackage(name, graph.parameters,
            graph.textures.front().product_id, "render/shader/decal_surface_cs.dxil", graph.decal_receivers);
        const auto decoded = rd::ReadDecalMaterialPackage(encoded, name);
        Require(decoded.receivers == graph.decal_receivers &&
                decoded.dependencies[decoded.texture_dependency_index] == graph.textures.front().product_id &&
                decoded.parameter_layout_hash == rd::MaterialParameterLayoutHash(graph.parameters),
                "decal material receiver, texture or parameter layout failed roundtrip");
        const auto asset = hs::MakeAssetId("render/material/" + std::string(name) + "/material.rpkg");
        const auto package = rd::ReadPackage(encoded, asset);
        const auto metadata = std::ranges::find(package.chunks, rd::ChunkType::DecalMaterialMetadata, &rd::ChunkView::type);
        Require(metadata != package.chunks.end() && metadata->version == 2 &&
                metadata->count == 1 && metadata->stride == 28 && metadata->bytes.size() == 28,
                "decal metadata must use version 2 and 28-byte records");
        const auto directory_offset = static_cast<std::size_t>(Get64(encoded, 56));
        const auto entry = directory_offset + static_cast<std::size_t>(metadata - package.chunks.begin()) * 80;
        auto legacy = encoded;
        legacy[entry + 4] = std::byte{1};
        auto hash = rd::Sha256(std::span<const std::byte>(legacy).subspan(96));
        std::copy(hash.begin(), hash.end(), legacy.begin() + 24);
        Reject([&] { (void)rd::ReadDecalMaterialPackage(legacy, name); },
               "legacy decal metadata version accepted as current layout");
        std::vector<std::byte> old_metadata(32);
        const std::array old_chunks{rd::ChunkSource{
            static_cast<std::uint32_t>(rd::ChunkType::DecalMaterialMetadata), 1, true, old_metadata, 1, 32}};
        Reject([&] { (void)rd::BuildPackage(asset, {}, old_chunks); },
               "legacy decal metadata producer layout accepted");
        for (const auto offset : {22u, 24u})
        {
            auto invalid = encoded;
            const auto payload = static_cast<std::size_t>(Get64(invalid, entry + 8));
            invalid[payload + offset] = std::byte{1};
            hash = rd::Sha256(std::span<const std::byte>(invalid).subspan(payload, 28));
            std::copy(hash.begin(), hash.end(), invalid.begin() + entry + 48);
            hash = rd::Sha256(std::span<const std::byte>(invalid).subspan(96));
            std::copy(hash.begin(), hash.end(), invalid.begin() + 24);
            Reject([&] { (void)rd::ReadDecalMaterialPackage(invalid, name); },
                   "nonzero decal metadata reserved field accepted");
        }
        auto invalid = nlohmann::json::parse(source);
        invalid["render_flags"]["rt_policy"] = "raster_only";
        Reject([&] { (void)hs::material_content::ParseDecalMaterialGraph(name, invalid.dump()); },
               "removed decal authoring policy accepted");
    }
}

void TestMaterialParameters()
{
    const auto f = [](float value) { return std::bit_cast<std::uint32_t>(value); };
    const std::array parameters{
        rd::MaterialParameterDefinition{"enabled", rd::MaterialParameterType::Bool, {1, 0, 0, 0}},
        rd::MaterialParameterDefinition{"count", rd::MaterialParameterType::Uint, {7, 0, 0, 0}, 2, 9},
        rd::MaterialParameterDefinition{"gain", rd::MaterialParameterType::Float, {f(0.5f), 0, 0, 0}},
        rd::MaterialParameterDefinition{"offset", rd::MaterialParameterType::Float2, {f(1.0f), f(-2.0f), 0, 0}},
        rd::MaterialParameterDefinition{"tint", rd::MaterialParameterType::Float3, {f(1.0f), f(0.5f), f(0.25f), 0}},
        rd::MaterialParameterDefinition{"plane", rd::MaterialParameterType::Float4, {f(1.0f), f(2.0f), f(3.0f), f(4.0f)}}};
    rd::ValidateMaterialParameters(parameters);
    const auto encoded = rd::EncodeMaterialParameters(parameters);
    const auto decoded = rd::DecodeMaterialParameters(encoded);
    Require(decoded.size() == parameters.size() && decoded[1].uint_min == 2 &&
            decoded[1].uint_max == 9 && decoded[4].default_words[2] == f(0.25f),
            "material parameter declaration roundtrip");
    auto changed = decoded;
    changed[2].default_words[0] = f(0.75f);
    Require(rd::MaterialParameterLayoutHash(parameters) == rd::MaterialParameterLayoutHash(changed),
            "material parameter layout identity includes defaults");
    const auto layout = rd::EncodeMaterialParameterLayout(parameters);
    Require(rd::DecodeMaterialParameterLayout(layout) == rd::MaterialParameterLayoutHash(parameters),
            "material parameter layout digest encoding");
    auto invalid = parameters;
    invalid[0].default_words[0] = 2;
    Reject([&] { rd::ValidateMaterialParameters(invalid); }, "invalid bool material parameter accepted");
    invalid = parameters;
    invalid[1].uint_min = 10;
    Reject([&] { rd::ValidateMaterialParameters(invalid); }, "invalid uint material parameter range accepted");
    invalid = parameters;
    invalid[3].default_words[2] = 1;
    Reject([&] { rd::ValidateMaterialParameters(invalid); }, "nonzero unused parameter lane accepted");
    invalid = parameters;
    invalid[2].default_words[0] = 0x7fc00000u;
    Reject([&] { rd::ValidateMaterialParameters(invalid); }, "nonfinite parameter default accepted");

    const auto asset = hs::MakeAssetId("render/material/parameter_fixture.rpkg");
    const auto package = rd::BuildMaterialParameterPackage(
        "render/material/parameter_fixture.rpkg", parameters);
    const auto package_roundtrip = rd::ReadMaterialParameterPackage(package, asset);
    Require(package_roundtrip == decoded, "material parameter package roundtrip");
    const auto package_view = rd::ReadPackage(package, asset);
    const auto parameter_chunk = std::ranges::find(package_view.chunks,
        rd::ChunkType::MaterialParameters, &rd::ChunkView::type);
    const auto layout_chunk = std::ranges::find(package_view.chunks,
        rd::ChunkType::MaterialParameterLayout, &rd::ChunkView::type);
    Require(parameter_chunk != package_view.chunks.end() && parameter_chunk->stride == 1 &&
            parameter_chunk->count == parameter_chunk->bytes.size() &&
            layout_chunk != package_view.chunks.end() && layout_chunk->count == 1 &&
            layout_chunk->stride == 32,
            "material parameter package chunk contracts");
    auto corrupt = encoded;
    corrupt[8 + 2] ^= std::byte{1}; // first declaration reserved bytes must remain zero
    Reject([&] { (void)rd::DecodeMaterialParameters(corrupt); },
           "corrupt material parameter payload accepted");
}

void TestMaterialGraph()
{
    namespace mc = hs::material_content;
    const auto document = [](std::string_view nodes, std::string_view color,
                             std::string_view roughness) {
        return std::string{"{\"id\":\"rock_0\",\"version\":2,\"domain\":\"surface\","
            "\"shading_model\":\"metallic_roughness\",\"parameters\":{},\"textures\":{},"
            "\"render_flags\":{\"two_sided\":false,\"alpha_mode\":\"opaque\"},\"nodes\":"} + std::string(nodes) +
            ",\"outputs\":{\"surface\":\"builtin_metallic_roughness\",\"base_color\":{"
            "\"node\":\"" + std::string(color) + "\",\"type\":\"float3\"},"
            "\"roughness\":{\"node\":\"" + std::string(roughness) +
            "\",\"type\":\"float\"}}}";
    };
    constexpr std::string_view identity_nodes = R"([
        {"id":"sampled","op":"sampled_base_color","type":"float3"},
        {"id":"rough_sample","op":"sampled_roughness","type":"float"},
        {"id":"rough_scale","op":"parameter","name":"roughness_scale","type":"float"},
        {"id":"rough_product","op":"multiply","type":"float",
         "a":{"node":"rough_sample","type":"float"},
         "b":{"node":"rough_scale","type":"float"}},
        {"id":"rough_final","op":"saturate","type":"float",
         "input":{"node":"rough_product","type":"float"}}])";
    const auto identity = mc::ParseRock0MaterialGraph(
        document(identity_nodes, "sampled", "rough_final"));
    for (const std::string_view invalid_id : {"42", "null", "\"rock_1\""})
    {
        auto invalid = document(identity_nodes, "sampled", "rough_final");
        const auto id_value = invalid.find("\"rock_0\"");
        Require(id_value != std::string::npos, "material ID fixture");
        invalid.replace(id_value, std::string_view("\"rock_0\"").size(), invalid_id);
        Reject([&] { (void)mc::ParseRock0MaterialGraph(invalid); },
               "non-string or mismatched material ID accepted");
    }
    const auto emitted = rd::EmitRock0MaterialHlsl(identity);
    Require(identity.nodes.size() == 5 && identity.base_color_output == 0 &&
            identity.roughness_output == 4 &&
            emitted.find("float3 HsRock0BaseColor(float3 sampled_base_color)") !=
                std::string::npos &&
            emitted.find("float HsRock0Roughness(float sampled_roughness, float roughness_scale)") !=
                std::string::npos &&
            emitted.find("float n3 = n1 * n2;") != std::string::npos &&
            emitted.find("float n4 = saturate(n3);") != std::string::npos,
            "identity roughness graph did not emit sampled * scale and saturation");
    auto wrong_type = std::string(identity_nodes);
    const auto edge = wrong_type.find("\"rough_scale\",\"type\":\"float\"}");
    Require(edge != std::string::npos, "scalar edge fixture");
    wrong_type.replace(edge, std::string("\"rough_scale\",\"type\":\"float\"}").size(),
                       "\"rough_scale\",\"type\":\"float3\"}");
    Reject([&] { (void)mc::ParseRock0MaterialGraph(
        document(wrong_type, "sampled", "rough_final")); },
        "scalar-to-vector edge accepted");
    auto wrong_ir = identity;
    wrong_ir.nodes[3].inputs[1] = 0; // bypass JSON parser: typed IR must still reject it.
    Reject([&] { rd::ValidateRock0MaterialGraph(wrong_ir); },
           "typed IR accepted mismatched scalar input");
    Reject([&] { (void)mc::ParseRock0MaterialGraph(document(
        R"([{"id":"sampled","op":"sampled_base_color","type":"float3"},
             {"id":"rough_a","op":"multiply","type":"float",
              "a":{"node":"rough_b","type":"float"},"b":{"node":"rough_b","type":"float"}},
             {"id":"rough_b","op":"multiply","type":"float",
              "a":{"node":"rough_a","type":"float"},"b":{"node":"rough_a","type":"float"}}])",
        "sampled", "rough_a")); }, "scalar cycle accepted");
    const auto dead = mc::ParseRock0MaterialGraph(document(
        R"([{"id":"sampled","op":"sampled_base_color","type":"float3"},
             {"id":"rough_sample","op":"sampled_roughness","type":"float"},
             {"id":"orphan","op":"constant","type":"float","value":1.0}])",
        "sampled", "rough_sample"));
    Require(dead.nodes.size() == 3 &&
            rd::EmitRock0MaterialHlsl(dead).find("n2") == std::string::npos,
            "dead scalar node was not pruned");

    const auto leaf_document = [](std::string_view nodes, std::string_view output,
                                  std::string_view type = "float") {
        return std::string{"{\"id\":\"leaf_0\",\"version\":2,\"domain\":\"surface\","
            "\"shading_model\":\"metallic_roughness\",\"parameters\":{},\"textures\":{},"
            "\"render_flags\":{\"two_sided\":true,\"alpha_mode\":\"mask\"},\"nodes\":"} + std::string(nodes) +
            ",\"outputs\":{\"surface\":\"builtin_metallic_roughness\","
            "\"opacity\":{\"node\":\"" + std::string(output) + "\",\"type\":\"" +
            std::string(type) + "\"}}}";
    };
    const auto leaf = mc::ParseLeaf0MaterialGraph(leaf_document(
        R"([{"id":"alpha","op":"sampled_alpha","type":"float"}])", "alpha"));
    const auto leaf_hlsl = rd::EmitLeaf0MaterialHlsl(leaf);
    Require(leaf.kind == rd::MaterialGraphKind::Leaf0 && leaf.nodes.size() == 1 &&
            leaf.opacity_output == 0 &&
            leaf_hlsl.find("float HsLeaf0Opacity(float sampled_alpha)") != std::string::npos &&
            leaf_hlsl.find("float n0 = sampled_alpha;\n    return n0;") !=
                std::string::npos, "leaf opacity identity was not emitted");
    Reject([&] { (void)mc::ParseLeaf0MaterialGraph(leaf_document(
        R"([{"id":"alpha","op":"sampled_alpha","type":"float3"}])",
        "alpha", "float3")); }, "vector opacity accepted");
    Reject([&] { (void)mc::ParseLeaf0MaterialGraph(leaf_document(
        R"([{"id":"alpha","op":"sampled_roughness","type":"float"}])",
        "alpha")); }, "rock sampled roughness accepted in leaf graph");
}

void TestGenericMaterialOutputInputs()
{
    namespace mc = hs::material_content;
    const std::string source = R"({"id":"leaf_1","version":2,"domain":"surface",
        "shading_model":"metallic_roughness","parameters":{},"textures":{},
        "render_flags":{"two_sided":true,"alpha_mode":"mask"},
        "nodes":[
            {"id":"color","op":"sampled_base_color","type":"float3"},
            {"id":"rough","op":"sampled_roughness","type":"float"},
            {"id":"scale","op":"parameter","name":"roughness_scale","type":"float"},
            {"id":"rough_product","op":"multiply","type":"float",
             "a":{"node":"rough","type":"float"},"b":{"node":"scale","type":"float"}},
            {"id":"alpha","op":"sampled_alpha","type":"float"},
            {"id":"gain","op":"constant","type":"float","value":0.5},
            {"id":"opacity_product","op":"multiply","type":"float",
             "a":{"node":"alpha","type":"float"},"b":{"node":"gain","type":"float"}},
            {"id":"opacity","op":"saturate","type":"float",
             "input":{"node":"opacity_product","type":"float"}}],
        "outputs":{"surface":"builtin_metallic_roughness",
            "base_color":{"node":"color","type":"float3"},
            "roughness":{"node":"rough_product","type":"float"},
            "opacity":{"node":"opacity","type":"float"}}})";
    const auto graph = mc::ParseEnvironmentMaterialGraph("leaf_1", source);
    const auto hlsl = rd::EmitEnvironmentMaterialHlsl(graph);
    Require(graph.kind == rd::MaterialGraphKind::Masked &&
            hlsl.find("float HsMaterialRoughness(float sampled, float scale)\n{\n"
                      "    float n1 = sampled;\n    float n2 = scale;\n"
                      "    float n3 = n1 * n2;\n    return n3;") != std::string::npos &&
            hlsl.find("float HsMaterialOpacity(float sampled)\n{\n"
                      "    float n4 = sampled;\n    float n5 = 0.5;\n"
                      "    float n6 = n4 * n5;\n    float n7 = saturate(n6);\n"
                      "    return n7;") != std::string::npos,
            "generic masked expression lost output-local inputs");

    const auto reject_input = [&](std::size_t node, rd::MaterialOp op,
                                  std::string_view original, std::string_view replacement) {
        auto invalid_source = source;
        const auto at = invalid_source.find(original);
        Require(at != std::string::npos, "generic scalar input fixture");
        invalid_source.replace(at, original.size(), replacement);
        Reject([&] { (void)mc::ParseEnvironmentMaterialGraph("leaf_1", invalid_source); },
               "parser accepted cross-output scalar input");
        auto invalid_ir = graph;
        invalid_ir.nodes[node].op = op;
        Reject([&] { rd::ValidateEnvironmentMaterialGraph(invalid_ir); },
               "typed IR accepted cross-output scalar input");
        Reject([&] { (void)rd::EmitEnvironmentMaterialHlsl(invalid_ir); },
               "emitter accepted cross-output scalar input");
    };
    reject_input(1, rd::MaterialOp::SampledAlpha,
        "\"op\":\"sampled_roughness\"", "\"op\":\"sampled_alpha\"");
    reject_input(4, rd::MaterialOp::SampledRoughness,
        "\"op\":\"sampled_alpha\"", "\"op\":\"sampled_roughness\"");
    reject_input(4, rd::MaterialOp::RoughnessScale,
        "\"op\":\"sampled_alpha\"", "\"op\":\"parameter\",\"name\":\"roughness_scale\"");
}

void TestMaterialPrograms()
{
    const std::vector<std::string> rock_deps{std::string(rd::kRock0ShaderProduct)};
    const auto rock_bytes = rd::EncodeMaterialProgram(rock_deps, rd::kRock0ShaderProduct);
    const auto rock = rd::DecodeMaterialProgram(rock_bytes, rock_deps);
    Require(rock_bytes.size() == rd::kMaterialProgramStride &&
            rock.kind == rd::MaterialProgramKind::OpaqueScenePS &&
            rock.dependency_index == 0, "rock program record changed");

    const std::vector<std::string> leaf_deps{
        std::string(rd::kLeaf0SceneShaderProduct),
        std::string(rd::kLeaf0ShadowShaderProduct)};
    const std::array sources{
        rd::MaterialProgramSource{rd::kLeaf0SceneShaderProduct,
                                  rd::MaterialProgramKind::MaskedScenePS},
        rd::MaterialProgramSource{rd::kLeaf0ShadowShaderProduct,
                                  rd::MaterialProgramKind::MaskedShadowPS}};
    const auto leaf_bytes = rd::EncodeMaterialPrograms(leaf_deps, sources);
    const auto leaf = rd::DecodeMaterialPrograms(leaf_bytes, leaf_deps);
    Require(leaf_bytes.size() == 2 * rd::kMaterialProgramStride && leaf.size() == 2 &&
            leaf[0].kind == rd::MaterialProgramKind::MaskedScenePS &&
            leaf[0].dependency_index == 0 &&
            leaf[1].kind == rd::MaterialProgramKind::MaskedShadowPS &&
            leaf[1].dependency_index == 1, "paired leaf program records changed");
    Reject([&] { (void)rd::DecodeMaterialProgram(leaf_bytes, leaf_deps); },
           "legacy one-record decoder accepted leaf pair");
    auto duplicate = leaf_bytes;
    duplicate[6] = duplicate[2];
    duplicate[7] = duplicate[3];
    Reject([&] { (void)rd::DecodeMaterialPrograms(duplicate, leaf_deps); },
           "duplicate scene kind accepted");
    Reject([&] { (void)rd::DecodeMaterialPrograms(
        std::span(leaf_bytes).first(rd::kMaterialProgramStride), leaf_deps); },
        "missing shadow kind accepted");
    auto wrong_dependency = leaf_bytes;
    wrong_dependency[4] = std::byte{0};
    Reject([&] { (void)rd::DecodeMaterialPrograms(wrong_dependency, leaf_deps); },
           "shadow kind with scene dependency accepted");
    const auto asset = hs::MakeAssetId("render/leaf_0.rpkg");
    const std::array chunks{rd::ChunkSource{
        static_cast<std::uint32_t>(rd::ChunkType::MaterialProgram), 1, true,
        leaf_bytes, 2, rd::kMaterialProgramStride}};
    const auto package = rd::BuildPackage(asset, leaf_deps, chunks);
    Require(rd::ReadPackage(package, asset).chunks.size() == 1,
            "two-record MaterialProgram package rejected");
    const std::array invalid_chunks{rd::ChunkSource{
        static_cast<std::uint32_t>(rd::ChunkType::MaterialProgram), 1, true,
        duplicate, 2, rd::kMaterialProgramStride}};
    Reject([&] { (void)rd::ReadPackage(rd::BuildPackage(asset, leaf_deps, invalid_chunks),
                                       asset); },
           "package reader accepted duplicate required program kind");
}

std::size_t MaterialNode(rd::MaterialGraph &graph, rd::MaterialType type, rd::MaterialOp op,
                         std::initializer_list<std::size_t> inputs = {})
{
    rd::MaterialNode node;
    node.id = "node_" + std::to_string(graph.nodes.size());
    node.type = type;
    node.op = op;
    std::copy(inputs.begin(), inputs.end(), node.inputs.begin());
    graph.nodes.push_back(node);
    return graph.nodes.size() - 1;
}

rd::MaterialGraph NumericGraph()
{
    rd::MaterialGraph graph;
    MaterialNode(graph, rd::MaterialType::Float3, rd::MaterialOp::SampledBaseColor);
    MaterialNode(graph, rd::MaterialType::Float, rd::MaterialOp::SampledRoughness);
    graph.base_color_output = 0;
    graph.roughness_output = 1;
    return graph;
}

std::size_t FloatConstant(rd::MaterialGraph &graph, rd::MaterialType type, float value)
{
    const auto index = MaterialNode(graph, type, rd::MaterialOp::Constant);
    graph.nodes[index].value.fill(value);
    return index;
}

std::size_t UintConstant(rd::MaterialGraph &graph, std::uint32_t value)
{
    const auto index = MaterialNode(graph, rd::MaterialType::Uint, rd::MaterialOp::Constant);
    graph.nodes[index].uint_value = value;
    return index;
}

void NumericOutput(rd::MaterialGraph &graph, std::size_t index)
{
    const auto type = graph.nodes[index].type;
    if (type == rd::MaterialType::Bool || type == rd::MaterialType::Uint)
        index = MaterialNode(graph, rd::MaterialType::Float, rd::MaterialOp::Convert, {index});
    else if (type != rd::MaterialType::Float)
    {
        const auto component = rd::MaterialComponentCount(type) - 1;
        index = MaterialNode(graph, rd::MaterialType::Float, rd::MaterialOp::Extract, {index});
        graph.nodes[index].component = static_cast<std::uint32_t>(component);
    }
    graph.roughness_output = index;
}

std::string MaterialTypeName(rd::MaterialType type)
{
    switch (type)
    {
    case rd::MaterialType::Float: return "float";
    case rd::MaterialType::Float2: return "float2";
    case rd::MaterialType::Float3: return "float3";
    case rd::MaterialType::Float4: return "float4";
    case rd::MaterialType::Bool: return "bool";
    case rd::MaterialType::Uint: return "uint";
    default: throw std::runtime_error("invalid test type");
    }
}

nlohmann::json MaterialDocument(const rd::MaterialGraph &graph)
{
    using Json = nlohmann::json;
    constexpr std::array names{"sampled_base_color", "sampled_roughness", "sampled_alpha",
        "roughness_scale", "parameter", "constant", "multiply", "saturate", "add", "subtract", "divide",
        "min", "max", "negate", "lerp", "clamp", "construct", "extract", "convert",
        "less", "less_equal", "equal", "select", "logical_and", "logical_or", "logical_not",
        "texture_ref", "sampler_ref", "context", "sample_grad", "sample_level",
        "decode_normal_xy", "normal_rnm", "detail_fade", "tangent_to_world",
        "seeded_noise_2d", "fresnel_schlick"};
    const auto edge = [&](std::size_t index) {
        return Json{{"node", graph.nodes.at(index).id},
                    {"type", MaterialTypeName(graph.nodes.at(index).type)}};
    };
    Json nodes = Json::array();
    for (const auto &node : graph.nodes)
    {
        Json item{{"id", node.id}, {"op", names.at(static_cast<std::size_t>(node.op))},
                  {"type", MaterialTypeName(node.type)}};
        if (node.op == rd::MaterialOp::RoughnessScale) item["name"] = "roughness_scale";
        else if (node.op == rd::MaterialOp::Constant)
        {
            if (node.type == rd::MaterialType::Bool) item["value"] = node.bool_value;
            else if (node.type == rd::MaterialType::Uint) item["value"] = node.uint_value;
            else if (node.type == rd::MaterialType::Float) item["value"] = node.value[0];
            else
            {
                item["value"] = Json::array();
                for (std::size_t i = 0; i < rd::MaterialComponentCount(node.type); ++i)
                    item["value"].push_back(node.value[i]);
            }
        }
        else if (node.op == rd::MaterialOp::Construct)
        {
            item["components"] = Json::array();
            for (std::size_t i = 0; i < rd::MaterialComponentCount(node.type); ++i)
                item["components"].push_back(edge(node.inputs[i]));
        }
        else if (node.op == rd::MaterialOp::Select)
        {
            item["condition"] = edge(node.inputs[0]);
            item["a"] = edge(node.inputs[1]); item["b"] = edge(node.inputs[2]);
        }
        else if (node.op == rd::MaterialOp::Clamp)
        {
            item["input"] = edge(node.inputs[0]);
            item["min"] = edge(node.inputs[1]); item["max"] = edge(node.inputs[2]);
        }
        else if (node.op == rd::MaterialOp::SeededNoise2D)
        {
            item["position"] = edge(node.inputs[0]);
            item["seed"] = edge(node.inputs[1]);
        }
        else if (node.op == rd::MaterialOp::FresnelSchlick)
        {
            item["f0"] = edge(node.inputs[0]);
            item["cos_theta"] = edge(node.inputs[1]);
        }
        else if (rd::MaterialInputCount(node.op, node.type) >= 2)
        {
            item["a"] = edge(node.inputs[0]); item["b"] = edge(node.inputs[1]);
            if (node.op == rd::MaterialOp::Lerp) item["t"] = edge(node.inputs[2]);
        }
        else if (rd::MaterialInputCount(node.op, node.type) == 1)
        {
            item["input"] = edge(node.inputs[0]);
            if (node.op == rd::MaterialOp::Extract) item["component"] = node.component;
        }
        nodes.push_back(std::move(item));
    }
    return Json{{"id", "rock_0"}, {"version", 2}, {"domain", "surface"},
                {"shading_model", "metallic_roughness"}, {"parameters", Json::object()},
                {"textures", Json::object()}, {"nodes", nodes},
                {"render_flags", {{"two_sided", false}, {"alpha_mode", "opaque"}}},
                {"outputs", {{"surface", "builtin_metallic_roughness"},
                             {"base_color", edge(graph.base_color_output)},
                             {"roughness", edge(graph.roughness_output)}}}};
}

std::string NumericHlsl(const rd::MaterialGraph &graph)
{
    const auto hlsl = rd::EmitRock0MaterialHlsl(graph);
    const auto parsed = hs::material_content::ParseRock0MaterialGraph(MaterialDocument(graph).dump());
    // JSON topological sorting may renumber nodes; both paths must emit the
    // operation and result, while repeated parses must produce identical HLSL.
    const auto parsed_hlsl = rd::EmitRock0MaterialHlsl(parsed);
    Require(parsed_hlsl == rd::EmitRock0MaterialHlsl(
        hs::material_content::ParseRock0MaterialGraph(MaterialDocument(graph).dump())),
        "material emission is nondeterministic");
    const auto start = hlsl.find("float HsRock0Roughness");
    Require(start != std::string::npos, "roughness function missing");
    if (dump_numeric_fixtures &&
        hlsl.substr(start).find(" = sampled_roughness;") != std::string::npos)
    {
        auto generic = graph;
        generic.kind = rd::MaterialGraphKind::Opaque;
        numeric_fixtures.push_back(rd::EmitEnvironmentMaterialHlsl(generic));
    }
    return hlsl.substr(start);
}

void DumpNumericMaterialHlsl(const std::filesystem::path &directory)
{
    Require(!numeric_fixtures.empty(), "no dynamic material fixtures collected");
    std::string source = "// Generated by hs_render_package_tests --dump-material-hlsl.\n";
    for (std::size_t i = 0; i < numeric_fixtures.size(); ++i)
    {
        auto fixture = numeric_fixtures[i];
        for (const std::string_view function : {"HsMaterialBaseColor", "HsMaterialRoughness",
                "HsMaterialOpacity", "HsMaterialNoiseHash", "HsMaterialNoiseRandom",
                "HsMaterialNoiseCell", "HsMaterialSeededNoise2D", "HsMaterialFresnelSchlick"})
        {
            const auto replacement = "Fixture" + std::to_string(i) + "_" + std::string(function);
            std::size_t at = 0;
            while ((at = fixture.find(function, at)) != std::string::npos)
            {
                fixture.replace(at, function.size(), replacement);
                at += replacement.size();
            }
        }
        source += fixture;
    }
    source += "float4 MaterialNumericPS(float3 sampled : TEXCOORD0, float scale : TEXCOORD1) : SV_Target\n"
              "{\n    float3 color = float3(0.0, 0.0, 0.0);\n"
              "    float roughness = 0.0;\n    float opacity = 0.0;\n";
    for (std::size_t i = 0; i < numeric_fixtures.size(); ++i)
    {
        const auto prefix = "Fixture" + std::to_string(i) + "_";
        source += "    color += " + prefix + "HsMaterialBaseColor(sampled);\n";
        source += "    roughness += " + prefix + "HsMaterialRoughness(sampled.x, scale);\n";
        source += "    opacity += " + prefix + "HsMaterialOpacity(sampled.y);\n";
    }
    source += "    return float4(color + float3(roughness, opacity, 0.0), 1.0);\n}\n";
    std::filesystem::create_directories(directory);
    const auto path = directory / "material_numeric_fixture.hlsl";
    std::ofstream file(path, std::ios::binary);
    Require(file.is_open(), "cannot open numeric HLSL dump");
    file.write(source.data(), static_cast<std::streamsize>(source.size()));
    file.close();
    Require(file.good(), "cannot write numeric HLSL dump");
    std::cout << "material numeric HLSL: " << path.string() << " ("
              << numeric_fixtures.size() << " dynamic fixtures)\n";
}

void TestNumericMaterialOperations()
{
    constexpr std::array floating{rd::MaterialType::Float, rd::MaterialType::Float2,
                                  rd::MaterialType::Float3, rd::MaterialType::Float4};
    constexpr std::array operations{
        std::pair{rd::MaterialOp::Add, " + "}, std::pair{rd::MaterialOp::Subtract, " - "},
        std::pair{rd::MaterialOp::Multiply, " * "}, std::pair{rd::MaterialOp::Divide, " / "},
        std::pair{rd::MaterialOp::Min, "min("}, std::pair{rd::MaterialOp::Max, "max("},
        std::pair{rd::MaterialOp::Negate, " = -n"}, std::pair{rd::MaterialOp::Saturate, "saturate("},
        std::pair{rd::MaterialOp::Lerp, "lerp("}, std::pair{rd::MaterialOp::Clamp, "clamp("}};
    constexpr std::array expected{"12.0", "4.0", "32.0", "2.0", "4.0",
                                 "8.0", "-8.0", "1.0", "0.0", "2.0"};
    for (const auto type : floating)
    {
        for (std::size_t operation = 0; operation < operations.size(); ++operation)
        {
            const auto [op, token] = operations[operation];
            auto graph = NumericGraph();
            const auto a = FloatConstant(graph, type, 8.0f);
            const auto b = FloatConstant(graph, type, 4.0f);
            const auto c = FloatConstant(graph, type, 2.0f);
            const auto result = rd::MaterialInputCount(op, type) == 1
                ? MaterialNode(graph, type, op, {a})
                : rd::MaterialInputCount(op, type) == 2
                    ? MaterialNode(graph, type, op, {a, b})
                    : MaterialNode(graph, type, op, {a, b, c});
            NumericOutput(graph, result);
            const auto folded = NumericHlsl(graph);
            Require(folded.find(std::string(" = ") + expected[operation] + ";") != std::string::npos &&
                    folded.find(token) == std::string::npos &&
                    folded.find("sampled;") == std::string::npos,
                    "floating arithmetic fold or dependency pruning failed");
            if (type == rd::MaterialType::Float) graph.nodes[a].op = rd::MaterialOp::SampledRoughness;
            else
            {
                graph.nodes[a].op = rd::MaterialOp::Construct;
                for (std::size_t i = 0; i < rd::MaterialComponentCount(type); ++i)
                    graph.nodes[a].inputs[i] = 1;
            }
            Require(NumericHlsl(graph).find(token) != std::string::npos,
                    "dynamic floating operation was not emitted");
        }
    }

    for (const auto [op, token] : std::span(operations).first(6))
    {
        auto graph = NumericGraph();
        const auto flag = MaterialNode(graph, rd::MaterialType::Bool, rd::MaterialOp::Convert, {1});
        const auto dynamic = MaterialNode(graph, rd::MaterialType::Uint, rd::MaterialOp::Convert, {flag});
        const auto divisor = UintConstant(graph, 3);
        NumericOutput(graph, MaterialNode(graph, rd::MaterialType::Uint, op, {dynamic, divisor}));
        const auto hlsl = NumericHlsl(graph);
        Require(hlsl.find("uint(") != std::string::npos && hlsl.find("float(") != std::string::npos &&
                hlsl.find(token) != std::string::npos, "dynamic uint operation or explicit conversion missing");
    }
    constexpr std::array uint_cases{
        std::array<std::uint32_t, 3>{UINT32_MAX, 1, 0},
        std::array<std::uint32_t, 3>{0, 1, UINT32_MAX},
        std::array<std::uint32_t, 3>{0x80000001u, 2, 2},
        std::array<std::uint32_t, 3>{9, 3, 3},
        std::array<std::uint32_t, 3>{8, 4, 4},
        std::array<std::uint32_t, 3>{8, 4, 8}};
    for (std::size_t i = 0; i < uint_cases.size(); ++i)
    {
        auto graph = NumericGraph();
        const auto a = UintConstant(graph, uint_cases[i][0]);
        const auto b = UintConstant(graph, uint_cases[i][1]);
        const auto result = MaterialNode(graph, rd::MaterialType::Uint, operations[i].first, {a, b});
        const auto expected_value = UintConstant(graph, uint_cases[i][2]);
        NumericOutput(graph, MaterialNode(graph, rd::MaterialType::Bool, rd::MaterialOp::Equal,
                                          {result, expected_value}));
        Require(NumericHlsl(graph).find(" = 1.0;") != std::string::npos,
                "uint folding lost wraparound or integer precision");
    }
    auto precision = NumericGraph();
    const auto exact = UintConstant(precision, 16777217u);
    const auto rounded = UintConstant(precision, 16777216u);
    NumericOutput(precision, MaterialNode(precision, rd::MaterialType::Bool, rd::MaterialOp::Equal,
                                          {exact, rounded}));
    Require(NumericHlsl(precision).find(" = 0.0;") != std::string::npos,
            "uint constants rounded through float before comparison");

    auto uint_clamp = NumericGraph();
    const auto x = UintConstant(uint_clamp, 8), lo = UintConstant(uint_clamp, 1),
               hi = UintConstant(uint_clamp, 4);
    NumericOutput(uint_clamp, MaterialNode(uint_clamp, rd::MaterialType::Uint,
                                          rd::MaterialOp::Clamp, {x, lo, hi}));
    Require(NumericHlsl(uint_clamp).find(" = 4.0;") != std::string::npos, "uint clamp fold failed");

    auto library_graph = NumericGraph();
    const auto position = MaterialNode(library_graph, rd::MaterialType::Float2,
                                        rd::MaterialOp::Construct, {1, 1});
    const auto seed = UintConstant(library_graph, 17);
    const auto noise = MaterialNode(library_graph, rd::MaterialType::Float,
                                    rd::MaterialOp::SeededNoise2D, {position, seed});
    const auto duplicate_noise = MaterialNode(library_graph, rd::MaterialType::Float,
                                               rd::MaterialOp::SeededNoise2D, {position, seed});
    const auto roughness = MaterialNode(library_graph, rd::MaterialType::Float,
                                         rd::MaterialOp::Multiply, {noise, duplicate_noise});
    library_graph.roughness_output = roughness;
    const auto cos_theta = FloatConstant(library_graph, rd::MaterialType::Float, 0.5f);
    library_graph.base_color_output = MaterialNode(library_graph, rd::MaterialType::Float3,
        rd::MaterialOp::FresnelSchlick, {0, cos_theta});
    const auto library_full = rd::EmitRock0MaterialHlsl(library_graph);
    Require(library_full.find("float HsMaterialSeededNoise2D(float2 p, uint seed)") != std::string::npos &&
            library_full.find("float3 HsMaterialFresnelSchlick(float3 f0, float cos_theta)") !=
                std::string::npos &&
            library_full.find("HsMaterialSeededNoise2D(n") != std::string::npos &&
            library_full.find("HsMaterialSeededNoise2D(n", library_full.find("HsMaterialSeededNoise2D(n") + 1) ==
                std::string::npos,
            "seeded noise or Schlick Fresnel emission/CSE changed");
    Require(NumericHlsl(library_graph).find("HsMaterialSeededNoise2D(n") != std::string::npos,
            "library node parser roundtrip lost seeded noise emission");
}

void TestMaterialLogicAndOptimizer()
{
    for (const auto [op, token] : {std::pair{rd::MaterialOp::Less, " < "},
                                   std::pair{rd::MaterialOp::LessEqual, " <= "},
                                   std::pair{rd::MaterialOp::Equal, " == "}})
    {
        for (const auto type : {rd::MaterialType::Float, rd::MaterialType::Uint})
        {
            auto graph = NumericGraph();
            auto source = std::size_t{1};
            const auto limit = type == rd::MaterialType::Uint ? UintConstant(graph, UINT32_MAX)
                : FloatConstant(graph, rd::MaterialType::Float, 0.5f);
            if (type == rd::MaterialType::Uint)
            {
                source = MaterialNode(graph, rd::MaterialType::Bool, rd::MaterialOp::Convert, {source});
                source = MaterialNode(graph, rd::MaterialType::Uint, rd::MaterialOp::Convert, {source});
            }
            const auto compare = MaterialNode(graph, rd::MaterialType::Bool, op, {source, limit});
            NumericOutput(graph, MaterialNode(graph, type, rd::MaterialOp::Select, {compare, source, limit}));
            const auto hlsl = NumericHlsl(graph);
            Require(hlsl.find(token) != std::string::npos && hlsl.find(" ? ") != std::string::npos &&
                    (type != rd::MaterialType::Uint || hlsl.find("4294967295u") != std::string::npos),
                    "typed comparison/select or full uint literal missing");
        }
    }
    for (const auto [op, token] : {std::pair{rd::MaterialOp::LogicalAnd, " && "},
                                   std::pair{rd::MaterialOp::LogicalOr, " || "},
                                   std::pair{rd::MaterialOp::LogicalNot, " = !n"},
                                   std::pair{rd::MaterialOp::Equal, " == "}})
    {
        auto graph = NumericGraph();
        const auto a = MaterialNode(graph, rd::MaterialType::Bool, rd::MaterialOp::Convert, {1});
        const auto b = MaterialNode(graph, rd::MaterialType::Bool, rd::MaterialOp::Constant);
        graph.nodes[b].bool_value = true;
        const auto result = op == rd::MaterialOp::LogicalNot
            ? MaterialNode(graph, rd::MaterialType::Bool, op, {a})
            : MaterialNode(graph, rd::MaterialType::Bool, op, {a, b});
        NumericOutput(graph, result);
        Require(NumericHlsl(graph).find(token) != std::string::npos, "bool operation emission failed");
        graph.nodes[a].op = rd::MaterialOp::Constant;
        graph.nodes[a].inputs.fill(rd::kNoMaterialInput);
        graph.nodes[a].bool_value = false;
        const auto expected = op == rd::MaterialOp::LogicalOr || op == rd::MaterialOp::LogicalNot
            ? " = 1.0;" : " = 0.0;";
        Require(NumericHlsl(graph).find(expected) != std::string::npos, "bool operation folding failed");
    }
    for (const auto type : {rd::MaterialType::Float2, rd::MaterialType::Float3, rd::MaterialType::Float4})
    {
        auto graph = NumericGraph();
        const auto vector = MaterialNode(graph, type, rd::MaterialOp::Construct);
        for (std::size_t i = 0; i < rd::MaterialComponentCount(type); ++i)
            graph.nodes[vector].inputs[i] = 1;
        const auto identity = MaterialNode(graph, type, rd::MaterialOp::Convert, {vector});
        NumericOutput(graph, identity);
        const auto hlsl = NumericHlsl(graph);
        Require(hlsl.find(MaterialTypeName(type) + "(") != std::string::npos &&
                hlsl.find("." + std::string(1, "xyzw"[rd::MaterialComponentCount(type) - 1])) != std::string::npos &&
                hlsl.find(" n3 = ") == std::string::npos, "construct/extract or identity conversion failed");
    }

    auto graph = NumericGraph();
    const auto half = FloatConstant(graph, rd::MaterialType::Float, 0.5f);
    const auto a = MaterialNode(graph, rd::MaterialType::Float, rd::MaterialOp::Add, {1, half});
    const auto duplicate = MaterialNode(graph, rd::MaterialType::Float, rd::MaterialOp::Add, {1, half});
    const auto product = MaterialNode(graph, rd::MaterialType::Float, rd::MaterialOp::Multiply, {a, duplicate});
    const auto condition = MaterialNode(graph, rd::MaterialType::Bool, rd::MaterialOp::Constant);
    graph.nodes[condition].bool_value = true;
    graph.roughness_output = MaterialNode(graph, rd::MaterialType::Float, rd::MaterialOp::Select,
                                          {condition, product, half});
    FloatConstant(graph, rd::MaterialType::Float4, 9.0f); // globally dead
    const auto hlsl = NumericHlsl(graph);
    Require(hlsl.find("n3 * n3") != std::string::npos &&
            hlsl.find(" n4 = ") == std::string::npos && hlsl.find(" n6 = ") == std::string::npos &&
            hlsl.find(" n7 = ") == std::string::npos && hlsl.find("float4") == std::string::npos &&
            hlsl.find("float3") == std::string::npos, "CSE, constant select or output-local pruning failed");
}

void TestUnsafeMaterialGraphs()
{
    const auto reject = [](const rd::MaterialGraph &graph) {
        Reject([&] { rd::ValidateRock0MaterialGraph(graph); }, "unsafe typed graph accepted");
        Reject([&] { (void)hs::material_content::ParseRock0MaterialGraph(MaterialDocument(graph).dump()); },
               "unsafe JSON graph accepted");
    };
    auto graph = NumericGraph();
    const auto dynamic_uint = MaterialNode(graph, rd::MaterialType::Uint, rd::MaterialOp::Convert, {1});
    NumericOutput(graph, dynamic_uint);
    reject(graph);
    for (const auto value : {-1.0f, 16.0f})
    {
        graph = NumericGraph();
        const auto input = FloatConstant(graph, rd::MaterialType::Float, value);
        auto conversion_input = input;
        if (value > 0)
        {
            // 16^(2^3) = 2^32, the first float outside uint conversion range.
            for (int i = 0; i < 3; ++i)
                conversion_input = MaterialNode(graph, rd::MaterialType::Float,
                    rd::MaterialOp::Multiply, {conversion_input, conversion_input});
        }
        NumericOutput(graph, MaterialNode(graph, rd::MaterialType::Uint,
                                          rd::MaterialOp::Convert, {conversion_input}));
        reject(graph);
    }
    graph = NumericGraph();
    const auto fractional = FloatConstant(graph, rd::MaterialType::Float, 3.75f);
    NumericOutput(graph, MaterialNode(graph, rd::MaterialType::Uint, rd::MaterialOp::Convert, {fractional}));
    Require(NumericHlsl(graph).find(" = 3.0;") != std::string::npos,
            "safe float to uint did not truncate toward zero");
    graph = NumericGraph();
    const auto vector = FloatConstant(graph, rd::MaterialType::Float2, 1);
    NumericOutput(graph, MaterialNode(graph, rd::MaterialType::Float, rd::MaterialOp::Convert, {vector}));
    reject(graph);
    graph = NumericGraph();
    const auto bad_vector = MaterialNode(graph, rd::MaterialType::Float3, rd::MaterialOp::Construct, {1, 1});
    graph.base_color_output = bad_vector;
    Reject([&] { rd::ValidateRock0MaterialGraph(graph); }, "missing construct input accepted");
    graph = NumericGraph();
    const auto v = FloatConstant(graph, rd::MaterialType::Float2, 1);
    graph.roughness_output = MaterialNode(graph, rd::MaterialType::Float, rd::MaterialOp::Extract, {v});
    graph.nodes.back().component = 2;
    reject(graph);
    graph = NumericGraph();
    graph.roughness_output = MaterialNode(graph, rd::MaterialType::Float, rd::MaterialOp::Add, {1, 0});
    reject(graph);
    graph = NumericGraph();
    const auto zero = FloatConstant(graph, rd::MaterialType::Float, 0);
    graph.roughness_output = MaterialNode(graph, rd::MaterialType::Float, rd::MaterialOp::Divide, {1, zero});
    reject(graph);
    graph = NumericGraph();
    const auto cyclic = MaterialNode(graph, rd::MaterialType::Float, rd::MaterialOp::Add, {1, 1});
    graph.nodes[cyclic].inputs[0] = cyclic; // dead cycles must still fail
    reject(graph);
    graph = NumericGraph();
    graph.nodes[1].inputs[0] = 0;
    Reject([&] { rd::ValidateRock0MaterialGraph(graph); }, "extra source input accepted");
    graph = NumericGraph();
    graph.nodes[1].op = static_cast<rd::MaterialOp>(999);
    Reject([&] { rd::ValidateRock0MaterialGraph(graph); }, "unknown IR operation accepted");
    graph = NumericGraph();
    FloatConstant(graph, rd::MaterialType::Float, std::numeric_limits<float>::infinity());
    Reject([&] { rd::ValidateRock0MaterialGraph(graph); }, "nonfinite dead constant accepted");
    graph = NumericGraph();
    FloatConstant(graph, rd::MaterialType::Float4, 17);
    reject(graph);
    graph = NumericGraph();
    const auto extreme_position = FloatConstant(graph, rd::MaterialType::Float2,
                                                 std::numeric_limits<float>::max());
    const auto extreme_seed = UintConstant(graph, 17);
    graph.roughness_output = MaterialNode(graph, rd::MaterialType::Float,
        rd::MaterialOp::SeededNoise2D, {extreme_position, extreme_seed});
    reject(graph);
    auto document = MaterialDocument(NumericGraph());
    for (const auto op : {"sample_grad", "world_position", "closure", "hlsl"})
    {
        document["nodes"][0]["op"] = op;
        Reject([&] { (void)hs::material_content::ParseRock0MaterialGraph(document.dump()); },
               "unsupported context or arbitrary HLSL node accepted");
    }
    document = MaterialDocument(NumericGraph());
    document["domain"] = "vertex";
    Reject([&] { (void)hs::material_content::ParseRock0MaterialGraph(document.dump()); },
           "unsupported stage accepted");
    graph = NumericGraph();
    const auto uint_node = UintConstant(graph, UINT32_MAX);
    NumericOutput(graph, uint_node);
    document = MaterialDocument(graph);
    for (const auto value : {nlohmann::json(-1), nlohmann::json(4294967296ull),
                             nlohmann::json(3.5), nlohmann::json(true)})
    {
        document["nodes"][uint_node]["value"] = value;
        Reject([&] { (void)hs::material_content::ParseRock0MaterialGraph(document.dump()); },
               "invalid uint JSON constant accepted");
    }

    auto library_graph = NumericGraph();
    const auto position = MaterialNode(library_graph, rd::MaterialType::Float2,
                                        rd::MaterialOp::Construct, {1, 1});
    const auto seed = UintConstant(library_graph, 17);
    const auto noise = MaterialNode(library_graph, rd::MaterialType::Float,
                                    rd::MaterialOp::SeededNoise2D, {position, seed});
    library_graph.roughness_output = noise;
    library_graph.nodes[noise].type = rd::MaterialType::Float3;
    reject(library_graph);

    library_graph = NumericGraph();
    const auto bad_position = MaterialNode(library_graph, rd::MaterialType::Float2,
                                            rd::MaterialOp::Construct, {1, 1});
    const auto bad_seed = FloatConstant(library_graph, rd::MaterialType::Float, 17.0f);
    const auto bad_noise = MaterialNode(library_graph, rd::MaterialType::Float,
                                        rd::MaterialOp::SeededNoise2D, {bad_position, bad_seed});
    library_graph.roughness_output = bad_noise;
    reject(library_graph);

    library_graph = NumericGraph();
    const auto fresnel_cos = FloatConstant(library_graph, rd::MaterialType::Float, 0.5f);
    const auto bad_fresnel = MaterialNode(library_graph, rd::MaterialType::Float3,
                                          rd::MaterialOp::FresnelSchlick, {0, fresnel_cos});
    library_graph.base_color_output = bad_fresnel;
    library_graph.nodes[bad_fresnel].inputs[1] = 0;
    reject(library_graph);
}

void TestRenderBindingCompilerContract()
{
    namespace sc = hs::shader_content;
    std::vector<std::wstring> arguments{L"-WX"};
    sc::AppendRenderBindingArguments(arguments);
    Require(arguments.front()==L"-WX" && arguments.size()==sc::kRenderBindingDefines.size()+1,
            "binding definitions replaced compiler options");
    std::string canonical;
    for(std::size_t i=0;i<sc::kRenderBindingDefines.size();++i)
    {
        const auto &[name,value]=sc::kRenderBindingDefines[i];
        Require(std::ranges::count(sc::kRenderBindingDefines,name,
                    [](const auto &definition) { return definition.first; })==1,
                "duplicate compiler binding definition");
        const std::string definition=std::string(name)+"="+std::to_string(value);
        Require(arguments[i+1]==L"-D"+std::wstring(definition.begin(),definition.end()),
                "compiler definitions differ from the action identity");
        canonical+=definition+"\n";
    }
    Require(canonical==sc::RenderBindingContract(), "binding action identity is not canonical");
    const auto value=[&](std::string_view name) {
        const auto found=std::ranges::find(sc::kRenderBindingDefines,name,
            [](const auto &definition) { return definition.first; });
        Require(found!=sc::kRenderBindingDefines.end(),"required compiler binding missing");
        return found->second;
    };
    Require(value("HS_RENDER_GEOMETRY_PAGE_CAPACITY")==rd::kGeometryPageCapacity &&
            value("HS_RENDER_GEOMETRY_BUFFERS_PER_PAGE")==rd::kGeometryBuffersPerPage &&
            rd::kGeometryDescriptorCount==1+rd::kGeometryPageCapacity*rd::kGeometryBuffersPerPage &&
            value("HS_RENDER_AUTHORED_MATERIAL_COUNT")==rd::kAuthoredMaterialNames.size() &&
            value("HS_RENDER_HERO_MATERIAL_COUNT")==rd::kHeroMaterialNames.size() &&
            value("HS_RENDER_ENVIRONMENT_GEOMETRY_COUNT")==rd::kEnvironmentMaterialNames.size() &&
            value("HS_RENDER_ENVIRONMENT_GEOMETRY_LOD_COUNT")==rd::kEnvironmentGeometryLodCount &&
            value("HS_RENDER_MATERIAL_TEXTURE_SLOT_COUNT")==rd::kMaterialTextureSlots &&
            value("HS_RENDER_GROUND_DETAIL_GEOMETRY_BEGIN")==rd::kGroundDetailGeometryBegin &&
            value("HS_RENDER_GROUND_DETAIL_MATERIAL_ORDINAL")==rd::kGroundDetailMaterialOrdinal,
            "compiler binding definitions diverged from the public catalog");
    Require(value("HS_RENDER_ENVIRONMENT_GEOMETRY_COUNT")*value("HS_RENDER_ENVIRONMENT_GEOMETRY_LOD_COUNT")==105 &&
            value("HS_RENDER_AUTHORED_MATERIAL_COUNT")*value("HS_RENDER_MATERIAL_TEXTURE_SLOT_COUNT")==96 &&
            1+value("HS_RENDER_AUTHORED_MATERIAL_COUNT")+value("HS_RENDER_HERO_MATERIAL_COUNT")==24,
            "existing geometry, feedback or visibility ABI changed");
}

void TestGroundDetailCatalogMapping()
{
    Require(static_cast<std::uint32_t>(hs::RenderMesh::DirtPatch)==20u &&
            static_cast<std::uint32_t>(hs::RenderMesh::GroundDetail)==21u &&
            static_cast<std::uint32_t>(hs::RenderMesh::MeadowSurface)==22u &&
            rd::kEnvironmentMaterialNames[33]=="meadow_ground" && rd::kEnvironmentMaterialNames[34]=="meadow_water",
            "existing render mesh ordinals changed");
    Require(rd::kEnvironmentMaterialNames.size()==35 && rd::kAuthoredMaterialNames.size()==16 &&
            rd::kAuthoredMaterialNames[14]=="terrain" && rd::kAuthoredMaterialNames[15]=="ground_detail",
            "geometry append changed fixed terrain/material ordinals");
    constexpr std::array additions{"fence_broken", "fence_splitrail", "sack_tied", "fallen_log", "rock_mossy"};
    Require(hs::kGroundDetailVariantCount==19, "ground-detail variant append is incomplete");
    for (std::size_t i=0;i<additions.size();++i)
        Require(rd::kEnvironmentMaterialNames[28+i]==additions[i], "new prop geometry order changed");
    for(std::uint32_t ordinal=0;ordinal<rd::kEnvironmentMaterialNames.size();++ordinal)
    {
        const auto mesh=rd::EnvironmentMeshForOrdinal(ordinal);
        const auto variant=ordinal<3?ordinal:ordinal<6?ordinal-3:ordinal<10?ordinal-6:
            ordinal<14?ordinal-10:ordinal<33?ordinal-14:ordinal-33;
        Require(rd::IsEnvironmentMesh(mesh) && rd::EnvironmentGeometryOrdinal(mesh,variant)==ordinal,
                "geometry family/variant mapping is not reversible");
        Require(rd::EnvironmentGeometryMaterialOrdinal(ordinal)==(ordinal<14?ordinal:ordinal==33?14u:15u),
                "ground geometry ordinal leaked into the shared material table");
        hs::RenderInstance instance{};instance.mesh=mesh;instance.environment_variant=variant;
        hs::PopulateRenderInstanceRevisions(instance);
        Require(instance.asset_id.value==hs::MakeAssetId("render/"+std::string(rd::kEnvironmentMaterialNames[ordinal])+".rpkg").value,
                "core geometry asset identity differs from the cooked catalog");
        Require(instance.material_id.value==(ordinal<14?instance.asset_id.value:ordinal==33?hs::MakeAssetId("render/material/terrain.rpkg").value:hs::MakeAssetId("render/material/ground_detail.rpkg").value),
                "detail material identity differs from the actual standalone product");
    }
    Require(!rd::IsEnvironmentMesh(hs::RenderMesh::DirtPatch) &&
            rd::EnvironmentGeometryOrdinal(hs::RenderMesh::Ground,0)==UINT32_MAX,
            "terrain was mistaken for an environment geometry family");
}

void TestAuthoredEnvironmentMaterials()
{
    namespace mc = hs::material_content;
    constexpr std::array<std::pair<std::string_view, bool>, 14> materials{{
        {"trunk_0", false}, {"trunk_1", false}, {"trunk_2", false},
        {"rock_0", false}, {"rock_1", false}, {"rock_2", false}, {"rock_3", false},
        {"leaf_0", true}, {"leaf_1", true}, {"leaf_2", true},
        {"grass_0", true}, {"grass_1", true}, {"grass_2", true}, {"grass_3", true}}};
    const auto directory = std::filesystem::path(__FILE__).parent_path().parent_path() /
                           "ContentSource/Materials";
    for (const auto &[name, masked] : materials)
    {
        try
        {
            std::ifstream file(directory / (std::string(name) + ".json"));
            Require(file.is_open(), "authored material source missing");
            const std::string source{std::istreambuf_iterator<char>(file), {}};
            const auto source_document = nlohmann::json::parse(source);
            const bool authored_typed_closure =
                source_document.at("outputs").contains("surface") &&
                source_document.at("outputs").at("surface") == "typed_closure";
            Require(mc::IsEnvironmentMaterialName(name) &&
                    mc::IsMaskedEnvironmentMaterial(name) == masked,
                    "authored material classification changed");
            const auto definition = mc::ParseMaterialDefinition(name, source);
            const auto &graph = definition.graph;
            const auto expected_kind = masked ? rd::MaterialGraphKind::Masked
                                              : rd::MaterialGraphKind::Opaque;
            Require(graph.kind == expected_kind, "authored graph kind changed");
            const auto ordinal = static_cast<std::size_t>(std::ranges::find(rd::kEnvironmentMaterialNames, name) -
                                                          rd::kEnvironmentMaterialNames.begin());
            const auto hlsl = rd::EmitEnvironmentMaterialContextHlsl(graph, ordinal);
            for (const auto signature : {
                     "HsMaterialSurface HsMaterialEvaluateSurface(HsMaterialContext ctx)",
                     "float HsMaterialEvaluateOpacity(HsMaterialContext ctx)",
                     "result.normal_is_world = false"})
                Require(hlsl.find(signature) != std::string::npos,
                        "generic material shader function missing");
            const bool typed_closure = graph.closure_output != rd::kNoMaterialInput;
            Require(graph.textures.size() == (masked ? 3u : 4u) &&
                    (authored_typed_closure ? graph.normal_output == rd::kNoMaterialInput &&
                                                  graph.ao_output == rd::kNoMaterialInput
                                            : graph.normal_output != rd::kNoMaterialInput &&
                                                  graph.ao_output != rd::kNoMaterialInput) &&
                    definition.material.textures[0] == graph.textures[0].product_id &&
                    hlsl.find("register(t" + std::to_string(ordinal * rd::kMaterialTextureSlots) +
                               ", space4)") != std::string::npos,
                    "authored texture declaration, binding or normal/AO output missing");
            if (typed_closure)
                Require(graph.nodes[graph.closure_output].type == rd::MaterialType::SurfaceClosure &&
                        hlsl.find("#include \"material_surface.hlsli\"") != std::string::npos &&
                        hlsl.find("HsMaterialMakeSurfaceClosure") != std::string::npos &&
                        hlsl.find("MaterialParameters[ctx.parameter_base + 0]") != std::string::npos,
                        "typed closure graph or shared surface ABI missing");
            if (!authored_typed_closure)
            {
                const auto &closure = graph.nodes[graph.closure_output];
                Require(closure.inputs[0] == graph.base_color_output &&
                        closure.inputs[1] == graph.normal_output &&
                        closure.inputs[2] == graph.roughness_output &&
                        closure.inputs[4] == graph.ao_output &&
                        graph.nodes[closure.inputs[3]].op == rd::MaterialOp::Parameter &&
                        graph.parameters[graph.nodes[closure.inputs[3]].declaration_index].id == "metallic" &&
                        (!masked ? graph.nodes[closure.inputs[5]].op == rd::MaterialOp::Constant
                                 : closure.inputs[5] == graph.opacity_output),
                        "legacy graph outputs were not lowered into the typed closure fields");
            }
            const auto opacity_hlsl = hlsl.substr(hlsl.find("float HsMaterialEvaluateOpacity"));
            Require(opacity_hlsl.find("HsMaterialTexture1") == std::string::npos &&
                    opacity_hlsl.find("HsMaterialTexture2") == std::string::npos &&
                    opacity_hlsl.find("HsMaterialTexture3") == std::string::npos,
                    "opacity evaluator retained unrelated texture samples");

            const auto shader = "render/shader/" + std::string(name);
            const auto scene = shader + "_scene.dxil";
            const auto shadow = shader + "_shadow.dxil";
            std::vector<std::string> dependencies{"environment/texture.dds", scene};
            std::vector<rd::MaterialProgramSource> programs{
                {scene, masked ? rd::MaterialProgramKind::MaskedScenePS
                               : rd::MaterialProgramKind::OpaqueScenePS}};
            if (masked)
            {
                dependencies.push_back(shadow);
                programs.push_back({shadow, rd::MaterialProgramKind::MaskedShadowPS});
            }
            const auto bytes = rd::EncodeMaterialPrograms(dependencies, programs);
            const auto decoded = rd::DecodeMaterialPrograms(bytes, dependencies);
            Require(decoded.size() == programs.size(), "authored program count changed");
            for (std::size_t i = 0; i < decoded.size(); ++i)
                Require(decoded[i].kind == programs[i].kind &&
                        decoded[i].dependency_index == i + 1 &&
                        dependencies[decoded[i].dependency_index] == programs[i].shader_id,
                        "authored program dependency or kind changed");
            if (masked)
            {
                Reject([&] { (void)rd::DecodeMaterialPrograms(
                    std::span(bytes).first(rd::kMaterialProgramStride), dependencies); },
                    "masked material accepted without shadow program");
                dependencies.back() = "render/shader/other_material_shadow.dxil";
                Reject([&] { (void)rd::DecodeMaterialPrograms(bytes, dependencies); },
                       "masked scene/shadow pair accepted different materials");
            }
        }
        catch (const std::exception &error)
        {
            throw std::runtime_error(std::string(name) + ": " + error.what());
        }
    }
}

void TestAuthoredTerrainMaterial()
{
    namespace mc = hs::material_content;
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "ContentSource/Materials/terrain.json";
    std::ifstream file(path);
    Require(file.is_open(), "terrain material source missing");
    const std::string source{std::istreambuf_iterator<char>(file), {}};
    const auto document = nlohmann::json::parse(source);
    Require(document.at("version") == 6 && document.at("textures").size() == 6 &&
            document.at("samplers").size() == 2,
            "terrain schema fixture lost its v6 resource contract");
    const auto definition = mc::ParseMaterialDefinition("terrain", source);
    const auto &graph = definition.graph;
    Require(mc::IsAuthoredMaterialName("terrain") && !mc::IsEnvironmentMaterialName("terrain") &&
            graph.kind == rd::MaterialGraphKind::Terrain && graph.textures.size() == 6 &&
            graph.samplers.size() == 2 && graph.terrain_output != rd::kNoMaterialInput &&
            graph.closure_output != rd::kNoMaterialInput,
            "terrain material classification or declarations changed");
    const auto &terrain_closure = graph.nodes[graph.closure_output];
    Require(graph.nodes[terrain_closure.inputs[0]].type == rd::MaterialType::Float3 &&
            graph.nodes[terrain_closure.inputs[1]].type == rd::MaterialType::Float3 &&
            graph.nodes[terrain_closure.inputs[2]].type == rd::MaterialType::Float &&
            graph.nodes[terrain_closure.inputs[4]].type == rd::MaterialType::Float &&
            graph.nodes[terrain_closure.inputs[3]].op == rd::MaterialOp::Parameter &&
            graph.parameters[graph.nodes[terrain_closure.inputs[3]].declaration_index].id == "metallic" &&
            graph.nodes[terrain_closure.inputs[5]].op == rd::MaterialOp::Constant,
            "terrain typed outputs were not lowered into the typed closure fields");
    const auto aggregate = std::ranges::find_if(graph.nodes, [](const rd::MaterialNode &node) {
        return node.op == rd::MaterialOp::TerrainStochastic;
    });
    Require(aggregate != graph.nodes.end() && rd::MaterialInputCount(
                aggregate->op, aggregate->type) == 15,
            "terrain aggregate signature input count changed");
    for (std::size_t i = 0; i < 15; ++i)
        Require(aggregate->inputs[i] != rd::kNoMaterialInput,
                "terrain aggregate input was left uninitialized");
    std::array<bool, 5> fields{};
    for (const auto &node : graph.nodes)
        if (node.op == rd::MaterialOp::TerrainField)
            fields[static_cast<std::size_t>(node.terrain_field)] = true;
    Require(std::ranges::all_of(fields, [](bool present) { return present; }),
            "terrain aggregate field projections are incomplete");

    const auto ordinal = static_cast<std::size_t>(
        std::ranges::find(rd::kAuthoredMaterialNames, std::string_view{"terrain"}) -
        rd::kAuthoredMaterialNames.begin());
    const auto hlsl = rd::EmitEnvironmentMaterialContextHlsl(graph, ordinal);
    Require(hlsl.find("#include \"material_terrain.hlsli\"") != std::string::npos &&
            hlsl.find("HsMaterialTerrainStochastic(") != std::string::npos &&
            hlsl.find("HsMaterialTexture5") != std::string::npos &&
            hlsl.find("ctx.mesh") != std::string::npos &&
            hlsl.find("ctx.seed") != std::string::npos &&
            hlsl.find(", ctx.uv)") != std::string::npos &&
            hlsl.find("ctx.view_direction") != std::string::npos &&
            hlsl.find("MaterialParameters[ctx.parameter_base + 1]") != std::string::npos &&
            hlsl.find("result.normal_is_world = true") != std::string::npos,
            "terrain HLSL context evaluator or resource ABI missing");
    std::size_t evaluations = 0;
    for (std::size_t at = hlsl.find("HsMaterialTerrainStochastic("); at != std::string::npos;
         at = hlsl.find("HsMaterialTerrainStochastic(", at + 1))
        ++evaluations;
    Require(evaluations == 1, "terrain aggregate was emitted more than once");

    auto invalid = graph;
    invalid.nodes[static_cast<std::size_t>(aggregate - graph.nodes.begin())].inputs[14] =
        rd::kNoMaterialInput;
    Reject([&] { rd::ValidateTerrainMaterialGraph(invalid); },
           "terrain aggregate accepted a missing context signature input");
    invalid = graph;
    invalid.textures.pop_back();
    Reject([&] { rd::ValidateTerrainMaterialGraph(invalid); },
           "terrain aggregate accepted an invalid texture count");
    invalid = graph;
    invalid.samplers[1].profile = rd::MaterialSamplerProfile::LinearWrap;
    Reject([&] { rd::ValidateTerrainMaterialGraph(invalid); },
           "terrain aggregate accepted an invalid sampler profile");
    const auto aggregate_index = static_cast<std::size_t>(aggregate - graph.nodes.begin());
    invalid = graph;
    invalid.nodes[aggregate_index].inputs[8] = invalid.nodes[aggregate_index].inputs[9];
    Reject([&] { rd::ValidateTerrainMaterialGraph(invalid); },
           "terrain aggregate accepted a gradient context in the world slot");
    invalid = graph;
    rd::MaterialNode substituted_constant;
    substituted_constant.id = "invalid_terrain_world_constant";
    substituted_constant.op = rd::MaterialOp::Constant;
    substituted_constant.type = rd::MaterialType::Float3;
    substituted_constant.value = {0.0f, 0.0f, 0.0f, 0.0f};
    invalid.nodes.push_back(substituted_constant);
    invalid.nodes[aggregate_index].inputs[8] = invalid.nodes.size() - 1;
    Reject([&] { rd::ValidateTerrainMaterialGraph(invalid); },
           "terrain aggregate accepted a constant in the world slot");
    invalid = graph;
    rd::MaterialNode substituted_arithmetic;
    substituted_arithmetic.id = "invalid_terrain_world_product";
    substituted_arithmetic.op = rd::MaterialOp::Multiply;
    substituted_arithmetic.type = rd::MaterialType::Float3;
    substituted_arithmetic.inputs[0] = invalid.nodes[aggregate_index].inputs[8];
    substituted_arithmetic.inputs[1] = invalid.nodes[aggregate_index].inputs[9];
    invalid.nodes.push_back(substituted_arithmetic);
    invalid.nodes[aggregate_index].inputs[8] = invalid.nodes.size() - 1;
    Reject([&] { rd::ValidateTerrainMaterialGraph(invalid); },
           "terrain aggregate accepted arithmetic in the world slot");

    std::vector<std::string> dependencies;
    for (const auto &texture : graph.textures) dependencies.push_back(texture.product_id);
    const auto bindings = rd::EncodeMaterialBindings(dependencies, graph.textures);
    const auto decoded_bindings = rd::DecodeMaterialBindings(bindings, dependencies);
    Require(decoded_bindings.size() == 6 &&
            decoded_bindings[0].semantic == rd::MaterialTextureSemantic::Gaussian &&
            decoded_bindings[4].semantic == rd::MaterialTextureSemantic::Height &&
            decoded_bindings[5].semantic == rd::MaterialTextureSemantic::Histogram,
            "terrain binding semantic roles changed");
    auto material = definition.material;
    const auto metadata = rd::EncodeRockMaterial(material, dependencies);
    Require(metadata.size() == 40 &&
            rd::DecodeRockMaterial(metadata, dependencies).textures == material.textures,
            "terrain v4 metadata roundtrip failed");
    material.two_sided = true;
    material.alpha_mask = false;
    const auto independent_flags = rd::DecodeRockMaterial(
        rd::EncodeRockMaterial(material, dependencies), dependencies);
    Require(independent_flags.two_sided && !independent_flags.alpha_mask,
            "v4 metadata coupled two-sided and alpha flags");
}

void TestSurfaceClosure()
{
    namespace mc = hs::material_content;
    using Json = nlohmann::json;
    const auto directory = std::filesystem::path(__FILE__).parent_path().parent_path() /
                           "ContentSource/Materials";
    std::ifstream file(directory / "rock_0.json");
    Require(file.is_open(), "surface closure fixture missing");
    const std::string source{std::istreambuf_iterator<char>(file), {}};
    const auto definition = mc::ParseMaterialDefinition("rock_0", source);
    const auto &graph = definition.graph;
    Require(graph.closure_output != rd::kNoMaterialInput &&
            graph.nodes[graph.closure_output].op == rd::MaterialOp::MetallicRoughnessClosure &&
            rd::MaterialInputCount(rd::MaterialOp::MetallicRoughnessClosure,
                                   rd::MaterialType::SurfaceClosure) == 6,
            "typed surface closure root or signature missing");
    const auto hlsl = rd::EmitEnvironmentMaterialContextHlsl(graph, 3);
    Require(hlsl.find("#include \"material_surface.hlsli\"") != std::string::npos &&
            hlsl.find("HsMaterialMakeSurfaceClosure(n") != std::string::npos &&
            hlsl.find("MaterialParameters[ctx.parameter_base + 0]") != std::string::npos &&
            hlsl.find("ctx.sampled_alpha") != std::string::npos &&
            hlsl.find("HsTextureSampleGrad(HsMaterialTexture3, 21u, EnvironmentWrap,") != std::string::npos &&
            hlsl.find("struct HsMaterialContext") == std::string::npos &&
            hlsl.find("struct HsMaterialSurface") == std::string::npos,
            "typed closure emission did not retain all fields or shared surface ABI");

    auto renamed = Json::parse(source);
    renamed["id"] = "rock_1";
    const auto renamed_definition = mc::ParseMaterialDefinition("rock_1", renamed.dump());
    Require(renamed_definition.graph.kind == rd::MaterialGraphKind::Opaque &&
            renamed_definition.graph.closure_output != rd::kNoMaterialInput,
            "typed closure was incorrectly restricted to rock_0");
    auto masked = Json::parse(source);
    masked["id"] = "leaf_0";
    masked["render_flags"] = {{"two_sided", true}, {"alpha_mode", "mask"}};
    const auto masked_definition = mc::ParseMaterialDefinition("leaf_0", masked.dump());
    const auto masked_closure = masked_definition.graph.closure_output;
    Require(masked_definition.graph.kind == rd::MaterialGraphKind::Masked &&
            masked_closure != rd::kNoMaterialInput &&
            masked_definition.graph.nodes[masked_closure].inputs[5] != rd::kNoMaterialInput,
            "masked typed closure lost its opacity root");
    const auto masked_hlsl = rd::EmitEnvironmentMaterialContextHlsl(masked_definition.graph, 3);
    Require(masked_hlsl.find("float HsMaterialEvaluateOpacity") != std::string::npos &&
            masked_hlsl.find("ctx.sampled_alpha") != std::string::npos,
            "masked typed closure opacity evaluator missing");

    rd::MaterialGraph constant_closures;
    constant_closures.kind = rd::MaterialGraphKind::Opaque;
    constant_closures.base_color_output = rd::kNoMaterialInput;
    constant_closures.roughness_output = rd::kNoMaterialInput;
    constant_closures.opacity_output = rd::kNoMaterialInput;
    const auto constant = [&](rd::MaterialType type, std::initializer_list<float> values) {
        const auto index = MaterialNode(constant_closures, type, rd::MaterialOp::Constant);
        std::copy(values.begin(), values.end(), constant_closures.nodes[index].value.begin());
        return index;
    };
    const auto constant_base = constant(rd::MaterialType::Float3, {0.2f, 0.3f, 0.4f});
    const auto constant_normal = constant(rd::MaterialType::Float3, {0.0f, 0.0f, 1.0f});
    const auto constant_roughness_a = constant(rd::MaterialType::Float, {0.2f});
    const auto constant_metallic_a = constant(rd::MaterialType::Float, {0.1f});
    const auto constant_ao = constant(rd::MaterialType::Float, {0.8f});
    const auto constant_opacity = constant(rd::MaterialType::Float, {1.0f});
    MaterialNode(constant_closures, rd::MaterialType::SurfaceClosure,
                 rd::MaterialOp::MetallicRoughnessClosure,
                 {constant_base, constant_normal, constant_roughness_a,
                  constant_metallic_a, constant_ao, constant_opacity});
    const auto constant_roughness_b = constant(rd::MaterialType::Float, {0.7f});
    const auto constant_metallic_b = constant(rd::MaterialType::Float, {0.6f});
    const auto constant_root = MaterialNode(constant_closures, rd::MaterialType::SurfaceClosure,
        rd::MaterialOp::MetallicRoughnessClosure,
        {constant_base, constant_normal, constant_roughness_b,
         constant_metallic_b, constant_ao, constant_opacity});
    constant_closures.closure_output = constant_root;
    const auto constant_hlsl = rd::EmitEnvironmentMaterialContextHlsl(constant_closures, 0);
    Require(constant_hlsl.find("HsMaterialMakeSurfaceClosure(") != std::string::npos &&
            constant_hlsl.find("HsSurfaceClosure()") == std::string::npos,
            "constant closure aggregate was folded into an invalid empty struct");

    auto invalid = graph;
    const auto closure = graph.closure_output;
    invalid.nodes[closure].inputs[0] = invalid.nodes[closure].inputs[2];
    Reject([&] { rd::ValidateEnvironmentMaterialGraph(invalid); },
           "typed closure accepted a scalar base-color input");
    invalid = graph;
    invalid.nodes[closure].op = rd::MaterialOp::Constant;
    invalid.nodes[closure].inputs.fill(rd::kNoMaterialInput);
    Reject([&] { rd::ValidateEnvironmentMaterialGraph(invalid); },
           "typed closure accepted a numeric constant aggregate");
    invalid = graph;
    rd::MaterialNode selected;
    selected.id = "invalid_surface_select";
    selected.op = rd::MaterialOp::Select;
    selected.type = rd::MaterialType::SurfaceClosure;
    selected.inputs[0] = invalid.nodes[closure].inputs[2];
    selected.inputs[1] = invalid.nodes[closure].inputs[0];
    selected.inputs[2] = invalid.nodes[closure].inputs[1];
    invalid.nodes.push_back(selected);
    invalid.closure_output = invalid.nodes.size() - 1;
    Reject([&] { rd::ValidateEnvironmentMaterialGraph(invalid); },
           "typed closure accepted a select aggregate");

    auto legacy_document = Json::parse(source);
    legacy_document["id"] = "rock_1";
    legacy_document["version"] = 3;
    legacy_document["parameters"] = {{"metallic", 0.0}, {"roughness_scale", 1.0}};
    const auto authored_closure = std::ranges::find_if(legacy_document["nodes"], [](const auto &node) {
        return node.at("op") == "metallic_roughness_closure";
    });
    Require(authored_closure != legacy_document["nodes"].end(), "legacy closure fixture missing");
    const auto closure_fields = *authored_closure;
    legacy_document["nodes"].erase(authored_closure);
    for (auto &node : legacy_document["nodes"])
    {
        if (node.at("op") == "parameter" && node.at("name") == "tint")
        {
            node["op"] = "constant";
            node.erase("name");
            node["value"] = {1.0, 1.0, 1.0};
        }
    }
    legacy_document["outputs"] = {
        {"surface", "builtin_metallic_roughness"},
        {"base_color", closure_fields.at("base_color")},
        {"roughness", closure_fields.at("roughness")},
        {"normal", closure_fields.at("normal")},
        {"normal_space", "tangent"},
        {"ao", closure_fields.at("ao")}};
    const std::string legacy_source = legacy_document.dump();
    const auto legacy = mc::ParseMaterialDefinition("rock_1", legacy_source);
    Require(legacy.graph.closure_output != rd::kNoMaterialInput &&
            legacy.graph.nodes[legacy.graph.closure_output].op == rd::MaterialOp::MetallicRoughnessClosure,
            "legacy material did not lower to a typed closure root");
    const auto legacy_hlsl = rd::EmitEnvironmentMaterialContextHlsl(legacy.graph, 4);
    Require(legacy_hlsl.find("HsMaterialSurface HsMaterialEvaluateSurface") != std::string::npos &&
            legacy_hlsl.find("HsMaterialMakeSurfaceClosure(n") != std::string::npos &&
            legacy_hlsl.find("MaterialParameters[ctx.parameter_base + 0]") != std::string::npos,
            "legacy flat outputs did not lower to the shared surface root");
}

void TestMaterialTextureBindingsAndContexts()
{
    namespace mc = hs::material_content;
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                       "ContentSource/Materials/rock_1.json";
    std::ifstream file(path);
    Require(file.is_open(), "context fixture missing");
    const std::string source{std::istreambuf_iterator<char>(file), {}};
    const auto document = nlohmann::json::parse(source);
    const auto definition = mc::ParseMaterialDefinition("rock_1", source);
    const auto reject_json = [&](const auto &invalid) {
        Reject([&] { (void)mc::ParseMaterialDefinition("rock_1", invalid.dump()); },
               "invalid authored declaration or sample accepted");
    };
    auto invalid = document;
    invalid["nodes"].push_back({{"id", "dead_ref"}, {"op", "texture_ref"},
        {"type", "texture_ref"}, {"declaration", "missing"}});
    reject_json(invalid);
    invalid = document;
    invalid["samplers"][0]["profile"] = "arbitrary_hlsl";
    reject_json(invalid);
    invalid = document;
    invalid["samplers"].push_back(invalid["samplers"][0]);
    reject_json(invalid);
    invalid = document;
    invalid["textures"][0]["dimension"] = "texture_cube";
    reject_json(invalid);
    invalid = document;
    invalid["outputs"].erase("normal_space");
    reject_json(invalid);
    invalid = document;
    for (auto &node : invalid["nodes"])
        if (node["op"] == "sample_grad") { node["layer"]["type"] = "float"; break; }
    reject_json(invalid);
    auto invalid_graph = definition.graph;
    auto ref = std::ranges::find(invalid_graph.nodes, rd::MaterialOp::TextureRef, &rd::MaterialNode::op);
    Require(ref != invalid_graph.nodes.end(), "texture reference fixture missing");
    ref->declaration_index = invalid_graph.textures.size();
    Reject([&] { rd::ValidateEnvironmentMaterialGraph(invalid_graph); },
           "out-of-range public texture reference accepted");
    invalid_graph = definition.graph;
    rd::MaterialNode dead;
    dead.id = "dead_sampler"; dead.op = rd::MaterialOp::SamplerRef;
    dead.type = rd::MaterialType::SamplerRef; dead.declaration_index = invalid_graph.samplers.size();
    invalid_graph.nodes.push_back(dead);
    Reject([&] { rd::ValidateEnvironmentMaterialGraph(invalid_graph); },
           "dead out-of-range public sampler reference accepted");
    auto sample_level_graph = definition.graph;
    Require(sample_level_graph.closure_output + 1 == sample_level_graph.nodes.size(),
            "SampleLevel fixture closure is not the terminal node");
    auto sample_level_closure = sample_level_graph.nodes[sample_level_graph.closure_output];
    sample_level_graph.nodes.pop_back();
    sample_level_graph.closure_output = rd::kNoMaterialInput;
    const auto lod = FloatConstant(sample_level_graph, rd::MaterialType::Float, 0.0f);
    // Put a valid level sample after its declaration references and a constant LOD.
    const auto grad = std::ranges::find(sample_level_graph.nodes, rd::MaterialOp::SampleGrad,
                                        &rd::MaterialNode::op);
    Require(grad != sample_level_graph.nodes.end(), "gradient sample fixture missing");
    auto level = *grad;
    level.id = "level_sample"; level.op = rd::MaterialOp::SampleLevel;
    level.inputs[4] = lod; level.inputs[5] = rd::kNoMaterialInput;
    sample_level_graph.nodes.push_back(level);
    const auto level_output = MaterialNode(sample_level_graph, rd::MaterialType::Float,
        rd::MaterialOp::Extract, {sample_level_graph.nodes.size() - 1});
    sample_level_graph.roughness_output = level_output;
    sample_level_closure.inputs[2] = level_output;
    sample_level_graph.closure_output = sample_level_graph.nodes.size();
    sample_level_graph.nodes.push_back(sample_level_closure);
    rd::ValidateEnvironmentMaterialGraph(sample_level_graph);
    Require(rd::EmitEnvironmentMaterialContextHlsl(sample_level_graph, 6).find(
                "HsTextureSampleLevel(HsMaterialTexture0, 36u, EnvironmentWrap,") !=
                std::string::npos, "live SampleLevel was not emitted");
    level.id = "invalid_level"; level.inputs[4] = level.inputs[2];
    sample_level_graph.nodes.push_back(level);
    Reject([&] { rd::ValidateEnvironmentMaterialGraph(sample_level_graph); },
           "SampleLevel accepted vector LOD");
    const auto original_sample = *std::ranges::find(definition.graph.nodes,
        rd::MaterialOp::SampleGrad, &rd::MaterialNode::op);
    const std::vector<std::uint32_t> two_layers(definition.graph.textures.size(), 2);
    auto layer_graph = definition.graph;
    const auto layer_node = static_cast<std::size_t>(std::ranges::find_if(layer_graph.nodes,
        [](const rd::MaterialNode &node) { return node.op == rd::MaterialOp::Context &&
            node.context_input == rd::MaterialContextInput::Layer; }) - layer_graph.nodes.begin());
    const auto eye_node = static_cast<std::size_t>(std::ranges::find_if(layer_graph.nodes,
        [](const rd::MaterialNode &node) { return node.op == rd::MaterialOp::Context &&
            node.context_input == rd::MaterialContextInput::EyeDistance; }) - layer_graph.nodes.begin());
    Require(layer_node < layer_graph.nodes.size() && eye_node < layer_graph.nodes.size(),
            "layer/context fixture missing");
    const auto zero = UintConstant(layer_graph, 0);
    const auto one = UintConstant(layer_graph, 1);
    const auto float_zero = FloatConstant(layer_graph, rd::MaterialType::Float, 0.0f);
    const auto unknown = MaterialNode(layer_graph, rd::MaterialType::Bool, rd::MaterialOp::Less,
                                     {eye_node, float_zero});
    const auto selected = MaterialNode(layer_graph, rd::MaterialType::Uint, rd::MaterialOp::Select,
                                      {unknown, zero, one});
    auto sample = original_sample;
    sample.id = "selected_layer_sample"; sample.inputs[3] = selected;
    layer_graph.nodes.push_back(sample);
    rd::ValidateMaterialSampleLayers(layer_graph, two_layers, 1);
    const auto bool_uint = MaterialNode(layer_graph, rd::MaterialType::Uint,
                                       rd::MaterialOp::Convert, {unknown});
    sample.id = "bool_layer_sample"; sample.inputs[3] = bool_uint;
    layer_graph.nodes.push_back(sample);
    rd::ValidateMaterialSampleLayers(layer_graph, two_layers, 1);
    auto invalid_layer_graph = layer_graph;
    const auto increment = MaterialNode(invalid_layer_graph, rd::MaterialType::Uint,
                                       rd::MaterialOp::Add, {layer_node, one});
    sample.id = "invalid_layer_sample"; sample.inputs[3] = increment;
    invalid_layer_graph.nodes.push_back(sample);
    Reject([&] { rd::ValidateMaterialSampleLayers(invalid_layer_graph, two_layers, 1); },
           "context layer plus one accepted unavailable array layer");
    const auto increment_valid = MaterialNode(layer_graph, rd::MaterialType::Uint,
                                             rd::MaterialOp::Add, {layer_node, one});
    const auto max_uint = UintConstant(layer_graph, UINT32_MAX);
    const auto wrapped = MaterialNode(layer_graph, rd::MaterialType::Uint,
                                     rd::MaterialOp::Add, {max_uint, increment_valid});
    sample.id = "wrapped_layer_sample"; sample.inputs[3] = wrapped;
    layer_graph.nodes.push_back(sample);
    rd::ValidateMaterialSampleLayers(layer_graph, two_layers, 1);
    const auto invalid_wrap = MaterialNode(layer_graph, rd::MaterialType::Uint,
                                          rd::MaterialOp::Add, {max_uint, layer_node});
    sample.id = "invalid_wrapped_sample"; sample.inputs[3] = invalid_wrap;
    layer_graph.nodes.push_back(sample);
    Reject([&] { rd::ValidateMaterialSampleLayers(layer_graph, two_layers, 1); },
           "uint wrap layer domain hid unavailable array layer");
    Require(rd::MaterialSamplerRegister(rd::MaterialSamplerProfile::LinearClamp) == 0 &&
            rd::MaterialSamplerRegister(rd::MaterialSamplerProfile::LinearWrap) == 2 &&
            rd::MaterialSamplerRegister(rd::MaterialSamplerProfile::EnvironmentAnisotropicWrap) == 3 &&
            rd::MaterialSamplerRegister(rd::MaterialSamplerProfile::FoliageAnisotropicClamp) == 4,
            "sampler profile register ABI changed");
    Reject([&] { (void)rd::MaterialSamplerRegister(static_cast<rd::MaterialSamplerProfile>(99)); },
           "unknown sampler profile accepted");

    auto library_context = definition.graph;
    const auto context_node = [&](rd::MaterialContextInput input) {
        const auto found = std::ranges::find_if(library_context.nodes,
            [&](const rd::MaterialNode &node) { return node.op == rd::MaterialOp::Context &&
                node.context_input == input; });
        Require(found != library_context.nodes.end(), "library context input fixture missing");
        return static_cast<std::size_t>(found - library_context.nodes.begin());
    };
    // Typed authored graphs keep their material expressions on the closure root.
    // Reuse its base-color expression while extending the context graph fixture.
    const auto context_base = library_context.nodes[library_context.closure_output].inputs[0];
    const auto context_uv = context_node(rd::MaterialContextInput::Uv);
    // ParseMaterialGraph appends the typed closure after the authored nodes.  Keep
    // that root at the end while extending its inputs so the fixture remains
    // topologically ordered.
    Require(library_context.closure_output + 1 == library_context.nodes.size(),
            "library context closure fixture is not the terminal node");
    auto library_closure = library_context.nodes[library_context.closure_output];
    library_context.nodes.pop_back();
    library_context.closure_output = rd::kNoMaterialInput;
    const auto fresnel_cos = FloatConstant(library_context, rd::MaterialType::Float, 0.5f);
    library_context.base_color_output = MaterialNode(library_context, rd::MaterialType::Float3,
        rd::MaterialOp::FresnelSchlick, {context_base, fresnel_cos});
    const auto library_seed = UintConstant(library_context, 17);
    library_context.roughness_output = MaterialNode(library_context, rd::MaterialType::Float,
        rd::MaterialOp::SeededNoise2D, {context_uv, library_seed});
    library_closure.inputs[0] = library_context.base_color_output;
    library_closure.inputs[2] = library_context.roughness_output;
    library_context.closure_output = library_context.nodes.size();
    library_context.nodes.push_back(library_closure);
    const auto library_context_hlsl = rd::EmitEnvironmentMaterialContextHlsl(library_context, 6);
    Require(library_context_hlsl.find("float HsMaterialSeededNoise2D(float2 p, uint seed)") !=
                std::string::npos &&
            library_context_hlsl.find("float3 HsMaterialFresnelSchlick(float3 f0, float cos_theta)") !=
                std::string::npos &&
            library_context_hlsl.find("HsMaterialSeededNoise2D(n") != std::string::npos &&
            library_context_hlsl.find("HsMaterialFresnelSchlick(n") != std::string::npos,
            "context library node helpers or expressions missing");

    std::vector<std::string> dependencies;
    for (const auto &texture : definition.graph.textures) dependencies.push_back(texture.product_id);
    std::ranges::sort(dependencies);
    const auto bindings = rd::EncodeMaterialBindings(dependencies, definition.graph.textures);
    const auto decoded = rd::DecodeMaterialBindings(bindings, dependencies);
    Require(decoded.size() == rd::kMaterialTextureSlots, "texture binding count changed");
    for (std::size_t slot = 0; slot < definition.graph.textures.size(); ++slot)
        Require(decoded[slot].slot == slot &&
                dependencies[decoded[slot].dependency_index] == definition.graph.textures[slot].product_id &&
                decoded[slot].semantic == definition.graph.textures[slot].semantic &&
                decoded[slot].shader_binding_abi == rd::kMaterialShaderBindingAbi,
                "texture binding slot/dependency/semantic/ABI roundtrip failed");
    Require(decoded[4].dependency_index == 0xffff && decoded[5].dependency_index == 0xffff &&
            decoded[4].semantic == rd::MaterialTextureSemantic::Height &&
            decoded[5].semantic == rd::MaterialTextureSemantic::Histogram,
            "empty terrain binding slots lost their immutable roles");
    for (const auto offset : {0u, 2u, 4u, 6u, 8u})
    {
        auto corrupt = bindings; corrupt[offset] = std::byte{0xff};
        Reject([&] { (void)rd::DecodeMaterialBindings(corrupt, dependencies); },
               "invalid material binding accepted");
    }
    auto missing = dependencies; missing.erase(missing.begin());
    Reject([&] { (void)rd::EncodeMaterialBindings(missing, definition.graph.textures); },
           "binding accepted missing texture package dependency");
    auto wrong_namespace = dependencies;
    for (auto &id : wrong_namespace) id = "environment/texture.dds";
    Reject([&] { (void)rd::DecodeMaterialBindings(bindings, wrong_namespace); },
           "binding accepted DDS dependency as texture package");
    const auto base = definition.graph.textures.front();
    rd::ValidateMaterialTexturePackage(base, {4, 4, 4, 3, rd::kTextureFormatBc7Srgb}, 3);
    Reject([&] { rd::ValidateMaterialTexturePackage(base, {4, 4, 4, 3, rd::kTextureFormatBc7}, 3); },
           "linear histogram texture accepted as sRGB base color");
    Reject([&] { rd::ValidateMaterialTexturePackage(base, {4, 4, 4, 3, rd::kTextureFormatBc7Srgb}, 4); },
           "unavailable material atlas layer accepted");
    Reject([&] { rd::ValidateMaterialTexturePackage(base, {4, 4, 4, 0, rd::kTextureFormatBc7Srgb}, 0); },
           "empty mip chain accepted");
}

void TestLegacyMaterialContextLowering()
{
    using Json = nlohmann::json;
    namespace mc = hs::material_content;
    auto document = Json::parse(R"json({"id":"rock_0","version":2,"domain":"surface",
        "shading_model":"metallic_roughness","parameters":{"metallic":0.0,"roughness_scale":1.0},
        "textures":{"base_color":"environment/environment_rock_basecolor.dds",
            "surface":"environment/environment_rock_surface.dds",
            "normal":"environment/environment_rock_normal.dds","detail":"environment/environment_rock_detail.dds"},
        "render_flags":{"two_sided":false,"alpha_mode":"opaque"},
        "nodes":[{"id":"color","op":"sampled_base_color","type":"float3"},
            {"id":"rough","op":"sampled_roughness","type":"float"},
            {"id":"scale","op":"parameter","name":"roughness_scale","type":"float"},
            {"id":"rough_product","op":"multiply","type":"float",
                "a":{"node":"rough","type":"float"},"b":{"node":"scale","type":"float"}}],
        "outputs":{"surface":"builtin_metallic_roughness","base_color":{"node":"color","type":"float3"},
            "roughness":{"node":"rough_product","type":"float"}}})json");
    const auto numeric_graph = mc::ParseEnvironmentMaterialGraph("rock_0", document.dump());
    Require(rd::EmitEnvironmentMaterialHlsl(numeric_graph).find("float3 HsMaterialBaseColor(float3 sampled)") !=
                std::string::npos, "legacy numeric emitter API changed");
    const auto definition = mc::ParseMaterialDefinition("rock_0", document.dump());
    const auto &graph = definition.graph;
    Require(graph.textures.size() == 4 && graph.samplers.size() == 1 &&
            graph.samplers[0].profile == rd::MaterialSamplerProfile::EnvironmentAnisotropicWrap &&
            graph.normal_output != rd::kNoMaterialInput && graph.ao_output != rd::kNoMaterialInput &&
            definition.material.textures[0] == "render/texture/environment_rock_basecolor.rpkg",
            "legacy material was not lowered to typed texture declarations and normal/AO outputs");
    for (const auto &node : graph.nodes)
        Require(node.op != rd::MaterialOp::SampledBaseColor && node.op != rd::MaterialOp::SampledRoughness &&
                node.op != rd::MaterialOp::SampledAlpha, "legacy sampled input survived context lowering");
    const auto hlsl = rd::EmitEnvironmentMaterialContextHlsl(graph, 6);
    const auto surface = hlsl.substr(hlsl.find("HsMaterialSurface HsMaterialEvaluateSurface"));
    Require(surface.find("HsTextureSampleGrad(HsMaterialTexture0, 36u, EnvironmentWrap,") != std::string::npos &&
            surface.find("HsTextureSampleGrad(HsMaterialTexture3, 39u, EnvironmentWrap,") != std::string::npos &&
            surface.find("ctx.sampled_base_color") == std::string::npos &&
            surface.find("ctx.sampled_roughness") == std::string::npos,
            "version2 context evaluator retained untextured sampled-value inputs");
    rd::ValidateMaterialSampleLayers(graph, std::array<std::uint32_t, 4>{4,4,4,4}, 3);
    document["id"] = "leaf_0";
    document["textures"] = {{"base_color", "environment/environment_leaf_basecolor.dds"},
        {"surface", "environment/environment_leaf_roughness.dds"},
        {"normal", "environment/environment_leaf_normal.dds"}, {"detail", ""}};
    document["render_flags"] = {{"two_sided", true}, {"alpha_mode", "mask"}};
    document["nodes"].push_back({{"id", "alpha"}, {"op", "sampled_alpha"}, {"type", "float"}});
    document["outputs"]["opacity"] = {{"node", "alpha"}, {"type", "float"}};
    const auto leaf = mc::ParseMaterialDefinition("leaf_0", document.dump());
    const auto leaf_hlsl = rd::EmitEnvironmentMaterialContextHlsl(leaf.graph, 3);
    const auto opacity = leaf_hlsl.substr(leaf_hlsl.find("float HsMaterialEvaluateOpacity"));
    Require(leaf.graph.samplers[0].profile == rd::MaterialSamplerProfile::FoliageAnisotropicClamp &&
            opacity.find("HsTextureSampleGrad(HsMaterialTexture0, 18u, FoliageClamp,") != std::string::npos &&
            opacity.find("HsMaterialTexture1") == std::string::npos &&
            opacity.find("ctx.sampled_alpha") == std::string::npos,
            "legacy masked opacity lowering lost typed sampling or pruning");
}

rd::GeometryData GeometryFixture()
{
    rd::GeometryData g;g.flags=rd::kGeometryFlagTwoSided;
    for(unsigned material=0;material<2;++material)
        for(const auto p:{std::array<float,3>{0,0,0},std::array<float,3>{1,0,0},std::array<float,3>{0,1,0}})
        {rd::ExpandedVertex v;v.position=p;v.material_index=static_cast<std::uint16_t>(material);g.vertices.push_back(v);}
    g.indices={0,1,2,0,1,2,3,4,5};g.submeshes={{0,6,0,1,0},{6,3,1,1,1}};
    rd::GeometryMeshlet a;a.vertex_ref_count=3;a.triangle_count=2;a.sphere={0.5f,0.5f,0,1};
    auto b=a;b.first_vertex_ref=3;b.first_triangle=2;b.triangle_count=1;b.submesh_index=1;
    g.meshlets={a,b};g.meshlet_vertex_refs={0,1,2,3,4,5};g.meshlet_triangles={0x020100,0x020100,0x020100};g.canonical_primitive_remap={1,0,2};
    g.bounds.minimum={0,0,0};g.bounds.maximum={1,1,0};g.bounds.sphere={0.5f,0.5f,0,1};return g;
}
rd::GeometryMetadata GeometryMetadataFixture(const rd::GeometryData&g)
{
    rd::GeometryMetadata m;m.flags=g.flags;m.vertex_count=static_cast<std::uint32_t>(g.vertices.size());m.index_count=static_cast<std::uint32_t>(g.indices.size());m.submesh_count=static_cast<std::uint32_t>(g.submeshes.size());m.meshlet_count=static_cast<std::uint32_t>(g.meshlets.size());m.vertex_ref_count=static_cast<std::uint32_t>(g.meshlet_vertex_refs.size());m.max_material_index=1;m.bounds=g.bounds;m.page_dependency_index=2;m.fallback_dependency_index=0;m.terminal_fallback_dependency_index=1;return m;
}
void TestGeometryPackage()
{
    const std::vector<std::string>deps{"render/fixture_lod1.rpkg","render/fixture_lod2.rpkg","render/geometry/fixture.rpage"};
    const auto g=GeometryFixture();const auto m=GeometryMetadataFixture(g);
    const auto metadata=rd::EncodeGeometryMetadata(m,deps,"render/fixture.rpkg");
    const auto decoded_metadata=rd::DecodeGeometryMetadata(metadata,deps,"render/fixture.rpkg");
    const auto page_id=hs::MakeAssetId("render/geometry/fixture.rpage");const auto page=rd::BuildGeometryPage(page_id,{},g);
    const auto decoded=rd::DecodeGeometryPage(page,page_id,decoded_metadata);
    Require(decoded.vertices.size()==6 && decoded.indices==g.indices && decoded.canonical_primitive_remap==g.canonical_primitive_remap && decoded.meshlets.size()==2,"geometry roundtrip/duplicate occurrences");
    Reject([&]{(void)rd::DecodeGeometryMetadata(metadata,deps,"render/other.rpkg");},"wrong page product accepted");
    auto reserved=metadata;reserved[88]=std::byte{1};Reject([&]{(void)rd::DecodeGeometryMetadata(reserved,deps,"render/fixture.rpkg");},"metadata reserved accepted");
    auto bad=g;bad.canonical_primitive_remap[1]=1;Reject([&]{rd::ValidateGeometryData(bad);},"duplicate primitive occurrence accepted");
    bad=g;bad.meshlet_triangles[0]|=0x01000000;Reject([&]{rd::ValidateGeometryData(bad);},"packed triangle high byte accepted");
    bad=g;bad.meshlet_triangles[0]=0x000102;Reject([&]{rd::ValidateGeometryData(bad);},"reversed primitive orientation accepted");
    bad=g;bad.meshlet_vertex_refs[0]=3;Reject([&]{rd::ValidateGeometryData(bad);},"mixed meshlet material accepted");
    bad=g;bad.meshlets[0].sphere[3]=0;Reject([&]{rd::ValidateGeometryData(bad);},"undersized meshlet sphere accepted");
    bad=g;bad.meshlets[0].flags=rd::kGeometryMeshletConeValid;bad.meshlets[0].cone={0,0,1,0.5f};Reject([&]{rd::ValidateGeometryData(bad);},"two sided cone accepted");
    auto mismatch=m;++mismatch.index_count;Reject([&]{(void)rd::DecodeGeometryPage(page,page_id,mismatch);},"metadata/page counts accepted");
    const auto view=rd::ReadPackage(page,page_id);std::vector<rd::ChunkSource>chunks;for(const auto&c:view.chunks)chunks.push_back({static_cast<std::uint32_t>(c.type),c.version,c.required,c.bytes,c.count,c.stride});
    auto meshlet_bytes=std::vector<std::byte>(chunks[3].bytes.begin(),chunks[3].bytes.end());meshlet_bytes[56]=std::byte{1};chunks[3].bytes=meshlet_bytes;const auto malformed=rd::BuildPackage(page_id,{},chunks);Reject([&]{(void)rd::DecodeGeometryPage(malformed,page_id,m);},"meshlet reserved accepted");
    chunks[3].bytes=view.chunks[3].bytes;chunks.erase(chunks.begin()+5);const auto missing=rd::BuildPackage(page_id,{},chunks);Reject([&]{(void)rd::DecodeGeometryPage(missing,page_id,m);},"missing required geometry array accepted");
    // Degenerate primitives still own referenced positions and cannot lose them.
    bad=g;bad.indices[1]=0;bad.meshlet_triangles[1]=0x020000;rd::ValidateGeometryData(bad);
}

void TestCharacterPackage()
{
    const std::string id = "render/character/test.rpkg";
    const auto asset = hs::MakeAssetId(id);
    rd::CharacterPackageData source;
    source.bone_count = 2;
    source.material_count = 1;
    source.ground_offset = 0.25f;
    source.parents = {UINT16_MAX, 0};
    source.inverse_bind_matrices.resize(2);
    source.inverse_bind_matrices[0][0] = 1.0f;
    source.inverse_bind_matrices[1][15] = 1.0f;
    source.upper_body_weights = {0.0f, 1.0f};
    hs::SkinnedVertex vertex{};
    vertex.bone_weights = {1.0f, 0.0f, 0.0f, 0.0f};
    vertex.normal = {0.0f, 1.0f, 0.0f};
    source.vertices.assign(3, vertex);
    for (std::uint32_t i = 0; i < 5; ++i)
        source.clips.push_back({static_cast<hs::CharacterAnimationClip>(i), 1, 0,
                                i * 4, 2, 1.0f});
    source.transforms.resize(20);
    source.transforms[3].translation = {1.25f, -2.0f, 0.5f};
    source.transforms[3].rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    source.transforms[3].scale = {2.0f, 1.0f, 0.5f};
    const std::vector<std::string> dependencies{"character/test.meshbin"};
    const auto package = rd::BuildCharacterPackage(asset, dependencies, source);
    const auto view = rd::ReadPackage(package, asset);
    Require(view.chunks.size() == 7 && view.dependencies == dependencies,
            "character package chunks/dependencies");
    const auto decoded = rd::DecodeCharacterPackage(package, asset);
    Require(decoded.vertices.size() == 3 && decoded.bone_count == 2 &&
            decoded.material_count == 1 && decoded.ground_offset == 0.25f &&
            decoded.parents == source.parents &&
            decoded.inverse_bind_matrices == source.inverse_bind_matrices &&
            decoded.upper_body_weights == source.upper_body_weights &&
            decoded.clips.size() == 5 && decoded.clips[4].first_transform == 16 &&
            decoded.transforms.size() == 20 &&
            decoded.transforms[3].translation == source.transforms[3].translation &&
            decoded.transforms[3].rotation == source.transforms[3].rotation &&
            decoded.transforms[3].scale == source.transforms[3].scale,
            "character package roundtrip");

    rd::GeometryData geometry;geometry.flags=rd::kGeometryFlagSkinned;
    for(const auto&v:source.vertices)geometry.vertices.push_back({v.position,v.normal,v.bone_indices,v.bone_weights,v.uv,v.tangent,v.material_index,v.reserved});
    geometry.indices={0,1,2};geometry.submeshes={{0,3,0,1,0}};rd::GeometryMeshlet cl;cl.vertex_ref_count=3;cl.triangle_count=1;
    geometry.bounds.minimum=geometry.bounds.maximum=geometry.vertices.front().position;
    for(const auto&v:geometry.vertices)for(unsigned k=0;k<3;++k){geometry.bounds.minimum[k]=std::min(geometry.bounds.minimum[k],v.position[k]);geometry.bounds.maximum[k]=std::max(geometry.bounds.maximum[k],v.position[k]);}
    geometry.bounds.sphere={0,0,0,100};cl.sphere=geometry.bounds.sphere;geometry.meshlets={cl};geometry.meshlet_vertex_refs={0,1,2};geometry.meshlet_triangles={0x020100};geometry.canonical_primitive_remap={0};
    auto gm=GeometryMetadataFixture(geometry);gm.max_material_index=0;gm.page_dependency_index=0;gm.fallback_dependency_index=gm.terminal_fallback_dependency_index=rd::kNoGeometryDependency;
    const std::vector<std::string>production_deps{
        "render/geometry/character/test.rpage",
        "render/material/archer_skin/material.rpkg"};
    const std::array production_bindings{rd::CharacterMaterialBinding{
        0, 1, 0, rd::HeroMaterialDomain::Opaque}};
    const auto production=rd::BuildCharacterMetadataPackage(
        asset,production_deps,source,gm,production_bindings);
    const auto character_metadata=rd::DecodeCharacterMetadataPackage(production,asset);
    rd::ValidateCharacterGeometry(character_metadata,geometry);
    const auto production_view=rd::ReadPackage(production,asset);
    const auto production_binding = std::ranges::find(production_view.chunks,
        rd::ChunkType::CharacterMaterialBindings, &rd::ChunkView::type);
    Require(production_view.dependencies == production_deps && production_view.chunks.size()==8 &&
            std::ranges::none_of(production_view.chunks,[](const auto&c){return c.type==rd::ChunkType::ExpandedVertices;}) &&
            production_binding != production_view.chunks.end() &&
            production_binding->count == production_bindings.size() &&
            production_binding->stride == rd::kCharacterMaterialBindingStride,
            "production character embeds geometry or omitted material bindings");
    const auto decoded_bindings = rd::DecodeCharacterMaterialBindings(
        production_binding->bytes, production_view.dependencies, source.material_count);
    Require(decoded_bindings.size() == 1 && decoded_bindings[0].slot == 0 &&
            decoded_bindings[0].dependency_index == 1 && decoded_bindings[0].hero_ordinal == 0 &&
            decoded_bindings[0].domain == rd::HeroMaterialDomain::Opaque,
            "production character material binding roundtrip");
    for(const auto&c:view.chunks)if(c.type!=rd::ChunkType::ExpandedVertices){auto found=std::ranges::find(production_view.chunks,c.type,&rd::ChunkView::type);Require(found!=production_view.chunks.end() && std::ranges::equal(found->bytes,c.bytes),"character skeleton scalar bytes changed");}
    Reject([&]{(void)rd::DecodeCharacterMetadataPackage(package,asset);},"production decoder accepted embedded package");
    Reject([&]{(void)rd::DecodeEmbeddedCharacterPackage(production,asset);},"embedded decoder accepted production package");
    auto invalid_skin=geometry;invalid_skin.vertices[0].bone_indices[0]=2;Reject([&]{rd::ValidateCharacterGeometry(character_metadata,invalid_skin);},"unique character bone range accepted");

    std::vector<rd::ChunkSource> chunks;
    for (const auto &chunk : view.chunks)
        chunks.push_back({static_cast<std::uint32_t>(chunk.type), chunk.version,
                          chunk.required, chunk.bytes, chunk.count, chunk.stride});
    chunks.push_back(chunks[1]);
    const auto duplicate = rd::BuildPackage(asset, dependencies, chunks);
    Reject([&] { (void)rd::ReadPackage(duplicate, asset); },
           "duplicate character metadata accepted");
    chunks.pop_back();
    auto invalid_parent = std::vector<std::byte>(chunks[2].bytes.begin(),
                                                 chunks[2].bytes.end());
    invalid_parent[2] = std::byte{2};
    chunks[2].bytes = invalid_parent;
    const auto bad_hierarchy = rd::BuildPackage(asset, dependencies, chunks);
    Reject([&] { (void)rd::DecodeCharacterPackage(bad_hierarchy, asset); },
           "invalid parent accepted");
    chunks[2].bytes = view.chunks[2].bytes;
    auto mismatched_count = std::vector<std::byte>(chunks[1].bytes.begin(),
                                                  chunks[1].bytes.end());
    mismatched_count[0] = std::byte{3};
    chunks[1].bytes = mismatched_count;
    const auto bad_count = rd::BuildPackage(asset, dependencies, chunks);
    Reject([&] { (void)rd::DecodeCharacterPackage(bad_count, asset); },
           "cross-chunk bone count mismatch accepted");
}

void TestTexturePackage()
{
    const std::string id = "render/texture/test.rpkg";
    const auto asset = hs::MakeAssetId(id);
    const std::vector<std::string> dependencies{"environment/test.dds"};
    const rd::TextureDescription description{8, 8, 2, 4, rd::kTextureFormatBc7Srgb};
    std::vector<std::byte> payload(224);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = std::byte(i & 0xff);
    const auto package = rd::BuildTexturePackage(asset, dependencies, description, payload);
    const auto decoded = rd::ReadTexturePackage(package, asset);
    Require(decoded.description.width == 8 && decoded.description.height == 8 &&
            decoded.description.array_size == 2 && decoded.description.mip_count == 4 &&
            decoded.description.format == rd::kTextureFormatBc7Srgb &&
            decoded.dependencies == dependencies && decoded.subresources.size() == 8,
            "texture package metadata roundtrip");
    const auto &first = decoded.subresources[0];
    const auto &last = decoded.subresources[7];
    Require(first.array_slice == 0 && first.mip_level == 0 &&
            first.width == 8 && first.height == 8 &&
            first.row_pitch == 32 && first.slice_pitch == 64 &&
            first.bytes.size() == 64 && first.bytes[0] == payload[0] &&
            last.array_slice == 1 && last.mip_level == 3 &&
            last.width == 1 && last.height == 1 &&
            last.row_pitch == 16 && last.slice_pitch == 16 &&
            last.bytes[0] == payload[208],
            "texture subresource offset or pitch roundtrip");

    const auto base = rd::ReadPackage(package, asset);
    auto metadata = std::vector<std::byte>(base.chunks[0].bytes.begin(),
                                           base.chunks[0].bytes.end());
    metadata[40] = std::byte{1}; // First subresource must begin at payload offset zero.
    const std::array malformed{
        rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::TextureMetadata),
                        1, true, metadata, 1, static_cast<std::uint32_t>(metadata.size())},
        rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::TextureSubresources),
                        1, true, base.chunks[1].bytes, payload.size() / 16, 16}};
    const auto bad_offset = rd::BuildPackage(asset, dependencies, malformed);
    Reject([&] { (void)rd::ReadTexturePackage(bad_offset, asset); },
           "invalid texture subresource offset accepted");
    Reject([&] { (void)rd::BuildTexturePackage(asset, dependencies, description,
                                               std::span(payload).first(208)); },
           "truncated BC7 payload accepted");

    for (const auto format : {rd::kTextureFormatBc4, rd::kTextureFormatBc5,
                              rd::kTextureFormatBc7, rd::kTextureFormatBc7Srgb})
    {
        const auto block_bytes = format == rd::kTextureFormatBc4 ? 8u : 16u;
        const rd::TextureDescription variant{8, 8, 2, 4, format};
        std::vector<std::byte> variant_payload(14 * block_bytes);
        for (std::size_t i = 0; i < variant_payload.size(); ++i)
            variant_payload[i] = std::byte(i & 0xff);
        const auto encoded = rd::BuildTexturePackage(
            asset, dependencies, variant, variant_payload);
        const auto decoded_variant = rd::ReadTexturePackage(encoded, asset);
        Require(decoded_variant.description.format == format &&
                decoded_variant.subresources.size() == 8 &&
                decoded_variant.subresources[0].row_pitch == 2 * block_bytes &&
                decoded_variant.subresources[0].slice_pitch == 4 * block_bytes &&
                decoded_variant.subresources[7].row_pitch == block_bytes &&
                decoded_variant.subresources[7].slice_pitch == block_bytes &&
                decoded_variant.subresources[7].bytes[0] ==
                    variant_payload[13 * block_bytes],
                "BC texture format pitch or offset roundtrip");
        Reject([&] { (void)rd::BuildTexturePackage(
                    asset, dependencies, variant,
                    std::span(variant_payload).first(variant_payload.size() - block_bytes)); },
               "truncated BC format payload accepted");
    }
    for (const auto format : {rd::kTextureFormatRgba8, rd::kTextureFormatRgba8Srgb})
    {
        const rd::TextureDescription tiny{1, 1, 1, 1, format};
        const std::array tiny_pixels{std::byte{0x12}, std::byte{0x34},
                                     std::byte{0x56}, std::byte{0x78}};
        const auto tiny_package = rd::BuildTexturePackage(asset, dependencies,
                                                           tiny, tiny_pixels);
        const auto tiny_view = rd::ReadTexturePackage(tiny_package, asset);
        Require(tiny_view.description.format == format &&
                tiny_view.subresources.size() == 1 &&
                tiny_view.subresources[0].row_pitch == 4 &&
                tiny_view.subresources[0].slice_pitch == 4 &&
                tiny_view.subresources[0].bytes[3] == tiny_pixels[3],
                "RGBA8 one-pixel package roundtrip");
        Reject([&] { (void)rd::BuildTexturePackage(
                    asset, dependencies, tiny, std::span(tiny_pixels).first(3)); },
               "truncated RGBA8 pixel accepted");
        const rd::TextureDescription mipmapped{4, 4, 1, 3, format};
        std::vector<std::byte> pixels(84, std::byte{0x5a});
        const auto mip_package = rd::BuildTexturePackage(asset, dependencies,
                                                          mipmapped, pixels);
        const auto mip_view = rd::ReadTexturePackage(mip_package, asset);
        Require(mip_view.subresources.size() == 3 &&
                mip_view.subresources[0].row_pitch == 16 &&
                mip_view.subresources[0].slice_pitch == 64 &&
                mip_view.subresources[1].row_pitch == 8 &&
                mip_view.subresources[1].slice_pitch == 16 &&
                mip_view.subresources[2].row_pitch == 4 &&
                mip_view.subresources[2].slice_pitch == 4,
                "RGBA8 mip offsets and pitches");
    }
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        std::filesystem::path dump_directory;
        if (argc != 1)
        {
            Require(argc == 3 && std::string_view(argv[1]) == "--dump-material-hlsl",
                    "usage: hs_render_package_tests [--dump-material-hlsl <directory>]");
            dump_directory = argv[2];
            Require(!dump_directory.empty(), "numeric HLSL dump directory is empty");
            dump_numeric_fixtures = true;
        }
        RunNamed("TestExpandedVertexFields", TestExpandedVertexFields);
        RunNamed("TestPackage", TestPackage);
        RunNamed("TestQualityProfiles", TestQualityProfiles);
        RunNamed("TestDecalMaterialPackages", TestDecalMaterialPackages);
        RunNamed("TestMaterialParameters", TestMaterialParameters);
        RunNamed("TestMaterialGraph", TestMaterialGraph);
        RunNamed("TestGenericMaterialOutputInputs", TestGenericMaterialOutputInputs);
        RunNamed("TestNumericMaterialOperations", TestNumericMaterialOperations);
        RunNamed("TestMaterialLogicAndOptimizer", TestMaterialLogicAndOptimizer);
        RunNamed("TestUnsafeMaterialGraphs", TestUnsafeMaterialGraphs);
        RunNamed("TestMaterialPrograms", TestMaterialPrograms);
        RunNamed("TestRenderBindingCompilerContract", TestRenderBindingCompilerContract);
        RunNamed("TestGroundDetailCatalogMapping", TestGroundDetailCatalogMapping);
        RunNamed("TestAuthoredEnvironmentMaterials", TestAuthoredEnvironmentMaterials);
        RunNamed("TestSurfaceClosure", TestSurfaceClosure);
        RunNamed("TestAuthoredTerrainMaterial", TestAuthoredTerrainMaterial);
        RunNamed("TestMaterialTextureBindingsAndContexts", TestMaterialTextureBindingsAndContexts);
        RunNamed("TestLegacyMaterialContextLowering", TestLegacyMaterialContextLowering);
        RunNamed("TestCharacterPackage", TestCharacterPackage);
        RunNamed("TestGeometryPackage", TestGeometryPackage);
        RunNamed("TestTexturePackage", TestTexturePackage);
        if (dump_numeric_fixtures) DumpNumericMaterialHlsl(dump_directory);
        std::cout << "render package tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
