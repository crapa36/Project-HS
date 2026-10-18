#include "../AssetPipeline/action.hpp"

#include <hs/core/cooked_format.hpp>
#include <hs/render_data/texture_package.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace
{
namespace ap = hs::asset_pipeline;
namespace rd = hs::render_data;
namespace fs = std::filesystem;

struct TextureSpec
{
    std::string_view name;
    rd::TextureDescription description;
};
constexpr TextureSpec kSpecs[] = {
    {"terrain_gaussian", {2048, 2048, 4, 12, rd::kTextureFormatBc7}},
    {"terrain_histogram", {256, 16, 4, 1, rd::kTextureFormatRgba32Float}},
    {"terrain_surface", {2048, 2048, 4, 12, rd::kTextureFormatBc5}},
    {"terrain_normal", {2048, 2048, 4, 12, rd::kTextureFormatBc5}},
    {"terrain_detail", {2048, 2048, 4, 12, rd::kTextureFormatBc5}},
    {"terrain_height", {2048, 2048, 4, 12, rd::kTextureFormatBc4}},
    {"rock_basecolor", {2048, 2048, 4, 12, rd::kTextureFormatBc7Srgb}},
    {"rock_surface", {2048, 2048, 4, 12, rd::kTextureFormatBc5}},
    {"rock_normal", {2048, 2048, 4, 12, rd::kTextureFormatBc5}},
    {"rock_detail", {2048, 2048, 4, 12, rd::kTextureFormatBc5}},
    {"bark_basecolor", {2048, 2048, 3, 12, rd::kTextureFormatBc7Srgb}},
    {"bark_surface", {2048, 2048, 3, 12, rd::kTextureFormatBc5}},
    {"bark_normal", {2048, 2048, 3, 12, rd::kTextureFormatBc5}},
    {"bark_detail", {2048, 2048, 3, 12, rd::kTextureFormatBc5}},
    {"grass_basecolor", {2048, 2048, 2, 6, rd::kTextureFormatBc7Srgb}},
    {"grass_normal", {2048, 2048, 2, 6, rd::kTextureFormatBc5}},
    {"grass_roughness", {2048, 2048, 2, 6, rd::kTextureFormatBc4}},
    {"leaf_basecolor", {2048, 2048, 3, 6, rd::kTextureFormatBc7Srgb}},
    {"leaf_normal", {2048, 2048, 3, 6, rd::kTextureFormatBc5}},
    {"leaf_roughness", {2048, 2048, 3, 6, rd::kTextureFormatBc4}},
    {"ground_detail_basecolor", {2048, 2048, 1, 12, rd::kTextureFormatBc7Srgb}},
    {"ground_detail_surface", {2048, 2048, 1, 12, rd::kTextureFormatBc5}},
    {"ground_detail_normal", {2048, 2048, 1, 12, rd::kTextureFormatBc5}},
};

std::string SourceId(const TextureSpec &texture)
{
    return "environment/environment_" + std::string(texture.name) + ".dds";
}

std::string ProductId(const TextureSpec &texture)
{
    return "render/texture/environment_" + std::string(texture.name) + ".rpkg";
}

std::string SourceNamespace(const TextureSpec &texture)
{
    if (texture.name == "terrain_gaussian" || texture.name == "terrain_histogram")
        return "environment/histogram";
    const auto separator = texture.name.rfind('_');
    const auto group = texture.name.substr(0, separator);
    const auto suffix = texture.name.substr(separator + 1);
    const auto role = suffix == "basecolor" ? "base_color" :
                      suffix == "surface" ? "surface_ra" :
                      suffix == "detail" ? "detail_normal" : suffix;
    return "environment/textures/" + std::string(group) + "/" + std::string(role);
}

const TextureSpec &FindTexture(std::wstring_view name)
{
    for (const auto &texture : kSpecs)
        if (std::wstring(texture.name.begin(), texture.name.end()) == name)
            return texture;
    throw std::runtime_error("hs_render_texture_package: unknown texture name");
}

void Require(bool ok, std::string_view message)
{
    if (!ok) throw std::runtime_error("hs_render_texture_package: " + std::string(message));
}

std::uint32_t U32(std::span<const std::byte> bytes, std::size_t offset)
{
    Require(offset <= bytes.size() && 4 <= bytes.size() - offset, "truncated DDS header");
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i)
        value |= std::uint32_t(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
    return value;
}

std::span<const std::byte> DdsPayload(std::span<const std::byte> bytes,
                                      rd::TextureDescription expected)
{
    Require(bytes.size() >= 148 && U32(bytes, 0) == 0x20534444 &&
            U32(bytes, 4) == 124 && U32(bytes, 76) == 32 &&
            U32(bytes, 80) == 4 && U32(bytes, 84) == 0x30315844,
            "expected DDS with DX10 pixel format");
    Require(U32(bytes, 12) == expected.height &&
            U32(bytes, 16) == expected.width &&
            U32(bytes, 28) == expected.mip_count &&
            U32(bytes, 128) == expected.format &&
            U32(bytes, 132) == 3 && // D3D10_RESOURCE_DIMENSION_TEXTURE2D
            U32(bytes, 136) == 0 && U32(bytes, 140) == expected.array_size &&
            U32(bytes, 144) == 0,
            "environment DDS shape or format changed");
    const bool bc4 = expected.format == rd::kTextureFormatBc4;
    const bool bc = bc4 || expected.format == rd::kTextureFormatBc5 ||
                    expected.format == rd::kTextureFormatBc7 ||
                    expected.format == rd::kTextureFormatBc7Srgb;
    const bool rgba32 = expected.format == rd::kTextureFormatRgba32Float;
    Require(bc || rgba32, "unsupported environment DDS format");
    const auto block_bytes = bc4 ? 8u : 16u;
    const auto base_row_pitch = bc ?
        ((expected.width + 3) / 4) * block_bytes : expected.width * 16u;
    const auto header_pitch = bc ? base_row_pitch * ((expected.height + 3) / 4)
                                 : base_row_pitch;
    const auto flags = U32(bytes, 8);
    const auto common_flags = 0x1007u | (expected.mip_count > 1 ? 0x20000u : 0u);
    const auto required_caps = 0x1000u | (expected.mip_count > 1 ? 0x400008u : 0u);
    Require((bc ? ((flags & (common_flags | 0x80000u)) ==
                       (common_flags | 0x80000u)) :
                    ((flags & (common_flags | 0x8u)) == (common_flags | 0x8u) &&
                     (flags & 0x80000u) == 0)) &&
            U32(bytes, 20) == header_pitch &&
            U32(bytes, 24) == 1 &&
            (U32(bytes, 108) & required_caps) == required_caps &&
            U32(bytes, 112) == 0 && U32(bytes, 116) == 0 &&
            U32(bytes, 120) == 0 && U32(bytes, 124) == 0,
            "invalid DDS dimensions, caps, or array flags");
    for (std::size_t offset = 32; offset < 76; offset += 4)
        Require(U32(bytes, offset) == 0, "nonzero DDS reserved header");
    for (std::size_t offset = 88; offset < 108; offset += 4)
        Require(U32(bytes, offset) == 0, "nonzero DDS pixel format fields");
    std::uint64_t payload_size = 0;
    for (std::uint32_t slice = 0; slice < expected.array_size; ++slice)
        for (std::uint32_t mip = 0; mip < expected.mip_count; ++mip)
        {
            const auto width = std::max(1u, expected.width >> mip);
            const auto height = std::max(1u, expected.height >> mip);
            const auto row_pitch = bc ? ((width + 3) / 4) * block_bytes : width * 16u;
            const auto slice_pitch = bc ? row_pitch * ((height + 3) / 4) : row_pitch * height;
            Require(row_pitch > 0 && slice_pitch > 0 &&
                    payload_size <= std::numeric_limits<std::size_t>::max() - slice_pitch,
                    "DDS subresource range overflow");
            payload_size += slice_pitch;
        }
    Require(payload_size == bytes.size() - 148, "DDS subresource ranges do not exactly cover payload");
    return bytes.subspan(148, static_cast<std::size_t>(payload_size));
}

bool Current(const fs::path &build_root, const ap::ActionSpec &spec,
             const TextureSpec &texture, std::string_view source_id,
             std::string_view product_id)
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
                ap::Json::array({std::string(source_id)}))
            return false;
        ap::UpstreamProduct ignored;
        fs::path package_path;
        (void)ap::ReadActionResult(build_root, spec.namespace_path,
                                   product_id, ignored, package_path);
        const auto contents = ap::ReadBytes(package_path);
        const auto bytes = std::as_bytes(std::span(contents.data(), contents.size()));
        const auto view = rd::ReadTexturePackage(bytes, hs::MakeAssetId(product_id));
        const auto &expected = texture.description;
        return view.description.width == expected.width &&
               view.description.height == expected.height &&
               view.description.array_size == expected.array_size &&
               view.description.mip_count == expected.mip_count &&
               view.description.format == expected.format &&
               view.dependencies == std::vector<std::string>{std::string(source_id)} &&
               view.subresources.size() == expected.array_size * expected.mip_count;
    }
    catch (const std::exception &) { return false; }
}

