#include <hs/core/shipped_content_owner.hpp>
#include <hs/render_data/render_snapshot_storage.hpp>
#include <hs/render_data/render_package.hpp>
#include <hs/core/process_info.hpp>
#include <hs/game_rules/simulation_rules_io.hpp>
#include <hs/game_rules/simulation_rules.hpp>
#include <hs/presentation/environment_layout.hpp>
#include <hs/render_data/surface_layout.hpp>
#include <hs/render_data/renderer_catalog.hpp>
#include <hs/renderer/renderer.hpp>
#include <hs/runtime/shipping_content.hpp>
#include "view_fixture.hpp"
#include "../Common/world_transport_scene.hpp"
#include <hs/jobs/task_system.hpp>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

extern "C"
{
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}

namespace
{
using Json = nlohmann::ordered_json;

void Require(const hs::Result &result)
{
    if (!result)
        throw std::runtime_error(std::string(result.Subsystem()) + ": " +
                                 std::string(result.Message()));
}

struct TestWindow
{
    HWND handle{};
    HINSTANCE instance{GetModuleHandleW(nullptr)};
    bool registered{};
    static constexpr auto class_name = L"ProjectHSRenderTest";

    void Create(std::uint32_t width, std::uint32_t height)
    {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = instance;
        window_class.lpszClassName = class_name;
        if (!RegisterClassW(&window_class))
            throw std::runtime_error("RegisterClassW failed");
        registered = true;
        RECT rectangle{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        if (!AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE))
            throw std::runtime_error("AdjustWindowRect failed");
        handle = CreateWindowExW(0, class_name, L"Project HS render validation",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
            nullptr, nullptr, instance, nullptr);
        if (!handle)
            throw std::runtime_error("CreateWindowExW failed");
    }

