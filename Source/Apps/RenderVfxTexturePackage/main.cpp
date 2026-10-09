#include "../AssetPipeline/action.hpp"

#include <hs/core/cooked_format.hpp>
#include <hs/render_data/texture_package.hpp>
#include <hs/render_data/volume_texture_package.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace ap = hs::asset_pipeline;
namespace rd = hs::render_data;
namespace fs = std::filesystem;

struct TextureSpec
{
    std::string_view name;
    rd::TextureDescription description;
    bool masks{};
    bool volume{};
};

// Zero height/array count means that the cooked upstream DDS owns that value.
// The gradient grows when generated rows are appended; mask count follows the
// cooked sprite layout. The current outputs are 256x68 and 39 slices.
constexpr TextureSpec kSpecs[] = {
    {"vfx_authored_mask_array", {256, 256, 12, 9, rd::kTextureFormatR8}},
    {"vfx_curve_lut", {256, 7, 1, 1, rd::kTextureFormatR16Float}},
    {"vfx_flow_curl_2d", {256, 256, 1, 9, rd::kTextureFormatRg8Snorm}},
    {"vfx_fracture_decal_array", {512, 512, 4, 10, rd::kTextureFormatRgba8}},
    {"vfx_gradient_lut", {256, 0, 1, 1, rd::kTextureFormatRgba16Float}},
    {"vfx_noise_basis_2d", {256, 256, 1, 9, rd::kTextureFormatRgba8}},
    {"vfx_noise_volume_64", {64, 64, 1, 1, rd::kTextureFormatRgba8}, false, true},
    {"vfx_organic_decal_array", {512, 512, 8, 10, rd::kTextureFormatR8}},
    {"vfx_ribbon_detail_array", {256, 64, 8, 9, rd::kTextureFormatR8}},
    {"vfx_smoke6way_neg", {128, 128, 64, 8, rd::kTextureFormatRgba8}},
    {"vfx_smoke6way_pos", {128, 128, 64, 8, rd::kTextureFormatRgba8}},
    {"vfx_smoke_motion_vectors", {128, 128, 64, 8, rd::kTextureFormatRg8Snorm}},
    {"vfx_stbn_scalar", {64, 64, 64, 1, rd::kTextureFormatR8}},
    {"vfx_masks", {512, 512, 0, 10, rd::kTextureFormatBc4}, true},
};

void Require(bool ok, std::string_view message)
{
    if (!ok) throw std::runtime_error("hs_render_vfx_texture_package: " +
                                      std::string(message));
}

const TextureSpec &FindTexture(std::wstring_view name)
{
    for (const auto &texture : kSpecs)
        if (std::wstring(texture.name.begin(), texture.name.end()) == name)
            return texture;
    throw std::runtime_error("hs_render_vfx_texture_package: unknown texture name");
}

std::string SourceId(const TextureSpec &texture)
{
    return texture.masks ? "vfx/vfx_masks.dds" :
        "vfx/Content/Textures/VFX/" + std::string(texture.name) + ".dds";
}

std::string SourceNamespace(const TextureSpec &texture)
{
    return texture.masks ? "vfx/masks" : "vfx/vfx-runtime";
}

std::string ProductId(const TextureSpec &texture)
{
    return "render/texture/" + std::string(texture.name) + ".rpkg";
}

std::uint32_t U32(std::span<const std::byte> bytes, std::size_t offset)
{
    Require(offset <= bytes.size() && 4 <= bytes.size() - offset,
            "truncated DDS header");
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i)
        value |= std::uint32_t(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
    return value;
}

