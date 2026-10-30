#include <hs/core/cooked_format.hpp>
#include <hs/render_data/render_snapshot_storage.hpp>
#include <hs/core/sha256.hpp>
#include "../Source/RendererD3D12/Private/renderer_packed_assets.hpp"
#include "../Source/RendererD3D12/Private/renderer_hlod_scene.hpp"
#include "../Source/RendererD3D12/Private/renderer_hero_work_math.hpp"
#include "../Source/RendererD3D12/Private/renderer_instance_order.hpp"
#include "../Source/RendererD3D12/Private/renderer_monster_lod.hpp"
#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/surface_layout.hpp>
#include <hs/render_data/material_descriptor.hpp>
#include <hs/render_data/material_ir.hpp>
#include <hs/render_data/character_package.hpp>
#include <hs/render_data/character_descriptor.hpp>
#include <hs/render_data/render_asset_format.hpp>
#include <hs/render_data/texture_package.hpp>
#include <hs/render_data/texture_descriptor.hpp>
#include <hs/render_data/geometry_descriptor.hpp>
#include <hs/render_data/vfx_mesh_descriptor.hpp>
#include <hs/render_data/dds_texture.hpp>
#include "../Source/Apps/Content/character_texture_mips.hpp"
#include "../Source/Apps/Content/character_bc7_coverage.hpp"
#include "../Source/Apps/Content/character_bc7_normal.hpp"
#include <hs/render_data/quality_profiles.hpp>
#include <hs/render_data/static_shadow.hpp>
#include <hs/render_data/static_shadow_archive.hpp>
#include "../Source/Apps/RenderMaterialShader/material_graph_parser.hpp"
#include "../Source/Apps/AssetPipeline/render_shader_bindings.hpp"
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

void TestReleaseAudioCodegen();
void TestShippingRendererIdentity();
void TestVirtualShadowGuarantees();