    ~TestWindow()
    {
        if (handle) DestroyWindow(handle);
        if (registered) UnregisterClassW(class_name, instance);
    }
};

std::uint32_t Number(std::string_view value, std::uint32_t minimum,
                     std::uint32_t maximum)
{
    std::uint32_t number{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        number < minimum || number > maximum)
        throw std::runtime_error("invalid numeric argument: " + std::string(value));
    return number;
}

std::shared_ptr<const hs::StaticRenderScene> Scene(std::string_view generation)
{
    auto scene = std::make_unique<hs::StaticRenderSceneStorage>();
    scene->content_generation_id = generation;
    std::uint64_t identity = 100;
    const auto add = [&](hs::RenderMesh mesh, hs::Float3 position,
                         hs::Float3 scale, std::uint32_t variant) {
        hs::RenderInstance instance;
        instance.mesh = mesh;
        instance.position = position;
        instance.scale = scale;
        instance.environment_variant = variant;
        instance.environment_seed = 0x48535254u;
        instance.stable_id = identity++;
        scene->instances.push_back(instance);
    };
    add(hs::RenderMesh::Ground, {0, -.055f, 0}, {50, .1f, 50}, 0);
    for (std::uint32_t variant = 0; variant < 3; ++variant)
    {
        const auto x = static_cast<float>(variant) * 5.f - 5.f;
        add(hs::RenderMesh::TreeTrunk, {x, 0, 3}, {1, 1, 1}, variant);
        add(hs::RenderMesh::TreeCanopy, {x, 0, 3}, {1, 1, 1}, variant);
    }
    for (std::uint32_t variant = 0; variant < 4; ++variant)
    {
        const auto x = static_cast<float>(variant) * 3.f - 4.5f;
        add(hs::RenderMesh::Rock, {x, 0, -3}, {.8f, .8f, .8f}, variant);
        add(hs::RenderMesh::Grass, {x, 0, -6}, {1, 1, 1}, variant);
    }
    return hs::FreezeStaticRenderScene(std::move(scene));
}

hs::AssetId SelectedArcherAsset(const hs::RendererConfig &config)
{
    const auto asset = hs::MakeAssetId("render/character/archer.rpkg");
    if (std::ranges::find(config.packed_characters, asset.value,
            [](const auto &row) { return row.asset_id.value; }) == config.packed_characters.end())
        throw std::runtime_error("shipping renderer catalog lacks the Archer character");
    return asset;
}

std::shared_ptr<const hs::StaticRenderScene> HzbScene(std::string_view generation)
{
    auto scene = std::make_unique<hs::StaticRenderSceneStorage>();
    scene->content_generation_id = generation;
    hs::RenderInstance target;
    target.mesh = hs::RenderMesh::Rock;
    target.position = {0, 0, 4};
    target.scale = {.35f, .35f, .35f};
    target.environment_variant = 0;
    target.environment_seed = 0x485A4254u;
    target.stable_id = 0x485A4201ull;
    scene->instances.push_back(target);
    return hs::FreezeStaticRenderScene(std::move(scene));
}

void ValidateVisibilityCommandCapture(const std::filesystem::path &path,
    std::span<const hs::RenderInstance> static_instances,
    std::span<const hs::RenderInstance> dynamic_instances, std::uint32_t frame, Json &receipt)
{
    const auto diagnostic = Json::parse(std::ifstream(path));
    const auto &metadata = diagnostic.at("visibility_commands");
    const auto &masks = metadata.at("range_flag_masks");
    const auto mesh_eligible = masks.at("mesh_eligible").get<std::uint32_t>();
    const auto lod_family = masks.at("lod_family").get<std::uint32_t>();
    const auto empty_source = masks.at("empty_source").get<std::uint32_t>();
    const auto static_source = masks.at("static_source").get<std::uint32_t>();
    if (!mesh_eligible || !lod_family || !empty_source || !static_source ||
        (mesh_eligible & (lod_family | empty_source | static_source)) ||
        (lod_family & (empty_source | static_source)) || (empty_source & static_source))
        throw std::runtime_error("visibility range flag masks are invalid");
    const auto buckets = metadata.at("bucket_count").get<std::size_t>();
    const auto views = metadata.at("active_views").get<std::size_t>();
    const auto expected_views = dynamic_instances.empty() ? 4u : 1u;
    if (views != expected_views || metadata.at("command_stride") != 24 ||
        metadata.at("range_stride") != 32 || metadata.at("range_count_per_set") != buckets + 1)
        throw std::runtime_error("visibility command capture layout mismatch");
    const auto read = [&](const char *key, std::size_t words) {
        std::ifstream file(path.parent_path() / metadata.at(key).get<std::string>(), std::ios::binary | std::ios::ate);
        if (!file || file.tellg() != std::streamoff(words * sizeof(std::uint32_t)))
            throw std::runtime_error("visibility command capture byte count mismatch");
        std::vector<std::uint32_t> values(words);
        file.seekg(0);
        file.read(reinterpret_cast<char *>(values.data()), static_cast<std::streamsize>(words * sizeof(std::uint32_t)));
        if (!file) throw std::runtime_error("visibility command capture read failed");
        return values;
    };
    const auto ranges = read("visibility_ranges", 3 * (buckets + 1) * 8);
    const auto static_draw = read("static_draw_args", buckets * views * 6);
    const auto dynamic_draw = read("dynamic_draw_args", buckets * views * 6);
    const auto rejected = read("static_rejected_args", buckets * 6);
    const auto retest = read("static_retest_args", buckets * 6);
    const auto &records = diagnostic.at("records");
    const std::size_t environment_buckets = hs::render_data::kEnvironmentMaterialNames.size() *
        hs::render_data::kEnvironmentGeometryLodCount;
    if (environment_buckets >= buckets) throw std::runtime_error("visibility bucket catalog is incomplete");
    const auto present = [&](std::span<const hs::RenderInstance> instances, std::size_t bucket) {
        return std::ranges::any_of(instances, [&](const auto &instance) {
            if (hs::render_data::IsEnvironmentMesh(instance.mesh))
                return bucket / 3 == hs::render_data::EnvironmentGeometryOrdinal(instance.mesh, instance.environment_variant) &&
                    bucket < environment_buckets;
            return instance.mesh == hs::RenderMesh::Archer && bucket == environment_buckets;
        });
    };
    const auto verify = [&](const std::vector<std::uint32_t> &commands, std::size_t set,
                            std::size_t view_count, bool is_static) {
        for (std::size_t bucket = 0; bucket < buckets; ++bucket)
        {
            const auto *range = ranges.data() + (set * (buckets + 1) + bucket) * 8;
            const bool expected = present(is_static ? static_instances : dynamic_instances, bucket);
            if (((range[6] & empty_source) == 0) != expected || range[3] != buckets ||
                ((range[6] & static_source) != 0) != (is_static && !expected) ||
                ((range[6] & lod_family) != 0) != (set != 2 && bucket < environment_buckets))
                throw std::runtime_error("visibility source range disagrees with fixture input");
            for (std::size_t view = 0; view < view_count; ++view)
            {
                const auto *command = commands.data() + (view * buckets + bucket) * 6;
                if (!expected)
                {
                    if (std::any_of(command, command + 6, [](auto value) { return value != 0; }))
                        throw std::runtime_error("empty source left a nonzero indirect command");
                    continue;
                }
                const auto record_id = command[0] >> 2;
                if ((command[0] & 3u) != view || record_id != range[4] + view || record_id >= records.size())
                    throw std::runtime_error("indirect command record/view encoding mismatch");
                const auto &record = records.at(record_id);
                if (record.at("id") != record_id || record.at("refsource") != (set == 0 ? 1 : set == 1 ? 2 : 3))
                    throw std::runtime_error("indirect command refers to the wrong geometry source");
                const auto mesh = bucket < environment_buckets
                    ? hs::render_data::EnvironmentMeshForOrdinal(static_cast<std::uint32_t>(bucket / 3))
                    : hs::RenderMesh::Archer;
                if (record.at("mesh").get<std::uint32_t>() != static_cast<std::uint32_t>(mesh))
                    throw std::runtime_error("indirect command geometry mesh does not match the input bucket");
                if (record.at("index_count").get<std::uint32_t>() != range[2] ||
                    record.at("meshlet_count").get<std::uint32_t>() != range[5] ||
                    (record.at("flags").get<std::uint32_t>() & mesh_eligible) != (range[6] & mesh_eligible))
                    throw std::runtime_error("indirect range does not match its geometry record");
                const auto count = record.at("count").get<std::uint32_t>();
                if (count > range[1] || command[4] || command[5])
                    throw std::runtime_error("indirect command count/reserved fields invalid");
                if ((range[6] & mesh_eligible) != 0)
                {
                    const auto groups = std::uint64_t(range[5]) * count;
                    if (command[1] != std::min<std::uint64_t>(groups, 65535u) ||
                        command[2] != (groups ? (groups + 65534u) / 65535u : 1u) || command[3] != 1)
                        throw std::runtime_error("mesh indirect command encoding mismatch");
                }
                else if (command[1] != range[2] || command[2] != count ||
                         command[3] != record.at("first_index").get<std::uint32_t>())
                    throw std::runtime_error("indexed indirect command encoding mismatch");
            }
        }
    };
    verify(static_draw, 0, views, true);
    verify(dynamic_draw, 1, views, false);
    verify(retest, 2, 1, true);
    std::uint64_t meadow_rejected{};
    const auto meadow_begin = hs::render_data::EnvironmentGeometryOrdinal(hs::RenderMesh::MeadowSurface, 0) * 3u;
    for (std::size_t bucket = 0; bucket < buckets; ++bucket)
    {
        const auto *command = rejected.data() + bucket * 6;
        const auto *range = ranges.data() + bucket * 8;
        if (!present(static_instances, bucket))
        {
            if (std::any_of(command, command + 6, [](auto value) { return value != 0; }))
                throw std::runtime_error("empty static source left a rejected command");
        }
        else if (command[0] || command[1] != range[2] || command[2] > range[1] ||
                 command[3] || command[4] || command[5])
            throw std::runtime_error("static rejected command encoding mismatch");
        if (bucket >= meadow_begin && bucket < meadow_begin + 3u) meadow_rejected += command[2];
    }
    if ((frame == 16 || frame == 17 || frame >= 22) && meadow_rejected == 0)
        throw std::runtime_error("empty dynamic source erased or failed to produce the occluded static meadow command");
    receipt.push_back({{"frame", frame}, {"active_views", views},
        {"static_instances", static_instances.size()}, {"dynamic_instances", dynamic_instances.size()},
        {"meadow_rejected", meadow_rejected}});
}

Json ValidationStatusReceipt(const hs::RendererValidationStatus &status)
{
    return {{"d3d12_debug_interface", {{"acquisition_attempted", status.debug_interface_acquisition_attempted},
            {"acquired", status.debug_interface_acquired}}},
        {"gpu_validation_interface_queried", status.gpu_validation_interface_queried},
        {"message_collection", {{"attempted", status.message_collection_attempted},
            {"hresult", status.message_collection_hresult}}}};
}

} // namespace