rd::TextureDescription ParseDds(std::span<const std::byte> bytes,
                                const TextureSpec &texture)
{
    Require(bytes.size() >= 148 && U32(bytes, 0) == 0x20534444 &&
            U32(bytes, 4) == 124 && U32(bytes, 76) == 32 &&
            U32(bytes, 80) == 4 && U32(bytes, 84) == 0x30315844,
            "expected a complete DX10 DDS header");
    rd::TextureDescription actual{
        U32(bytes, 16), U32(bytes, 12), U32(bytes, 140), U32(bytes, 28),
        U32(bytes, 128)};
    const auto &expected = texture.description;
    Require(actual.width == expected.width &&
            (expected.height == 0 || actual.height == expected.height) &&
            (expected.array_size == 0 || actual.array_size == expected.array_size) &&
            actual.mip_count == expected.mip_count &&
            actual.format == expected.format &&
            actual.height > 0 && actual.array_size > 0 &&
            U32(bytes, 132) == 3 && // D3D10_RESOURCE_DIMENSION_TEXTURE2D
            U32(bytes, 136) == 0 && U32(bytes, 144) == 0,
            "VFX DDS shape, format, or resource dimension changed");
    const bool bc4 = actual.format == rd::kTextureFormatBc4;
    const auto bytes_per_pixel = actual.format == rd::kTextureFormatR8 ? 1u :
        actual.format == rd::kTextureFormatRg8Snorm ||
        actual.format == rd::kTextureFormatR16Float ? 2u :
        actual.format == rd::kTextureFormatRgba8 ? 4u : 8u;
    const auto top_pitch = bc4 ?
        std::uint64_t((actual.width + 3) / 4) * ((actual.height + 3) / 4) * 8 :
        std::uint64_t(actual.width) * bytes_per_pixel;
    Require(top_pitch <= UINT32_MAX && U32(bytes, 20) == top_pitch &&
            U32(bytes, 8) == (bc4 ? 0xa1007u :
                actual.mip_count > 1 ? 0x2100fu : 0x100fu) &&
            U32(bytes, 24) <= 1 &&
            U32(bytes, 108) == (actual.mip_count > 1 ? 0x401008u :
                actual.array_size > 1 ? 0x1008u : 0x1000u) &&
            U32(bytes, 112) == 0 && U32(bytes, 116) == 0 &&
            U32(bytes, 120) == 0 && U32(bytes, 124) == 0,
            "VFX DDS pitch, flags, or caps changed");
    for (std::size_t offset = 32; offset < 76; offset += 4)
        Require(U32(bytes, offset) == 0, "nonzero DDS reserved header");
    for (std::size_t offset = 88; offset < 108; offset += 4)
        Require(U32(bytes, offset) == 0, "nonzero DDS pixel format fields");
    return actual;
}

rd::VolumeTextureDescription ParseVolumeDds(std::span<const std::byte> bytes,
                                            const TextureSpec &texture)
{
    Require(bytes.size() == 148ull + 64ull * 64ull * 64ull * 4ull &&
                U32(bytes, 0) == 0x20534444 && U32(bytes, 4) == 124 &&
                U32(bytes, 8) == 0x0080100f && U32(bytes, 12) == 64 &&
                U32(bytes, 16) == 64 && U32(bytes, 20) == 256 &&
                U32(bytes, 24) == 64 && U32(bytes, 28) == 1 &&
                U32(bytes, 76) == 32 && U32(bytes, 80) == 4 &&
                U32(bytes, 84) == 0x30315844 && U32(bytes, 108) == 0x1008 &&
                U32(bytes, 112) == 0x00200000 && U32(bytes, 116) == 0 &&
                U32(bytes, 120) == 0 && U32(bytes, 124) == 0 &&
                U32(bytes, 132) == 4 && U32(bytes, 136) == 0 &&
                U32(bytes, 140) == 1 && U32(bytes, 144) == 0 &&
                U32(bytes, 128) == rd::kTextureFormatRgba8,
            "expected the authored 64x64x64 RGBA8 volume DDS header and payload");
    for (std::size_t offset = 32; offset < 76; offset += 4)
        Require(U32(bytes, offset) == 0, "nonzero DDS reserved header");
    for (std::size_t offset = 88; offset < 108; offset += 4)
        Require(U32(bytes, offset) == 0, "nonzero DDS DX10 reserved fields");
    const auto &expected = texture.description;
    Require(texture.volume && expected.width == 64 && expected.height == 64 &&
                expected.array_size == 1 && expected.mip_count == 1 &&
                expected.format == rd::kTextureFormatRgba8,
            "volume spec contract changed");
    return {64, 64, 64, 1, rd::kTextureFormatRgba8};
}