namespace
{
namespace rd = hs::render_data;
bool dump_numeric_fixtures{};
std::vector<std::string> numeric_fixtures;

void Require(bool condition, const char *reason)
{
    if (!condition) throw std::runtime_error(reason);
}

void TestMonsterLodDrawOrder()
{
    Require(hs::MergeActiveGiVoxelSize(0.0, 0.25) == 0.25 &&
        hs::MergeActiveGiVoxelSize(0.25, 0.0) == 0.25 &&
        hs::MergeActiveGiVoxelSize(0.5, 0.25) == 0.25 &&
        hs::MergeActiveGiVoxelSize(0.25, 0.5) == 0.25 &&
        hs::MergeActiveGiVoxelSize(0.0, 0.0) == 0.0 &&
        !std::isfinite(hs::MergeActiveGiVoxelSize(0.25, std::numeric_limits<double>::quiet_NaN())),
        "native GI union lost an active view or treated disabled zero as a voxel size");
    std::array<hs::RenderInstance, 4> sources{};
    sources[0].mesh = hs::RenderMesh::Archer;
    sources[1].mesh = hs::RenderMesh::MonsterMelee;
    sources[2].mesh = hs::RenderMesh::MonsterRanged;
    sources[3].mesh = hs::RenderMesh::MonsterMelee;
    const std::array<std::uint16_t, 4> lods{0, 2, 1, 0};
    std::vector<std::uint32_t> indices, order_scratch;
    const auto order = hs::BuildRenderInstanceOrderWithMonsterLods(sources, lods, 1.0f, indices, order_scratch);
    Require(order.has_value() && indices == std::vector<std::uint32_t>{0, 3, 1, 2},
        "monster LOD partition changed canonical draw identity");
    Require(order->monster_lod_ranges[0] == std::pair<std::size_t, std::size_t>{1, 1} &&
        order->monster_lod_ranges[2] == std::pair<std::size_t, std::size_t>{2, 1} &&
        order->monster_lod_ranges[4] == std::pair<std::size_t, std::size_t>{3, 1},
        "monster family LOD ranges are not contiguous");
    auto invalid = lods;
    invalid[1] = 3;
    Require(!hs::BuildRenderInstanceOrderWithMonsterLods(sources, invalid, 1.0f, indices, order_scratch),
        "unsupported monster LOD was accepted");

    std::array<hs::render_data::GeometryMetadata, 3> pages{};
    for (std::size_t lod = 0; lod < pages.size(); ++lod)
    {
        pages[lod].lod = static_cast<std::uint16_t>(lod);
        pages[lod].flags = hs::render_data::kGeometryFlagSkinned;
        pages[lod].error_method_abi = lod == 0 ? hs::render_data::kGeometryErrorMethodAbi
            : hs::render_data::kSkinnedPoseErrorMethodAbi;
    }
    pages[1].geometric_error = 0.1f;
    pages[2].geometric_error = 0.3f;
    hs::MonsterLodProjection projection;
    projection.Include(2.0, 1.0);
    Require(hs::SelectMonsterLod(pages, 0b111u, 1.0f, projection) == 2 &&
        hs::SelectMonsterLod(pages, 0b001u, 1.0f, projection) == 0,
        "monster LOD ignored error or own resident mask");
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

    rd::ValidateExpandedVertices(std::span(&original, 1));
    Reject([] { rd::ValidateExpandedVertices({}); }, "empty vertex validation accepted");
    Reject([] { (void)rd::EncodeExpandedVertices({}); }, "empty vertex encoding accepted");
    auto vertex = original;
    const auto reject_vertex = [&] {
        Reject([&] { rd::ValidateExpandedVertices(std::span(&vertex, 1)); },
               "invalid vertex accepted by direct validation");
        Reject([&] { (void)rd::EncodeExpandedVertices(std::span(&vertex, 1)); },
               "invalid vertex accepted by encoding");
    };
    for (auto field : std::array<std::span<float>, 5>{
             vertex.position, vertex.normal, vertex.uv, vertex.tangent, vertex.bone_weights})
    {
        const auto saved = field.front();
        field.front() = std::numeric_limits<float>::infinity();
        reject_vertex();
        field.front() = saved;
    }
    for (const auto weights : std::array<std::array<float, 4>, 3>{
             std::array{-0.1f, 0.5f, 0.3f, 0.3f},
             std::array{1.1f, 0.0f, 0.0f, 0.0f},
             std::array{0.1f, 0.1f, 0.1f, 0.1f}})
    {
        vertex.bone_weights = weights;
        reject_vertex();
    }
    vertex = original; vertex.bone_weights = {};
    rd::ValidateExpandedVertices(std::span(&vertex, 1));
    vertex.reserved = 1;
    reject_vertex();
}

void TestVfxMeshDescriptor()
{
    constexpr std::array<std::string_view, 7> names{
        "arrowhead_mesh", "shard_mesh", "debris_shard_mesh", "small_shard_mesh",
        "enemy_thorn_mesh", "boss_crest_lance_mesh", "needle_shard_mesh"};
    std::vector<std::uint32_t> words(25 + 7 * 3 * 12);
    words[0] = 0x4d565348u; words[1] = 1; words[2] = 7; words[3] = 21;
    for (std::size_t mesh = 0; mesh < names.size(); ++mesh)
    {
        std::uint32_t hash = 2166136261u;
        for (const auto character : names[mesh])
            hash = (hash ^ static_cast<unsigned char>(character)) * 16777619u;
        words[4 + mesh * 3] = hash;
        words[5 + mesh * 3] = static_cast<std::uint32_t>(mesh * 3);
        words[6 + mesh * 3] = 3;
        words[25 + (mesh * 3) * 12] = std::bit_cast<std::uint32_t>(1.0f);
        words[25 + (mesh * 3 + 1) * 12 + 2] = std::bit_cast<std::uint32_t>(1.0f);
    }
    const auto bytes = std::as_bytes(std::span(words));
    const auto descriptor = rd::CompileVfxMeshAtlasDescriptor(bytes, hs::MakeAssetId("vfx/vfx_mesh_atlas.hsbin"));
    for (const auto &mesh : descriptor.meshes)
        Require(mesh.vertex_count == 3 && mesh.transverse_radius == 1 && mesh.minimum_z == 0 &&
            mesh.widest_z == 0 && mesh.maximum_z == 1, "VFX mesh derived bounds differ");
    words[25 + 3] = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity());
    Reject([&] { (void)rd::CompileVfxMeshAtlasDescriptor(bytes, descriptor.asset_id); },
        "nonfinite VFX mesh vertex accepted");
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

void TestMaterialDescriptors()
{
    const auto scalar = [](std::string id, float value) {
        rd::MaterialParameterDefinition p; p.id = std::move(id); p.type = rd::MaterialParameterType::Float;
        p.default_words[0] = std::bit_cast<std::uint32_t>(value); return p;
    };
    const std::string env_id = "render/material/rock_0/material.rpkg";
    const std::string env_parameters = rd::MaterialParametersProduct(env_id);
    const std::array parameters{scalar("metallic", 0.25f), scalar("roughness_scale", 1.2f)};
    const std::array definitions{
        rd::MaterialTextureDefinition{"base", "render/texture/environment_test_base.rpkg", rd::MaterialTextureDimension::Texture2DArray, rd::MaterialTextureSemantic::BaseColor},
        rd::MaterialTextureDefinition{"surface", "render/texture/environment_test_surface.rpkg", rd::MaterialTextureDimension::Texture2DArray, rd::MaterialTextureSemantic::Surface},
        rd::MaterialTextureDefinition{"normal", "render/texture/environment_test_normal.rpkg", rd::MaterialTextureDimension::Texture2DArray, rd::MaterialTextureSemantic::Normal}};
    std::vector<rd::TextureDescriptor> textures;
    for (std::size_t i=0; i<definitions.size(); ++i)
    {
        rd::TextureDescriptor texture{}; texture.asset_id = hs::MakeAssetId(definitions[i].product_id);
        texture.description = {4,4,1,1,i ? rd::kTextureFormatBc5 : rd::kTextureFormatRgba8Srgb};
        textures.push_back(texture);
    }
    rd::TextureDescriptor decal_texture{};
    decal_texture.asset_id = hs::MakeAssetId("render/texture/vfx_fracture_decal_array.rpkg");
    decal_texture.description = {4,4,1,1,rd::kTextureFormatRgba8};
    textures.push_back(decal_texture);
    rd::RockMaterial rock;
    rock.metallic = 0.25f; rock.roughness_scale = 1.2f;
    std::vector<std::string> dependencies{env_parameters, "render/shader/rock_0_scene.dxil"};
    for (std::size_t i=0;i<definitions.size();++i) { rock.textures[i]=definitions[i].product_id; dependencies.push_back(definitions[i].product_id); }
    std::ranges::sort(dependencies);
    const auto metadata = rd::EncodeRockMaterial(rock, dependencies);
    const auto bindings = rd::EncodeMaterialBindings(dependencies, definitions);
    const std::array program_sources{rd::MaterialProgramSource{"render/shader/rock_0_scene.dxil", rd::MaterialProgramKind::OpaqueScenePS}};
    const auto programs = rd::EncodeMaterialPrograms(dependencies, program_sources);
    const auto layout = rd::EncodeMaterialParameterLayout(parameters);
    const rd::MaterialSurfaceLayout surface{1,0};
    const auto surface_bytes = rd::EncodeMaterialSurfaceLayout(surface);
    const std::array chunks{
        rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::MaterialMetadata),1,true,metadata,1,static_cast<std::uint32_t>(metadata.size())},
        rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::MaterialBindings),1,true,bindings,6,rd::kMaterialBindingStride},
        rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::MaterialProgram),1,true,programs,1,rd::kMaterialProgramStride},
        rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::MaterialParameterLayout),1,true,layout,1,32},
        rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::MaterialSurfaceLayout),1,true,surface_bytes,1,rd::kMaterialSurfaceLayoutStride}};
    const auto material_bytes = rd::BuildPackage(hs::MakeAssetId(env_id), dependencies, chunks);
    const auto parameter_bytes = rd::BuildMaterialParameterPackage(env_parameters, parameters);
    const auto assets_for = [](std::string_view id, std::span<const std::string> dependencies) {
        std::vector<hs::AssetId> assets{hs::MakeAssetId(id)};
        for (const auto &dependency : dependencies) assets.push_back(hs::MakeAssetId(dependency));
        std::ranges::sort(assets, {}, &hs::AssetId::value); return assets;
    };
    const auto bulk_index = [](std::span<const hs::AssetId> assets, std::string_view id) {
        return static_cast<std::uint32_t>(std::ranges::find(assets, hs::MakeAssetId(id).value, &hs::AssetId::value)-assets.begin());
    };
    auto assets = assets_for(env_id, dependencies);
    auto compiled = rd::CompileMaterialDescriptor(env_id, material_bytes, parameter_bytes, bulk_index(assets,env_id), assets, textures);
    Require(compiled.View().surface == surface && compiled.value.metallic == rock.metallic && compiled.value.roughness_scale == rock.roughness_scale &&
            compiled.value.kind == rd::MaterialKind::Environment && compiled.defaults.size() == parameters.size() &&
            compiled.defaults[0].words == parameters[0].default_words && compiled.defaults[1].words == parameters[1].default_words &&
            std::ranges::equal(compiled.value.parameter_layout,layout) && compiled.programs.size()==1 && compiled.textures.size()==3,
            "environment descriptor differs from decoded material/defaults");
    const auto retained_name = compiled.programs[0].product_id.data();
    auto moved = std::move(compiled);
    Require(moved.programs[0].product_id == program_sources[0].shader_id && moved.programs[0].product_id.data() == retained_name &&
            moved.View().programs.data() == moved.programs.data(), "material storage move invalidated views");
    auto invalid = moved.View(); invalid.programs = {};
    Reject([&] { rd::ValidateMaterialDescriptor(invalid,assets,textures); }, "material descriptor accepted absent programs");
    auto dead_program = moved.programs; dead_program[0].bulk_index=UINT32_MAX; invalid=moved.View(); invalid.programs=dead_program;
    Reject([&] { rd::ValidateMaterialDescriptor(invalid,assets,textures); }, "material descriptor accepted dead program reference");
    auto dead_texture = moved.textures; dead_texture[0].texture.value=UINT32_MAX; invalid=moved.View(); invalid.textures=dead_texture;
    Reject([&] { rd::ValidateMaterialDescriptor(invalid,assets,textures); }, "material descriptor accepted dead texture reference");
    auto changed = std::vector<rd::MaterialParameterDefinition>(parameters.begin(), parameters.end()); changed.push_back(scalar("extra",0));
    const auto incompatible = rd::BuildMaterialParameterPackage(env_parameters,changed);
    Reject([&] { (void)rd::CompileMaterialDescriptor(env_id,material_bytes,incompatible,bulk_index(assets,env_id),assets,textures); },
           "material compiler accepted mismatched parameter layout");
    Require(rd::IsMaterialDescriptorProduct(env_id) && rd::IsMaterialDescriptorProduct("render/material/terrain.rpkg") &&
            rd::MaterialProduct("rock_0") == env_id &&
            !rd::IsMaterialDescriptorProduct("render/rock_0.rpkg") &&
            !rd::IsMaterialDescriptorProduct("render/rock_0_lod1.rpkg") && !rd::IsMaterialDescriptorProduct("render/crate_intact.rpkg"),
            "material descriptor selection included geometry-only products");

    for (const std::string name : {"archer_skin", "monster_pbr", "slime_body"})
    {
        const auto id="render/material/"+name+"/material.rpkg";
        const auto parameter_id=rd::MaterialParametersProduct(id);
        std::array<std::string,4> ids{"render/shader/hero_scene_ps.dxil", "render/shader/hero_visibility_ps.dxil",
            "render/shader/hero_resolve_cs.dxil", "render/shader/hero_shadow_ps.dxil"};
        const std::array hero_programs{rd::MaterialProgramSource{ids[0],rd::MaterialProgramKind::HeroScenePS},
            rd::MaterialProgramSource{ids[1],rd::MaterialProgramKind::HeroVisibilityPS},
            rd::MaterialProgramSource{ids[2],rd::MaterialProgramKind::HeroSurfaceResolveCS},rd::MaterialProgramSource{ids[3],rd::MaterialProgramKind::HeroShadowPS}};
        const auto hero_bytes=rd::BuildHeroMaterialPackage(name,parameters,surface,hero_programs);
        const auto hero=rd::ReadHeroMaterialPackage(hero_bytes,name);
        if(name=="monster_pbr") for(const auto *suffix:{"monster_basecolor","monster_emissive","monster_ram"}) {
            rd::TextureDescriptor texture{};texture.asset_id=hs::MakeAssetId("render/texture/"+std::string(suffix)+".rpkg");
            texture.description={512,512,1,10,std::string_view(suffix)=="monster_ram"?rd::kTextureFormatBc7:rd::kTextureFormatBc7Srgb};textures.push_back(texture);
        }
        const auto hero_assets=assets_for(id,hero.dependencies);
        const auto hero_parameters=rd::BuildMaterialParameterPackage(parameter_id,parameters);
        const auto descriptor=rd::CompileMaterialDescriptor(id,hero_bytes,hero_parameters,bulk_index(hero_assets,id),hero_assets,textures);
        Require(descriptor.value.kind==rd::MaterialKind::Hero && descriptor.value.surface==hero.surface &&
                descriptor.value.domain==hero.domain && descriptor.programs.size()==hero.programs.size() && descriptor.textures.empty(),
                "Hero descriptor changed domain or claimed fixed adapter texture ownership");
    }
    const std::array translucent_program{rd::MaterialProgramSource{
        "render/shader/hero_translucent_ps.dxil", rd::MaterialProgramKind::HeroTranslucentPS}};
    const auto translucent_package = rd::BuildHeroMaterialPackage("slime_body", parameters,
        {257u, 0u}, translucent_program);
    Require(rd::ReadHeroMaterialPackage(translucent_package, "slime_body").domain ==
        rd::HeroMaterialDomain::Translucent,
        "Hero package domain was inferred from material name instead of surface/program contract");
    const std::string decal_id="render/material/decal_fracture/material.rpkg";
    const auto compile_decal = [&](std::span<const rd::MaterialParameterDefinition> defaults) {
        const auto bytes=rd::BuildDecalMaterialPackage("decal_fracture",defaults,"render/texture/vfx_fracture_decal_array.rpkg",
            "render/shader/decal_surface_cs.dxil",{});
        const auto decoded=rd::ReadDecalMaterialPackage(bytes,"decal_fracture");
        const auto decal_assets=assets_for(decal_id,decoded.dependencies);
        const auto default_bytes=rd::BuildMaterialParameterPackage(rd::MaterialParametersProduct(decal_id),defaults);
        return rd::CompileMaterialDescriptor(decal_id,bytes,default_bytes,bulk_index(decal_assets,decal_id),decal_assets,textures);
    };
    std::array decal_defaults{scalar("target_roughness",0.9f),scalar("roughness_blend",0.2f)};
    const auto decal=compile_decal(decal_defaults);
    Require(decal.value.kind==rd::MaterialKind::Decal && decal.value.receivers==rd::DecalReceiverPolicy{} &&
            decal.textures.size()==1 && decal.textures[0].semantic==rd::MaterialTextureSemantic::DecalStructure &&
            decal.defaults[1].words==decal_defaults[1].default_words, "decal descriptor parity failed");
    Reject([&] { (void)compile_decal(std::span(decal_defaults).first(1)); }, "decal missing required float accepted");
    decal_defaults[1]=scalar("roughness_blend",1.1f);
    Reject([&] { (void)compile_decal(decal_defaults); }, "decal float outside unit interval accepted");
    decal_defaults[1].type=rd::MaterialParameterType::Uint;decal_defaults[1].default_words[0]=1;
    Reject([&] { (void)compile_decal(decal_defaults); }, "decal non-float default accepted");
    decal_defaults[1]=scalar("roughness_blend",0.2f);
    textures[3].dimension=rd::TextureDimension::Texture3D;
    Reject([&] { (void)compile_decal(decal_defaults); }, "decal accepted a volume texture binding");
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
        const auto prepared = rd::PrepareMaterial(graph, rd::MaterialPreparationDomain::Decal);
        Require(prepared.Graph().kind == graph.kind &&
                rd::EmitDecalMaterialContextHlsl(graph, "HsDecalSurface", 9) ==
                    rd::EmitDecalMaterialContextHlsl(prepared, "HsDecalSurface", 9),
                "prepared decal changed graph kind or emitted source");
        if (name == rd::kDecalMaterialNames.front())
        {
            bool preserved_name_error = false;
            try { (void)rd::EmitDecalMaterialContextHlsl(graph, "!", 9); }
            catch (const std::invalid_argument &error)
            {
                preserved_name_error = std::string_view(error.what()).find("invalid decal function name") !=
                    std::string_view::npos;
            }
            Require(preserved_name_error, "decal emitter changed function-name error priority");
            Reject([&] { (void)rd::PrepareMaterial(graph, rd::MaterialPreparationDomain::Environment); },
                   "environment preparation accepted a decal graph");
        }
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
    const auto prepared = rd::PrepareMaterial(identity);
    const auto prepared_emitted = rd::EmitRock0MaterialHlsl(prepared);
    const auto raw_reflection = rd::AnalyzeClosure(identity);
    const auto prepared_reflection = rd::AnalyzeClosure(prepared);
    Require(prepared.Graph().nodes.size() == identity.nodes.size() && emitted == prepared_emitted &&
            raw_reflection.capability_mask == prepared_reflection.capability_mask &&
            raw_reflection.varying_mask == prepared_reflection.varying_mask,
            "prepared material changed graph, reflection, or emitted source");
    Require(identity.nodes.size() == 5 && identity.base_color_output == 0 &&
            identity.roughness_output == 4 &&
            emitted.find("float3 HsRock0BaseColor(float3 sampled_base_color)") !=
                std::string::npos &&
            emitted.find("float HsRock0Roughness(float sampled_roughness, float roughness_scale)") !=
                std::string::npos,
            "identity material function signatures changed");
    Reject([&] { (void)rd::PrepareMaterial(identity, rd::MaterialPreparationDomain::Environment); },
           "environment preparation accepted a rock_0 compiler graph");
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
    Reject([&] { (void)rd::PrepareMaterial(wrong_ir); },
           "prepared material accepted mismatched scalar input");
    auto cyclic_ir = identity;
    cyclic_ir.nodes[3].inputs[0] = 4;
    Reject([&] { (void)rd::PrepareMaterial(cyclic_ir); },
           "prepared material accepted a cyclic/backward edge");
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
    Require(dead.nodes.size() == 3, "dead scalar graph node count changed");
    auto invalid_dead = dead;
    invalid_dead.nodes.back().value[0] = std::numeric_limits<float>::infinity();
    Reject([&] { (void)rd::PrepareMaterial(invalid_dead); },
           "prepared material accepted an invalid dead node");

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
    Require(leaf_hlsl == rd::EmitLeaf0MaterialHlsl(rd::PrepareMaterial(leaf)),
            "prepared leaf emitter changed generated source");
    Require(leaf.kind == rd::MaterialGraphKind::Leaf0 && leaf.nodes.size() == 1 &&
            leaf.opacity_output == 0 &&
            leaf_hlsl.find("float HsLeaf0Opacity(float sampled_alpha)") != std::string::npos,
            "leaf opacity function signature changed");
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
    Require(hlsl == rd::EmitEnvironmentMaterialHlsl(rd::PrepareMaterial(graph,
                rd::MaterialPreparationDomain::Environment)),
            "prepared numeric environment emitter changed generated source");
    Require(graph.kind == rd::MaterialGraphKind::Masked,
            "generic material graph kind changed");

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
    Require(hlsl.find("float4") == std::string::npos &&
            hlsl.find("float3") == std::string::npos, "unused vector output leaked into the roughness function");
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
        Require(instance.material_id.value==(ordinal<14?hs::MakeAssetId(rd::MaterialProduct(rd::kAuthoredMaterialNames[ordinal])).value:ordinal==33?hs::MakeAssetId("render/material/terrain.rpkg").value:hs::MakeAssetId("render/material/ground_detail.rpkg").value),
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
            const auto &graph = definition.prepared.Graph();
            const auto expected_kind = masked ? rd::MaterialGraphKind::Masked
                                              : rd::MaterialGraphKind::Opaque;
            Require(graph.kind == expected_kind, "authored graph kind changed");
            const auto ordinal = static_cast<std::size_t>(std::ranges::find(rd::kEnvironmentMaterialNames, name) -
                                                          rd::kEnvironmentMaterialNames.begin());
            const auto hlsl = rd::EmitEnvironmentMaterialContextHlsl(definition.prepared, ordinal);
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
    const auto &graph = definition.prepared.Graph();
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
    const auto hlsl = rd::EmitEnvironmentMaterialContextHlsl(definition.prepared, ordinal);
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
    const auto &graph = definition.prepared.Graph();
    Require(graph.closure_output != rd::kNoMaterialInput &&
            graph.nodes[graph.closure_output].op == rd::MaterialOp::MetallicRoughnessClosure &&
            rd::MaterialInputCount(rd::MaterialOp::MetallicRoughnessClosure,
                                   rd::MaterialType::SurfaceClosure) == 6,
            "typed surface closure root or signature missing");
    const auto hlsl = rd::EmitEnvironmentMaterialContextHlsl(definition.prepared, 3);
    const auto raw_closure = rd::AnalyzeClosure(graph);
    const auto prepared_closure = rd::AnalyzeClosure(definition.prepared);
    Require(raw_closure.capability_mask == prepared_closure.capability_mask &&
            raw_closure.varying_mask == prepared_closure.varying_mask,
            "prepared closure reflection changed authored material");
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
    Require(renamed_definition.prepared.Graph().kind == rd::MaterialGraphKind::Opaque &&
            renamed_definition.prepared.Graph().closure_output != rd::kNoMaterialInput,
            "typed closure was incorrectly restricted to rock_0");
    auto masked = Json::parse(source);
    masked["id"] = "leaf_0";
    masked["render_flags"] = {{"two_sided", true}, {"alpha_mode", "mask"}};
    const auto masked_definition = mc::ParseMaterialDefinition("leaf_0", masked.dump());
    const auto masked_closure = masked_definition.prepared.Graph().closure_output;
    Require(masked_definition.prepared.Graph().kind == rd::MaterialGraphKind::Masked &&
            masked_closure != rd::kNoMaterialInput &&
            masked_definition.prepared.Graph().nodes[masked_closure].inputs[5] != rd::kNoMaterialInput,
            "masked typed closure lost its opacity root");
    const auto masked_hlsl = rd::EmitEnvironmentMaterialContextHlsl(masked_definition.prepared, 3);
    Require(masked_hlsl.find("float HsMaterialEvaluateOpacity") != std::string::npos &&
            masked_hlsl.find("ctx.sampled_alpha") != std::string::npos,
            "masked typed closure opacity evaluator missing");
    const auto covered_begin = masked_hlsl.find("HsMaterialSurface HsMaterialEvaluateCoveredSurface");
    const auto covered_end = masked_hlsl.find("float HsMaterialEvaluateOpacity", covered_begin);
    const auto covered = masked_hlsl.substr(covered_begin, covered_end - covered_begin);
    Require(covered.find("ctx.sampled_alpha") < covered.find("discard;") &&
            covered.find("discard;") < covered.find("HsMaterialMakeSurfaceClosure"),
            "masked closure must reject opacity before evaluating the remaining surface nodes");

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
    const auto constant_prepared = rd::PrepareMaterial(constant_closures,
        rd::MaterialPreparationDomain::Environment);
    Require(constant_hlsl == rd::EmitEnvironmentMaterialContextHlsl(constant_prepared, 0),
            "prepared environment context emitter changed generated source");
    const auto hero_prepared = rd::PrepareMaterial(constant_closures,
        rd::MaterialPreparationDomain::Hero);
    Require(rd::EmitHeroMaterialContextHlsl(constant_closures, "HsHeroSurface", "HsHeroOpacity") ==
            rd::EmitHeroMaterialContextHlsl(hero_prepared, "HsHeroSurface", "HsHeroOpacity"),
            "prepared hero context emitter changed generated source");
    bool preserved_hero_name_error = false;
    try { (void)rd::EmitHeroMaterialContextHlsl(constant_closures, "!", "HsHeroOpacity"); }
    catch (const std::invalid_argument &error)
    {
        preserved_hero_name_error = std::string_view(error.what()).find("invalid hero material function name") !=
            std::string_view::npos;
    }
    Require(preserved_hero_name_error, "hero emitter changed function-name error priority");
    Require(constant_hlsl.find("HsMaterialMakeSurfaceClosure(") != std::string::npos &&
            constant_hlsl.find("HsSurfaceClosure()") == std::string::npos,
            "constant closure aggregate was folded into an invalid empty struct");

    auto layered = constant_closures;
    layered.closure_output = MaterialNode(layered, rd::MaterialType::SurfaceClosure,
        rd::MaterialOp::CoatClosure, {constant_root, constant_ao, constant_roughness_b});
    const auto uniform_hlsl = rd::EmitUniformClosureHlsl(layered);
    Require(uniform_hlsl == rd::EmitUniformClosureHlsl(rd::PrepareMaterial(layered)) &&
            uniform_hlsl.find("HsMaterialApplyCoat(base") != std::string::npos,
            "uniform coat must remain in the deferred evaluator");
    const auto varying_weight = MaterialNode(layered, rd::MaterialType::Float,
        rd::MaterialOp::SampledRoughness, {});
    const auto varying_coat = MaterialNode(layered, rd::MaterialType::SurfaceClosure,
        rd::MaterialOp::CoatClosure, {constant_root, varying_weight, constant_roughness_b});
    layered.closure_output = varying_coat;
    Require(rd::EmitUniformClosureHlsl(layered).find("HsMaterialApplyCoat(base") == std::string::npos,
            "varying coat family must be supplied by surface sidecars");
    layered.nodes[varying_coat].inputs[1] = constant_base;
    Reject([&] { (void)rd::EmitUniformClosureHlsl(layered); },
           "uniform emitter skipped graph type validation");

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
    Require(legacy.prepared.Graph().closure_output != rd::kNoMaterialInput &&
            legacy.prepared.Graph().nodes[legacy.prepared.Graph().closure_output].op == rd::MaterialOp::MetallicRoughnessClosure,
            "legacy material did not lower to a typed closure root");
    const auto legacy_hlsl = rd::EmitEnvironmentMaterialContextHlsl(legacy.prepared, 4);
    Require(legacy_hlsl.find("HsMaterialSurface HsMaterialEvaluateSurface") != std::string::npos &&
            legacy_hlsl.find("HsMaterialMakeSurfaceClosure(n") != std::string::npos &&
            legacy_hlsl.find("MaterialParameters[ctx.parameter_base + 0]") != std::string::npos,
            "legacy flat outputs did not lower to the shared surface root");
}