int main(int argc, char **argv)
{

    Json receipt{{"schema_version", 1}, {"runner", "render-only"},
        {"fixture", "static-environment-moving-player-v1"}, {"execution_valid", false}};
    std::filesystem::path artifacts;
    bool temporary_artifacts{};
    std::string failure_code = "HS_RENDER_ARGUMENT_INVALID";
    int exit_code = 1;
    const auto write_receipt = [&] {
        if (!temporary_artifacts && !artifacts.empty() && artifacts.is_absolute() && std::filesystem::is_directory(artifacts))
        {
            std::ofstream output(artifacts / "result.json");
            output << receipt.dump(2) << '\n';
            output.close();
            if (!output)
            {
                std::cerr << "Could not write render result: " << artifacts / "result.json" << '\n';
                return 1;
            }
        }
        if (exit_code != 0)
            std::cerr << receipt.value("error_code", failure_code) << ": "
                      << receipt.value("error", std::string{}) << '\n';
        if (temporary_artifacts)
        {
            if (exit_code != 0) std::cerr << "Render diagnostics: " << artifacts << '\n';
            else
            {
                std::error_code error;
                std::filesystem::remove_all(artifacts, error);
                if (error) { std::cerr << error.message() << '\n'; return 1; }
            }
        }
        return exit_code;
    };
    try
    {
        hs::RendererConfig config;

        config.vsync = false;
        config.interpolate = false;
        config.devtools_visible = false;
        std::uint32_t frames = 60;

        bool persistent_scene_probe{};

        bool residency_streaming_probe{};

        bool require_multi_queue{};

        bool owned_view_probe{};

        for (int index = 1; index < argc; ++index)
        {
            const std::string_view arg{argv[index]};
            if (arg.starts_with("--artifacts="))
            {
                artifacts = arg.substr(12);
                if (!artifacts.is_absolute()) throw std::runtime_error("--artifacts requires an absolute path");
            }
            else if (arg.starts_with("--frames=")) {
                frames = Number(arg.substr(9), 1, 600);

            }
            else if (arg == "--warp") config.warp = true;
            else if (arg == "--validation") config.validation = true;
            else if (arg == "--gpu-validation") config.validation = config.gpu_validation = true;
            else if (arg == "--barriers=legacy") config.barrier_mode = hs::BarrierMode::Legacy;
            else if (arg == "--barriers=enhanced") config.barrier_mode = hs::BarrierMode::Enhanced;
            else if (arg.starts_with("--geometry=")) {
                const auto value = arg.substr(11);
                if (value == "auto") config.geometry_frontend = hs::GeometryFrontend::Automatic;
                else if (value == "indexed") config.geometry_frontend = hs::GeometryFrontend::Indexed;
                else if (value == "mesh") config.geometry_frontend = hs::GeometryFrontend::MeshShader;
                else throw std::runtime_error("geometry must be auto, indexed, or mesh: " +
                                              std::string(value));
            }
            else if (arg == "--surface=compact") config.surface_frontend = hs::SurfaceFrontend::CompactDeferred;
            else if (arg == "--surface=visibility") config.surface_frontend = hs::SurfaceFrontend::VisibilityBuffer;
            else if (arg == "--persistent-scene-probe") persistent_scene_probe = true;
            else if (arg == "--require-multi-queue") require_multi_queue = true;
            else if (arg == "--residency-streaming-probe") residency_streaming_probe = true;
            else if (arg == "--owned-view-probe") owned_view_probe = true;
            else if (arg.starts_with("--resolution=")) {
                const auto value = arg.substr(13);
                const auto separator = value.find('x');
                if (separator == std::string_view::npos)
                    throw std::runtime_error("resolution must be WIDTHxHEIGHT");
                config.width = Number(value.substr(0, separator), 640, 7680);
                config.height = Number(value.substr(separator + 1), 360, 4320);
            }
            else throw std::runtime_error("unknown argument: " + std::string(arg));
        }
        config.capture_timing_stream = owned_view_probe;
        receipt["geometry_frontend_requested"] = config.geometry_frontend == hs::GeometryFrontend::Indexed
            ? "indexed" : config.geometry_frontend == hs::GeometryFrontend::MeshShader ? "mesh" : "auto";
        receipt["surface_frontend"] = config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer
            ? "visibility" : "compact";


        if (owned_view_probe && frames != 3)
            throw std::runtime_error("--owned-view-probe requires exactly 3 frame packages");
        if (persistent_scene_probe && frames < 9)
            throw std::runtime_error("--persistent-scene-probe requires at least 9 frames");
        if (persistent_scene_probe && config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer && frames < 11)
            throw std::runtime_error("Visibility --persistent-scene-probe requires at least 11 frames for live Cluster Retest");

        if (static_cast<unsigned>(persistent_scene_probe) + static_cast<unsigned>(residency_streaming_probe) +
                static_cast<unsigned>(owned_view_probe) > 1)
            throw std::runtime_error("render-only probes are mutually exclusive");

        if (persistent_scene_probe || residency_streaming_probe)
        {
            // These probes compare scene/material/skin publications. Temporal
            // reconstruction and unrelated high-quality effects add costly driver
            // instrumentation without exercising those publication contracts.
            config.vfx_quality = hs::VfxQuality::Low;
            config.temporal_reconstruction = false;
            config.ao_mode = hs::GraphicsAoMode::Off;
            config.contact_shadows = false;
            config.reflection_mode = hs::GraphicsReflectionMode::Off;
            config.volumetric_quality = hs::GraphicsOptionalQuality::Off;
        }
        if (residency_streaming_probe)
        {
            config.gi_mode = hs::GraphicsGiMode::Off;
            config.bloom = false;
        }

        if (owned_view_probe)
        {
            config.vfx_quality = hs::VfxQuality::High;
            config.volumetric_quality = hs::GraphicsOptionalQuality::Medium;
        }
        receipt["fixture"] = owned_view_probe ? "owned-view-history-v1" : persistent_scene_probe
            ? "persistent-gpu-scene-v2" : residency_streaming_probe ? "residency-streaming-v3"
            : "static-environment-moving-player-v1";

        const bool export_diagnostics = !artifacts.empty();
        if (export_diagnostics)
        {
            std::filesystem::create_directories(artifacts);
            config.artifact_directory = artifacts;
        }
        else if (owned_view_probe || persistent_scene_probe)
        {
            const auto scratch = std::filesystem::temp_directory_path() /
                std::format("ProjectHS-render-{}-{}", GetCurrentProcessId(), GetTickCount64());
            if (!std::filesystem::create_directory(scratch))
                throw std::runtime_error("Cannot acquire a private render test directory");
            artifacts = scratch;
            temporary_artifacts = true;
        }
        const auto &image = hs::shipping::ImageInfo();
        auto owner = std::make_shared<hs::ShippedContentOwner>();
        failure_code = "HS_SHIPPING_IMAGE_INVALID";
        Require(hs::shipping::OpenImage(hs::CurrentExecutableDirectory(), image, *owner));
        std::vector<std::byte> renderer_table_bytes;
        Require(hs::shipping::ReadRange(*owner,
            image.tables[static_cast<std::size_t>(hs::shipping::ShippingTable::Renderer)], renderer_table_bytes));
        std::shared_ptr<const hs::render_data::RendererCatalogData> renderer_catalog;
        Require(hs::render_data::DecodeVerifiedRendererCatalog(renderer_table_bytes, owner->Size(), renderer_catalog));
        config.packed_content = owner;
        config.content_generation_lease = owner;
        config.UseRendererCatalog(renderer_catalog);
        config.content_generation_id.reserve(64);
        constexpr char hex[] = "0123456789abcdef";
        for (std::size_t i = 16; i != 48; ++i)
        {
            const auto byte = std::to_integer<unsigned>(image.header[i]);
            config.content_generation_id += hex[byte >> 4];
            config.content_generation_id += hex[byte & 15];
        }
        receipt["generation_id"] = config.content_generation_id;
        receipt["generation_scope"] = "shipping-image";
        receipt["packed_generation_maps_empty"] = config.product_paths.empty() && config.product_digests.empty();
        receipt["packed_renderer_products"] = config.packed_bulks.size();
        receipt["packed_renderer_textures"] = config.packed_textures.size();
        receipt["packed_renderer_geometry"] = config.packed_geometry.size();
        receipt["packed_renderer_materials"] = config.packed_materials.size();
        receipt["packed_renderer_characters"] = config.packed_characters.size();
        receipt["packed_renderer_animations"] = config.packed_animations.size();
        receipt["packed_renderer_source_revisions"] = config.packed_source_revisions.size();
        receipt["packed_renderer_revision_bulks"] = config.packed_revision_bulks.size();
        receipt["packed_renderer_volumes"] = std::ranges::count(config.packed_textures,
            hs::render_data::TextureDimension::Texture3D, &hs::render_data::TextureDescriptor::dimension);
        receipt["packed_image_bytes"] = owner->Size();
        const auto owned_view_archer_asset = owned_view_probe ? SelectedArcherAsset(config) : hs::AssetId{};
        std::shared_ptr<const hs::StaticRenderScene> world_scene;
        if (owned_view_probe)
        {
            failure_code = "HS_RENDER_WORLD_TRANSPORT_SCENE_INVALID";
            const auto has_bulk = [&](std::string_view logical_id) {
                const auto asset = hs::MakeAssetId(logical_id);
                return std::ranges::find(config.packed_bulks, asset.value,
                    [](const auto &row) { return row.asset_id.value; }) != config.packed_bulks.end();
            };
            if (!has_bulk(config.scene_hlod_product_id) || !has_bulk("render/lighting/local_probes.spkg"))
                throw std::runtime_error("shipping renderer catalog lacks required owned-view scene products");
            std::vector<std::byte> simulation_table_bytes;
            Require(hs::shipping::ReadRange(*owner,
                image.tables[static_cast<std::size_t>(hs::shipping::ShippingTable::Simulation)], simulation_table_bytes));
            auto rules = std::make_unique<hs::SimulationRules>();
            Require(hs::DecodeSimulationRules(simulation_table_bytes, *rules));
            world_scene = hs::app::WorldTransportScene(*rules, config.content_generation_id, 0);
            receipt["owned_view_scene"] = {
                {"layout_abi", world_scene->layout_abi},
                {"eligible_static_member_count", world_scene->eligible_static_members.size()},
                {"static_instance_count", world_scene->instances.size()},
                {"immutable_scene_frames", frames},
            };
        }
        TestWindow window;
        failure_code = "HS_RENDER_WINDOW_FAILED";
        { window.Create(config.width, config.height); }
        config.window = window.handle;
        hs::TaskSystem recording_workers(4);
        config.task_system = &recording_workers;
        hs::D3D12Renderer renderer;
        std::vector<std::byte> startup_font;
        failure_code = "HS_SHIPPING_IMAGE_INVALID";
        Require(hs::shipping::ReadFont(*owner, image, startup_font));
        config.font_bytes = startup_font;
        failure_code = "HS_RENDER_INITIALIZE_FAILED";
        receipt["validation_requested"] = {{"debug_layer", config.validation},
            {"gpu_based_validation", config.gpu_validation}};
        {
            const auto initialized = renderer.Initialize(config);
            if (!initialized)
            {
                const auto status = renderer.ValidationStatus();
                if (export_diagnostics) receipt["validation_status"] = ValidationStatusReceipt(status);
                if (status.debug_interface_acquisition_attempted &&
                    ((status.debug_layer_requested && !status.debug_interface_acquired) ||
                     (status.gpu_validation_requested &&
                        (!status.debug_interface_acquired || !status.gpu_validation_interface_queried))))
                    failure_code = status.gpu_validation_requested
                        ? "HS_RENDER_GBV_PREREQUISITE_FAILED" : "HS_RENDER_DEBUG_LAYER_PREREQUISITE_FAILED";
                Require(initialized);
            }
        }
        config.font_bytes = {};
        std::vector<std::byte>().swap(startup_font);
        if (residency_streaming_probe)
        {
            failure_code = "HS_RENDER_RESIDENCY_STREAMING_FAILED";
            const auto initial_residency = renderer.ResidencyStats();
            if (initial_residency.geometry_streaming_requests != 0 ||
                initial_residency.geometry_streaming_uploads != 0 ||
                initial_residency.geometry_streaming_publications != 0)
                throw std::runtime_error("Geometry streamed before any scene demand.");
            receipt["geometry_initial_requests"] = initial_residency.geometry_streaming_requests;
        }
        const auto scene = Scene(config.content_generation_id);
        const auto hzb_scene = HzbScene(config.content_generation_id);
        hs::RenderInstance first;
        first.mesh = hs::RenderMesh::Archer;
        first.scale = {.97174913f, .97174913f, .97174913f};
        first.stable_id = 1;

        hs::RenderInstance second = first;
        second.stable_id = 2;
        second.mesh = hs::RenderMesh::Rock;
        second.environment_variant = 0;
        second.position.x = 2.0f;
        hs::RenderInstance replacement = first;
        replacement.stable_id = 3;
        replacement.mesh = hs::RenderMesh::Rock;
        replacement.environment_variant = 0;
        replacement.position.x = -2.0f;
        hs::RenderInstance occluder;
        occluder.stable_id = 99;
        // Area flattens cube height by 0.1 and sits below the target's sightline.
        // Use the opaque builtin box across the orbit camera's target projection.
        occluder.mesh = hs::RenderMesh::Enemy;
        occluder.position = {0.0f, 5.0f, -9.25f};
        occluder.scale = {12.0f, 16.0f, 3.0f};
        occluder.color_rgba = 0xff303030u;
        std::shared_ptr<const hs::StaticRenderScene> visibility_empty_scene, visibility_filled_scene;
        if (persistent_scene_probe && frames >= 24)
        {
            auto visibility_empty = std::make_unique<hs::StaticRenderSceneStorage>();
            visibility_empty->content_generation_id = config.content_generation_id;
            auto visibility_filled = std::make_unique<hs::StaticRenderSceneStorage>(*visibility_empty);
            auto static_occluder = second;
            static_occluder.stable_id = 0x56495301u;
            static_occluder.position = {0, 0, 3};
            static_occluder.scale = {6, 6, 6};
            auto static_meadow = second;
            static_meadow.stable_id = 0x56495302u;
            static_meadow.mesh = hs::RenderMesh::MeadowSurface;
            static_meadow.environment_variant = 0;
            static_meadow.position = {0, 0, 10};
            static_meadow.scale = {.04f, .04f, .04f};
            visibility_filled->instances = {static_occluder, static_meadow};
            visibility_empty_scene = hs::FreezeStaticRenderScene(std::move(visibility_empty));
            visibility_filled_scene = hs::FreezeStaticRenderScene(std::move(visibility_filled));
        }
        std::vector<hs::RenderInstance> previous;
        std::vector<hs::RenderInstance> current = persistent_scene_probe
            ? std::vector<hs::RenderInstance>{first, second} : std::vector<hs::RenderInstance>{first};

        if (persistent_scene_probe)
            for (auto &source : current) source.revisions = {11, 22, 33, 44};
        hs::RenderSnapshotPair pair;
        const auto fixture_sun = hs::EnvironmentSun();
        pair.current.lights = std::span(&fixture_sun, 1);
        pair.current.static_scene = owned_view_probe ? world_scene : scene;
        pair.current.header.session_id = 1;
        pair.current.camera.target = {0, 1, 0};
        pair.current.camera.yaw_degrees = 0;
        pair.current.camera.pitch_degrees = 22;
        pair.current.camera.distance = 22.0f;

        std::array<hs::LightView, 3> world_lights{};
        world_lights[1] = hs::LightView{
            .quantity = 90000.0f,
            .color = {1.0f, 0.2f, 0.05f},
            .stable_id = 2,
            .type = hs::LightType::Point,
            .unit = hs::LightUnit::Lumen,
            .position = {8.0f, 8.0f, -2.0f},
            .range = 32.0f,
            .emitter_radius = 0.25f,
            .shadow_flags = hs::kLightShadowRequested};
        world_lights[2] = hs::LightView{
            .direction = {0.65f, -0.75f, 0.3f},
            .quantity = 90000.0f,
            .color = {0.05f, 0.3f, 1.0f},
            .stable_id = 3,
            .type = hs::LightType::Spot,
            .unit = hs::LightUnit::Lumen,
            .position = {-8.0f, 9.0f, -1.0f},
            .range = 32.0f,
            .emitter_radius = 0.25f,
            .inner_cone_radians = 0.35f,
            .outer_cone_radians = 0.65f,
            .shadow_flags = hs::kLightShadowRequested};

        hs::RendererToolFrameResult frame_result;
        failure_code = "HS_RENDER_FRAME_FAILED";
        hs::RendererQueueStats queue_totals;
        constexpr std::array residency_texture_qualities{
            hs::GraphicsQuality::Ultra, hs::GraphicsQuality::Low, hs::GraphicsQuality::Ultra};
        std::array<std::uint64_t, residency_texture_qualities.size()> residency_quality_generations{};
        std::array<std::uint32_t, residency_texture_qualities.size()> residency_quality_frames{};
        std::array<std::uint32_t, residency_texture_qualities.size()> residency_quality_mips{};
        std::array<std::uint64_t, residency_texture_qualities.size()> residency_quality_publication_baselines{};
        std::array<bool, residency_texture_qualities.size()> residency_quality_applied{};
        std::array<bool, residency_texture_qualities.size()> residency_quality_settled{};
        std::size_t residency_quality_phase{};
        Json residency_quality_samples = Json::array();
        std::uint32_t rendered_frame_count{};
        const auto residency_streaming_complete = [&] {
            if (!residency_streaming_probe ||
                residency_quality_phase != residency_texture_qualities.size() ||
                std::ranges::any_of(residency_quality_settled, [](bool settled) { return !settled; }))
                return false;
            const auto stats = renderer.ResidencyStats();
            const auto texture = renderer.GetTextureStreamingStatus(
                "render/texture/environment_terrain_gaussian.rpkg");
            return stats.budget_valid && stats.local_budget_bytes != 0 &&
                stats.tracked_live_bytes != 0 && stats.tracked_reclaimed_bytes != 0 &&
                stats.streaming_publications != 0 &&
                stats.streaming_uploads >= stats.streaming_publications &&
                stats.streaming_requests >= stats.streaming_uploads &&
                texture.registered && texture.publications != 0 && !texture.pending &&
                texture.publication_complete &&
                texture.actual_resident_mip == texture.effective_desired_mip &&
                texture.actual_resident_mip == texture.target_mip && texture.reclaims != 0 &&
                stats.geometry_streaming_requests != 0 &&
                stats.geometry_streaming_uploads == stats.geometry_streaming_requests &&
                stats.geometry_streaming_publications == stats.geometry_streaming_requests &&
                stats.geometry_streaming_reclaims >= stats.geometry_streaming_requests;
        };
        ([&] {
            if (owned_view_probe)
            {
                pair.current.instances = current;
                pair.current.header.tick = 1;
                hs::render_test::OwnedViewProbeEvidence evidence;
                Require(hs::render_test::RunOwnedViewProbe(renderer, pair, artifacts, export_diagnostics,
                    owned_view_archer_asset, evidence));
                frame_result = evidence.final_frame;
                if (export_diagnostics) receipt["owned_view_probe"] = hs::render_test::OwnedViewProbeReceipt(evidence);
                receipt["queue_execution_scope"] = "last_owned_view_submission";
                queue_totals = renderer.LastQueueStats();
                return;
            }
            const auto request_residency_texture_phase = [&](std::size_t phase, std::uint32_t request_frame) {
                auto status = renderer.GetGraphicsSettingsStatus();
                status.requested.texture_quality = residency_texture_qualities[phase];
                status.requested.shadow_quality = residency_texture_qualities[phase];
                residency_quality_generations[phase] = status.generation + 1u;
                residency_quality_frames[phase] = request_frame;
                const auto texture = renderer.GetTextureStreamingStatus(
                    "render/texture/environment_terrain_gaussian.rpkg");
                residency_quality_publication_baselines[phase] = texture.publications;
                Require(renderer.RequestGraphicsSettings({residency_quality_generations[phase],
                    hs::GraphicsSettingsRequestCause::User, status.requested}));
            };
            if (residency_streaming_probe) request_residency_texture_phase(0, 0);
            for (std::uint32_t frame = 1; frame <= frames; ++frame)
            {

                MSG message;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    if (message.message == WM_QUIT) throw std::runtime_error("window closed");
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                pair.previous = pair.current;
                pair.previous.instances = previous;

                pair.has_previous = frame > 1;
                if (persistent_scene_probe)
                {
                    if (frame == 3) current[0].position.x = 1.0f;
                    else if (frame == 4) current.resize(1);
                    else if (frame == 5) current.push_back(replacement);
                    else if (frame == 6) std::ranges::reverse(current);
                    else if (frame == 7)
                    {
                        current = {occluder};
                        pair.current.static_scene = hzb_scene;
                    }
                    else if (frame == 8) current[0].position.x = 30.0f;
                    // Scene publication at frame7 resets Cluster membership. Let
                    // selection/page history settle before a second HZB reveal.
                    else if (config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer && frame == 10)
                        current[0].position = occluder.position;
                    else if (config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer && frame == 11)
                        current[0].position.x = 30.0f;
                    else if (frames >= 24 && frame >= 15)
                    {
                        const bool dynamic_filled = frame >= 18 && frame <= 20;
                        const bool static_filled = !dynamic_filled;
                        pair.current.static_scene = static_filled ? visibility_filled_scene : visibility_empty_scene;
                        current = dynamic_filled ? std::vector<hs::RenderInstance>{second} : std::vector<hs::RenderInstance>{};
                        world_lights[0] = hs::LightView{};
                        world_lights[0].stable_id = 1;
                        world_lights[0].direction = hs::kEnvironmentSunDirection;
                        world_lights[0].quantity = 50000.0f;
                        world_lights[0].color = {1, 1, 1};
                        world_lights[0].shadow_flags = dynamic_filled ? 0 : hs::kLightShadowRequested;
                        pair.current.lights = std::span(world_lights).first(dynamic_filled ? 1u : world_lights.size());
                    }
                }
                else {
                    current[0].position.x = static_cast<float>(frame % 60) / 60.f;
                }

                pair.current.header.tick = frame;
                pair.current.header.simulation_time = persistent_scene_probe
                    ? std::chrono::nanoseconds::zero()
                    : std::chrono::nanoseconds(
                        static_cast<std::int64_t>(frame) * 1000000000ll / 60);

                pair.current.instances = current;
                std::vector<hs::RenderInstance> preview_instances;
                std::vector<hs::AnimationPoseRef> preview_poses;

                auto preview_camera = pair.current.camera;

                if (residency_streaming_probe)
                {
                    // Keep the terrain close enough to request fine mips even at the minimum test resolution.
                    preview_camera.target = {0, 0, 0};
                    preview_camera.pitch_degrees = 60;
                    preview_camera.distance = 0.8f;
                }
                auto main_view = renderer.MainViewInput(preview_camera);

                std::vector<hs::VfxFlashSpawnInput> oit_flashes;
                hs::VfxFlashAppearance flash_appearance{};

                auto preview_pair = pair;

                const hs::RenderPackage render_input{
                    .snapshots = preview_pair,
                    .vfx = {.flashes = oit_flashes,
                        .world_batch = {pair.current.header.session_id, pair.current.header.tick, frame}},
                    .devtools = {.delta_seconds = persistent_scene_probe
                        ? std::optional<float>{0.0f} : std::nullopt, .wait_for_streaming = false},
                    .display_sample = hs::RenderDisplaySample{std::chrono::steady_clock::now(), 1.0f}};
                const auto main_samples = hs::render_test::ViewSamples(std::span(&main_view, 1));

                Require(renderer.RenderViews(render_input, main_samples, frame_result));
                rendered_frame_count = frame;

                if (residency_streaming_probe && residency_quality_phase < residency_texture_qualities.size())
                {
                    const auto settings = renderer.GetGraphicsSettingsStatus();
                    const auto texture = renderer.GetTextureStreamingStatus(
                        "render/texture/environment_terrain_gaussian.rpkg");
                    const auto phase = residency_quality_phase;
                    const bool applied = settings.actual_applied_available &&
                        settings.generation == residency_quality_generations[phase] &&
                        settings.error == hs::ErrorCode::None &&
                        (settings.state == hs::SettingsApplyState::Applied ||
                         settings.state == hs::SettingsApplyState::Confirmed) &&
                        settings.actual_applied.texture_quality == residency_texture_qualities[phase] &&
                        settings.actual_applied.shadow_quality == residency_texture_qualities[phase];
                    residency_quality_applied[phase] = residency_quality_applied[phase] || applied;
                    const bool settled = applied && texture.registered && texture.publication_complete &&
                        !texture.pending && texture.publications > residency_quality_publication_baselines[phase] &&
                        texture.actual_resident_mip == texture.effective_desired_mip &&
                        texture.actual_resident_mip == texture.target_mip &&
                        (phase == 1
                            ? texture.actual_resident_mip >= 2 && texture.actual_resident_mip > residency_quality_mips[0]
                            : texture.actual_resident_mip <= 1 &&
                                (phase != 2 || texture.actual_resident_mip < residency_quality_mips[1]));
                    residency_quality_settled[phase] = settled;
                    residency_quality_samples.push_back({
                        {"frame", frame}, {"phase", phase},
                        {"request_frame", residency_quality_frames[phase]},
                        {"request_generation", residency_quality_generations[phase]},
                        {"settings_generation", settings.generation},
                        {"settings_state", static_cast<unsigned>(settings.state)},
                        {"actual_quality_available", settings.actual_applied_available},
                        {"actual_texture_quality", settings.actual_applied_available
                            ? Json(static_cast<unsigned>(settings.actual_applied.texture_quality)) : Json(nullptr)},
                        {"phase_applied", applied}, {"phase_settled", settled},
                        {"required_mip_direction_observed", settled}, {"registered", texture.registered},
                        {"pending", texture.pending}, {"resident_mip", texture.actual_resident_mip},
                        {"effective_desired_mip", texture.effective_desired_mip},
                        {"target_mip", texture.target_mip}, {"revision", texture.revision},
                        {"requests", texture.requests}, {"uploads", texture.uploads},
                        {"publications", texture.publications}, {"reclaims", texture.reclaims}});
                    if (settled)
                    {
                        residency_quality_mips[phase] = texture.actual_resident_mip;
                        residency_quality_phase = phase + 1u;
                        if (residency_quality_phase < residency_texture_qualities.size())
                        {
                            request_residency_texture_phase(residency_quality_phase, frame + 1u);
                        }
                    }
                }
                if (!frame_result.vfx_world_step_submitted ||
                    frame_result.vfx_world_tick != pair.current.header.tick ||
                    frame_result.vfx_world_session_id != pair.current.header.session_id)
                    throw std::runtime_error("Submitted world-step receipt does not match the rendered session/tick");

                const auto queue_stats = renderer.LastQueueStats();
                queue_totals.direct_batches += queue_stats.direct_batches;
                queue_totals.compute_batches += queue_stats.compute_batches;
                queue_totals.copy_batches += queue_stats.copy_batches;
                queue_totals.cross_queue_waits += queue_stats.cross_queue_waits;
                queue_totals.record_jobs += queue_stats.record_jobs;

                if (persistent_scene_probe && frames >= 24 && frame >= 15 &&
                    config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer)
                {
                    const auto path = artifacts / "visibility-commands.json";
                    Require(renderer.Capture({.path = path, .kind = hs::CaptureKind::GeometryDiagnostics}));
                    ValidateVisibilityCommandCapture(path, pair.current.static_scene->instances, current, frame,
                        receipt["visibility_command_cases"]);
                }
                if (persistent_scene_probe && frames >= 24 && frame == 14 &&
                    config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer)
                {
                    const auto path = artifacts / "hzb-empty-slot-reuse.json";
                    Require(renderer.Capture({.path = path, .kind = hs::CaptureKind::VisibilityDiagnostics}));
                    const auto diagnostic = Json::parse(std::ifstream(path));
                    for (const auto &catalog : diagnostic.at("cluster_commands"))
                        for (const auto &view : catalog.at("views"))
                            if (view.at("view").get<std::uint32_t>() == 4 &&
                                (view.at("published_count").get<std::uint64_t>() != 0 ||
                                 view.at("nonzero_count").get<std::uint64_t>() != 0 ||
                                 view.at("nonzero_outside_published_prefix").get<std::uint64_t>() != 0))
                                throw std::runtime_error("Cluster Retest count survived frame 14 slot reuse");
                    receipt["hzb_cluster_retest"]["empty_after_slot_reuse"] = true;
                }
                if (persistent_scene_probe && frame == 11 &&
                    config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer)
                {
                    failure_code = "HS_RENDER_HZB_CLUSTER_RETEST_FAILED";
                    const auto path = artifacts / "hzb-retest-commands.json";
                    Require(renderer.Capture({.path = path, .kind = hs::CaptureKind::VisibilityDiagnostics}));
                    const auto diagnostic = Json::parse(std::ifstream(path));
                    if (!diagnostic.at("execution_valid").get<bool>())
                        throw std::runtime_error("HZB Cluster Retest diagnostic is invalid");
                    std::uint64_t committed{}, published{}, live{}, vertices{}, mesh_groups{};
                    for (const auto &catalog : diagnostic.at("cluster_commands"))
                    {
                        if (catalog.at("frame").get<std::uint64_t>() != frame ||
                            catalog.at("failure_flags").get<std::uint32_t>() != 0)
                            throw std::runtime_error("HZB Cluster Retest has stale or failed status");
                        committed += catalog.at("committed_records").get<std::uint64_t>();
                        for (const auto &view : catalog.at("views"))
                        {
                            if (view.at("nonzero_outside_published_prefix").get<std::uint64_t>() != 0)
                                throw std::runtime_error("HZB Cluster draw prefix omits live commands");
                            if (view.at("view").get<std::uint32_t>() != 4) continue;
                            const auto count = view.at("published_count").get<std::uint64_t>();
                            if (count < view.at("last_nonzero_exclusive").get<std::uint64_t>())
                                throw std::runtime_error("HZB Cluster Retest suffix excludes a live command");
                            const auto draw_work = view.at("draw_vertex_count_sum").get<std::uint64_t>();
                            const auto mesh_work = view.at("mesh_group_count_sum").get<std::uint64_t>();
                            const bool mesh = catalog.at("geometry_frontend_flags").get<std::uint32_t>() != 0u;
                            if ((mesh ? draw_work : mesh_work) != 0 ||
                                (view.at("nonzero_count").get<std::uint64_t>() != 0 && !(mesh ? mesh_work : draw_work)))
                                throw std::runtime_error("HZB Cluster Retest workload does not match its frontend");
                            published += count;
                            live += view.at("nonzero_count").get<std::uint64_t>();
                            vertices += draw_work;
                            mesh_groups += mesh_work;
                        }
                    }
                    if (!committed || !live || !(vertices || mesh_groups))
                        throw std::runtime_error("HZB reveal did not exercise live Cluster Retest draws");
                    receipt["hzb_cluster_retest"] = {
                        {"frame", frame}, {"diagnostic", path.filename().string()},
                        {"committed", committed}, {"published", published},
                        {"nonzero", live}, {"draw_vertex_count_sum", vertices},
                        {"mesh_group_count_sum", mesh_groups},
                        {"nonzero_outside_published_prefix", 0},
                    };
                }

                previous = current;
                // Stop once every texture quality transition and every demanded
                // geometry request has published and reclaimed its old backing.
                // The larger frame count remains only the failure bound.
                if (residency_streaming_complete()) break;
            }
        })();

        const auto residency_stats = renderer.ResidencyStats();
        const auto representative_texture = renderer.GetTextureStreamingStatus(
            "render/texture/environment_terrain_gaussian.rpkg");
        if (residency_streaming_probe)
        {
            receipt["residency_streaming"] = {
                {"rendered_frames", rendered_frame_count},
                {"frame_bound", frames},
                {"terminated_after_completion", rendered_frame_count < frames},
                {"budget_valid", residency_stats.budget_valid},
                {"local_budget_bytes", residency_stats.local_budget_bytes},
                {"local_usage_bytes", residency_stats.local_usage_bytes},
                {"tracked_live_bytes", residency_stats.tracked_live_bytes},
                {"tracked_retiring_bytes", residency_stats.tracked_retiring_bytes},
                {"tracked_reclaimed_bytes", residency_stats.tracked_reclaimed_bytes},
                {"requests", residency_stats.streaming_requests},
                {"uploads", residency_stats.streaming_uploads},
                {"publications", residency_stats.streaming_publications},
                {"reclaims", residency_stats.streaming_reclaims},
                {"generation", residency_stats.streaming_generation},
                {"state", residency_stats.streaming_state},
                {"texture", {
                    {"logical_id", "render/texture/environment_terrain_gaussian.rpkg"},
                    {"registered", representative_texture.registered},
                    {"pending", representative_texture.pending},
                    {"publication_complete", representative_texture.publication_complete},
                    {"requests", representative_texture.requests},
                    {"uploads", representative_texture.uploads},
                    {"publications", representative_texture.publications},
                    {"reclaims", representative_texture.reclaims},
                    {"revision", representative_texture.revision},
                    {"actual_resident_mip", representative_texture.actual_resident_mip},
                    {"effective_desired_mip", representative_texture.effective_desired_mip},
                    {"target_mip", representative_texture.target_mip},
                }},
                {"geometry_requests", residency_stats.geometry_streaming_requests},
                {"geometry_uploads", residency_stats.geometry_streaming_uploads},
                {"geometry_publications", residency_stats.geometry_streaming_publications},
                {"geometry_reclaims", residency_stats.geometry_streaming_reclaims},
                {"geometry_generation", residency_stats.geometry_streaming_generation},
                {"geometry_state", residency_stats.geometry_streaming_state},
                {"texture_quality_phases", {
                    {"requested_qualities", {static_cast<unsigned>(residency_texture_qualities[0]),
                        static_cast<unsigned>(residency_texture_qualities[1]),
                        static_cast<unsigned>(residency_texture_qualities[2])}},
                    {"request_frames", residency_quality_frames},
                    {"applied", residency_quality_applied},
                    {"settled", residency_quality_settled},
                    {"settled_mips", residency_quality_mips},
                    {"samples", std::move(residency_quality_samples)}}},
            };
            if (std::ranges::any_of(residency_quality_settled, [](bool settled) { return !settled; }))
                throw std::runtime_error("Texture streaming did not publish a settled fine→coarse→fine mip sequence within the probe's frame bound.");
        }

        receipt["queue_execution"] = {
            {"direct_batches", queue_totals.direct_batches},
            {"compute_batches", queue_totals.compute_batches},
            {"copy_batches", queue_totals.copy_batches},
            {"cross_queue_waits", queue_totals.cross_queue_waits},
            {"record_jobs", queue_totals.record_jobs}};
        if (require_multi_queue && (queue_totals.direct_batches == 0 ||
            queue_totals.compute_batches == 0 || queue_totals.copy_batches == 0 ||
            queue_totals.cross_queue_waits == 0 || queue_totals.record_jobs == 0))
            throw std::runtime_error("Production fixture did not execute three queues and worker recording");
        if (residency_streaming_probe)
        {
            failure_code = "HS_RENDER_RESIDENCY_STREAMING_FAILED";
            if (!residency_stats.budget_valid ||
                residency_stats.local_budget_bytes == 0 ||
                residency_stats.tracked_live_bytes == 0 ||
                residency_stats.tracked_reclaimed_bytes == 0)
                throw std::runtime_error("Residency budget accounting did not report a valid live/reclaimed transaction.");
            if (residency_stats.streaming_publications == 0 ||
                residency_stats.streaming_uploads < residency_stats.streaming_publications ||
                residency_stats.streaming_requests < residency_stats.streaming_uploads)
                throw std::runtime_error("Texture streaming transaction conservation failed: requests >= uploads >= publications is false.");
            if (!representative_texture.registered || representative_texture.publications == 0 ||
                representative_texture.pending || !representative_texture.publication_complete ||
                representative_texture.actual_resident_mip != representative_texture.effective_desired_mip ||
                representative_texture.actual_resident_mip != representative_texture.target_mip ||
                representative_texture.reclaims == 0)
                throw std::runtime_error("Texture streaming representative did not complete at its effective mip target and reclaim staging/backing.");
            // View demand selects the families; require completion of actual requests.
            if (residency_stats.geometry_streaming_requests == 0 ||
                residency_stats.geometry_streaming_uploads !=
                    residency_stats.geometry_streaming_requests ||
                residency_stats.geometry_streaming_publications !=
                    residency_stats.geometry_streaming_requests ||
                residency_stats.geometry_streaming_reclaims <
                    residency_stats.geometry_streaming_requests)
                throw std::runtime_error("Geometry streaming did not publish and reclaim every requested geometry family.");
        }

        failure_code = "HS_RENDER_SHUTDOWN_FAILED";
        { Require(renderer.Shutdown()); }

        const auto validation_errors = renderer.ValidationErrorCount();
        const auto native_validation = renderer.ValidationStatus();
        const bool collection_valid = native_validation.MessageCollectionValid();
        receipt["validation_errors"] = validation_errors;
        if (export_diagnostics) receipt["validation_status"] = ValidationStatusReceipt(native_validation);
        receipt["frames"] = residency_streaming_probe ? rendered_frame_count : frames;
        receipt["static_scene_upload_count"] = frame_result.static_scene_upload_count;
        receipt["enhanced_barriers"] = renderer.UsesEnhancedBarriers();

        receipt["execution_valid"] = collection_valid && validation_errors == 0;
        receipt["error_code"] = !collection_valid ? "HS_RENDER_VALIDATION_COLLECTION_FAILED"
            : validation_errors == 0 ? "" : "HS_RENDER_VALIDATION_FAILED";
        exit_code = collection_valid && validation_errors == 0 ? 0 : 1;
    }
    catch (const std::exception &error)
    {
        receipt["error_code"] = failure_code;
        receipt["error"] = error.what();
    }
    return write_receipt();
}