bool Current(const fs::path &build_root, const ap::ActionSpec &spec,
             const rd::TextureDescription &description,
             std::string_view source_id, std::string_view product_id)
{
    const auto result_path = build_root / "Actions" / spec.namespace_path / "result.json";
    if (!fs::exists(result_path) || !fs::exists(result_path.parent_path() / "result.d"))
        return false;
    try
    {
        const auto result = ap::Json::parse(ap::ReadBytes(result_path));
        if (result.at("action_id") != spec.action_id ||
            result.at("action_key") != ap::ActionKey(spec)) return false;
        const auto &products = result.at("products");
        if (!products.is_array() || products.size() != 1 ||
            products.at(0).at("logical_id") != std::string(product_id) ||
            products.at(0).at("dependencies") !=
                ap::Json::array({std::string(source_id)})) return false;
        ap::UpstreamProduct ignored;
        fs::path package_path;
        (void)ap::ReadActionResult(build_root, spec.namespace_path,
                                   product_id, ignored, package_path);
        const auto contents = ap::ReadBytes(package_path);
        const auto bytes = std::as_bytes(std::span(contents.data(), contents.size()));
        const auto view = rd::ReadTexturePackage(bytes, hs::MakeAssetId(product_id));
        const auto &actual = view.description;
        return actual.width == description.width && actual.height == description.height &&
               actual.array_size == description.array_size &&
               actual.mip_count == description.mip_count &&
               actual.format == description.format &&
               view.dependencies == std::vector<std::string>{std::string(source_id)} &&
               view.subresources.size() == description.array_size * description.mip_count;
    }
    catch (const std::exception &) { return false; }
}

bool CurrentVolume(const fs::path &build_root, const ap::ActionSpec &spec,
                   const rd::VolumeTextureDescription &description,
                   std::string_view source_id, std::string_view product_id)
{
    const auto result_path = build_root / "Actions" / spec.namespace_path / "result.json";
    if (!fs::exists(result_path) || !fs::exists(result_path.parent_path() / "result.d"))
        return false;
    try
    {
        const auto result = ap::Json::parse(ap::ReadBytes(result_path));
        if (result.at("action_id") != spec.action_id ||
            result.at("action_key") != ap::ActionKey(spec)) return false;
        const auto &products = result.at("products");
        if (!products.is_array() || products.size() != 1 ||
            products.at(0).at("logical_id") != std::string(product_id) ||
            products.at(0).at("dependencies") !=
                ap::Json::array({std::string(source_id)})) return false;
        ap::UpstreamProduct ignored;
        fs::path package_path;
        (void)ap::ReadActionResult(build_root, spec.namespace_path,
                                   product_id, ignored, package_path);
        const auto contents = ap::ReadBytes(package_path);
        const auto bytes = std::as_bytes(std::span(contents.data(), contents.size()));
        const auto view = rd::ReadVolumeTexturePackage(bytes, hs::MakeAssetId(product_id));
        const auto &actual = view.description;
        return actual.width == description.width && actual.height == description.height &&
               actual.depth == description.depth && actual.mip_count == description.mip_count &&
               actual.format == description.format &&
               view.dependencies == std::vector<std::string>{std::string(source_id)} &&
               view.subresources.size() == description.mip_count;
    }
    catch (const std::exception &) { return false; }
}

void ProduceVolume(const fs::path &build_root, const TextureSpec &texture)
{
    Require(build_root.is_absolute(), "build root must be absolute");
    ap::MakeDirectory(build_root);
    const auto source_id = SourceId(texture);
    const auto product_id = ProductId(texture);
    const auto action_namespace = "render/texture/" + std::string(texture.name);
    const auto filename = std::string(texture.name) + ".rpkg";
    ap::UpstreamProduct upstream;
    fs::path dds_path;
    (void)ap::ReadActionResult(build_root, SourceNamespace(texture), source_id,
                               upstream, dds_path);
    const auto dds = ap::ReadBytes(dds_path);
    const auto bytes = std::as_bytes(std::span(dds.data(), dds.size()));
    const auto description = ParseVolumeDds(bytes, texture);
    ap::ActionSpec spec;
    spec.action_id = "render.texture." + std::string(texture.name);
    spec.recipe_id = "project-hs.render.texture.vfx-3d";
    spec.recipe_version = 1;
    spec.namespace_path = action_namespace;
    spec.upstream.push_back(upstream);
    if (CurrentVolume(build_root, spec, description, source_id, product_id))
    {
        ap::TouchActionResult(build_root / "Actions" / spec.namespace_path / "result.json");
        std::cout << spec.action_id << " current\n";
        return;
    }

    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try
    {
        const auto payload = bytes.subspan(148);
        const std::array dependencies{source_id};
        const auto package = rd::BuildVolumeTexturePackage(
            hs::MakeAssetId(product_id), dependencies, description, payload);
        const auto view = rd::ReadVolumeTexturePackage(package, hs::MakeAssetId(product_id));
        Require(view.dependencies == std::vector<std::string>{source_id} &&
                    view.subresources.size() == description.mip_count &&
                    view.subresources.front().row_pitch == 256 &&
                    view.subresources.front().slice_pitch == 16384 &&
                    view.subresources.front().volume_pitch == payload.size(),
                "volume package self-validation failed");
        Require(std::equal(view.subresources.front().bytes.begin(),
                           view.subresources.front().bytes.end(), payload.begin()),
                "volume package changed DDS payload bytes");
        const auto output = stage / filename;
        ap::WriteBytes(output, package);
        auto product = ap::HashProduct(product_id, filename, output);
        product.dependencies.assign(dependencies.begin(), dependencies.end());
        ap::Publish(build_root, spec, stage, {std::move(product)});
    }
    catch (...)
    {
        ap::RemoveStage(stage);
        throw;
    }
    ap::RemoveStage(stage);
    std::cout << spec.action_id << " published\n";
}

