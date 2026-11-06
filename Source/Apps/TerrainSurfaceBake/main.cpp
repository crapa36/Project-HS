#include "../AssetPipeline/action.hpp"
#include "../Common/world_transport_scene.hpp"

#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/core/types.hpp>
#include <hs/render_data/terrain_surface.hpp>
#include <hs/render_data/texture_package.hpp>
#include <hs/renderer/renderer.hpp>

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <DirectXTex.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

extern "C"
{
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}

namespace
{
namespace ap = hs::asset_pipeline;
namespace rd = hs::render_data;
namespace fs = std::filesystem;
constexpr std::string_view kAction = "render.terrain.surface";
constexpr std::string_view kNamespace = "render/terrain/surface";
constexpr std::string_view kMetadata = "render/terrain/arena_surface.rpkg";
constexpr std::string_view kMetadataFile = "arena_surface.rpkg";
constexpr std::uint64_t kGameplaySeed = 1;

void Require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}
void Require(const hs::Result &result)
{
    if (!result) throw std::runtime_error(std::string(result.Subsystem()) + ": " + std::string(result.Message()));
}
std::string SectionId(std::size_t index, std::size_t plane)
{
    return "render/terrain/arena_surface_section_" + std::to_string(index * 3 + plane) + ".rpkg";
}
std::string SectionFile(std::size_t index, std::size_t plane)
{
    return "arena_surface_section_" + std::to_string(index * 3 + plane) + ".rpkg";
}
std::string TileId(std::size_t index)
{
    return "render/terrain/arena_surface_tile_" + std::to_string(index) + ".rpkg";
}
std::string TileFile(std::size_t index)
{
    return "arena_surface_tile_" + std::to_string(index) + ".rpkg";
}

struct TerrainBc7Encoder
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    std::string identity{".cpu-bc7"};

    TerrainBc7Encoder()
    {
        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                levels, 1, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr)))
            return;
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        LARGE_INTEGER driver{};
        Require(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
            SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)) &&
            SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver)),
            "Terrain BC7 compressor adapter identity is unavailable");
        identity = ".d3d11-bc7." + std::to_string(desc.VendorId) + "." +
            std::to_string(desc.DeviceId) + "." + std::to_string(desc.SubSysId) + "." +
            std::to_string(desc.Revision) + "." + std::to_string(static_cast<std::uint64_t>(driver.QuadPart));
    }

    std::vector<std::byte> Compress(std::span<const std::byte> payload) const
    {
        constexpr std::size_t edge = rd::kTerrainSurfacePageEdge, slices = 3, mips = rd::kTerrainSurfacePageMips;
        DirectX::TexMetadata metadata{};
        metadata.width = edge; metadata.height = edge; metadata.depth = 1;
        metadata.arraySize = slices; metadata.mipLevels = mips;
        metadata.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        metadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;
        std::array<DirectX::Image, slices * mips> input{};
        std::size_t offset{};
        for (std::size_t slice = 0; slice < slices; ++slice)
            for (std::size_t mip = 0; mip < mips; ++mip)
            {
                const auto width = std::max<std::size_t>(1, edge >> mip);
                const auto row = width * 4;
                const auto size = row * width;
                Require(offset <= payload.size() && size <= payload.size() - offset,
                    "Invalid terrain RGBA8 subresource payload");
                auto &image = input[slice * mips + mip];
                image.width = width; image.height = width; image.format = metadata.format;
                image.rowPitch = row; image.slicePitch = size;
                image.pixels = reinterpret_cast<std::uint8_t *>(const_cast<std::byte *>(payload.data() + offset));
                offset += size;
            }
        Require(offset == payload.size(), "Terrain RGBA8 payload has trailing bytes");
        DirectX::ScratchImage compressed;
        const auto result = device
            ? DirectX::Compress(device.Get(), input.data(), input.size(), metadata,
                DXGI_FORMAT_BC7_UNORM, DirectX::TEX_COMPRESS_DEFAULT, 1.0f, compressed)
            : DirectX::Compress(input.data(), input.size(), metadata,
                DXGI_FORMAT_BC7_UNORM, DirectX::TEX_COMPRESS_PARALLEL, 0.5f, compressed);
        Require(SUCCEEDED(result), "Terrain BC7 compression failed");
        std::vector<std::byte> output;
        std::size_t encoded_bytes{};
        for (std::size_t mip = 0; mip < mips; ++mip)
        {
            const auto width = std::max<std::size_t>(1, edge >> mip);
            const auto blocks = (width + 3) / 4;
            encoded_bytes += blocks * blocks * 16 * slices;
        }
        output.reserve(encoded_bytes);
        for (std::size_t slice = 0; slice < slices; ++slice)
            for (std::size_t mip = 0; mip < mips; ++mip)
            {
                const auto *image = compressed.GetImage(mip, slice, 0);
                const auto width = std::max<std::size_t>(1, edge >> mip);
                const auto blocks = (width + 3) / 4;
                const auto row = blocks * 16;
                Require(image && image->rowPitch >= row && image->slicePitch >= image->rowPitch * blocks,
                    "Invalid terrain BC7 subresource layout");
                for (std::size_t y = 0; y < blocks; ++y)
                {
                    const auto *begin = reinterpret_cast<const std::byte *>(image->pixels + y * image->rowPitch);
                    output.insert(output.end(), begin, begin + row);
                }
            }
        return output;
    }
};