void TestSurfaceMetadataCodec()
{
    constexpr std::array<std::array<std::uint32_t, 3>, 7> cases{{
        {0x0000ffffu, 0x0000ffffu, 0u},
        {0x13000016u, 0x30000016u, 3u},
        {0x20123456u, 0x00123456u, 65539u},
        {0x32000007u, 0x20000007u, 7u},
        {0x41000000u, 0x10000000u, 5u},
        {0x51000001u, 0x10000001u, 1285u},
        {0x61000002u, 0x10000002u, 1541u},
    }};
    for (const auto &[packed, expected_material, expected_flags] : cases)
    {
        const auto decoded = rd::DecodeSurfaceMetadata(packed);
        Require(decoded.material == expected_material && decoded.flags == expected_flags,
                "surface metadata tuple decode changed");
    }
    const auto page_255_material_65535 = rd::DecodeSurfaceMetadata(0x02ffffffu);
    Require(page_255_material_65535.material == 0x20ffffffu &&
            page_255_material_65535.flags == 0u,
            "page/material namespace boundary was not restored");
    const auto zero_material = rd::DecodeSurfaceMetadata(0x10000000u);
    Require(zero_material.material == 0u && zero_material.flags == 3u,
            "zero material with nonzero surface flags was lost");
    const auto reserved = rd::DecodeSurfaceMetadata(0xf2000007u);
    Require(reserved.material == 0x20000007u && reserved.flags == 0u,
            "reserved or unsupported metadata tuple changed decode");
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
    auto invalid_graph = definition.prepared.Graph();
    auto ref = std::ranges::find(invalid_graph.nodes, rd::MaterialOp::TextureRef, &rd::MaterialNode::op);
    Require(ref != invalid_graph.nodes.end(), "texture reference fixture missing");
    ref->declaration_index = invalid_graph.textures.size();
    Reject([&] { rd::ValidateEnvironmentMaterialGraph(invalid_graph); },
           "out-of-range public texture reference accepted");
    invalid_graph = definition.prepared.Graph();
    rd::MaterialNode dead;
    dead.id = "dead_sampler"; dead.op = rd::MaterialOp::SamplerRef;
    dead.type = rd::MaterialType::SamplerRef; dead.declaration_index = invalid_graph.samplers.size();
    invalid_graph.nodes.push_back(dead);
    Reject([&] { rd::ValidateEnvironmentMaterialGraph(invalid_graph); },
           "dead out-of-range public sampler reference accepted");
    auto sample_level_graph = definition.prepared.Graph();
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
    const auto original_sample = *std::ranges::find(definition.prepared.Graph().nodes,
        rd::MaterialOp::SampleGrad, &rd::MaterialNode::op);
    const std::vector<std::uint32_t> two_layers(definition.prepared.Graph().textures.size(), 2);
    auto layer_graph = definition.prepared.Graph();
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
    const auto prepared_layers = rd::PrepareMaterial(layer_graph,
        rd::MaterialPreparationDomain::Environment);
    rd::ValidateMaterialSampleLayers(prepared_layers, two_layers, 1);
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
    Reject([&] { rd::ValidateMaterialSampleLayers(rd::PrepareMaterial(invalid_layer_graph,
        rd::MaterialPreparationDomain::Environment), two_layers, 1); },
        "prepared layer validation accepted an unavailable array layer");
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

    auto library_context = definition.prepared.Graph();
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
    for (const auto &texture : definition.prepared.Graph().textures) dependencies.push_back(texture.product_id);
    std::ranges::sort(dependencies);
    const auto bindings = rd::EncodeMaterialBindings(dependencies, definition.prepared.Graph().textures);
    const auto decoded = rd::DecodeMaterialBindings(bindings, dependencies);
    Require(decoded.size() == rd::kMaterialTextureSlots, "texture binding count changed");
    for (std::size_t slot = 0; slot < definition.prepared.Graph().textures.size(); ++slot)
        Require(decoded[slot].slot == slot &&
                dependencies[decoded[slot].dependency_index] == definition.prepared.Graph().textures[slot].product_id &&
                decoded[slot].semantic == definition.prepared.Graph().textures[slot].semantic &&
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
    Reject([&] { (void)rd::EncodeMaterialBindings(missing, definition.prepared.Graph().textures); },
           "binding accepted missing texture package dependency");
    auto wrong_namespace = dependencies;
    for (auto &id : wrong_namespace) id = "environment/texture.dds";
    Reject([&] { (void)rd::DecodeMaterialBindings(bindings, wrong_namespace); },
           "binding accepted DDS dependency as texture package");
    const auto base = definition.prepared.Graph().textures.front();
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
    const auto &graph = definition.prepared.Graph();
    Require(graph.textures.size() == 4 && graph.samplers.size() == 1 &&
            graph.samplers[0].profile == rd::MaterialSamplerProfile::EnvironmentAnisotropicWrap &&
            graph.normal_output != rd::kNoMaterialInput && graph.ao_output != rd::kNoMaterialInput &&
            definition.material.textures[0] == "render/texture/environment_rock_basecolor.rpkg",
            "legacy material was not lowered to typed texture declarations and normal/AO outputs");
    for (const auto &node : graph.nodes)
        Require(node.op != rd::MaterialOp::SampledBaseColor && node.op != rd::MaterialOp::SampledRoughness &&
                node.op != rd::MaterialOp::SampledAlpha, "legacy sampled input survived context lowering");
    const auto hlsl = rd::EmitEnvironmentMaterialContextHlsl(definition.prepared, 6);
    const auto surface = hlsl.substr(hlsl.find("HsMaterialSurface HsMaterialEvaluateSurface"));
    Require(surface.find("HsTextureSampleGrad(HsMaterialTexture0, 36u, EnvironmentWrap,") != std::string::npos &&
            surface.find("HsTextureSampleGrad(HsMaterialTexture3, 39u, EnvironmentWrap,") != std::string::npos &&
            surface.find("ctx.sampled_base_color") == std::string::npos &&
            surface.find("ctx.sampled_roughness") == std::string::npos,
            "version2 context evaluator retained untextured sampled-value inputs");
    rd::ValidateMaterialSampleLayers(definition.prepared, std::array<std::uint32_t, 4>{4,4,4,4}, 3);
    document["id"] = "leaf_0";
    document["textures"] = {{"base_color", "environment/environment_leaf_basecolor.dds"},
        {"surface", "environment/environment_leaf_roughness.dds"},
        {"normal", "environment/environment_leaf_normal.dds"}, {"detail", ""}};
    document["render_flags"] = {{"two_sided", true}, {"alpha_mode", "mask"}};
    document["nodes"].push_back({{"id", "alpha"}, {"op", "sampled_alpha"}, {"type", "float"}});
    document["outputs"]["opacity"] = {{"node", "alpha"}, {"type", "float"}};
    const auto leaf = mc::ParseMaterialDefinition("leaf_0", document.dump());
    const auto leaf_hlsl = rd::EmitEnvironmentMaterialContextHlsl(leaf.prepared, 3);
    const auto opacity = leaf_hlsl.substr(leaf_hlsl.find("float HsMaterialEvaluateOpacity"));
    Require(leaf.prepared.Graph().samplers[0].profile == rd::MaterialSamplerProfile::FoliageAnisotropicClamp &&
            opacity.find("HsTextureSampleGrad(HsMaterialTexture0, 18u, FoliageClamp,") != std::string::npos &&
            opacity.find("HsMaterialTexture1") == std::string::npos &&
            opacity.find("ctx.sampled_alpha") == std::string::npos,
            "legacy masked opacity lowering lost typed sampling or pruning");
    const auto covered_begin = leaf_hlsl.find("HsMaterialSurface HsMaterialEvaluateCoveredSurface");
    const auto covered_end = leaf_hlsl.find("float HsMaterialEvaluateOpacity", covered_begin);
    const auto covered = leaf_hlsl.substr(covered_begin, covered_end - covered_begin);
    const std::string base_sample = "HsTextureSampleGrad(HsMaterialTexture0, 18u, FoliageClamp,";
    const auto first_base = covered.find(base_sample);
    const auto discard = covered.find("discard;");
    Require(first_base < discard && covered.find(base_sample, first_base + 1) == std::string::npos &&
            discard < covered.find("HsTextureSampleGrad(HsMaterialTexture1,") &&
            discard < covered.find("HsTextureSampleGrad(HsMaterialTexture2,"),
            "masked raster must reuse its alpha/base sample and defer roughness/normal until coverage passes");
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
void TestPackedRendererAssets()
{
    const auto path = std::filesystem::temp_directory_path() /
        ("hs-renderer-bulk-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".hsp");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); } } cleanup{path};
    std::array<std::byte, 256> file;
    for (std::size_t i = 0; i < file.size(); ++i) file[i] = static_cast<std::byte>(i);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(file.data()), file.size()); output.close();
    Require(bool(output), "bulk fixture write failed");
    hs::ShippedContentOwner owner;
    Require(owner.Open(path, owner).Succeeded(), "bulk fixture open failed");
    std::array<rd::RendererBulkDescriptor, 2> entries{{
        {{11}, {64, 64, rd::RenderBulkFormat::Package, hs::Sha256(std::span(file).subspan(64, 64))}},
        {{22}, {128, 16, rd::RenderBulkFormat::ShaderDxil, hs::Sha256(std::span(file).subspan(128, 16))}}}};
    Require(hs::ValidatePackedRendererAssets(owner, entries).Succeeded(), "valid bulk table rejected");
    std::vector<std::byte> bytes;
    Require(hs::ReadPackedRendererAsset(owner, entries, {11}, rd::RenderBulkFormat::Package, bytes).Succeeded() &&
        std::ranges::equal(bytes, std::span(file).subspan(64, 64)), "typed bulk read differs");
    const auto accepted = bytes;
    Require(!hs::ReadPackedRendererAsset(owner, entries, {11}, rd::RenderBulkFormat::ShaderDxil, bytes) &&
        !hs::ReadPackedRendererAsset(owner, entries, {99}, rd::RenderBulkFormat::Package, bytes) && bytes == accepted,
        "wrong bulk type or identity accepted");
    const auto second = entries[1].range;
    entries[1].range = entries[0].range;
    Require(hs::ValidatePackedRendererAssets(owner, entries).Succeeded(), "immutable bulk aliases must be allowed");
    entries[1].range = second;
    entries[0].range.expected_digest[0] ^= std::byte{1};
    Require(!hs::ReadPackedRendererAsset(owner, entries, {11}, rd::RenderBulkFormat::Package, bytes) && bytes == accepted,
        "unverified bulk bytes published");
    entries[0].range.offset = std::numeric_limits<std::uint64_t>::max();
    Require(!hs::ValidatePackedRendererAssets(owner, entries) &&
        !hs::ReadPackedRendererAsset(owner, entries, {11}, rd::RenderBulkFormat::Package, bytes) && bytes == accepted,
        "overflowing bulk range accepted");
    entries[0].range.offset = 65;
    Require(!hs::ValidatePackedRendererAssets(owner, entries), "unaligned bulk accepted");
    entries[0].range.offset = 64; entries[1].asset_id = {11};
    Require(!hs::ValidatePackedRendererAssets(owner, entries), "duplicate bulk identity accepted");
}
void TestGeometryPackage()
{
    const std::vector<std::string>deps{"render/fixture_lod1.rpkg","render/fixture_lod2.rpkg","render/geometry/fixture.rpage"};
    const auto g=GeometryFixture();const auto m=GeometryMetadataFixture(g);
    const auto metadata=rd::EncodeGeometryMetadata(m,deps,"render/fixture.rpkg");
    const auto decoded_metadata=rd::DecodeGeometryMetadata(metadata,deps,"render/fixture.rpkg");
    const auto page_id=hs::MakeAssetId("render/geometry/fixture.rpage");const auto page=rd::BuildGeometryPage(page_id,{},g);
    const auto decoded=rd::DecodeGeometryPage(page,page_id,decoded_metadata);
    const auto prepared=rd::PrepareGeometryPage(page,page_id,decoded_metadata);
    Require(rd::BuildGeometryPage(page_id,{},prepared.geometry)==rd::BuildGeometryPage(page_id,{},decoded),
        "prepared geometry differs from decode API");
    for(const auto &chunk:prepared.package.chunks)
        Require(chunk.bytes.data()>=page.data() && chunk.bytes.data()+chunk.bytes.size()<=page.data()+page.size(),
            "prepared package must borrow original page bytes");
    Require(decoded.vertices.size()==6 && decoded.indices==g.indices && decoded.canonical_primitive_remap==g.canonical_primitive_remap && decoded.meshlets.size()==2,"geometry roundtrip/duplicate occurrences");
    const std::vector<std::string> material_dependencies{"render/geometry/fixture.rpage",
        "render/material/rock_0/material.rpkg"};
    const auto material_reference = rd::EncodeGeometryMaterialReference(1, material_dependencies);
    Require(rd::DecodeGeometryMaterialReference(material_reference, material_dependencies) == 1,
        "canonical geometry material reference changed");
    Reject([&] { (void)rd::EncodeGeometryMaterialReference(0, material_dependencies); },
        "geometry page accepted as material owner");
    auto invalid_reference = material_reference;
    invalid_reference[2] = std::byte{1};
    Reject([&] { (void)rd::DecodeGeometryMaterialReference(invalid_reference, material_dependencies); },
        "geometry material reference reserved bits accepted");
    invalid_reference = material_reference;
    invalid_reference[0] = std::byte{2};
    Reject([&] { (void)rd::DecodeGeometryMaterialReference(invalid_reference, material_dependencies); },
        "geometry material dependency index out of range accepted");
    Reject([&]{(void)rd::DecodeGeometryMetadata(metadata,deps,"render/other.rpkg");},"wrong page product accepted");
    auto reserved=metadata;reserved[88]=std::byte{1};Reject([&]{(void)rd::DecodeGeometryMetadata(reserved,deps,"render/fixture.rpkg");},"metadata reserved accepted");
    auto bad=g;bad.canonical_primitive_remap[1]=1;Reject([&]{rd::ValidateGeometryData(bad);},"duplicate primitive occurrence accepted");
    bad=g;bad.vertices[0].normal[0]=std::numeric_limits<float>::quiet_NaN();Reject([&]{rd::ValidateGeometryData(bad);},"nonfinite geometry vertex field accepted");
    bad=g;bad.vertices[0].bone_weights[0]=0.5f;Reject([&]{rd::ValidateGeometryData(bad);},"unnormalized geometry vertex weights accepted");
    bad=g;bad.vertices[0].reserved=1;Reject([&]{rd::ValidateGeometryData(bad);},"reserved geometry vertex field accepted");
    bad=g;bad.meshlet_triangles[0]|=0x01000000;Reject([&]{rd::ValidateGeometryData(bad);},"packed triangle high byte accepted");
    bad=g;bad.meshlet_triangles[0]=0x000102;Reject([&]{rd::ValidateGeometryData(bad);},"reversed primitive orientation accepted");
    bad=g;bad.meshlet_vertex_refs[0]=3;Reject([&]{rd::ValidateGeometryData(bad);},"mixed meshlet material accepted");
    bad=g;bad.meshlets[0].sphere[3]=0;Reject([&]{rd::ValidateGeometryData(bad);},"undersized meshlet sphere accepted");
    bad=g;bad.meshlets[0].flags=rd::kGeometryMeshletConeValid;bad.meshlets[0].cone={0,0,1,0.5f};Reject([&]{rd::ValidateGeometryData(bad);},"two sided cone accepted");
    auto mismatch=m;++mismatch.index_count;Reject([&]{(void)rd::DecodeGeometryPage(page,page_id,mismatch);},"metadata/page counts accepted");
    Reject([&]{(void)rd::PrepareGeometryPage(page,page_id,mismatch);},"prepared metadata mismatch accepted");
    auto excluded=m;excluded.bounds.sphere[3]=0;
    Reject([&]{(void)rd::PrepareGeometryPage(page,page_id,excluded);},"prepared metadata excludes vertices");
    const auto view=rd::ReadPackage(page,page_id);std::vector<rd::ChunkSource>chunks;for(const auto&c:view.chunks)chunks.push_back({static_cast<std::uint32_t>(c.type),c.version,c.required,c.bytes,c.count,c.stride});
    auto meshlet_bytes=std::vector<std::byte>(chunks[3].bytes.begin(),chunks[3].bytes.end());meshlet_bytes[56]=std::byte{1};chunks[3].bytes=meshlet_bytes;const auto malformed=rd::BuildPackage(page_id,{},chunks);Reject([&]{(void)rd::DecodeGeometryPage(malformed,page_id,m);},"meshlet reserved accepted");
    Reject([&]{(void)rd::PrepareGeometryPage(malformed,page_id,m);},"prepared malformed payload accepted");
    Reject([&]{(void)rd::DecodeGeometryPagePayload(malformed,page_id,m.flags);},"payload decode malformed meshlet accepted");
    chunks[3].bytes=view.chunks[3].bytes;chunks.erase(chunks.begin()+5);const auto missing=rd::BuildPackage(page_id,{},chunks);Reject([&]{(void)rd::DecodeGeometryPage(missing,page_id,m);},"missing required geometry array accepted");
    // Degenerate primitives still own referenced positions and cannot lose them.
    bad=g;bad.indices[1]=0;bad.meshlet_triangles[1]=0x020000;rd::ValidateGeometryData(bad);
}

void TestGeometryDescriptor()
{
    const auto geometry = GeometryFixture();
    const auto metadata = GeometryMetadataFixture(geometry);
    const auto id = hs::MakeAssetId("render/geometry/descriptor.rpage");
    const std::vector<std::string> dependencies{"render/fixture.rpkg"};
    const auto bytes = rd::BuildGeometryPage(id, dependencies, geometry);
    const auto prepared = rd::PrepareGeometryPage(bytes, id, metadata);
    const auto compiled = rd::CompileGeometryDescriptor(bytes, prepared, 13);
    rd::ValidateGeometryDescriptor(compiled.View(), bytes.size());
    rd::ValidateGeometryDescriptorMetadata(compiled.View(), metadata);
    const auto bound = rd::BindGeometryDescriptor(bytes, compiled.View());
    Require(compiled.asset_id.value == id.value && compiled.bulk_index == 13 && compiled.max_material_index == 1 &&
            compiled.dependencies.size() == 1 && compiled.dependencies[0].value == hs::MakeAssetId(dependencies[0]).value,
            "compiled geometry identity or typed dependencies differ");
    Require(bound.submeshes.data() == compiled.submeshes.data() && bound.submeshes.size() == geometry.submeshes.size(),
            "GPU geometry must borrow descriptor submeshes");
    for (std::size_t i = 0; i < bound.chunks.size(); ++i)
        Require(bound.chunks[i].data() == prepared.package.chunks[i].bytes.data() &&
                std::ranges::equal(bound.chunks[i], prepared.package.chunks[i].bytes), "GPU geometry bytes or aliases differ");
    for (std::size_t i = 0; i < bound.submeshes.size(); ++i)
    {
        const auto &a = bound.submeshes[i]; const auto &b = geometry.submeshes[i];
        Require(a.first_index == b.first_index && a.index_count == b.index_count && a.first_meshlet == b.first_meshlet &&
                a.meshlet_count == b.meshlet_count && a.material_index == b.material_index, "compiled geometry submesh differs");
    }
    const auto payload = rd::PrepareGeometryPagePayload(bytes, id, geometry.flags);
    const auto old_payload = rd::DecodeGeometryPagePayload(bytes, id, geometry.flags);
    Require(rd::BuildGeometryPage(id, dependencies, payload.geometry) == rd::BuildGeometryPage(id, dependencies, old_payload),
            "prepared payload differs from legacy decoder");
    auto wrong_metadata = metadata;
    wrong_metadata.bounds.sphere[3] = std::nextafter(wrong_metadata.bounds.sphere[3], 2.0f);
    Reject([&] { rd::ValidateGeometryDescriptorMetadata(compiled.View(), wrong_metadata); }, "direct geometry exact bounds mismatch accepted");
    wrong_metadata = metadata; ++wrong_metadata.max_material_index;
    Reject([&] { rd::ValidateGeometryDescriptorMetadata(compiled.View(), wrong_metadata); }, "geometry material maximum mismatch accepted");
    auto invalid = compiled;
    invalid.bounds.minimum[0] = std::numeric_limits<float>::quiet_NaN();
    Reject([&] { rd::ValidateGeometryDescriptor(invalid.View(), bytes.size()); }, "descriptor nonfinite bounds accepted");
    invalid = compiled; ++invalid.vertex_count;
    Reject([&] { rd::ValidateGeometryDescriptor(invalid.View(), bytes.size()); }, "descriptor vertex byte count mismatch accepted");
    invalid = compiled; invalid.asset_id.value = 0;
    Reject([&] { rd::ValidateGeometryDescriptor(invalid.View(), bytes.size()); }, "descriptor empty identity accepted");
    invalid = compiled; invalid.chunks[0].offset = UINT64_MAX;
    Reject([&] { rd::ValidateGeometryDescriptor(invalid.View(), bytes.size()); }, "descriptor range overflow accepted");
    Reject([&] { (void)rd::BindGeometryDescriptor(bytes, invalid.View()); }, "bound geometry range overflow accepted");
    invalid = compiled; ++invalid.submeshes[1].first_index;
    Reject([&] { rd::ValidateGeometryDescriptor(invalid.View(), bytes.size()); }, "descriptor submesh coverage gap accepted");
    const auto end = compiled.chunks.back().offset + compiled.chunks.back().byte_count;
    Reject([&] { rd::ValidateGeometryDescriptor(compiled.View(), end - 1); }, "descriptor truncated extent accepted");
    Reject([&] { (void)rd::BindGeometryDescriptor(std::span(bytes).first(static_cast<std::size_t>(end - 1)), compiled.View()); },
           "bound geometry truncation accepted");
    auto copied_bytes = bytes;
    Reject([&] { (void)rd::CompileGeometryDescriptor(copied_bytes, prepared, 13); }, "prepared geometry accepted unrelated byte owner");
    copied_bytes[0] ^= std::byte{1};
    const auto rebound = rd::BindGeometryDescriptor(copied_bytes, compiled.View());
    Require(rebound.chunks[0].data() == copied_bytes.data() + compiled.chunks[0].offset,
            "descriptor binding reparsed the package or copied GPU bytes");

    auto skinned = geometry; skinned.flags |= rd::kGeometryFlagSkinned;
    for (auto &vertex : skinned.vertices) { vertex.bone_weights[0] = 1; vertex.bone_indices[3] = 17; }
    const auto skin_bytes = rd::BuildGeometryPage(id, {}, skinned);
    auto skin_prepared = rd::PrepareGeometryPagePayload(skin_bytes, id, skinned.flags);
    Require(rd::CompileGeometryDescriptor(skin_bytes, skin_prepared, 13).max_bone_index == 17,
            "zero-weight bone index omitted from descriptor maximum");
    skin_prepared.geometry.vertices[0].bone_weights[0] = 0;
    Reject([&] { (void)rd::CompileGeometryDescriptor(skin_bytes, skin_prepared, 13); }, "unnormalized skinned descriptor accepted");
    skin_prepared.geometry.vertices[0].bone_weights[0] = 1;
    skin_prepared.geometry.vertices[0].reserved = 1;
    Reject([&] { (void)rd::CompileGeometryDescriptor(skin_bytes, skin_prepared, 13); }, "skinned descriptor reserved field accepted");

    rd::ExpandedVertex first;
    first.position = {1, -0.25f + 0.12f, 2}; first.normal = {0, 1, 0}; first.uv = {0.2f, 0.3f};
    first.tangent = {1, 0, 0, 1}; first.material_index = 5;
    auto duplicate = first; duplicate.normal = {1, 0, 0}; duplicate.material_index = 7;
    auto distinct_bone = first; distinct_bone.bone_indices[3] = 9;
    auto distinct_weight = first; distinct_weight.bone_weights[0] = 1;
    auto above = first; above.position[1] += 0.001f;
    const std::array input{above, first, duplicate, distinct_bone, distinct_weight};
    const std::array expected{first, distinct_bone, distinct_weight};
    const auto support = rd::BuildGroundSupport(input, 0.25f);
    Require(rd::EncodeExpandedVertices(support) == rd::EncodeExpandedVertices(expected),
            "ground support changed cutoff, first occurrence, influence keys or retained vertex fields");
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
    const auto character_page_id = hs::MakeAssetId(production_deps[0]);
    const auto character_page = rd::BuildGeometryPage(character_page_id, {}, geometry);
    const auto prepared_character = rd::PrepareGeometryPage(character_page, character_page_id, gm);
    rd::ValidateCharacterGeometry(character_metadata, prepared_character);
    auto wrong_character = character_metadata;
    ++wrong_character.geometry.vertex_count;
    Reject([&] { rd::ValidateCharacterGeometry(wrong_character, prepared_character); },
           "prepared character accepted mismatched metadata");
    const auto production_view=rd::ReadPackage(production,asset);
    // Descriptor compilation consumes already admitted data and retains wire samples as ranges.
    std::vector<rd::RendererBulkDescriptor> descriptor_bulks{
        {asset, {64, production.size(), rd::RenderBulkFormat::Package, hs::Sha256(production)}}};
    auto descriptor_geometry = rd::CompileGeometryDescriptor(character_page, prepared_character, 1);
    const std::array descriptor_pages{descriptor_geometry.View()};
    rd::MaterialDescriptor descriptor_material;
    descriptor_material.asset_id = hs::MakeAssetId(production_deps[1]);
    descriptor_material.kind = rd::MaterialKind::Hero;
    descriptor_material.domain = rd::HeroMaterialDomain::Opaque;
    const std::array descriptor_materials{descriptor_material};
    auto compiled_character = rd::CompileCharacterDescriptor(production, production_view, character_metadata,
        0, descriptor_bulks, descriptor_pages, descriptor_materials);
    auto compiled_view = compiled_character.View();
    const auto transform_chunk = std::ranges::find(production_view.chunks,
        rd::ChunkType::CharacterLocalTransforms, &rd::ChunkView::type);
    Require(compiled_character.data.transforms.empty() && compiled_character.data.transforms.capacity() == 0 &&
        compiled_view.transforms.offset == 64 + static_cast<std::size_t>(transform_chunk->bytes.data() - production.data()) &&
        compiled_view.transforms.byte_count == source.transforms.size() * 40 &&
        compiled_view.transforms.expected_digest == hs::Sha256(transform_chunk->bytes) &&
        compiled_view.materials[0].value == 0 && compiled_view.geometry_page.value == 0,
        "compiled character sample range and typed references");
    std::vector<rd::PackedCharacterTransform> wire_samples(source.transforms.size());
    std::memcpy(wire_samples.data(), transform_chunk->bytes.data(), transform_chunk->bytes.size());
    for (std::size_t i = 0; i < wire_samples.size(); ++i)
    {
        const auto value = wire_samples[i].Value();
        Require(value.translation == source.transforms[i].translation && value.rotation == source.transforms[i].rotation &&
            value.scale == source.transforms[i].scale && value.translation_padding == 0 && value.scale_padding == 0,
            "packed 40-byte transform conversion differs from decoded values");
    }
    auto bad_descriptor = compiled_view; bad_descriptor.transforms.offset = UINT64_MAX;
    Reject([&] { rd::ValidateCharacterDescriptor(bad_descriptor, descriptor_bulks, descriptor_pages, descriptor_materials); },
        "compiled character accepted overflowing range");
    bad_descriptor = compiled_view; bad_descriptor.geometry_page = {99};
    Reject([&] { rd::ValidateCharacterDescriptor(bad_descriptor, descriptor_bulks, descriptor_pages, descriptor_materials); },
        "compiled character accepted dead geometry handle");
    rd::CharacterAnimationExtensionData named;
    named.character_asset_id = asset; named.sample_rate_hz = 60;
    named.bone_names = {"root", "child"}; named.parents = source.parents;
    named.inverse_bind_matrices = source.inverse_bind_matrices;
    named.clips = {{hs::MakeAssetId("animation/test/idle"), "Idle", 0, 2, 1.0f, true}};
    named.transforms.assign(source.transforms.begin(), source.transforms.begin() + 4);
    named.sockets = {{"hand", 1, {}}};
    const auto animation_id = hs::MakeAssetId("render/character/test_animation.rpkg");
    const auto animation_bytes = rd::BuildCharacterAnimationExtension(animation_id, {}, named);
    const auto animation_package = rd::ReadPackage(animation_bytes, animation_id);
    descriptor_bulks.push_back({animation_id, {8192, animation_bytes.size(), rd::RenderBulkFormat::Package, hs::Sha256(animation_bytes)}});
    auto compiled_animation = rd::CompileAnimationDescriptor(animation_bytes, animation_package,
        rd::DecodeCharacterAnimationExtension(animation_package), 1, descriptor_bulks, {0}, compiled_character);
    const std::array characters{compiled_character.View()};
    rd::ValidateAnimationDescriptor(compiled_animation.View(), descriptor_bulks, characters);
    const auto name_address = compiled_animation.View().clips[0].name.data();
    auto moved_animation = std::move(compiled_animation);
    Require(moved_animation.data.transforms.empty() && moved_animation.data.transforms.capacity() == 0 &&
        moved_animation.View().clips[0].name.data() == name_address && moved_animation.View().bone_names[1] == "child" &&
        moved_animation.View().transforms.byte_count == 160, "compiled animation move ownership or wire stride");
    auto wrong_binding = named; wrong_binding.inverse_bind_matrices[0][0] += 1;
    Reject([&] { (void)rd::CompileAnimationDescriptor(animation_bytes, animation_package, wrong_binding,
        1, descriptor_bulks, {0}, compiled_character); }, "compiled animation accepted wrong ordered bind matrices");
    auto bad_animation = moved_animation.View(); bad_animation.character = {99};
    Reject([&] { rd::ValidateAnimationDescriptor(bad_animation, descriptor_bulks, characters); },
        "compiled animation accepted dead character handle");
    rd::MonsterCharacterData monster;
    monster.bone_count = source.bone_count; monster.flags = rd::kMonsterBoundsPositionEnclosure;
    monster.skeleton_binding[0] = std::byte{1}; monster.numerical_margin = 0.001f;
    monster.lods = {gm}; monster.final_matrices.resize(source.transforms.size());
    for (auto &matrix : monster.final_matrices) matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1;
    for (const auto &clip : source.clips) for (std::uint32_t frame = 0; frame < clip.frame_count; ++frame)
        monster.interval_bounds.push_back({0, clip.clip, frame, gm.bounds.minimum, gm.bounds.maximum});
    const auto monster_bytes = rd::BuildCharacterMetadataPackage(asset, production_deps, source, gm, production_bindings, &monster);
    const auto monster_package = rd::ReadPackage(monster_bytes, asset);
    auto monster_bulks = descriptor_bulks;
    monster_bulks[0].range = {64, monster_bytes.size(), rd::RenderBulkFormat::Package, hs::Sha256(monster_bytes)};
    auto compiled_monster = rd::CompileCharacterDescriptor(monster_bytes, monster_package,
        rd::DecodeCharacterMetadataPackage(monster_package), 0, monster_bulks, descriptor_pages, descriptor_materials);
    const auto matrix_chunk = std::ranges::find(monster_package.chunks, rd::ChunkType::CharacterFinalMatrices, &rd::ChunkView::type);
    Require(compiled_monster.data.monster->final_matrices.empty() && compiled_monster.data.monster->final_matrices.capacity() == 0 &&
        compiled_monster.View().final_matrices.byte_count == monster.final_matrices.size() * 64 &&
        compiled_monster.View().final_matrices.expected_digest == hs::Sha256(matrix_chunk->bytes) &&
        compiled_monster.View().interval_bounds.size() == monster.interval_bounds.size() && !compiled_monster.View().transforms.byte_count,
        "compiled monster must retain bounds and authenticated matrix range only");
    std::vector<std::array<float, 16>> wire_matrices(monster.final_matrices.size());
    std::memcpy(wire_matrices.data(), matrix_chunk->bytes.data(), matrix_chunk->bytes.size());
    Require(wire_matrices == monster.final_matrices, "compiled monster matrix wire layout");
    compiled_monster.data.monster->lods[0].geometric_error += 1;
    Reject([&] { rd::ValidateCharacterDescriptor(compiled_monster.View(), monster_bulks, descriptor_pages, descriptor_materials); },
        "compiled monster accepted differing direct LOD metadata");
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
    const std::array translucent_character_dependencies{
        std::string("render/material/slime_body/material.rpkg")};
    const std::array translucent_character_binding{rd::CharacterMaterialBinding{
        0, 0, 5, rd::HeroMaterialDomain::Translucent}};
    const auto translucent_bindings = rd::EncodeCharacterMaterialBindings(
        translucent_character_binding, translucent_character_dependencies, 1);
    Require(rd::DecodeCharacterMaterialBindings(translucent_bindings,
        translucent_character_dependencies, 1)[0].domain == rd::HeroMaterialDomain::Translucent,
        "standalone translucent character slot was rejected");
    for(const auto&c:view.chunks)if(c.type!=rd::ChunkType::ExpandedVertices){auto found=std::ranges::find(production_view.chunks,c.type,&rd::ChunkView::type);Require(found!=production_view.chunks.end() && std::ranges::equal(found->bytes,c.bytes),"character skeleton scalar bytes changed");}
    Reject([&]{(void)rd::DecodeCharacterMetadataPackage(package,asset);},"production decoder accepted embedded package");
    Reject([&]{(void)rd::DecodeEmbeddedCharacterPackage(production,asset);},"embedded decoder accepted production package");
    auto invalid_skin=geometry;invalid_skin.vertices[0].bone_indices[0]=2;Reject([&]{rd::ValidateCharacterGeometry(character_metadata,invalid_skin);},"unique character bone range accepted");
    const auto bad_skin_page = rd::BuildGeometryPage(character_page_id, {}, invalid_skin);
    const auto prepared_bad_skin = rd::PrepareGeometryPage(bad_skin_page, character_page_id, gm);
    Reject([&] { rd::ValidateCharacterGeometry(character_metadata, prepared_bad_skin); },
           "prepared character bypassed bone range validation");
    invalid_skin = geometry;
    invalid_skin.vertices[0].bone_weights = {0, 0, 0, 0};
    const auto bad_weight_page = rd::BuildGeometryPage(character_page_id, {}, invalid_skin);
    const auto prepared_bad_weight = rd::PrepareGeometryPage(bad_weight_page, character_page_id, gm);
    Reject([&] { rd::ValidateCharacterGeometry(character_metadata, prepared_bad_weight); },
           "prepared character bypassed normalized skin validation");

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

void TestHeroWorkRawDecoding()
{
    hs::HeroWorkShaderLayout layout;
    Require(hs::HeroWorkMakeShaderLayout(1, 1, 1, layout) && layout.pixels == 1 && layout.bitmap_words == 1 &&
        layout.contributor_bytes == 512 && layout.phase_bytes == 6144 && layout.total_bytes == 135168,
        "hero two exact bitmaps/header/aligned stride/slot total");
    Require(hs::HeroWorkMakeShaderLayout(1280, 720, 3, layout) && layout.bitmap_words == 28800 &&
        layout.total_bytes <= hs::kHeroWorkShaderStorageLimit, "hero actual three-slot capture fits bounded storage");
    Require(!hs::HeroWorkMakeShaderLayout(0, 720, 3, layout) && !hs::HeroWorkMakeShaderLayout(1280, 0, 3, layout) &&
        !hs::HeroWorkMakeShaderLayout(1280, 720, 0, layout), "hero zero diagnostic dimensions/slots are invalid");
    Require(!hs::HeroWorkMakeShaderLayout(UINT32_MAX, 2, 1, layout) &&
        !hs::HeroWorkMakeShaderLayout(1, 1, UINT32_MAX, layout), "hero checked pixel/slot product cannot wrap");
    Require(!hs::HeroWorkMakeShaderLayout(3840, 2160, 3, layout), "hero exact two-bitmap 4K capture exceeds explicit 512MiB limit");

    std::uint64_t qpc = 123;
    Require(hs::HeroWorkCalibrate(1100, 1000, 5000, 100, 1000, qpc) && qpc == 6000,
        "hero queue timestamp after calibration anchor");
    Require(hs::HeroWorkCalibrate(900, 1000, 5000, 100, 1000, qpc) && qpc == 4000,
        "hero timestamp before anchor must not unsigned-wrap");
    Require(!hs::HeroWorkCalibrate(0, 1000, 100, 100, 1000, qpc) && qpc == 0,
        "hero negative QPC calibration must be unavailable");
    Require(!hs::HeroWorkCalibrate(1000, 1000, 5000, 0, 1000, qpc), "hero zero GPU frequency");
    Require(!hs::HeroWorkCalibrate(1000, 1000, 5000, 100, 0, qpc), "hero zero QPC frequency");
    Require(!hs::HeroWorkCalibrate(UINT64_MAX, 0, UINT64_MAX, 1, UINT64_MAX, qpc), "hero calibration overflow");
    const std::array<std::uint32_t, 6> indexed{7, 30, 4, 6, UINT32_MAX, 0};
    Require(hs::HeroWorkIndexedArgumentsValid(indexed, 36, 4), "hero actual indexed argument layout");
    auto invalid = indexed; invalid[1] = 37;
    Require(!hs::HeroWorkIndexedArgumentsValid(invalid, 36, 4), "hero index-count subtraction underflow");
    invalid = indexed; invalid[3] = 7;
    Require(!hs::HeroWorkIndexedArgumentsValid(invalid, 36, 4), "hero start-index range");
    invalid = indexed; invalid[2] = 5;
    Require(!hs::HeroWorkIndexedArgumentsValid(invalid, 36, 4), "hero visible instance range");
    invalid = indexed; invalid[2] = 0;
    Require(hs::HeroWorkIndexedArgumentsValid(invalid, 36, 4), "zero-instance ExecuteIndirect still has copied arguments");
    Require(hs::kHeroWorkStatisticsOffset + 14 * 8 == hs::kHeroWorkOcclusionOffset &&
        hs::kHeroWorkOcclusionOffset + 8 == hs::kHeroWorkRowBytes && hs::kHeroWorkRowBytes % 8 == 0,
        "hero native timestamp/statistics/occlusion readback spans overlap");
}

void TestHlodScenePreparation()
{
    auto storage = std::make_unique<hs::StaticRenderSceneStorage>();
    storage->content_generation_id = "scene-generation";
    storage->layout_abi = 2;
    rd::SceneHlod scene;
    scene.layout_abi = storage->layout_abi;
    for (std::uint32_t index = 0; index < 2; ++index)
    {
        hs::RenderInstance instance;
        instance.stable_id = 10 + index;
        instance.asset_id = hs::MakeAssetId(index ? "source/b" : "source/a");
        instance.mesh = hs::RenderMesh::Rock;
        instance.position = {float(index), 0, 0};
        storage->instances.push_back(instance);
        storage->eligible_static_members.push_back({instance.stable_id, 100 + index, index});
        rd::SceneHlodMember member;
        member.stable_id = instance.stable_id; member.object_id = 100 + index;
        member.asset_id = instance.asset_id; member.mesh_semantic = static_cast<std::uint32_t>(instance.mesh);
        member.variant = instance.environment_variant; member.tint_rgba = instance.color_rgba;
        member.position = instance.position; member.scale = instance.scale; member.yaw = instance.yaw;
        scene.members.push_back(member);
    }
    scene.layout_digest = rd::ComputeSceneHlodLayoutDigest(scene.layout_abi, scene.members);
    storage->eligible_layout_digest = scene.layout_digest;
    std::reverse(scene.members.begin(), scene.members.end());
    scene.dependencies = {"material/a", "material/b"};
    scene.pages.push_back({"proxy/page"});
    for (std::uint32_t index = 0; index < 2; ++index)
    {
        rd::SceneHlodProxy proxy;
        proxy.material_dependency = index;
        proxy.mesh_semantic = static_cast<std::uint32_t>(hs::RenderMesh::Rock);
        scene.proxies.push_back(proxy);
    }
    const auto source = hs::FreezeStaticRenderScene(std::move(storage));
    hs::HlodScenePreparation prepared;
    const std::array<std::uint32_t, 2> materials{3, 1};
    Require(prepared.Resolve(source, scene, "scene-generation", materials), "valid HLOD scene rejected");
    Require(prepared.member_slots == std::vector<std::uint32_t>{1, 0} &&
            prepared.proxy_order == std::vector<std::uint32_t>{1, 0},
            "HLOD preparation lost native member order or canonical material/proxy order");
    const auto *proxies = prepared.proxies.data();
    const auto *slots = prepared.member_slots.data();
    Require(prepared.Resolve(source, scene, "scene-generation", materials) &&
            proxies == prepared.proxies.data() && slots == prepared.member_slots.data(),
            "unchanged immutable scene must retain prepared proxy/member storage");
    auto bad = std::make_unique<hs::StaticRenderSceneStorage>(*source);
    bad->instances.front().asset_id = hs::MakeAssetId("wrong/geometry");
    Require(!prepared.Resolve(hs::FreezeStaticRenderScene(std::move(bad)), scene, "scene-generation", materials) &&
            prepared.source == source && prepared.proxies.data() == proxies,
            "asset mismatch must not publish a partially rebound HLOD preparation");
    bad = std::make_unique<hs::StaticRenderSceneStorage>(*source);
    bad->instances.front().position.x += 1;
    Require(!prepared.Resolve(hs::FreezeStaticRenderScene(std::move(bad)), scene, "scene-generation", materials),
            "copied digest must not authenticate changed instance geometry");
    bad = std::make_unique<hs::StaticRenderSceneStorage>(*source);
    bad->eligible_static_members[1] = bad->eligible_static_members[0];
    Require(!prepared.Resolve(hs::FreezeStaticRenderScene(std::move(bad)), scene, "scene-generation", materials),
            "duplicate HLOD member must not overwrite a prepared slot");
    const auto replacement = hs::FreezeStaticRenderScene(std::make_unique<hs::StaticRenderSceneStorage>(*source));
    Require(prepared.Resolve(replacement, scene, "scene-generation", materials) && prepared.source == replacement &&
            source->instances.size() == 2, "new scene owner must bind without retiring old packet storage");
}

void TestStaticShadowLayout()
{
    hs::StaticRenderSceneStorage scene;
    const auto current_layout = [&] {
        scene.BindExportedView();
        return rd::StaticShadowLayoutDigest(scene.exported_view);
    };
    hs::RenderInstance rock;
    rock.mesh = hs::RenderMesh::Rock;
    rock.position = {2, 1, -3};
    scene.instances.push_back(rock);
    const auto layout = current_layout();
    scene.content_generation_id = "different-generation";
    scene.instances[0].stable_id = 456;
    scene.instances[0].revisions.transform = 99;
    scene.instances[0].environment_seed = 123;
    Require(current_layout() == layout, "non-geometric identity changed shadow layout");
    auto grass = rock;
    grass.mesh = hs::RenderMesh::Grass;
    scene.instances.push_back(grass);
    Require(!rd::IsStaticShadowCaster(grass) && current_layout() == layout,
            "wind geometry changed static shadow layout");
    scene.instances[0].position.x += 1;
    Require(current_layout() != layout, "changed rock transform reused shadow layout");
    auto animated = rock;
    animated.animation_clip_id = hs::AssetId{1};
    Require(!rd::IsStaticShadowCaster(animated), "animated geometry entered static bake");
    animated = rock; animated.vfx_owner_id = 1;
    Require(!rd::IsStaticShadowCaster(animated), "VFX geometry entered static bake");
    for (std::uint32_t variant : {0u, 3u, 7u})
    {
        auto detail = rock;
        detail.mesh = hs::RenderMesh::GroundDetail;
        detail.environment_variant = variant;
        Require(!rd::IsStaticShadowCaster(detail), "wind-driven detail entered static bake");
    }
    scene.instances[0].mesh = hs::RenderMesh::Ground;
    const auto ground_layout = current_layout();
    ++scene.instances[0].environment_seed;
    Require(current_layout() == ground_layout, "opaque terrain seed invalidated static shadows");
}

void TestStaticShadowArchive()
{
    constexpr auto count = rd::kStaticShadowPageTexels;
    std::vector<float> clear(count), plane(count), cutout(count), noise(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto x = std::clamp<int>(int(i % rd::kStaticShadowPageTile) - 2, 0, 127);
        const auto y = std::clamp<int>(int(i / rd::kStaticShadowPageTile) - 2, 0, 127);
        plane[i] = .1f + float(x) * .003f + float(y) * .002f;
        cutout[i] = ((x / 8 + y / 8) % 5 == 0) ? 0.0f : plane[i];
    }
    plane[0] = -0.0f;
    plane[1] = std::numeric_limits<float>::denorm_min();
    std::uint32_t random = 0x12345678;
    for (auto &value : noise)
    {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        auto bits = random & 0x3fffffff;
        if (bits > 0x3f800000) bits -= 0x00800000;
        value = std::bit_cast<float>(bits);
    }
    std::vector<float> decoded(count + 2, -123.0f);
    const auto output = std::span(decoded).subspan(1, count);
    const auto roundtrip = [&](const std::vector<float> &page) {
        auto encoded = rd::EncodeStaticShadowPage(page);
        rd::DecodeStaticShadowPage(encoded, output);
        Require(std::ranges::equal(std::as_bytes(std::span(page)), std::as_bytes(output)) &&
                decoded.front() == -123.0f && decoded.back() == -123.0f,
                "shadow page words or guarded output changed");
        return encoded;
    };
    const auto empty = roundtrip(clear);
    Require(empty.empty(), "positive-zero shadow tile must be implicit clear");
    const auto compressed = roundtrip(plane);
    Require(!compressed.empty() && compressed[0] == std::byte{1}, "plane page should use lossless codec");
    const auto holes = roundtrip(cutout);
    const auto raw = roundtrip(noise);
    Require(raw.size() == count * 4 + 1 && raw[0] == std::byte{0}, "incompressible page needs bounded raw fallback");
    std::vector<float> negative_zero(count, -0.0f);
    Require(!roundtrip(negative_zero).empty(), "negative-zero words must not become implicit clear");
    Reject([&] { (void)rd::EncodeStaticShadowPage(std::span(plane).first(count - 1)); }, "short page encoded");
    auto invalid_depth = plane;
    invalid_depth[0] = std::numeric_limits<float>::quiet_NaN();
    Reject([&] { (void)rd::EncodeStaticShadowPage(invalid_depth); }, "nonfinite page encoded");
    invalid_depth[0] = 1.01f;
    Reject([&] { (void)rd::EncodeStaticShadowPage(invalid_depth); }, "out-of-range page encoded");
    Reject([&] { rd::DecodeStaticShadowPage(compressed, output.first(count - 1)); }, "short decode output accepted");
    for (const auto length : {std::size_t{1}, compressed.size() / 2, compressed.size() - 1})
        Reject([&] { rd::DecodeStaticShadowPage(std::span(compressed).first(length), output); }, "truncated codec accepted");
    Reject([&] { rd::DecodeStaticShadowPage(std::span(raw).first(raw.size() - 1), output); }, "truncated raw page accepted");
    auto malformed = compressed;
    malformed[0] = std::byte{2};
    Reject([&] { rd::DecodeStaticShadowPage(malformed, output); }, "unknown page codec accepted");
    malformed = raw;
    const auto nan_bits = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
    for (unsigned i = 0; i < 4; ++i) malformed[1 + i] = std::byte((nan_bits >> (i * 8)) & 255);
    Reject([&] { rd::DecodeStaticShadowPage(malformed, output); }, "nonfinite raw page decoded");
    Require(decoded.front() == -123.0f && decoded.back() == -123.0f, "failed decode overwrote output guards");

    rd::StaticShadowArchiveIndex index;
    auto &d = index.description;
    d.light_right = {1, 0, 0}; d.light_up = {0, 1, 0}; d.light_forward = {0, 0, 1};
    d.depth_minimum = -4; d.depth_maximum = 12;
    d.levels = {{{1, -1, -1, 1, 1}, {2, -2, -2, 1, 1}, {4, -1, 0, 3, 1}}};
    d.layout_digest = rd::Sha256(compressed);
    index.pages.resize(5);
    index.pages[4].bytes = UINT32_MAX;
    // Payload order follows baker chunks, independently of canonical directory slots.
    std::vector<std::byte> payload;
    const std::array encoded_pages{empty, compressed, holes, raw};
    for (std::size_t i = encoded_pages.size(); i-- > 0;)
    {
        if (encoded_pages[i].empty()) continue;
        auto &entry = index.pages[i];
        entry.offset = payload.size(); entry.bytes = static_cast<std::uint32_t>(encoded_pages[i].size());
        entry.digest = rd::Sha256(encoded_pages[i]);
        payload.insert(payload.end(), encoded_pages[i].begin(), encoded_pages[i].end());
    }
    index.payload_bytes = payload.size();
    const auto bytes = rd::BuildStaticShadowArchiveIndex(index);
    const auto read = rd::ReadStaticShadowArchiveIndex(bytes);
    Require(rd::BuildStaticShadowArchiveIndex(read) == bytes && read.pages[0].bytes == 0 &&
            read.pages[4].bytes == UINT32_MAX && read.pages[3].offset == 0 && read.pages[1].offset > read.pages[3].offset,
            "archive clear/missing page, page zero or reverse payload order changed");
    auto coarse_only = index;
    for (std::size_t level = 0; level < 2; ++level)
        coarse_only.description.levels[level].width = coarse_only.description.levels[level].height = 0;
    coarse_only.pages = {index.pages[0], index.pages[1], index.pages[3]};
    const auto coarse_bytes = rd::BuildStaticShadowArchiveIndex(coarse_only);
    const auto coarse_read = rd::ReadStaticShadowArchiveIndex(coarse_bytes);
    Require(coarse_read.pages.size() == 3 && coarse_read.description.levels[0].width == 0 &&
            coarse_read.description.levels[1].height == 0 && rd::BuildStaticShadowArchiveIndex(coarse_read) == coarse_bytes,
            "coarse-only archive must omit empty fine-level directories");
    coarse_only.description.levels[0].width = 1;
    Reject([&] { (void)rd::BuildStaticShadowArchiveIndex(coarse_only); }, "one-sided zero level extent accepted");
    for (std::size_t i = 0; i < encoded_pages.size(); ++i)
    {
        const auto &entry = read.pages[i];
        const auto encoded = std::span(payload).subspan(static_cast<std::size_t>(entry.offset), entry.bytes);
        Require(encoded.empty() || rd::Sha256(encoded) == entry.digest, "caller page authentication fixture");
        rd::DecodeStaticShadowPage(encoded, output);
        Require(std::ranges::equal(std::as_bytes(output), std::as_bytes(std::span(i == 0 ? clear : i == 1 ? plane : i == 2 ? cutout : noise))),
                "archive directory did not resolve the current page");
    }
    const auto put32 = [](std::vector<std::byte> &b, std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) b[at + i] = std::byte((value >> (i * 8)) & 255);
    };
    Reject([&] { (void)rd::ReadStaticShadowArchiveIndex(std::span(bytes).first(80)); }, "short archive header accepted");
    Reject([&] { (void)rd::ReadStaticShadowArchiveIndex(std::span(bytes).first(bytes.size() - 1)); }, "short archive directory accepted");
    for (const std::size_t at : {0u, 8u, 16u, 24u, 28u, 92u, 188u, 256u + 12u, 256u + 16u})
    {
        auto bad = bytes; bad[at] ^= std::byte{0xff};
        Reject([&] { (void)rd::ReadStaticShadowArchiveIndex(bad); }, "invalid archive header/record accepted");
    }
    for (const auto [at, value] : std::array{
            std::pair{48u, std::bit_cast<std::uint32_t>(2.0f)},
            std::pair{60u, std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity())},
            std::pair{64u, 0u}, std::pair{88u, std::bit_cast<std::uint32_t>(-4.0f)},
            std::pair{96u, 0u}, std::pair{108u, UINT32_MAX}, std::pair{100u, std::uint32_t(INT32_MAX)}})
    {
        auto bad = bytes; put32(bad, at, value);
        if (at == 108) put32(bad, 112, 2);
        if (at == 100) put32(bad, 108, 2);
        Reject([&] { (void)rd::ReadStaticShadowArchiveIndex(bad); }, "invalid basis/depth/level span/count accepted");
    }
    auto bad = bytes;
    std::fill(bad.begin() + 156, bad.begin() + 188, std::byte{});
    Reject([&] { (void)rd::ReadStaticShadowArchiveIndex(bad); }, "missing archive layout identity accepted");
    bad = bytes; Put64(bad, 40, 6);
    Reject([&] { (void)rd::ReadStaticShadowArchiveIndex(bad); }, "archive count disagreement accepted");
    bad = bytes; Put64(bad, 256 + 48, UINT64_MAX);
    Reject([&] { (void)rd::ReadStaticShadowArchiveIndex(bad); }, "overflowing archive page offset accepted");
    bad = bytes; put32(bad, 256 + 48 + 8, count * 4 + 2);
    Reject([&] { (void)rd::ReadStaticShadowArchiveIndex(bad); }, "oversized archive page accepted");
    bad = bytes; std::fill(bad.begin() + 256 + 48 + 16, bad.begin() + 256 + 96, std::byte{});
    Reject([&] { (void)rd::ReadStaticShadowArchiveIndex(bad); }, "missing encoded-page digest accepted");
    bad = bytes; Put64(bad, 256 + 4 * 48, 1);
    Reject([&] { (void)rd::ReadStaticShadowArchiveIndex(bad); }, "missing page with payload offset accepted");
    auto invalid_index = index; invalid_index.pages[0].offset = 1;
    Reject([&] { (void)rd::BuildStaticShadowArchiveIndex(invalid_index); }, "producer emitted malformed clear page");
    invalid_index = index; invalid_index.description.light_up = d.light_right;
    Reject([&] { (void)rd::BuildStaticShadowArchiveIndex(invalid_index); }, "nonorthogonal archive basis accepted");
    invalid_index = index;
    invalid_index.description.levels[0].minimum_x = INT32_MIN;
    invalid_index.description.levels[0].page_world_size = std::numeric_limits<float>::max();
    Reject([&] { (void)rd::BuildStaticShadowArchiveIndex(invalid_index); }, "unbounded level world span accepted");
}

void TestDdsConsumerRoles()
{
    // Independent wire fixtures: no production DDS writer constructs these headers.
    const auto put = [](std::vector<std::byte> &bytes, std::size_t offset, std::uint32_t value) {
        for (std::size_t index = 0; index < 4; ++index) bytes[offset + index] = std::byte((value >> (8 * index)) & 255);
    };
    const auto legacy = [&](std::uint32_t side, std::uint32_t mips) {
        std::size_t size = 128;
        for (std::uint32_t mip = 0; mip < mips; ++mip) size += std::size_t(std::max(1u, side >> mip)) * std::max(1u, side >> mip) * 4;
        std::vector<std::byte> bytes(size);
        for (const auto [offset, value] : std::array<std::pair<std::size_t, std::uint32_t>, 15>{{
            {0, 0x20534444}, {4, 124}, {8, mips > 1 ? 0x2100fu : 0x100fu}, {12, side}, {16, side}, {20, side * 4},
            {28, mips}, {76, 32}, {80, 65}, {88, 32}, {92, 255}, {96, 0xff00}, {100, 0xff0000}, {104, 0xff000000},
            {108, mips > 1 ? 0x401008u : 0x1000u}}}) put(bytes, offset, value);
        return bytes;
    };
    for (const auto [side, mips] : std::array{std::pair{2048u, 1u}, std::pair{1024u, 11u}})
    {
        auto bytes = legacy(side, mips);
        const auto view = rd::DecodeDdsTexture(bytes);
        const rd::TextureDescription spec{side, side, 1, mips, rd::kTextureFormatRgba8};
        rd::ValidateDdsTextureRole(view, rd::DdsTextureRole::CharacterLegacy, spec);
        Require(view.subresources.size() == mips, "legacy Archer1/Slime11 mip count rejected");
        auto wrong = spec; wrong.mip_count = mips == 1 ? 2 : 1;
        Reject([&] { rd::ValidateDdsTextureRole(view, rd::DdsTextureRole::CharacterLegacy, wrong); }, "legacy spec mip mismatch accepted");
        Reject([&] { rd::ValidateDdsTextureRole(view, rd::DdsTextureRole::CharacterFinalBc7, spec); }, "legacy proof substituted for final BC7 role");
        for (const auto offset : {8u, 20u, 24u, 32u, 108u, 112u})
        {
            auto malformed = bytes; put(malformed, offset, 7);
            Reject([&] { rd::ValidateDdsTextureRole(rd::DecodeDdsTexture(malformed), rd::DdsTextureRole::CharacterLegacy, spec); },
                "legacy flags/pitch/depth/reserved/caps accepted");
        }
        bytes.pop_back();
        Reject([&] { (void)rd::DecodeDdsTexture(bytes); }, "truncated legacy last mip accepted");
    }
    // 7x5, 3x2, 1x1: BC7 tails stay one whole 16-byte block per slice.
    const rd::TextureDescription shape{7, 5, 2, 3, rd::kTextureFormatBc7Srgb};
    std::vector<std::byte> bc7(148 + 192);
    for (const auto [offset, value] : std::array<std::pair<std::size_t, std::uint32_t>, 13>{{
        {0, 0x20534444}, {4, 124}, {8, 0xa1007}, {12, 5}, {16, 7}, {20, 64}, {28, 3}, {76, 32},
        {80, 4}, {84, 0x30315844}, {108, 0x401008}, {128, rd::kTextureFormatBc7Srgb}, {132, 3}}}) put(bc7, offset, value);
    put(bc7, 140, 2);
    for (std::size_t index = 148; index < bc7.size(); ++index) bc7[index] = std::byte(index & 255);
    const auto view = rd::DecodeDdsTexture(bc7);
    rd::ValidateDdsTextureRole(view, rd::DdsTextureRole::CharacterFinalBc7, shape);
    Require(view.subresources.size() == 6 && view.subresources[0].row_pitch == 32 &&
        view.subresources[0].slice_pitch == 64 && view.subresources[1].row_pitch == 16 &&
        view.subresources.back().bytes.size() == 16 && view.subresources.back().bytes.data() == bc7.data() + 324,
        "BC7 odd extent/array/tail partition changed");
    // CPU padded-row copy checks sentinels. Actual native footprints require the native renderer gate.
    std::array<std::byte, 512> padded; padded.fill(std::byte{0x5a});
    for (std::size_t row = 0; row < 2; ++row)
        std::memcpy(padded.data() + row * 256, view.subresources[0].bytes.data() + row * 32, 32);
    Require(std::equal(padded.begin(), padded.begin() + 32, view.subresources[0].bytes.begin()) &&
        padded[32] == std::byte{0x5a} && padded[255] == std::byte{0x5a} && padded[288] == std::byte{0x5a},
        "BC block rows overwrote native-style padding");
    for (const auto offset : {20u, 32u, 88u, 112u, 132u, 136u, 140u, 144u})
    {
        auto malformed = bc7; put(malformed, offset, 7);
        Reject([&] { rd::ValidateDdsTextureRole(rd::DecodeDdsTexture(malformed), rd::DdsTextureRole::CharacterFinalBc7, shape); },
            "BC7 role accepted invalid pitch/reserved/dimension/array flags");
    }
    auto wrong = shape; wrong.format = rd::kTextureFormatBc7;
    Reject([&] { rd::ValidateDdsTextureRole(view, rd::DdsTextureRole::CharacterFinalBc7, wrong); }, "BC7 color/data role format changed");
    auto trailing = bc7; trailing.push_back(std::byte{});
    Reject([&] { (void)rd::DecodeDdsTexture(trailing); }, "DDS trailing payload accepted");
    Reject([&] { (void)rd::DecodeDdsTexture(std::span(bc7).first(147)); }, "truncated DX10 header accepted");
    auto environment = bc7; put(environment, 24, 1);
    rd::ValidateDdsTextureRole(rd::DecodeDdsTexture(environment), rd::DdsTextureRole::Environment, shape);
    Reject([&] { rd::ValidateDdsTextureRole(view, rd::DdsTextureRole::Environment, shape); },
        "character non-volume depth rule weakened the independent environment role");
    auto rgba = bc7; rgba.resize(148 + 168); put(rgba, 8, 0x2100f); put(rgba, 12, 4); put(rgba, 16, 4);
    put(rgba, 20, 16); put(rgba, 128, rd::kTextureFormatRgba8);
    const rd::TextureDescription vfx{4, 4, 2, 3, rd::kTextureFormatRgba8};
    rd::ValidateDdsTextureRole(rd::DecodeDdsTexture(rgba), rd::DdsTextureRole::VfxTexture, vfx);
    put(rgba, 8, 0xa1007);
    Reject([&] { rd::ValidateDdsTextureRole(rd::DecodeDdsTexture(rgba), rd::DdsTextureRole::VfxTexture, vfx); },
        "VFX uncompressed texture accepted BC flags");
    auto volume = rgba; volume.resize(148 + 64 * 64 * 64 * 4); put(volume, 8, 0x80100f);
    put(volume, 12, 64); put(volume, 16, 64); put(volume, 20, 256); put(volume, 24, 64); put(volume, 28, 1);
    put(volume, 108, 0x1008); put(volume, 112, 0x200000); put(volume, 132, 4); put(volume, 140, 1);
    const rd::TextureDescription volume_spec{64, 64, 1, 1, rd::kTextureFormatRgba8};
    const auto volume_view = rd::DecodeDdsTexture(volume);
    rd::ValidateDdsTextureRole(volume_view, rd::DdsTextureRole::VfxVolume, volume_spec);
    Require(volume_view.subresources.size() == 1 && volume_view.subresources[0].depth == 64 &&
        volume_view.subresources[0].slice_pitch == 16384 && volume_view.payload.size() == 1048576,
        "DDS volume depth slices were mistaken for array layers");
    Reject([&] { rd::ValidateDdsTextureRole(volume_view, rd::DdsTextureRole::VfxTexture, volume_spec); },
        "VFX volume role substituted for 2D texture");
}

void TestCharacterSemanticMips()
{
    using namespace hs::content;
    CharacterMip checker{2, 2, {{{0, 0, 0, 0}}, {{1, 1, 1, 1}}, {{1, 1, 1, 1}}, {{0, 0, 0, 0}}}};
    const auto color = BuildCharacterMipChain(checker, CharacterTextureSemantic::Color);
    Require(color.size() == 2 && std::abs(color[1].pixels[0][0] - 0.735356983f) < 1e-6f &&
        color[1].pixels[0][3] == 0.5f, "color RGB was averaged in sRGB or alpha was gamma encoded");
    const auto data = BuildCharacterMipChain(checker, CharacterTextureSemantic::LinearData);
    Require(data[1].pixels[0][0] == 0.5f && data[1].pixels[0][3] == 0.5f, "linear channels changed meaning");
    CharacterMip normals{2, 2, {{{1, 0.5f, 0.5f, 0.2f}}, {{0.5f, 1, 0.5f, 0.4f}},
                               {{1, 0.5f, 0.5f, 0.6f}}, {{0.5f, 1, 0.5f, 0.8f}}}};
    const auto normal = BuildCharacterMipChain(normals, CharacterTextureSemantic::NormalRoughness);
    Require(std::abs(normal[1].pixels[0][0] - 0.853553391f) < 1e-6f &&
        std::abs(normal[1].pixels[0][1] - 0.853553391f) < 1e-6f && normal[1].pixels[0][2] == 0.5f &&
        std::abs(normal[1].pixels[0][3] - 0.5f) < 1e-6f, "normal was not renormalized separately from roughness alpha");
    CharacterMip unequal{2, 2, {{{0.625f, 0.5f, 0.5f, 0.2f}}, {{0.5f, 1, 0.5f, 0.4f}},
                              {{0.625f, 0.5f, 0.5f, 0.6f}}, {{0.5f, 1, 0.5f, 0.8f}}}};
    auto canonical = BuildCharacterMipChain(unequal, CharacterTextureSemantic::NormalRoughness);
    const auto previous_lower = canonical[1].pixels;
    CanonicalizeCharacterNormalBase(canonical.front());
    Require(canonical[0].pixels[0][0] == 1 && canonical[0].pixels[0][1] == 0.5f &&
        canonical[0].pixels[0][3] == unequal.pixels[0][3] && canonical[1].pixels == previous_lower &&
        std::abs(canonical[1].pixels[0][0] - 0.6212678125f) < 1e-6f,
        "base normalization changed roughness or reweighted the existing lower-mip normals");
    CharacterMip bow{1, 1, {{{145.0f / 255, 114.0f / 255, 156.0f / 255, 0.37f}}}};
    const auto original_bow = bow.pixels.front();
    (void)CanonicalizeCharacterNormalBase(bow);
    // Same linear TBN operation followed by normalize as the actual shader,
    // including a non-orthonormal test basis. Positive magnitude must cancel.
    const auto transformed_unit = [](CharacterTexel pixel) {
        constexpr double basis[3][3]{{0.8, 0.1, -0.2}, {0.2, 0.9, 0.1}, {0.1, -0.3, 1.1}};
        std::array<double, 3> result{}; double length{};
        for (unsigned row = 0; row < 3; ++row)
        { for (unsigned column = 0; column < 3; ++column) result[row] += basis[row][column] * (double(pixel[column]) * 2 - 1); length += result[row] * result[row]; }
        for (auto &value : result) value /= std::sqrt(length);
        return result;
    };
    const auto original_direction = transformed_unit(original_bow), canonical_direction = transformed_unit(bow.pixels.front());
    for (unsigned channel = 0; channel < 3; ++channel)
        Require(std::abs(original_direction[channel] - canonical_direction[channel]) < 1e-6,
            "Bow base canonicalization changed point-sampled shader normal direction");
    Require(bow.pixels.front()[3] == original_bow[3], "normal canonicalization changed roughness alpha");
    CharacterMip zero_normal{1, 1, {{{0.5f, 0.5f, 0.5f, 0.7f}}}};
    Reject([&] { (void)CanonicalizeCharacterNormalBase(zero_normal); }, "zero authored normal was silently replaced by another direction");
    CharacterNormalBlock reference_block{},bad_block{};
    for(auto &pixel:reference_block) pixel={0.5f,0.5f,1,0.37f};
    reference_block[0]={0.8395207524f,0.1525834799f,0.618437469f,1};
    bad_block=reference_block;bad_block[0]={0.5725490451f,0.5529412031f,0.3882353306f,1};
    const auto failed_score=ScoreCharacterNormalBlock(reference_block,bad_block);
    Require(!failed_score.valid&&failed_score.invalid_texels==1,
        "real Bow mip1 opposite-hemisphere candidate passed the unchanged normal gate");
    const auto exact_score=ScoreCharacterNormalBlock(reference_block,reference_block);
    Require(exact_score.valid&&PreferCharacterNormalBlock(exact_score,failed_score),
        "valid normal candidate did not outrank a lower scalar-error invalid candidate");
    auto weaker=reference_block;weaker[0]={0.5f,0.5f,1,1};
    const auto weaker_score=ScoreCharacterNormalBlock(reference_block,weaker);
    Require(weaker_score.valid&&PreferCharacterNormalBlock(exact_score,weaker_score),
        "normal candidates were not ranked by actual maximum angular error");
    auto roughness_changed=reference_block;roughness_changed[7][3]=0.5f;
    Require(!CharacterNormalAlphaErrorNotWorse(reference_block,bad_block,roughness_changed)&&
        CharacterNormalAlphaErrorNotWorse(reference_block,bad_block,reference_block),
        "normal repair silently increased a texel's roughness-alpha error");
    auto zero_candidate=reference_block;zero_candidate[8]={0.5f,0.5f,0.5f,0.37f};
    const auto zero_score=ScoreCharacterNormalBlock(reference_block,zero_candidate);
    Require(!zero_score.valid&&zero_score.invalid_texels==1,
        "normal repair candidate skipped a non-failing neighbor texel in its 4x4 block");
    auto placeholder = CharacterConstantSlice(8, 8, {128, 128, 255, 255});
    const auto flat = BuildCharacterMipChain(placeholder, CharacterTextureSemantic::NormalRoughness);
    Require(flat.size() == 4 && flat.front().pixels.size() == 64 && flat.back().width == 1 &&
        flat.back().pixels[0][3] == 1, "placeholder was not expanded before building its final mip chain");
    CharacterMip mask{4, 4, std::vector<CharacterTexel>(16, {1, 1, 1, 0})};
    for (const auto index : {0u, 2u, 8u, 10u}) mask.pixels[index][3] = 1;
    const auto masked = BuildCharacterMipChain(mask, CharacterTextureSemantic::Color, {0.2f, false});
    Require(std::ranges::count_if(masked[1].pixels, [](const auto &pixel) { return pixel[3] >= 0.2f; }) == 1,
        "alpha coverage changed beyond its nearest representable mip texel count");
    auto body = CharacterConstantSlice(4, 4, {128, 128, 128, 173});
    const auto translucent = BuildCharacterMipChain(body, CharacterTextureSemantic::Color, {0.98f, true});
    for (const auto &mip : translucent)
        for (const auto &pixel : mip.pixels)
            Require(std::abs(pixel[3] - 173.0f / 255.0f) < 1e-6f, "coverage correction made homogeneous slime body opaque");
    auto invalid = checker; invalid.pixels[0][3] = std::numeric_limits<float>::quiet_NaN();
    Reject([&] { (void)BuildCharacterMipChain(invalid, CharacterTextureSemantic::Color); }, "nonfinite source channel accepted");
}

void TestCharacterBc7CoverageRepair()
{
    using namespace hs::content;
    // Independent mode5 alpha-field decoding from the fixed BC7 bit layout.
    const auto alpha_bytes = [](const CharacterBc7Block &block) {
        const auto bits = [&](unsigned offset, unsigned count) {
            unsigned value{};
            for (unsigned bit = 0; bit < count; ++bit)
                value |= unsigned((block[(offset + bit) / 8] >> ((offset + bit) % 8)) & 1u) << bit;
            return value;
        };
        Require(bits(0, 6) == 0x20 && bits(6, 2) == 0, "repair did not produce unrotated BC7 mode5");
        const auto a = bits(50, 8), b = bits(58, 8);
        constexpr std::array<unsigned, 4> interpolation{0, 21, 43, 64};
        std::array<unsigned, 16> result{};
        unsigned offset = 97;
        for (unsigned pixel = 0; pixel < 16; ++pixel)
        {
            const auto count = pixel ? 2u : 1u;
            const auto index = bits(offset, count); offset += count;
            result[pixel] = ((64 - interpolation[index]) * a + interpolation[index] * b + 32) / 64;
        }
        return result;
    };
    CharacterBc7Texels source{};
    for (auto &pixel : source) pixel = {0, 0, 0, 1};
    CharacterBc7Block original{}, encoded{};
    Require(EncodeCharacterBc7CoverageBlock(source, {0.2f, false}, original, encoded),
        "mode5 constant block repair failed");
    CharacterBc7Block golden{}; golden[0] = 0x20; golden[6] = 0xfc; golden[7] = 0xff; golden[8] = 0x03;
    Require(encoded == golden, "BC7 mode5 constant black/opaque golden packing differs");
    for (const auto alpha : alpha_bytes(encoded)) Require(alpha == 255, "mode5 opaque anchor changed");

    for (unsigned pixel = 0; pixel < 16; ++pixel)
        source[pixel] = {0.25f, 0.5f, 0.75f, pixel % 2 ? 173.0f / 255.0f : 1.0f};
    Require(EncodeCharacterBc7CoverageBlock(source, {0.98f, true}, original, encoded),
        "mode5 real slime body/opaque anchors failed");
    auto decoded = alpha_bytes(encoded);
    for (unsigned pixel = 0; pixel < 16; ++pixel)
        Require(decoded[pixel] == (pixel % 2 ? 173u : 255u), "mode5 anchor or first-index endpoint swap changed alpha");

    original.fill(0xa5); original[0] = 0x20;
    Require(EncodeCharacterBc7CoverageBlock(source, {0.98f, true}, original, encoded),
        "existing unrotated mode5 RGB was needlessly recompressed");
    for (unsigned bit = 0; bit < 128; ++bit)
        if (bit < 50 || (bit >= 66 && bit < 97))
            Require(((original[bit / 8] >> (bit % 8)) & 1u) == ((encoded[bit / 8] >> (bit % 8)) & 1u),
                "alpha repair modified original mode5 RGB/header bits");

    // Two semantic float decisions can collapse to the same R8 input value.
    // The repair must preserve those original decisions, not rewrite the reference.
    for (unsigned pixel = 0; pixel < 16; ++pixel)
        source[pixel][3] = pixel % 2 ? std::nextafter(0.2f, 0.0f) : 0.2f;
    Require(EncodeCharacterBc7CoverageBlock(source, {0.2f, false}, original, encoded),
        "R8 threshold crossing was not repaired against semantic float mip");
    decoded = alpha_bytes(encoded);
    for (unsigned pixel = 0; pixel < 16; ++pixel)
        Require((float(decoded[pixel]) / 255.0f >= 0.2f) == (source[pixel][3] >= 0.2f),
            "mode5 changed semantic float cutoff mask after R8 quantization");

    source[0][3] = 0; source[1][3] = 173.0f / 255.0f; source[2][3] = 1;
    Require(!EncodeCharacterBc7CoverageBlock(source, {0.98f, true}, original, encoded),
        "mode5 pretended that zero/body173/opaque255 are always simultaneously representable");
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

    const auto check_descriptor = [&](std::span<const std::byte> bytes, const rd::TexturePackageView &parsed) {
        const auto compiled = rd::CompileTextureDescriptor(bytes, asset, 7);
        const auto descriptor = compiled.View();
        const auto directory = rd::ReadPackageDirectory(bytes.first(std::min<std::size_t>(bytes.size(), 16 * 1024)),
            bytes.size(), asset);
        const auto &metadata = directory.chunks.front();
        const auto proof = rd::CompileTextureMetadataDescriptor(directory,
            bytes.subspan(static_cast<std::size_t>(metadata.offset), static_cast<std::size_t>(metadata.byte_count)),
            7, bytes.size());
        Require(proof.description.width == compiled.description.width &&
            proof.description.format == compiled.description.format &&
            proof.subresources.size() == compiled.subresources.size() &&
            proof.subresources.front().offset == compiled.subresources.front().offset &&
            proof.subresources.front().digest == compiled.subresources.front().digest,
            "bounded texture metadata proof differs from full admission");
        rd::ValidateTextureDescriptor(descriptor, bytes.size());
        const auto bound = rd::BindTextureDescriptor(bytes, descriptor);
        Require(descriptor.asset_id.value == asset.value && descriptor.bulk_index == 7 &&
                descriptor.dimension == rd::TextureDimension::Texture2DArray && descriptor.depth == 1 &&
                descriptor.dependencies.size() == 1 && descriptor.dependencies[0].value == hs::MakeAssetId(dependencies[0]).value &&
                bound.dependencies.empty() && bound.subresources.size() == parsed.subresources.size() &&
                bound.description.width == parsed.description.width && bound.description.height == parsed.description.height &&
                bound.description.array_size == parsed.description.array_size &&
                bound.description.mip_count == parsed.description.mip_count && bound.description.format == parsed.description.format,
                "compiled texture descriptor metadata differs from parsed texture");
        for (std::size_t i = 0; i < bound.subresources.size(); ++i)
        {
            const auto &a = bound.subresources[i];
            const auto &b = parsed.subresources[i];
            Require(a.array_slice == b.array_slice && a.mip_level == b.mip_level && a.width == b.width &&
                    a.height == b.height && a.row_pitch == b.row_pitch && a.slice_pitch == b.slice_pitch &&
                    a.bytes.data() == b.bytes.data() && a.bytes.size() == b.bytes.size() &&
                    descriptor.subresources[i].byte_count == b.slice_pitch &&
                    descriptor.subresources[i].digest == hs::Sha256(b.bytes),
                    "compiled texture binding changed layout or copied payload bytes");
        }
    };
    check_descriptor(package, decoded);

    const auto max_records_asset = hs::MakeAssetId("render/texture/max-metadata.rpkg");
    const rd::TextureDescription max_records_description{2, 2, 2048, 2, rd::kTextureFormatR8};
    std::vector<std::byte> max_records_payload(2048u * (4u + 1u));
    const auto max_records_package = rd::BuildTexturePackage(max_records_asset, dependencies,
        max_records_description, max_records_payload);
    const auto max_records_directory = rd::ReadPackageDirectory(
        std::span(max_records_package).first(std::min<std::size_t>(max_records_package.size(), 16 * 1024)),
        max_records_package.size(), max_records_asset);
    const auto &max_records_metadata = max_records_directory.chunks.front();
    const auto max_records_full = rd::CompileTextureDescriptor(max_records_package, max_records_asset, 9);
    const auto max_records_proof = rd::CompileTextureMetadataDescriptor(max_records_directory,
        std::span(max_records_package).subspan(static_cast<std::size_t>(max_records_metadata.offset),
            static_cast<std::size_t>(max_records_metadata.byte_count)), 9, max_records_package.size());
    const auto full_view = max_records_full.View();
    const auto proof_view = max_records_proof.View();
    Require(max_records_metadata.type == rd::ChunkType::TextureMetadata && max_records_metadata.version == 2 &&
            max_records_metadata.byte_count == rd::kMaxTextureMetadataBytes &&
            max_records_directory.chunks.size() == 2 &&
            full_view.asset_id.value == proof_view.asset_id.value &&
            full_view.bulk_index == proof_view.bulk_index &&
            full_view.description.width == proof_view.description.width &&
            full_view.description.height == proof_view.description.height &&
            full_view.description.array_size == proof_view.description.array_size &&
            full_view.description.mip_count == proof_view.description.mip_count &&
            full_view.description.format == proof_view.description.format &&
            full_view.dimension == proof_view.dimension && full_view.depth == proof_view.depth &&
            full_view.dependencies.size() == proof_view.dependencies.size() &&
            full_view.subresources.size() == rd::kMaxTextureSubresources &&
            proof_view.subresources.size() == full_view.subresources.size(),
            "maximum-size TXM1 v2 metadata proof shape differs from full descriptor");
    for (std::size_t i = 0; i < full_view.dependencies.size(); ++i)
        Require(full_view.dependencies[i].value == proof_view.dependencies[i].value,
                "maximum-size TXM1 v2 metadata proof dependencies differ from full descriptor");
    for (std::size_t i = 0; i < full_view.subresources.size(); ++i)
    {
        const auto &full = full_view.subresources[i], &proof = proof_view.subresources[i];
        Require(full.offset == proof.offset && full.row_pitch == proof.row_pitch &&
                full.slice_pitch == proof.slice_pitch && full.byte_count == proof.byte_count &&
                full.digest == proof.digest && full.has_digest == proof.has_digest,
                "maximum-size TXM1 v2 metadata proof differs from full descriptor layout");
    }

    const auto compiled = rd::CompileTextureDescriptor(package, asset, 7);
    const auto payload_end = compiled.subresources.back().offset + compiled.subresources.back().slice_pitch;
    Reject([&] { rd::ValidateTextureDescriptor(compiled.View(), payload_end - 1); },
           "texture descriptor accepted truncated product extent");
    Reject([&] { (void)rd::BindTextureDescriptor(std::span(package).first(static_cast<std::size_t>(payload_end - 1)), compiled.View()); },
           "texture descriptor binding accepted truncated bytes");
    auto invalid_descriptor = compiled;
    invalid_descriptor.subresources[0].offset = UINT64_MAX;
    Reject([&] { rd::ValidateTextureDescriptor(invalid_descriptor.View(), package.size()); },
           "texture descriptor accepted overflowing offset");
    Reject([&] { (void)rd::BindTextureDescriptor(package, invalid_descriptor.View()); },
           "texture binding accepted overflowing offset");
    invalid_descriptor = compiled;
    --invalid_descriptor.subresources[0].byte_count;
    Reject([&] { rd::ValidateTextureDescriptor(invalid_descriptor.View(), package.size()); },
           "2D descriptor accepted incorrect full byte count");
    invalid_descriptor = compiled;
    ++invalid_descriptor.subresources[0].row_pitch;
    Reject([&] { rd::ValidateTextureDescriptor(invalid_descriptor.View(), package.size()); },
           "texture descriptor accepted invalid pitch");
    invalid_descriptor = compiled;
    ++invalid_descriptor.subresources[1].offset;
    Reject([&] { rd::ValidateTextureDescriptor(invalid_descriptor.View(), package.size()); },
           "texture descriptor accepted noncontiguous subresources");
    invalid_descriptor = compiled;
    invalid_descriptor.description.mip_count = 0;
    Reject([&] { rd::ValidateTextureDescriptor(invalid_descriptor.View(), package.size()); },
           "texture descriptor accepted invalid mip count");
    invalid_descriptor = compiled;
    invalid_descriptor.description.width = UINT32_MAX;
    Reject([&] { rd::ValidateTextureDescriptor(invalid_descriptor.View(), package.size()); },
           "texture descriptor accepted invalid dimensions");
    invalid_descriptor = compiled;
    invalid_descriptor.description.format = UINT32_MAX;
    Reject([&] { rd::ValidateTextureDescriptor(invalid_descriptor.View(), package.size()); },
           "texture descriptor accepted unknown format");
    invalid_descriptor = compiled;
    invalid_descriptor.subresources.pop_back();
    Reject([&] { rd::ValidateTextureDescriptor(invalid_descriptor.View(), package.size()); },
           "texture descriptor accepted incorrect subresource count");
    auto authenticated_bytes = package;
    authenticated_bytes[0] ^= std::byte{1}; // Binding uses admitted metadata, not the package header.
    const auto rebound = rd::BindTextureDescriptor(authenticated_bytes, compiled.View());
    const auto first_offset = static_cast<std::size_t>(compiled.subresources[0].offset);
    authenticated_bytes[first_offset] ^= std::byte{1};
    Require(rebound.subresources[0].bytes.data() == authenticated_bytes.data() + first_offset &&
            rebound.subresources[0].bytes[0] == authenticated_bytes[first_offset],
            "bound texture did not retain caller byte aliases");
    Reject([&] { (void)rd::BindTextureDescriptor(authenticated_bytes, compiled.View()); },
           "texture binding accepted a subresource with the wrong range digest");

    const auto base = rd::ReadPackage(package, asset);
    auto metadata = std::vector<std::byte>(base.chunks[0].bytes.begin(),
                                           base.chunks[0].bytes.end());
    auto wrong_digest = metadata;
    wrong_digest[40 + 16] ^= std::byte{1};
    const std::array wrong_digest_chunks{
        rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::TextureMetadata), 2, true,
                        wrong_digest, 1, static_cast<std::uint32_t>(wrong_digest.size())},
        rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::TextureSubresources), 1, true,
                        base.chunks[1].bytes, payload.size() / 16, 16}};
    const auto wrong_digest_package = rd::BuildPackage(asset, dependencies, wrong_digest_chunks);
    Reject([&] { (void)rd::ReadTexturePackage(wrong_digest_package, asset); },
           "texture package accepted a metadata subresource digest mismatch");
    metadata[40] = std::byte{1}; // First subresource must begin at payload offset zero.
    const std::array malformed{
        rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::TextureMetadata),
                        2, true, metadata, 1, static_cast<std::uint32_t>(metadata.size())},
        rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::TextureSubresources),
                        1, true, base.chunks[1].bytes, payload.size() / 16, 16}};
    const auto bad_offset = rd::BuildPackage(asset, dependencies, malformed);
    Reject([&] { (void)rd::ReadTexturePackage(bad_offset, asset); },
           "invalid texture subresource offset accepted");
    Reject([&] { (void)rd::CompileTextureDescriptor(bad_offset, asset, 7); },
           "descriptor compilation bypassed malformed texture validation");
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
        check_descriptor(encoded, decoded_variant);
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
        check_descriptor(tiny_package, tiny_view);
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
        check_descriptor(mip_package, mip_view);
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
void TestVolumeTextureDescriptor()
{
    const auto asset = hs::MakeAssetId("render/texture/volume.rpkg");
    const std::vector<std::string> dependencies{"environment/volume.raw"};
    const rd::VolumeTextureDescription description{8, 4, 4, 4, rd::kTextureFormatRgba8};
    std::vector<std::byte> payload(588);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = std::byte(i & 0xff);
    const auto bytes = rd::BuildVolumeTexturePackage(asset, dependencies, description, payload);
    const auto parsed = rd::ReadVolumeTexturePackage(bytes, asset);
    const auto compiled = rd::CompileTextureDescriptor(bytes, asset, 11);
    const auto directory = rd::ReadPackageDirectory(std::span(bytes).first(std::min<std::size_t>(bytes.size(), 16 * 1024)),
        bytes.size(), asset);
    const auto &metadata_chunk = directory.chunks.front();
    const auto proof = rd::CompileVolumeTextureMetadataDescriptor(directory,
        std::span(bytes).subspan(static_cast<std::size_t>(metadata_chunk.offset), static_cast<std::size_t>(metadata_chunk.byte_count)),
        11, bytes.size());
    Require(proof.depth == compiled.depth && proof.subresources.size() == compiled.subresources.size() &&
        proof.subresources.front().offset == compiled.subresources.front().offset,
        "bounded volume metadata proof differs from full admission");
    rd::ValidateTextureDescriptor(compiled.View(), bytes.size());
    const auto bound = rd::BindVolumeTextureDescriptor(bytes, compiled.View());
    Require(compiled.dimension == rd::TextureDimension::Texture3D && compiled.depth == 4 &&
            compiled.description.array_size == 1 && compiled.asset_id.value == asset.value && compiled.bulk_index == 11 &&
            compiled.dependencies.size() == 1 && compiled.dependencies[0].value == hs::MakeAssetId(dependencies[0]).value &&
            bound.dependencies.empty() && bound.description.width == parsed.description.width &&
            bound.description.height == parsed.description.height && bound.description.depth == parsed.description.depth &&
            bound.description.mip_count == parsed.description.mip_count && bound.description.format == parsed.description.format &&
            bound.subresources.size() == parsed.subresources.size(), "volume descriptor metadata differs from package");
    for (std::size_t i = 0; i < bound.subresources.size(); ++i)
    {
        const auto &a = bound.subresources[i];
        const auto &b = parsed.subresources[i];
        Require(a.mip_level == b.mip_level && a.width == b.width && a.height == b.height && a.depth == b.depth &&
                a.row_pitch == b.row_pitch && a.slice_pitch == b.slice_pitch && a.volume_pitch == b.volume_pitch &&
                a.bytes.data() == b.bytes.data() && a.bytes.size() == b.bytes.size() &&
                compiled.subresources[i].byte_count == b.volume_pitch, "volume descriptor layout or aliases differ");
    }
    Require(compiled.subresources[0].slice_pitch == 128 && compiled.subresources[0].byte_count == 512,
            "volume descriptor lost complete-volume extent");
    Reject([&] { (void)rd::BindTextureDescriptor(bytes, compiled.View()); }, "volume bound as a 2D texture");
    auto invalid = compiled;
    invalid.depth = 0;
    Reject([&] { rd::ValidateTextureDescriptor(invalid.View(), bytes.size()); }, "zero-depth volume accepted");
    invalid.depth = UINT32_MAX;
    Reject([&] { rd::ValidateTextureDescriptor(invalid.View(), bytes.size()); }, "overflowing volume depth accepted");
    invalid = compiled;
    invalid.subresources[0].byte_count = invalid.subresources[0].slice_pitch;
    Reject([&] { rd::ValidateTextureDescriptor(invalid.View(), bytes.size()); }, "volume accepted slice-only byte count");
    invalid = compiled;
    invalid.subresources[0].offset = UINT64_MAX;
    Reject([&] { rd::ValidateTextureDescriptor(invalid.View(), bytes.size()); }, "volume offset overflow accepted");
    Reject([&] { (void)rd::BindVolumeTextureDescriptor(bytes, invalid.View()); }, "bound volume offset overflow accepted");
    invalid = compiled;
    invalid.dimension = rd::TextureDimension::Texture2DArray;
    Reject([&] { rd::ValidateTextureDescriptor(invalid.View(), bytes.size()); }, "volume layout accepted as 2D");
    Reject([&] { (void)rd::BindVolumeTextureDescriptor(bytes, invalid.View()); }, "2D descriptor bound as volume");
    invalid.dimension = static_cast<rd::TextureDimension>(UINT32_MAX);
    Reject([&] { rd::ValidateTextureDescriptor(invalid.View(), bytes.size()); }, "unknown texture dimension accepted");
    const auto end = compiled.subresources.back().offset + compiled.subresources.back().byte_count;
    Reject([&] { rd::ValidateTextureDescriptor(compiled.View(), end - 1); }, "truncated volume extent accepted");
    Reject([&] { (void)rd::BindVolumeTextureDescriptor(std::span(bytes).first(static_cast<std::size_t>(end - 1)), compiled.View()); },
           "truncated volume binding accepted");

    const auto package = rd::ReadPackage(bytes, asset);
    auto metadata = std::vector<std::byte>(package.chunks[0].bytes.begin(), package.chunks[0].bytes.end());
    const auto malformed_package = [&] {
        const std::array chunks{
            rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::TextureMetadata), 1, true,
                metadata, 1, static_cast<std::uint32_t>(metadata.size())},
            rd::ChunkSource{static_cast<std::uint32_t>(rd::ChunkType::TextureSubresources), 1, true,
                payload, payload.size(), 1}};
        return rd::BuildPackage(asset, dependencies, chunks);
    };
    metadata[56] ^= std::byte{1}; // First mip's complete-volume pitch.
    auto malformed = malformed_package();
    Reject([&] { (void)rd::ReadVolumeTexturePackage(malformed, asset); }, "malformed volume pitch accepted");
    Reject([&] { (void)rd::CompileTextureDescriptor(malformed, asset, 11); }, "volume compilation bypassed raw pitch validation");
    metadata[0] = std::byte{};
    malformed = malformed_package();
    Reject([&] { (void)rd::CompileTextureDescriptor(malformed, asset, 11); }, "unknown texture metadata magic accepted");
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
        RunNamed("TestShippingRendererIdentity", TestShippingRendererIdentity);
        RunNamed("TestVfxMeshDescriptor", TestVfxMeshDescriptor);
        RunNamed("TestPackage", TestPackage);
        RunNamed("TestQualityProfiles", TestQualityProfiles);
        RunNamed("TestDecalMaterialPackages", TestDecalMaterialPackages);
        RunNamed("TestMaterialDescriptors", TestMaterialDescriptors);
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
        RunNamed("TestSurfaceMetadataCodec", TestSurfaceMetadataCodec);
        RunNamed("TestAuthoredTerrainMaterial", TestAuthoredTerrainMaterial);
        RunNamed("TestMaterialTextureBindingsAndContexts", TestMaterialTextureBindingsAndContexts);
        RunNamed("TestLegacyMaterialContextLowering", TestLegacyMaterialContextLowering);
        RunNamed("TestCharacterPackage", TestCharacterPackage);
        RunNamed("TestGeometryPackage", TestGeometryPackage);
        RunNamed("TestMonsterLodDrawOrder", TestMonsterLodDrawOrder);
        RunNamed("TestGeometryDescriptor", TestGeometryDescriptor);
        RunNamed("TestPackedRendererAssets", TestPackedRendererAssets);
        RunNamed("TestTexturePackage", TestTexturePackage);
        RunNamed("TestVolumeTextureDescriptor", TestVolumeTextureDescriptor);
        RunNamed("TestDdsConsumerRoles", TestDdsConsumerRoles);
        RunNamed("TestCharacterSemanticMips", TestCharacterSemanticMips);
        RunNamed("TestCharacterBc7CoverageRepair", TestCharacterBc7CoverageRepair);
        RunNamed("TestHlodScenePreparation", TestHlodScenePreparation);
        RunNamed("TestHeroWorkRawDecoding", TestHeroWorkRawDecoding);
        RunNamed("TestVirtualShadowGuarantees", TestVirtualShadowGuarantees);
        RunNamed("TestStaticShadowLayout", TestStaticShadowLayout);
        RunNamed("TestStaticShadowArchive", TestStaticShadowArchive);
        RunNamed("TestReleaseAudioCodegen", TestReleaseAudioCodegen);
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