void Produce(const fs::path &build_root, const TextureSpec &texture)
{
    if (texture.volume)
    {
        ProduceVolume(build_root, texture);
        return;
    }
    Require(build_root.is_absolute(), "build root must be absolute");
    ap::MakeDirectory(build_root);
    const auto source_id = SourceId(texture);
    const auto product_id = ProductId(texture);
    const auto action_namespace = "render/texture/" + std::string(texture.name);
    const auto filename = std::string(texture.name) + ".rpkg";
    ap::UpstreamProduct upstream;
    fs::path dds_path;
    (void)ap::ReadActionResult(build_root, SourceNamespace(texture), source_id,
                               upstream, dds_path);
    const auto dds = ap::ReadBytes(dds_path);
    const auto bytes = std::as_bytes(std::span(dds.data(), dds.size()));
    const auto description = ParseDds(bytes, texture);
    ap::ActionSpec spec;
    spec.action_id = "render.texture." + std::string(texture.name);
    spec.recipe_id = "project-hs.render.texture.vfx-2d";
    spec.recipe_version = 1;
    spec.namespace_path = action_namespace;
    spec.upstream.push_back(upstream);
    if (Current(build_root, spec, description, source_id, product_id))
    {
        ap::TouchActionResult(build_root / "Actions" / spec.namespace_path / "result.json");
        std::cout << spec.action_id << " current\n";
        return;
    }

    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try
    {
        const auto payload = bytes.subspan(148);
        const std::array dependencies{source_id};
        const auto package = rd::BuildTexturePackage(
            hs::MakeAssetId(product_id), dependencies, description, payload);
        const auto view = rd::ReadTexturePackage(package, hs::MakeAssetId(product_id));
        Require(view.dependencies == std::vector<std::string>{source_id} &&
                view.subresources.size() == description.array_size * description.mip_count,
                "package self-validation failed");
        std::size_t offset{};
        for (const auto &subresource : view.subresources)
        {
            Require(subresource.bytes.size() <= payload.size() - offset &&
                    std::equal(subresource.bytes.begin(), subresource.bytes.end(),
                               payload.begin() + offset),
                    "package changed DDS payload bytes");
            offset += subresource.bytes.size();
        }
        Require(offset == payload.size(), "package omitted DDS payload bytes");
        const auto output = stage / filename;
        ap::WriteBytes(output, package);
        auto product = ap::HashProduct(product_id, filename, output);
        product.dependencies.assign(dependencies.begin(), dependencies.end());
        ap::Publish(build_root, spec, stage, {std::move(product)});
    }
    catch (...)
    {
        ap::RemoveStage(stage);
        throw;
    }
    ap::RemoveStage(stage);
    std::cout << spec.action_id << " published\n";
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    fs::path build_root;
    const TextureSpec *texture = nullptr;
    try
    {
        Require(argc == 5 && std::wstring_view(argv[1]) == L"--build-root" &&
                std::wstring_view(argv[3]) == L"--name",
                "usage: hs_render_vfx_texture_package --build-root <absolute> --name <vfx texture>");
        build_root = fs::path(argv[2]).lexically_normal();
        texture = &FindTexture(argv[4]);
        Produce(build_root, *texture);
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::Json context{{"build_root", ap::DiagnosticPath(build_root)}};
        if (texture)
        {
            context["action_id"] = "render.texture." + std::string(texture->name);
            context["dependency"] = SourceId(*texture);
        }
        ap::ReportFailure("hs_render_vfx_texture_package", error.what(), context);
        return 1;
    }
}