void Produce(const fs::path &build_root, const TextureSpec &texture)
{
    Require(build_root.is_absolute(), "build root must be absolute");
    ap::MakeDirectory(build_root);
    const auto source_id = SourceId(texture);
    const auto product_id = ProductId(texture);
    const auto source_namespace = SourceNamespace(texture);
    const auto action_namespace = "render/texture/environment_" + std::string(texture.name);
    const auto filename = "environment_" + std::string(texture.name) + ".rpkg";
    ap::UpstreamProduct upstream;
    fs::path dds_path;
    (void)ap::ReadActionResult(build_root, source_namespace, source_id,
                               upstream, dds_path);
    ap::ActionSpec spec;
    spec.action_id = "render.texture.environment_" + std::string(texture.name);
    spec.recipe_id = "project-hs.render.texture." +
        std::string(texture.description.format == rd::kTextureFormatBc4 ? "bc4" :
                    texture.description.format == rd::kTextureFormatBc5 ? "bc5" :
                    texture.description.format == rd::kTextureFormatRgba32Float ? "rgba32f-linear" :
                    texture.description.format == rd::kTextureFormatBc7 ? "bc7-linear" :
                    "bc7-srgb");
    spec.recipe_version = 1;
    spec.namespace_path = action_namespace;
    spec.upstream.push_back(upstream);
    if (Current(build_root, spec, texture, source_id, product_id))
    {
        ap::TouchActionResult(build_root / "Actions" / spec.namespace_path / "result.json");
        std::cout << spec.action_id << " current\n";
        return;
    }

    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try
    {
        const auto dds = ap::ReadBytes(dds_path);
        const auto bytes = std::as_bytes(std::span(dds.data(), dds.size()));
        const auto payload = DdsPayload(bytes, texture.description);
        const std::array dependencies{source_id};
        const auto package = rd::BuildTexturePackage(
            hs::MakeAssetId(product_id), dependencies, texture.description, payload);
        const auto view = rd::ReadTexturePackage(package, hs::MakeAssetId(product_id));
        Require(view.dependencies == std::vector<std::string>{source_id} &&
                view.subresources.size() == texture.description.array_size *
                    texture.description.mip_count, "package self-validation failed");
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
                "usage: hs_render_texture_package --build-root <absolute> --name <environment texture>");
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
            context["action_id"] = "render.texture.environment_" + std::string(texture->name);
            context["dependency"] = SourceId(*texture);
        }
        ap::ReportFailure("hs_render_texture_package", error.what(), context);
        return 1;
    }
}