bool Current(const fs::path &build, const ap::ActionSpec &spec)
{
    const auto result = build / "Actions" / kNamespace / "result.json";
    if (!fs::exists(result) || !fs::exists(result.parent_path() / "result.d")) return false;
    try
    {
        const ap::ActionResultSnapshot snapshot(build, kNamespace);
        if (snapshot.ActionId() != kAction || snapshot.ActionKeyValue() != ap::ActionKey(spec)) return false;
        ap::UpstreamProduct ignored;
        const auto bytes = snapshot.ReadProductBytes(kMetadata, ignored);
        const auto metadata = rd::ReadTerrainSurfacePackage(
            std::as_bytes(std::span(bytes.data(), bytes.size())), hs::MakeAssetId(kMetadata));
        if (metadata.layout_version != rd::kTerrainSurfaceLayoutVersion || metadata.sections.empty() ||
            metadata.sections.size() != metadata.section_products.size() ||
            snapshot.ProductCount() != metadata.sections.size() * 4 + 1) return false;
        std::vector<std::string> dependencies;
        for (std::size_t i = 0; i < metadata.sections.size(); ++i)
        {
          const auto leaf_id=TileId(i);
          if(metadata.section_products[i]!=leaf_id) return false;
          const auto leaf_bytes=snapshot.ReadProductBytes(leaf_id,ignored);
          const auto leaf=rd::ReadTerrainSurfacePackage(
              std::as_bytes(std::span(leaf_bytes.data(),leaf_bytes.size())),hs::MakeAssetId(leaf_id));
          if(leaf.layout_version!=rd::kTerrainSurfaceLeafLayoutVersion || leaf.sections.size()!=1 ||
              leaf.sections[0]!=metadata.sections[i] || leaf.section_products.size()!=3 ||
              leaf.source_identity!=metadata.source_identity || leaf.description.width!=metadata.description.width ||
              leaf.description.height!=metadata.description.height || leaf.description.array_size!=3 ||
              leaf.description.mip_count!=metadata.description.mip_count || leaf.description.format!=metadata.description.format)
              return false;
          std::vector<std::string> leaf_dependencies;
          for (std::size_t plane = 0; plane < 3; ++plane)
        {
            const auto id = SectionId(i, plane);
            if (leaf.section_products[plane] != id ||
                snapshot.ProductMetadata(id).value("dependencies", std::vector<std::string>{}) != dependencies)
                return false;
            leaf_dependencies.push_back(id);
            const auto plane_bytes = snapshot.ReadProductBytes(id, ignored);
            const auto plane_package = rd::ReadTexturePackage(
                std::as_bytes(std::span(plane_bytes.data(), plane_bytes.size())), hs::MakeAssetId(id));
            const auto &shape = plane_package.description;
            if (shape.width != metadata.description.width || shape.height != metadata.description.height ||
                shape.mip_count != metadata.description.mip_count || shape.format != metadata.description.format ||
                shape.array_size != 1 || plane_package.subresources.size() != shape.mip_count) return false;
        }
          std::ranges::sort(leaf_dependencies);
          if(snapshot.ProductMetadata(leaf_id).value("dependencies",std::vector<std::string>{})!=leaf_dependencies)
              return false;
        }
        dependencies = metadata.section_products;
        std::ranges::sort(dependencies);
        return snapshot.ProductMetadata(kMetadata).value("dependencies", std::vector<std::string>{}) == dependencies;
    }
    catch (const std::exception &) { return false; }
}
}

