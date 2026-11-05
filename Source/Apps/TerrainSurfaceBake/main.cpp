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
std::string SectionId(std::size_t index)
{
    return "render/terrain/arena_surface_section_" + std::to_string(index) + ".rpkg";
}
std::string SectionFile(std::size_t index)
{
    return "arena_surface_section_" + std::to_string(index) + ".rpkg";
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
        constexpr std::size_t edge = 2048, slices = 3, mips = 12;
        DirectX::ScratchImage input;
        Require(SUCCEEDED(input.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, edge, edge, slices, mips)),
            "Cannot allocate terrain BC7 input");
        std::size_t offset{};
        for (std::size_t slice = 0; slice < slices; ++slice)
            for (std::size_t mip = 0; mip < mips; ++mip)
            {
                const auto *image = input.GetImage(mip, slice, 0);
                const auto width = std::max<std::size_t>(1, edge >> mip);
                const auto row = width * 4;
                const auto size = row * width;
                Require(image && image->rowPitch >= row && offset <= payload.size() && size <= payload.size() - offset,
                    "Invalid terrain RGBA8 subresource payload");
                for (std::size_t y = 0; y < width; ++y)
                    std::memcpy(image->pixels + y * image->rowPitch, payload.data() + offset + y * row, row);
                offset += size;
            }
        Require(offset == payload.size(), "Terrain RGBA8 payload has trailing bytes");
        DirectX::ScratchImage compressed;
        const auto result = device
            ? DirectX::Compress(device.Get(), input.GetImages(), input.GetImageCount(), input.GetMetadata(),
                DXGI_FORMAT_BC7_UNORM, DirectX::TEX_COMPRESS_DEFAULT, 1.0f, compressed)
            : DirectX::Compress(input.GetImages(), input.GetImageCount(), input.GetMetadata(),
                DXGI_FORMAT_BC7_UNORM, DirectX::TEX_COMPRESS_PARALLEL, 0.5f, compressed);
        Require(SUCCEEDED(result), "Terrain BC7 compression failed");
        std::vector<std::byte> output;
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
            snapshot.ProductCount() != metadata.sections.size() + 1) return false;
        std::vector<std::string> dependencies;
        for (std::size_t i = 0; i < metadata.sections.size(); ++i)
        {
            const auto id = SectionId(i);
            if (metadata.section_products[i] != id ||
                snapshot.ProductMetadata(id).value("dependencies", std::vector<std::string>{}) != dependencies)
                return false;
            fs::path path;
            snapshot.ReadProduct(id, ignored, path);
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
        spec.recipe_id = "project-hs.render.terrain-surface/native-seed-1-bc7-layout2" + encoder.identity;
        spec.recipe_version = 2;
        for (const auto &[id, path] : std::initializer_list<std::pair<std::string_view, fs::path>>{
            {"terrain/world-transport", source / "Source/Apps/Common/world_transport_scene.cpp"},
            {"terrain/layout", source / "Source/Presentation/Private/environment_layout.cpp"},
            {"terrain/layout-contract", source / "Source/Presentation/Public/hs/presentation/environment_layout.hpp"}})
            spec.inputs.push_back(ap::HashInput(std::string(id), path));

        Require(!generation.product_paths.empty(), "Terrain bake input Generation is empty");
        std::set<std::string> visited;
        std::vector<std::string> pending{
            "game-data/simulation-rules", "render/material/terrain.rpkg", "render/meadow_ground.rpkg",
            "render/shader/terrain_resolve.dxil", "render/shader/terrain_resolve_feedback.dxil",
            "render/shader/terrain_page_bake_cs.dxil", "render/shader/terrain_page_mip_cs.dxil"};
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
            std::vector<std::string> section_ids, dependencies;
            std::vector<bool> written;
            std::vector<ap::Product> products;
            rd::TextureDescription description{};
            std::array<std::byte, 32> source_identity{};
            bool begun = false;
            Require(hs::D3D12Renderer::BakeTerrainSurfacePages(config, scene,
                [&](std::span<const rd::TerrainSurfaceSection> incoming, rd::TextureDescription global,
                    std::array<std::byte, 32> identity) {
                    Require(!begun && !incoming.empty() && incoming.size() <= UINT32_MAX / 3 &&
                        global.width == 2048 && global.height == 2048 && global.mip_count == 12 &&
                        global.format == rd::kTextureFormatRgba8 && global.array_size == incoming.size() * 3,
                        "Native terrain surface description is invalid");
                    sections.assign(incoming.begin(), incoming.end());
                    description = global;
                    description.format = rd::kTextureFormatBc7;
                    source_identity = identity;
                    written.resize(sections.size());
                    for (std::size_t i = 0; i < sections.size(); ++i) section_ids.push_back(SectionId(i));
                    begun = true;
                    return hs::Result::Success();
                },
                [&](std::uint32_t index, std::span<const std::byte> payload) {
                    Require(begun && index < sections.size() && !written[index], "Unexpected terrain section callback");
                    auto local = description; local.array_size = 3;
                    const auto compressed = encoder.Compress(payload);
                    const auto package = rd::BuildTexturePackage(hs::MakeAssetId(section_ids[index]), dependencies, local, compressed);
                    const auto filename = SectionFile(index);
                    const auto path = stage / filename;
                    ap::WriteBytes(path, package);
                    auto product = ap::HashProduct(section_ids[index], filename, path);
                    product.dependencies = dependencies;
                    products.push_back(std::move(product));
                    written[index] = true;
                    return hs::Result::Success();
                }));
            Require(begun && std::ranges::all_of(written, [](bool value) { return value; }),
                "Native terrain bake did not complete every section");
            const auto metadata = rd::BuildTerrainSurfacePackage(
                hs::MakeAssetId(kMetadata), section_ids, sections, description, source_identity);
            const auto metadata_path = stage / kMetadataFile;
            ap::WriteBytes(metadata_path, metadata);
            auto product = ap::HashProduct(std::string(kMetadata), std::string(kMetadataFile), metadata_path);
            product.dependencies = section_ids;
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
