#include "../AssetPipeline/action.hpp"
#include <hs/core/cooked_format.hpp>
#include <hs/render_data/volumetric_medium.hpp>
#include <algorithm>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string_view>
namespace
{
namespace ap = hs::asset_pipeline;
namespace rd = hs::render_data;
namespace fs = std::filesystem;
constexpr std::string_view kId = "render/lighting/volumetric_media.rpkg";
void Require(bool valid, std::string_view reason)
{ if (!valid) throw std::runtime_error("hs_render_volumetric_package: " + std::string(reason)); }
void Keys(const ap::Json &object, std::initializer_list<std::string_view> keys)
{
    Require(object.is_object() && object.size() == keys.size(), "invalid object keys");
    for (const auto key : keys) Require(object.contains(std::string(key)), "missing required field");
}
float Scalar(const ap::Json &value)
{ Require(value.is_number(), "expected a physical scalar"); return value.get<float>(); }
void Produce(const fs::path &source, const fs::path &build)
{
    ap::ActionSpec spec;
    spec.action_id = "render.lighting.volumetric-media";
    spec.recipe_id = "project-hs.render.volumetric-media";
    spec.recipe_version = 1;
    spec.namespace_path = "render/lighting/volumetric-media";
    spec.inputs.push_back(ap::HashInput("Schemas/volumetric_medium.schema.json", source / "Schemas/volumetric_medium.schema.json"));
    spec.inputs.push_back(ap::HashInput("ContentSource/Lighting/volumetrics.json", source / "ContentSource/Lighting/volumetrics.json"));
    const auto root = ap::Json::parse(ap::ReadBytes(source / "ContentSource/Lighting/volumetrics.json"));
    Keys(root, {"version", "units", "volumes"});
    Require(root.at("version") == 1 && root.at("units") == "metres_scene_linear_rec709" &&
        root.at("volumes").is_array() && root.at("volumes").size() <= 256, "invalid source ABI/units/count");
    std::vector<rd::VolumetricMedium> media;
    for (const auto &record : root.at("volumes"))
    {
        Keys(record, {"id","generation","shape","world_to_local","density","extinction_per_metre","scattering_per_metre","phase_g"});
        Require(record.at("id").is_string() && !record.at("id").get_ref<const std::string &>().empty() &&
            record.at("generation").is_number_unsigned() && record.at("generation").get<std::uint64_t>() <= UINT32_MAX,
            "invalid stable identity");
        rd::VolumetricMedium medium;
        medium.stable_id = hs::MakeAssetId(record.at("id").get<std::string>()).value;
        medium.generation = record.at("generation").get<std::uint32_t>();
        const auto &shape = record.at("shape");
        Require(shape == "global" || shape == "box" || shape == "sphere", "invalid shape");
        medium.shape = shape == "global" ? 0u : shape == "box" ? 1u : 2u;
        const auto &matrix = record.at("world_to_local"), &scattering = record.at("scattering_per_metre");
        Require(matrix.is_array() && matrix.size() == 16 && scattering.is_array() && scattering.size() == 3, "invalid physical vectors");
        for (std::size_t i = 0; i != 16; ++i) medium.world_to_local[i] = Scalar(matrix[i]);
        for (std::size_t i = 0; i != 3; ++i) medium.scattering[i] = Scalar(scattering[i]);
        medium.density_extinction = {Scalar(record.at("density")), Scalar(record.at("extinction_per_metre")), Scalar(record.at("phase_g")), 0};
        media.push_back(medium);
    }
    const auto payload = rd::EncodeVolumetricMedia(media);
    const rd::ChunkSource chunk{static_cast<std::uint32_t>(rd::ChunkType::VolumetricVolumes), 1, true, payload, media.size(), rd::kVolumetricMediumStride};
    const auto bytes = rd::BuildPackage(hs::MakeAssetId(kId), {}, std::span(&chunk, 1));
    const auto package = rd::ReadPackage(bytes, hs::MakeAssetId(kId));
    Require(package.chunks.size() == 1 && rd::DecodeVolumetricMedia(package.chunks[0].bytes).size() == media.size(), "self-validation failed");
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
    const auto result_path = build / "Actions" / spec.namespace_path / "result.json";
    if (fs::exists(result_path) && fs::exists(result_path.parent_path() / "result.d"))
    {
        try
        {
            const auto result = ap::Json::parse(ap::ReadBytes(result_path));
            ap::UpstreamProduct ignored; fs::path path;
            if (result.at("action_id") == spec.action_id && result.at("action_key") == ap::ActionKey(spec) && result.at("products").size() == 1)
            {
                (void)ap::ReadActionResult(build, spec.namespace_path, kId, ignored, path);
                const auto current = ap::ReadBytes(path);
                if (std::ranges::equal(std::as_bytes(std::span(current.data(), current.size())), bytes))
                { ap::TouchActionResult(result_path); std::cout << spec.action_id << " current\n"; return; }
            }
        }
        catch (const std::exception &) { }
    }
    const auto stage = ap::CreateStage(build / "Actions" / spec.namespace_path);
    try
    {
        ap::WriteBytes(stage / "volumetric_media.rpkg", bytes);
        auto product = ap::HashProduct(std::string(kId), "volumetric_media.rpkg", stage / "volumetric_media.rpkg");
        ap::Publish(build, spec, stage, {std::move(product)});
    }
    catch (...) { ap::RemoveStageAfterFailure(stage); throw; }
    ap::RemoveStage(stage); std::cout << spec.action_id << " published\n";
}
}
int wmain(int argc, wchar_t **argv)
{
    fs::path source, build;
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view option = argv[i];
            if (option == L"--source-root" && source.empty() && ++i < argc) source = argv[i];
            else if (option == L"--build-root" && build.empty() && ++i < argc) build = argv[i];
            else throw std::runtime_error("hs_render_volumetric_package: invalid command line");
        }
        Require(!source.empty() && !build.empty(), "--source-root and --build-root required");
        Produce(source.lexically_normal(), build.lexically_normal()); return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_render_volumetric_package", error.what(),
                          {{"action_id", "render.lighting.volumetric-media"},
                           {"source_root", ap::DiagnosticPath(source)}, {"build_root", ap::DiagnosticPath(build)}});
        return 1;
    }
}