int main(int argc, char **argv)
{
    fs::path source, build, pointer;
    try
    {
        std::set<std::string> seen;
        for (int i = 1; i < argc; ++i)
        {
            std::string_view argument(argv[i]);
            if (argument == "--produce-terrain-surface") continue;
            const auto separator = argument.find('=');
            const auto name = argument.substr(0, separator);
            Require(name == "--source-root" || name == "--build-root" || name == "--generation-pointer" ||
                name == "--generation", "Unknown terrain surface argument: " + std::string(name));
            const std::string key = name == "--generation" ? "--generation-pointer" : std::string(name);
            Require(seen.emplace(key).second, "Duplicate terrain surface argument: " + key);
            std::string_view value;
            if (separator != std::string_view::npos) value = argument.substr(separator + 1);
            else { Require(i + 1 < argc, "Missing argument value: " + key); value = argv[++i]; }
            Require(!value.empty(), "Empty terrain surface argument: " + key);
            if (name == "--source-root") source = value;
            else if (name == "--build-root") build = value;
            else pointer = value;
        }
        Require(source.is_absolute() && build.is_absolute() && pointer.is_absolute(),
            "Terrain surface producer requires absolute source/build/generation-pointer paths");
        ap::RequireDirectory(source); ap::RequireDirectory(build);
        source = fs::canonical(source); build = fs::canonical(build);
        hs::SelectedGeneration generation;
        Require(hs::LoadSelectedGeneration(pointer, generation));
        for (const auto &[id, unused] : generation.product_paths)
            Require(id != kMetadata && !id.starts_with("render/terrain/arena_surface_section_"),
                "Terrain bake input Generation contains its own output: " + id);

        ap::ActionSpec spec;
        spec.action_id = kAction; spec.namespace_path = kNamespace;
        const TerrainBc7Encoder encoder;
        spec.recipe_id = "project-hs.render.terrain-surface/native-seed-1-bc7-layout5-tiles-apron" + encoder.identity;
        spec.recipe_version = 5;
        for (const auto &[id, path] : std::initializer_list<std::pair<std::string_view, fs::path>>{
            {"terrain/world-transport", source / "Source/Apps/Common/world_transport_scene.cpp"},
            {"terrain/layout", source / "Source/Presentation/Private/environment_layout.cpp"},
            {"terrain/layout-contract", source / "Source/Presentation/Public/hs/presentation/environment_layout.hpp"}})
            spec.inputs.push_back(ap::HashInput(std::string(id), path));

        Require(!generation.product_paths.empty(), "Terrain bake input Generation is empty");
        std::set<std::string> visited;
        std::vector<std::string> pending{
            "game-data/simulation-rules", "render/material/terrain.rpkg", "render/meadow_ground.rpkg",
            "terrain/producer/bake_cs.dxil", "terrain/producer/mip_cs.dxil"};
        while (!pending.empty())
        {
            auto id = std::move(pending.back()); pending.pop_back();
            if (!visited.insert(id).second) continue;
            fs::path selected;
            Require(generation.Require(id, selected));
            const auto found_digest = generation.product_digests.find(id);
            const auto found_dependencies = generation.product_dependencies.find(id);
            Require(found_digest != generation.product_digests.end() &&
                found_dependencies != generation.product_dependencies.end() &&
                ap::Sha256File(selected) == found_digest->second,
                "Selected terrain input digest mismatch: " + id);
            spec.inputs.push_back({"terrain-bake/" + id, selected, found_digest->second});
            pending.insert(pending.end(), found_dependencies->second.begin(), found_dependencies->second.end());
        }
        std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
        if (Current(build, spec))
        {
            ap::TouchActionResult(build / "Actions" / kNamespace / "result.json");
            return 0;
        }

        const auto scene = hs::app::WorldTransportScene(generation, kGameplaySeed);
        Require(scene && !scene->instances.empty(), "Terrain bake world scene is empty");
        const auto stage = ap::CreateStage(build / "Actions" / kNamespace);
        try
        {
            hs::RendererConfig config;
            config.width = 320; config.height = 180;
#ifndef NDEBUG
            config.validation = true;
#endif
            config.material_quality = hs::GraphicsQuality::Ultra;
            config.pom_quality = hs::GraphicsOptionalQuality::Ultra;
            config.foliage_quality = hs::GraphicsQuality::Ultra;
            config.texture_quality = hs::GraphicsQuality::Ultra;
            config.geometry_quality = hs::GraphicsQuality::Ultra;
            config.content_generation_id = generation.id;
            config.content_generation_lease = std::make_shared<const hs::SelectedGeneration>(generation);
            config.product_paths = generation.product_paths;
            config.product_digests = generation.product_digests;

            std::vector<rd::TerrainSurfaceSection> sections;
            std::vector<std::string> section_ids, leaf_ids, dependencies;
            std::vector<bool> written;
            std::vector<ap::Product> products;
            rd::TextureDescription description{};
            std::array<std::byte, 32> source_identity{};
            bool begun = false;
            Require(hs::D3D12Renderer::BakeTerrainSurfacePages(config, scene,
                [&](std::span<const rd::TerrainSurfaceSection> incoming, rd::TextureDescription global,
                    std::array<std::byte, 32> identity) {
                    Require(!begun && !incoming.empty() && incoming.size() <= UINT32_MAX / 3 &&
                        global.width == rd::kTerrainSurfacePageEdge && global.height == rd::kTerrainSurfacePageEdge &&
                        global.mip_count == rd::kTerrainSurfacePageMips &&
                        global.format == rd::kTextureFormatRgba8 && global.array_size == incoming.size() * 3,
                        "Native terrain surface description is invalid");
                    sections.assign(incoming.begin(), incoming.end());
                    description = global;
                    description.format = rd::kTextureFormatBc7;
                    source_identity = identity;
                    written.resize(sections.size());
                    for (std::size_t i = 0; i < sections.size(); ++i)
                    {
                        leaf_ids.push_back(TileId(i));
                        for (std::size_t plane = 0; plane < 3; ++plane) section_ids.push_back(SectionId(i, plane));
                    }
                    begun = true;
                    return hs::Result::Success();
                },
                [&](std::uint32_t index, std::span<const std::byte> payload) {
                    Require(begun && index < sections.size() && !written[index], "Unexpected terrain section callback");
                    auto local = description; local.array_size = 1;
                    const auto compressed = encoder.Compress(payload);
                    Require(compressed.size() % 3 == 0, "Invalid terrain BC7 plane payload size");
                    const auto plane_bytes = compressed.size() / 3;
                    for (std::size_t plane = 0; plane < 3; ++plane)
                    {
                        const auto &id = section_ids[size_t(index) * 3 + plane];
                        const auto plane_payload = std::span(compressed).subspan(plane * plane_bytes, plane_bytes);
                        const auto package = rd::BuildTexturePackage(hs::MakeAssetId(id), dependencies, local, plane_payload);
                        const auto filename = SectionFile(index, plane);
                        const auto path = stage / filename;
                        ap::WriteBytes(path, package);
                        auto product = ap::HashProduct(id, filename, path);
                        product.dependencies = dependencies;
                        products.push_back(std::move(product));
                    }
                    auto leaf_description=description;leaf_description.array_size=3;
                    const auto plane_ids=std::span(section_ids).subspan(size_t(index)*3,3);
                    const auto leaf=rd::BuildTerrainSurfacePackage(hs::MakeAssetId(leaf_ids[index]),plane_ids,
                        std::span(sections).subspan(index,1),leaf_description,source_identity);
                    const auto filename=TileFile(index);
                    const auto path=stage/filename;
                    ap::WriteBytes(path,leaf);
                    auto product=ap::HashProduct(leaf_ids[index],filename,path);
                    product.dependencies.assign(plane_ids.begin(),plane_ids.end());
                    std::ranges::sort(product.dependencies);
                    products.push_back(std::move(product));
                    written[index] = true;
                    return hs::Result::Success();
                }));
            Require(begun && std::ranges::all_of(written, [](bool value) { return value; }),
                "Native terrain bake did not complete every section");
            const auto metadata = rd::BuildTerrainSurfaceIndex(
                hs::MakeAssetId(kMetadata), leaf_ids, sections, description, source_identity);
            const auto metadata_path = stage / kMetadataFile;
            ap::WriteBytes(metadata_path, metadata);
            auto product = ap::HashProduct(std::string(kMetadata), std::string(kMetadataFile), metadata_path);
            product.dependencies = leaf_ids;
            std::ranges::sort(product.dependencies);
            products.push_back(std::move(product));
            ap::Publish(build, spec, stage, std::move(products));
        }
        catch (...) { ap::RemoveStageAfterFailure(stage); throw; }
        ap::RemoveStage(stage);
        std::cout << kAction << " published\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure(kAction, error.what(), {{"build_root", build.string()}});
        return 1;
    }
}
