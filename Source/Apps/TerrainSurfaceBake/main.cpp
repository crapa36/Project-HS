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

    DirectX::ScratchImage Compress(std::span<const rd::TextureSubresourceView> subresources) const
    {
        constexpr std::size_t edge = rd::kTerrainSurfacePageEdge, slices = 3, mips = rd::kTerrainSurfacePageMips;
        DirectX::TexMetadata metadata{};
        metadata.width = edge; metadata.height = edge; metadata.depth = 1;
        metadata.arraySize = slices; metadata.mipLevels = mips;
        metadata.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        metadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;
        std::array<DirectX::Image, slices * mips> input{};
        Require(subresources.size() == input.size(), "Unexpected terrain readback subresource count");
        for (std::size_t i = 0; i < input.size(); ++i)
        {
            const auto &source = subresources[i];
            input[i] = {source.width, source.height, metadata.format, source.row_pitch, source.slice_pitch,
                reinterpret_cast<std::uint8_t *>(const_cast<std::byte *>(source.bytes.data()))};
        }
        DirectX::ScratchImage compressed;
        const auto result = device
            ? DirectX::Compress(device.Get(), input.data(), input.size(), metadata,
                DXGI_FORMAT_BC7_UNORM, DirectX::TEX_COMPRESS_DEFAULT, 1.0f, compressed)
            : DirectX::Compress(input.data(), input.size(), metadata,
                DXGI_FORMAT_BC7_UNORM, DirectX::TEX_COMPRESS_PARALLEL, 0.5f, compressed);
        Require(SUCCEEDED(result), "Terrain BC7 compression failed");
        return compressed;
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
            metadata.section_products.size() != metadata.sections.size() * 3 ||
            snapshot.ProductCount() != metadata.sections.size() * 3 + 1) return false;
        std::vector<std::string> dependencies;
        for (std::size_t i = 0; i < metadata.sections.size(); ++i)
        {
            for (std::size_t plane = 0; plane < 3; ++plane)
            {
                const auto id = SectionId(i, plane);
                if (metadata.section_products[i * 3 + plane] != id ||
                    snapshot.ProductMetadata(id).value("dependencies", std::vector<std::string>{}) != dependencies)
                    return false;
                fs::path path;
                snapshot.ReadProduct(id, ignored, path);
            }
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
    fs::path build, pointer;
    bool gpu_validation = false;
    try
    {
        std::set<std::string> seen;
        for (int i = 1; i < argc; ++i)
        {
            std::string_view argument(argv[i]);
            if (argument == "--gpu-validation") { gpu_validation = true; continue; }
            const auto separator = argument.find('=');
            const auto name = argument.substr(0, separator);
            Require(name == "--build-root" || name == "--generation",
                "Unknown terrain surface argument: " + std::string(name));
            const std::string key(name);
            Require(seen.emplace(key).second, "Duplicate terrain surface argument: " + key);
            std::string_view value;
            if (separator != std::string_view::npos) value = argument.substr(separator + 1);
            else { Require(i + 1 < argc, "Missing argument value: " + key); value = argv[++i]; }
            Require(!value.empty(), "Empty terrain surface argument: " + key);
            if (name == "--build-root") build = value;
            else pointer = value;
        }
        Require(build.is_absolute() && pointer.is_absolute(),
            "Terrain surface producer requires absolute build and generation paths");
        ap::RequireDirectory(build);
        build = fs::canonical(build);
        hs::SelectedGeneration generation;
        Require(hs::LoadSelectedGeneration(pointer, generation));
        for (const auto &[id, unused] : generation.product_paths)
            Require(id != kMetadata && !id.starts_with("render/terrain/arena_surface_section_"),
                "Terrain bake input Generation contains its own output: " + id);

        ap::ActionSpec spec;
        spec.action_id = kAction; spec.namespace_path = kNamespace;
        const TerrainBc7Encoder encoder;
        spec.recipe_id = "project-hs.render.terrain-surface/native-seed-1-bc7-layout6-tiles-apron" + encoder.identity;
        spec.recipe_version = 6;
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
            // Terrain bake consumes Meadow LOD0 and compute producers, not the runtime color shader or fallback LOD payloads.
            for (const auto &dependency : found_dependencies->second)
                if (!((id == "render/material/terrain.rpkg" && dependency == "render/shader/terrain_scene.dxil") ||
                    (id == "render/meadow_ground.rpkg" && (dependency == "render/meadow_ground_lod1.rpkg" ||
                        dependency == "render/meadow_ground_lod2.rpkg"))))
                    pending.push_back(dependency);
        }
        std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
        if (Current(build, spec))
        {
            return 0;
        }

        const auto scene = hs::app::WorldTransportScene(generation, kGameplaySeed);
        Require(scene && !scene->instances.empty(), "Terrain bake world scene is empty");
        const auto action_directory = build / "Actions" / kNamespace;
        const auto stage = ap::CreateStage(action_directory);
        try
        {
            hs::RendererConfig config;
            config.width = 320; config.height = 180;
            if (gpu_validation)
                config.artifact_directory = action_directory;
            config.gpu_validation = gpu_validation;
            config.validation = gpu_validation;
#ifndef NDEBUG
            config.validation = true;
#endif
            config.material_quality = hs::GraphicsQuality::Ultra;
            config.pom_quality = hs::GraphicsOptionalQuality::Ultra;
            config.foliage_quality = hs::GraphicsQuality::Ultra;
            config.texture_quality = hs::GraphicsQuality::Ultra;
            config.geometry_quality = hs::GraphicsQuality::Ultra;
            config.content_generation_id = std::move(generation.id);
            config.product_paths = std::move(generation.product_paths);
            config.product_digests = std::move(generation.product_digests);
            config.content_generation_lease = std::make_shared<const hs::SelectedGeneration>(std::move(generation));

            std::vector<std::string> section_ids, dependencies;
            std::vector<ap::Product> products;
            std::vector<std::byte> metadata;
            const auto write_product = [&](std::string logical_id, std::string filename,
                                           std::span<const std::byte> bytes,
                                           std::vector<std::string> product_dependencies) {
                ap::WriteBytes(stage / filename, bytes);
                return ap::Product{std::move(logical_id), std::move(filename), ap::Sha256Bytes(bytes),
                                   bytes.size(), std::move(product_dependencies)};
            };
            rd::TextureDescription description{};
            Require(hs::D3D12Renderer::BakeTerrainSurfacePages(std::move(config), scene,
                [&](std::span<const rd::TerrainSurfaceSection> incoming, rd::TextureDescription global,
                    std::array<std::byte, 32> identity) {
                    description = global;
                    description.format = rd::kTextureFormatBc7;
                    for (std::size_t i = 0; i < incoming.size(); ++i)
                        for (std::size_t plane = 0; plane < 3; ++plane) section_ids.push_back(SectionId(i, plane));
                    metadata = rd::BuildTerrainSurfacePackage(
                        hs::MakeAssetId(kMetadata), section_ids, incoming, description, identity);
                    return hs::Result::Success();
                },
                [&](std::uint32_t index, std::span<const rd::TextureSubresourceView> subresources) {
                    auto local = description; local.array_size = 1;
                    const auto compressed = encoder.Compress(subresources);
                    // DirectXTex stores tightly packed 2D array slices in slice/mip order.
                    const auto bytes = std::as_bytes(std::span(compressed.GetPixels(), compressed.GetPixelsSize()));
                    Require(bytes.size() % 3 == 0, "Invalid terrain BC7 plane payload size");
                    const auto plane_bytes = bytes.size() / 3;
                    for (std::size_t plane = 0; plane < 3; ++plane)
                    {
                        const auto &id = section_ids[size_t(index) * 3 + plane];
                        const auto plane_payload = bytes.subspan(plane * plane_bytes, plane_bytes);
                        const auto package = rd::BuildTexturePackage(hs::MakeAssetId(id), dependencies, local, plane_payload);
                        const auto filename = SectionFile(index, plane);
                        products.push_back(write_product(id, filename, package, dependencies));
                    }
                    return hs::Result::Success();
                }));
            std::ranges::sort(section_ids);
            products.push_back(write_product(std::string(kMetadata), std::string(kMetadataFile), metadata,
                                             std::move(section_ids)));
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
