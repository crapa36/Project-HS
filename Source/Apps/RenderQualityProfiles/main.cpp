#include "../AssetPipeline/action.hpp"
#include <hs/core/cooked_format.hpp>
#include <hs/render_data/quality_profiles.hpp>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
namespace
{
namespace ap = hs::asset_pipeline;
namespace rd = hs::render_data;
namespace fs = std::filesystem;
constexpr std::string_view kId = "render/config/quality_profiles.rpkg";
void Require(bool valid, std::string_view reason)
{ if (!valid) throw std::runtime_error("hs_render_quality_profiles: " + std::string(reason)); }
// This source schema uses only the checked keywords below. Validation reads the
// same immutable schema that participates in the Action identity.
void ValidateSchema(const ap::Json &value, const ap::Json &schema)
{
    const auto type = schema.at("type").get<std::string>();
    Require(type == "object" ? value.is_object() : type == "integer" ? value.is_number_integer() :
        type == "number" && value.is_number(), "source type disagrees with schema");
    if (type == "object")
    {
        const auto &properties = schema.at("properties");
        for (const auto &key : schema.at("required")) Require(value.contains(key.get<std::string>()), "missing schema field");
        for (auto field = value.begin(); field != value.end(); ++field)
        {
            Require(properties.contains(field.key()) || schema.value("additionalProperties", true), "unknown source field");
            if (properties.contains(field.key())) ValidateSchema(field.value(), properties.at(field.key()));
        }
    }
    else
    {
        const auto scalar = value.get<double>(); Require(std::isfinite(scalar), "nonfinite source scalar");
        if (schema.contains("minimum")) Require(scalar >= schema.at("minimum").get<double>(), "source below schema minimum");
        if (schema.contains("maximum")) Require(scalar <= schema.at("maximum").get<double>(), "source above schema maximum");
    }
    if (schema.contains("enum")) Require(std::ranges::find(schema.at("enum"), value) != schema.at("enum").end(), "source enum disagrees with schema");
}
ap::Json ParseSource(std::string_view text)
{
    std::vector<std::unordered_set<std::string>> object_keys;
    return ap::Json::parse(text, [&](int, ap::Json::parse_event_t event, ap::Json &value) {
        if (event == ap::Json::parse_event_t::object_start) object_keys.emplace_back();
        else if (event == ap::Json::parse_event_t::key)
            Require(!object_keys.empty() && object_keys.back().insert(value.get<std::string>()).second, "duplicate object key");
        else if (event == ap::Json::parse_event_t::object_end) object_keys.pop_back();
        return true;
    });
}
void Produce(const fs::path &source, const fs::path &build)
{
    ap::ActionSpec spec;
    spec.action_id = "render.config.quality-profiles";
    spec.recipe_id = "project-hs.render.quality-profiles";
    spec.recipe_version = 1;
    spec.namespace_path = "render/config/quality-profiles";
    const auto schema_path = source / "Schemas/quality_profiles.schema.json";
    const auto schema_bytes = ap::ReadBytes(schema_path);
    const auto source_path = source / "ContentSource/Graphics/quality_profiles.json";
    const auto source_bytes = ap::ReadBytes(source_path);
    spec.inputs.push_back({"Schemas/quality_profiles.schema.json", schema_path,
        ap::Sha256Bytes(std::as_bytes(std::span(schema_bytes.data(), schema_bytes.size())))});
    spec.inputs.push_back({"ContentSource/Graphics/quality_profiles.json", source_path,
        ap::Sha256Bytes(std::as_bytes(std::span(source_bytes.data(), source_bytes.size())))});
    const auto schema = ap::Json::parse(schema_bytes);
    const auto root = ParseSource(source_bytes);
    ValidateSchema(root, schema);
    rd::QualityProfiles table;
    constexpr std::array names{"low", "medium", "high", "ultra"};
    for (std::size_t i = 0; i != names.size(); ++i)
    {
        const auto &row = root.at("profiles").at(names[i]);
        auto &p = table.profiles[i];
        p.geometry_screen_error_px = row.at("geometry_screen_error_px").get<float>();
        p.foliage_density = row.at("foliage_density").get<float>();
        p.texture_finest_mip = row.at("texture_finest_mip").get<std::uint32_t>();
        p.decorative_particle_fraction = row.at("decorative_particle_fraction").get<float>();
        p.shadow_cache_mib = row.at("shadow_cache_mib").get<std::uint32_t>();
        p.shadow_filter_samples = row.at("shadow_filter_samples").get<std::uint32_t>();
        p.shadow_resolution = row.at("shadow_resolution").get<std::uint32_t>();
        p.ao_axis_ratio = row.at("ao_axis_ratio").get<float>();
        p.ao_directions = row.at("ao_directions").get<std::uint32_t>();
        p.ao_steps = row.at("ao_steps").get<std::uint32_t>();
        p.contact_shadow_steps = row.at("contact_shadow_steps").get<std::uint32_t>();
        p.ao_spatial_radius = row.at("ao_spatial_radius").get<std::uint32_t>();
        p.ssr_axis_ratio = row.at("ssr_axis_ratio").get<float>();
        p.ssr_samples = row.at("ssr_samples").get<std::uint32_t>();
        p.ssr_max_steps = row.at("ssr_max_steps").get<std::uint32_t>();
        p.ssr_refine_steps = row.at("ssr_refine_steps").get<std::uint32_t>();
        p.froxel_stride = row.at("froxel_stride").get<std::uint32_t>();
        p.froxel_slices = row.at("froxel_slices").get<std::uint32_t>();
        p.volumetric_area_samples = row.at("volumetric_area_samples").get<std::uint32_t>();
        p.hero_refraction_layers = row.at("hero_refraction_layers").get<std::uint32_t>();
        p.pom_max_steps = row.at("pom_max_steps").get<std::uint32_t>();
        p.pom_refinement_steps = row.at("pom_refinement_steps").get<std::uint32_t>();
        p.local_shadow_max_lights = row.at("local_shadow_max_lights").get<std::uint32_t>();
        p.local_shadow_resolution = row.at("local_shadow_resolution").get<std::uint32_t>();
        p.local_shadow_area_samples = row.at("local_shadow_area_samples").get<std::uint32_t>();
        p.local_shadow_budget_mib = row.at("local_shadow_budget_mib").get<std::uint32_t>();
        p.lighting_cluster_capacity = row.at("lighting_cluster_capacity").get<std::uint32_t>();
        p.gi_logical_cascades = row.at("gi_logical_cascades").get<std::uint32_t>();
        p.gi_internal_resolution_percent = row.at("gi_internal_resolution_percent").get<std::uint32_t>();
        p.vfx_light_fraction = row.at("vfx_light_fraction").get<float>();
        p.vfx_distortion_fraction = row.at("vfx_distortion_fraction").get<float>();
        p.vfx_decal_fraction = row.at("vfx_decal_fraction").get<float>();
        p.terrain_detail_fade_start_m = row.at("terrain_detail_fade_start_m").get<float>();
        p.terrain_detail_fade_end_m = row.at("terrain_detail_fade_end_m").get<float>();
        p.subsurface_half_samples = row.at("subsurface_half_samples").get<std::uint32_t>();
    }
    const auto &cadence = root.at("auto_fg_cadence");
    table.auto_fg_cadence = {cadence.at("disable_below_fps").get<float>(),
        cadence.at("reenable_above_fps").get<float>(), cadence.at("minimum_samples").get<std::uint32_t>()};
    const auto bytes = rd::EncodeQualityProfiles(table);
    (void)rd::DecodeQualityProfiles(bytes);
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
        ap::WriteBytes(stage / "quality_profiles.rpkg", bytes);
        auto product = ap::HashProduct(std::string(kId), "quality_profiles.rpkg", stage / "quality_profiles.rpkg");
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
            else throw std::runtime_error("hs_render_quality_profiles: invalid command line");
        }
        Require(!source.empty() && !build.empty(), "--source-root and --build-root required");
        Produce(source.lexically_normal(), build.lexically_normal()); return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_render_quality_profiles", error.what(),
                          {{"action_id", "render.config.quality-profiles"},
                           {"source_root", ap::DiagnosticPath(source)}, {"build_root", ap::DiagnosticPath(build)}});
        return 1;
    }
}
