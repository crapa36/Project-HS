#include <hs/runtime/vfx_image_io.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/render_data/render_snapshot_values.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <iostream>
#include <string_view>
#include <utility>
#include <cmath>
#include <bit>
#include <optional>
namespace hs {
namespace {
constexpr std::uint32_t ShapeId(std::string_view name) noexcept
{
    std::uint32_t hash = 2166136261u;
    for (const unsigned char character : name) { hash ^= character; hash *= 16777619u; }
    return hash;
}
constexpr std::uint64_t HashEffectName(std::string_view name) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : name) { hash ^= character; hash *= 1099511628211ull; }
    return hash;
}
std::uint8_t MeshIndex(std::uint32_t shape) noexcept
{
    constexpr std::string_view names[]{"arrowhead_mesh", "shard_mesh", "debris_shard_mesh",
        "small_shard_mesh", "enemy_thorn_mesh", "boss_crest_lance_mesh", "needle_shard_mesh"};
    for (std::uint8_t index = 0; index < 7; ++index)
        if (shape == ShapeId(names[index])) return index + 1;
    return 0;
}
bool ValidRange(VfxRange range,std::size_t size) noexcept
{
    return range.first<=size && range.count<=size-range.first;
}
const VfxTextureResourceRecord *GroundTextureResource(VfxProgramView p,const VfxTextureBindingRecord &binding) noexcept
{
    const auto found=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),
        [&](const auto &resource){return resource.slot==binding.catalog_slot;});
    if(found==p.texture_resources.end() || !found->asset_path_bytes.count ||
       !ValidRange(found->asset_path_bytes,p.strings.size())) return nullptr;
    return &*found;
}
bool GroundTexturePath(VfxProgramView p,const VfxTextureBindingRecord &binding,std::string_view path) noexcept
{
    const auto *resource=GroundTextureResource(p,binding);
    if(!resource) return false;
    return std::string_view(reinterpret_cast<const char *>(p.strings.data())+resource->asset_path_bytes.first,
        resource->asset_path_bytes.count)==path;
}
bool PrepareStateRingTextureContract(VfxProgramView p,const VfxOutputRecord &output,bool charged) noexcept
{
    if(!ValidRange(output.textures,p.texture_bindings.size()) || output.textures.count!=3) return false;
    bool gradient=false,curve=false,optional=false;
    for(std::size_t i=output.textures.first;i<static_cast<std::size_t>(output.textures.first)+output.textures.count;++i)
    {
        const auto &binding=p.texture_bindings[i];
        if(binding.role==ShapeId("profile.global") && GroundTexturePath(p,binding,"Content/Textures/VFX/vfx_gradient_lut.dds") && !gradient)
            gradient=true;
        else if(binding.role==ShapeId("profile.global") && GroundTexturePath(p,binding,"Content/Textures/VFX/vfx_curve_lut.dds") && !curve)
            curve=true;
        else if(binding.role==ShapeId("profile.optional_detail") && GroundTexturePath(p,binding,"Content/Textures/VFX/vfx_authored_mask_array.dds") &&
            binding.slices.count==0 && !optional) optional=true;
        else return false;
    }
    if(!gradient||!curve||!optional) return false;
    if(charged)
        for(std::size_t i=output.textures.first;i<static_cast<std::size_t>(output.textures.first)+output.textures.count;++i)
        {
            const auto &binding=p.texture_bindings[i];
            if(binding.selection!=0 || binding.min_quality!=0 || binding.strength!=1.0f || binding.slices.count!=0 ||
               binding.first_frame!=0 || binding.frame_count!=0 || binding.fps!=0.0f) return false;
        }
    return true;
}
void PrepareHexMaskVariants(VfxProgramView p,const VfxOutputRecord &output,VfxGroundRecipe &recipe)
{
    if(!ValidRange(output.textures,p.texture_bindings.size()))
    {
        for(auto &variant:recipe.mask_variants) variant.valid=false;
        return;
    }
    for(std::uint32_t quality=0;quality<3;++quality)
    {
        auto &variant=recipe.mask_variants[quality];
        for(std::size_t i=output.textures.first;i<static_cast<std::size_t>(output.textures.first)+output.textures.count;++i)
        {
            const auto &binding=p.texture_bindings[i];
            if(binding.role!=ShapeId("authored_mask_array")) continue;
            if(binding.min_quality>2 || !std::isfinite(binding.strength) || binding.strength<0 || binding.strength>1)
            { variant.valid=false; break; }
            if(binding.min_quality>quality) continue;
            const auto *resource=GroundTextureResource(p,binding);
            if(!resource || variant.present || binding.selection!=ShapeId("stable_seed_mod_group_size") ||
               !binding.slices.count || !ValidRange(binding.slices,p.slice_indices.size()) ||
               !GroundTexturePath(p,binding,"Content/Textures/VFX/vfx_authored_mask_array.dds"))
            { variant.valid=false; break; }
            bool slices_valid=true;
            for(std::size_t j=binding.slices.first;j<static_cast<std::size_t>(binding.slices.first)+binding.slices.count;++j)
                if(p.slice_indices[j]>=12) slices_valid=false;
            if(!slices_valid) { variant.valid=false; break; }
            variant.slices=binding.slices; variant.strength=binding.strength; variant.present=true;
        }
    }
}
void PrepareRelicMaskVariants(VfxProgramView p,const VfxOutputRecord &output,VfxGroundRecipe &recipe)
{
    for(std::uint32_t quality=0;quality<3;++quality)
    {
        auto &variant=recipe.mask_variants[quality];
        if(!ValidRange(output.textures,p.texture_bindings.size()) || output.textures.count!=4)
        { variant.valid=false; continue; }
        bool gradient=false,curve=false,optional=false,authored=false;
        for(std::size_t i=output.textures.first;i<static_cast<std::size_t>(output.textures.first)+output.textures.count;++i)
        {
            const auto &binding=p.texture_bindings[i];
            const bool gradient_match=binding.role==ShapeId("profile.global") && GroundTexturePath(p,binding,"Content/Textures/VFX/vfx_gradient_lut.dds") && !gradient;
            const bool curve_match=binding.role==ShapeId("profile.global") && GroundTexturePath(p,binding,"Content/Textures/VFX/vfx_curve_lut.dds") && !curve;
            const bool optional_match=binding.role==ShapeId("profile.optional_detail") && GroundTexturePath(p,binding,"Content/Textures/VFX/vfx_authored_mask_array.dds") && binding.slices.count==0 && !optional;
            if(gradient_match) {gradient=true;continue;}
            if(curve_match) {curve=true;continue;}
            if(optional_match) {optional=true;continue;}
            if(binding.role!=ShapeId("authored_mask_array") || authored || !GroundTexturePath(p,binding,"Content/Textures/VFX/vfx_authored_mask_array.dds") ||
               binding.min_quality>2 || binding.min_quality>quality || !std::isfinite(binding.strength) || binding.strength<0 || binding.strength>1 ||
               binding.selection!=ShapeId("stable_seed_mod_group_size") || !binding.slices.count || !ValidRange(binding.slices,p.slice_indices.size()))
            {variant.valid=false;break;}
            for(std::size_t j=binding.slices.first;j<static_cast<std::size_t>(binding.slices.first)+binding.slices.count;++j)
                if(p.slice_indices[j]>=12) variant.valid=false;
            if(!variant.valid) break;
            variant.slices=binding.slices;variant.strength=binding.strength;variant.present=true;authored=true;
        }
        if(!gradient||!curve||!optional||!authored) variant.valid=false;
    }
}
}

void PrepareVfxOutputProfiles(VfxProgramView program, std::vector<std::uint32_t> &profiles)
{
    profiles.assign(program.effects.size(), 0);
    for (std::size_t i = 0; i < program.effects.size(); ++i)
    {
        const auto range = program.effects[i].sources;
        if (range.first > program.sources.size() || range.count > program.sources.size() - range.first)
            continue;
        for (const auto &source : program.sources.subspan(range.first, range.count))
        {
            if (source.outputs.first > program.outputs.size() ||
                source.outputs.count > program.outputs.size() - source.outputs.first) continue;
            for (const auto &output : program.outputs.subspan(source.outputs.first, source.outputs.count))
            {
                const auto profile = static_cast<unsigned>(output.profile);
                if (profile < 32) profiles[i] |= 1u << profile;
            }
        }
    }
}

void PrepareVfxMoteRecipes(VfxProgramView p, std::vector<VfxMoteSourceRecipe> &sources,
                          std::vector<VfxMoteOutputRecipe> &outputs)
{
    const auto valid = [](VfxRange range, std::size_t size) {
        return range.first <= size && range.count <= size - range.first;
    };
    const auto hash = [](std::string_view value) {
        std::uint32_t result = 2166136261u;
        for (const unsigned char c : value) { result ^= c; result *= 16777619u; }
        return result;
    };
    sources.assign(p.sources.size(), {});
    for (std::size_t i = 0; i < p.sources.size(); ++i)
    {
        const auto &source = p.sources[i];
        auto &recipe = sources[i];
        if (valid(source.parameters, p.parameters.size()))
        {
            bool count_seen = false, curl_seen = false, drag_seen = false;
            bool okay = true;
            for (const auto &parameter : p.parameters.subspan(source.parameters.first, source.parameters.count))
            {
                if (parameter.key == hash("count"))
                {
                    if (count_seen || parameter.type != VfxParameterType::Int ||
                        parameter.bits == 0 || parameter.bits > 1024) { okay = false; break; }
                    count_seen = true; recipe.count = parameter.bits;
                }
                else if (parameter.key == hash("curl") || parameter.key == hash("drag"))
                {
                    auto &seen = parameter.key == hash("curl") ? curl_seen : drag_seen;
                    if (seen || parameter.type != VfxParameterType::Float) { okay = false; break; }
                    seen = true;
                    const float value = std::bit_cast<float>(parameter.bits);
                    if (!std::isfinite(value) || value < 0.0f) { okay = false; break; }
                    (parameter.key == hash("curl") ? recipe.curl : recipe.drag) = value;
                }
                else { okay = false; break; }
            }
            recipe.common_params_valid = okay && count_seen;
        }
        if (valid(source.parameters, p.parameters.size()) && source.parameters.count == 1)
        {
            const auto &parameter = p.parameters[source.parameters.first];
            if (parameter.key == hash("count_per_second") && parameter.type == VfxParameterType::Int)
            {
                recipe.rate = std::bit_cast<std::int32_t>(parameter.bits);
                recipe.rate_valid = recipe.rate > 0 && recipe.rate <= 1024;
            }
        }
        if (valid(source.parameters, p.parameters.size()) && source.parameters.count == 2)
        {
            std::uint32_t residual_count{};
            float residual_speed{};
            bool okay = true;
            for (const auto &parameter : p.parameters.subspan(source.parameters.first, source.parameters.count))
            {
                if (parameter.key == hash("count") && parameter.type == VfxParameterType::Int &&
                    !residual_count)
                    residual_count = parameter.bits;
                else if (parameter.key == hash("inward_speed") && parameter.type == VfxParameterType::Float &&
                         residual_speed == 0.0f)
                {
                    residual_speed = std::bit_cast<float>(parameter.bits);
                }
                else { okay = false; break; }
            }
            recipe.residual_valid = okay && residual_count > 0 && residual_count <= 1024 &&
                std::isfinite(residual_speed) && residual_speed > 0.0f;
            if (recipe.residual_valid) { recipe.count = residual_count; recipe.speed = residual_speed; }
        }
        if (valid(source.parameters, p.parameters.size()) && source.parameters.count == 3)
        {
            bool count_seen = false, cone_seen = false, drag_seen = false, okay = true;
            for (const auto &parameter : p.parameters.subspan(source.parameters.first, source.parameters.count))
            {
                if (parameter.key == hash("count") && parameter.type == VfxParameterType::Int &&
                    !count_seen && parameter.bits > 0 && parameter.bits <= 64)
                { recipe.count = parameter.bits; count_seen = true; }
                else if (parameter.key == hash("cone_deg") && parameter.type == VfxParameterType::Float && !cone_seen)
                { recipe.cone_degrees = std::bit_cast<float>(parameter.bits); cone_seen = true; }
                else if (parameter.key == hash("drag") && parameter.type == VfxParameterType::Float && !drag_seen)
                { recipe.drag = std::bit_cast<float>(parameter.bits); drag_seen = true; }
                else { okay = false; break; }
            }
            recipe.spark_valid = okay && count_seen && cone_seen && drag_seen &&
                std::isfinite(recipe.cone_degrees) && recipe.cone_degrees > 0.0f && recipe.cone_degrees <= 90.0f &&
                std::isfinite(recipe.drag) && recipe.drag >= 0.0f && recipe.drag <= 10.0f;
        }
    }

    outputs.assign(p.outputs.size(), {});
    constexpr std::string_view mask_path = "Content/Textures/VFX/vfx_authored_mask_array.dds";
    constexpr std::string_view gradient_path = "Content/Textures/VFX/vfx_gradient_lut.dds";
    constexpr std::string_view curve_path = "Content/Textures/VFX/vfx_curve_lut.dds";
    for (std::size_t i = 0; i < p.outputs.size(); ++i)
    {
        const auto &output = p.outputs[i];
        auto &recipe = outputs[i];
        recipe.min_quality = output.min_quality;
        recipe.gradient_row = output.gradient_row;
        recipe.hdr = output.hdr;
        recipe.color = {output.rgba[0] * output.hdr, output.rgba[1] * output.hdr,
            output.rgba[2] * output.hdr, output.rgba[3]};
        recipe.output_color_valid = std::isfinite(output.hdr) && output.hdr > 0.0f &&
            std::all_of(output.rgba.begin(), output.rgba.end(),
                [](float value) { return std::isfinite(value); }) &&
            output.rgba[3] >= 0.0f && output.rgba[3] <= 1.0f &&
            std::isfinite(recipe.color.x) && std::isfinite(recipe.color.y) &&
            std::isfinite(recipe.color.z);
        recipe.mask_quality_mask = 0;
        if (valid(output.textures, p.texture_bindings.size()))
        {
            for (std::uint32_t quality = 0; quality < 3; ++quality)
            {
                bool okay = true;
                bool selected = false;
                for (const auto &binding : p.texture_bindings.subspan(output.textures.first, output.textures.count))
                {
                    if (binding.role != hash("authored_mask_array")) continue;
                    if (binding.min_quality > 2 || !std::isfinite(binding.strength) ||
                        binding.strength < 0.0f || binding.strength > 1.0f) { okay = false; break; }
                    if (binding.min_quality > quality) continue;
                    const auto resource = std::find_if(p.texture_resources.begin(), p.texture_resources.end(),
                        [&](const auto &entry) { return entry.slot == binding.catalog_slot; });
                    if (resource == p.texture_resources.end() || !valid(resource->asset_path_bytes, p.strings.size()))
                    { okay = false; break; }
                    const std::string_view path(reinterpret_cast<const char *>(p.strings.data() + resource->asset_path_bytes.first),
                        resource->asset_path_bytes.count);
                    if (path != mask_path || selected ||
                        binding.selection != hash("stable_seed_mod_group_size") ||
                        !valid(binding.slices, p.slice_indices.size()) || binding.slices.count == 0)
                    { okay = false; break; }
                    for (const auto slice : p.slice_indices.subspan(binding.slices.first, binding.slices.count))
                        if (slice >= 12) { okay = false; break; }
                    if (!okay) break;
                    recipe.mask_slices[quality] = binding.slices;
                    recipe.mask_strengths[quality] = binding.strength;
                    recipe.mask_present_mask |= static_cast<std::uint8_t>(1u << quality);
                    selected = true;
                }
                if (okay) recipe.mask_quality_mask |= static_cast<std::uint8_t>(1u << quality);
                else recipe.mask_present_mask &= static_cast<std::uint8_t>(~(1u << quality));
            }
        }
        if (valid(output.parameters, p.parameters.size()))
        {
            bool size_seen = false, curve_seen = false, okay = true;
            for (const auto &parameter : p.parameters.subspan(output.parameters.first, output.parameters.count))
            {
                if (parameter.key == hash("size"))
                {
                    if (size_seen || parameter.type != VfxParameterType::Float) { okay = false; break; }
                    recipe.persistent_size = std::bit_cast<float>(parameter.bits);
                    size_seen = true;
                    if (!std::isfinite(recipe.persistent_size) || recipe.persistent_size <= 0.0f) { okay = false; break; }
                }
                else if (parameter.key == hash("scale_curve_ref"))
                {
                    if (curve_seen || parameter.type != VfxParameterType::CurveRow || parameter.bits >= 7u)
                    { okay = false; break; }
                    recipe.persistent_curve_row = parameter.bits; curve_seen = true;
                }
                else { okay = false; break; }
            }
            recipe.persistent_params_valid = okay;
        }
        if (valid(output.textures, p.texture_bindings.size()) && output.textures.count == 3)
        {
            bool gradient = false, curve = false, optional_mask = false, okay = true;
            for (const auto &binding : p.texture_bindings.subspan(output.textures.first, output.textures.count))
            {
                const auto resource = std::find_if(p.texture_resources.begin(), p.texture_resources.end(),
                    [&](const auto &entry) { return entry.slot == binding.catalog_slot; });
                if (resource == p.texture_resources.end() || resource->asset_path_bytes.count == 0 ||
                    !valid(resource->asset_path_bytes, p.strings.size())) { okay = false; break; }
                const std::string_view path(reinterpret_cast<const char *>(p.strings.data() + resource->asset_path_bytes.first),
                    resource->asset_path_bytes.count);
                if (binding.role == hash("profile.global") && path == gradient_path && !gradient) gradient = true;
                else if (binding.role == hash("profile.global") && path == curve_path && !curve) curve = true;
                else if (binding.role == hash("profile.optional_detail") && path == mask_path &&
                         !optional_mask && binding.slices.count == 0) optional_mask = true;
                else { okay = false; break; }
            }
            recipe.spark_textures_valid = okay && gradient && curve && optional_mask;
        }
    }

    std::vector<std::uint32_t> lookup_counts(p.effects.size());
    std::vector<std::uint64_t> lookup_ids(p.effects.size());
    for (const auto &lookup : p.effect_lookup)
        if (lookup.handle && lookup.handle <= p.effects.size())
        {
            auto &count = lookup_counts[lookup.handle - 1];
            if (!count) lookup_ids[lookup.handle - 1] = lookup.effect_id;
            ++count;
        }
    for (std::size_t i = 0; i < p.sources.size(); ++i)
    {
        const auto &source = p.sources[i];
        auto &recipe = sources[i];
        recipe.effect_handle = source.effect;
        recipe.stable_id = source.stable_id;
        recipe.output_range = source.outputs;
        recipe.knots = {source.knots[0], source.knots[1]};
        recipe.output_range_valid = valid(source.outputs, p.outputs.size());
        const bool effect_valid = source.effect && source.effect <= p.effects.size() &&
            p.effects[source.effect - 1].handle == source.effect;
        if (effect_valid)
        {
            recipe.effect_id = lookup_ids[source.effect - 1];
            recipe.effect_lookup_count = lookup_counts[source.effect - 1];
        }
        const bool knots_valid = source.knot_count == 2 && std::isfinite(source.knots[0]) &&
            std::isfinite(source.knots[1]) && source.knots[0] >= 0.0f &&
            source.knots[1] > source.knots[0] && source.knots[1] <= 1.0f;
        const bool curl_source = effect_valid && source.type == VfxSourceType::CurlMotes &&
            valid(source.parameters, p.parameters.size()) && recipe.output_range_valid;
        recipe.common_route_valid = curl_source && knots_valid && recipe.common_params_valid;
        recipe.residual_route_valid = curl_source && source.stable_id == hash("residual_motes") &&
            knots_valid && source.knots[0] == 0.0f && source.knots[1] == 1.0f && source.parameters.count == 2 &&
            recipe.residual_valid;
        recipe.rate_route_valid = curl_source && knots_valid && source.parameters.count == 1 && recipe.rate_valid;
        recipe.tick_route_valid = curl_source && source.stable_id == hash("tick_motes") &&
            knots_valid && source.knots[0] == 0.0f && source.knots[1] == 1.0f && source.parameters.count == 1 &&
            recipe.common_params_valid && recipe.count == 3;
        recipe.spark_route_valid = curl_source && source.stable_id == hash("micro_sparks") &&
            knots_valid && source.parameters.count == 3 && recipe.spark_valid;
    }
    const auto common_pattern = [&](const VfxOutputRecord &output) -> std::uint32_t {
        if (output.profile != VfxOutputProfile::SpriteAdd) return 0xffffffffu;
        if (output.shape == hash("soft_leaf_mote") && output.motion == VfxMotionKind::SlowUpwardHelix) return 0;
        if (output.shape == hash("small_motes") && output.motion == VfxMotionKind::VeryShortLocalDrift) return 1;
        if (output.shape == hash("diamond_motes") && output.motion == VfxMotionKind::ShortUpwardSpiral) return 2;
        return 0xffffffffu;
    };
    for (std::size_t i = 0; i < p.outputs.size(); ++i)
    {
        const auto &output = p.outputs[i];
        auto &recipe = outputs[i];
        recipe.empty_parameters_valid = valid(output.parameters, p.parameters.size()) &&
            output.parameters.count == 0;
        if (output.source >= sources.size()) continue;
        const auto &source = sources[output.source];
        const bool linked = source.effect_handle && source.effect_handle ==
            p.sources[output.source].effect && source.output_range_valid &&
            i >= source.output_range.first && i - source.output_range.first < source.output_range.count;
        if (!linked) continue;
        recipe.common_recipe_index = source.common_route_valid && recipe.empty_parameters_valid
            ? common_pattern(output) : 0xffffffffu;
        recipe.residual_route_valid = source.residual_route_valid && recipe.empty_parameters_valid &&
            output.profile == VfxOutputProfile::SpriteAdd &&
            output.shape == hash("camera_billboard") &&
            output.motion == VfxMotionKind::ParticlesPulledInward && recipe.min_quality <= 2 &&
            std::all_of(output.rgba.begin(), output.rgba.end(),
                [](float value) { return std::isfinite(value) && value >= 0.0f; });
        const bool soft_disc = output.profile == VfxOutputProfile::SpriteAdd &&
            output.shape == hash("soft_motes") && output.shape_domain == hash("sprite") &&
            output.shape_scale_rule == hash("component_size") && output.shape_component_kind == hash("particle") &&
            output.coverage_type == hash("analytic") && output.coverage_ref == hash("soft_disc") &&
            output.motion == VfxMotionKind::RareInwardDrift && recipe.empty_parameters_valid;
        recipe.rate_route_valid = source.rate_route_valid && soft_disc;
        recipe.persistent_route_valid = source.rate_route_valid && soft_disc && recipe.min_quality <= 2;
        recipe.tick_route_valid = source.tick_route_valid && recipe.empty_parameters_valid &&
            output.profile == VfxOutputProfile::SpriteAdd && output.shape == hash("small_motes") &&
            output.motion == VfxMotionKind::VeryShortLocalDrift && output.shape_domain == hash("sprite") &&
            output.shape_scale_rule == hash("component_size") && output.shape_component_kind == hash("particle") &&
            output.coverage_type == hash("analytic") && output.coverage_ref == hash("diamond_spark") &&
            recipe.min_quality <= 2;
        recipe.spark_route_valid = source.spark_route_valid && recipe.empty_parameters_valid &&
            output.profile == VfxOutputProfile::SpriteAdd && output.shape == hash("velocity_billboard") &&
            output.shape_domain == hash("sprite") && output.shape_scale_rule == hash("component_size") &&
            output.shape_component_kind == hash("particle") && output.coverage_type == hash("analytic") &&
            output.coverage_ref == hash("velocity_streak") &&
            output.motion == VfxMotionKind::SparseRearwardSparks && recipe.min_quality <= 2 &&
            std::isfinite(output.motion_rate_hz) && output.motion_rate_hz >= 0.0f &&
            std::isfinite(output.motion_amplitude) && output.motion_amplitude >= 0.0f && output.motion_amplitude <= 1.0f &&
            std::isfinite(output.motion_inset_fraction) && output.motion_inset_fraction >= 0.0f && output.motion_inset_fraction <= 1.0f;
    }
}

Result PrepareVfxScreenRecipes(VfxProgramView p, std::vector<VfxScreenEffectRecipe> &effects,
                             std::vector<VfxScreenRecipe> &outputs)
{
    constexpr auto hit=HashEffectName("particle.player.hit_direction");
    constexpr auto health=HashEffectName("post.player.low_health_vignette");
    constexpr auto presentation=ShapeId("PresentationContextPayload");
    const auto texture_ok=[&](const VfxOutputRecord &output) {
        if(!ValidRange(output.textures,p.texture_bindings.size())||output.textures.count!=1)return false;
        const auto &binding=p.texture_bindings[output.textures.first];
        const auto resource=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),
            [&](const auto &row){return row.slot==binding.catalog_slot;});
        if(resource==p.texture_resources.end()||!ValidRange(resource->asset_path_bytes,p.strings.size())||
           binding.role!=ShapeId("profile.global")||binding.selection||binding.slices.count||binding.first_frame||
           binding.frame_count||binding.fps!=0||binding.strength!=1)return false;
        const std::string_view path(reinterpret_cast<const char *>(p.strings.data())+resource->asset_path_bytes.first,
            resource->asset_path_bytes.count);
        return path=="Content/Textures/VFX/vfx_gradient_lut.dds";
    };
    effects.assign(p.effects.size(),{});outputs.clear();
    for(std::size_t ei=0;ei<p.effects.size();++ei)
    {
        const auto handle=static_cast<std::uint32_t>(ei+1);
        const auto &effect=p.effects[ei];
        auto &recipe=effects[ei];
        const auto lookup=std::find_if(p.effect_lookup.begin(),p.effect_lookup.end(),
            [&](const auto &row){return row.handle==handle;});
        recipe.effect_id=lookup==p.effect_lookup.end()?0:lookup->effect_id;
        const bool is_hit=recipe.effect_id==hit,is_health=recipe.effect_id==health;
        if(!is_hit&&!is_health)continue;
        recipe.seconds=effect.seconds;recipe.outputs.first=static_cast<std::uint32_t>(outputs.size());
        bool valid=effect.handle==handle&&ValidRange(effect.sources,p.sources.size());
        valid=valid&&(is_hit
            ? effect.input_mode==0&&effect.timing_kind==0&&effect.payload_kind==presentation&&
                std::isfinite(effect.seconds)&&effect.seconds>0
            : effect.input_mode==1&&effect.timing_kind==1&&effect.payload_kind==presentation);
        if(valid)
        for(std::uint32_t si=effect.sources.first;si<effect.sources.first+effect.sources.count;++si)
        {
            const auto &source=p.sources[si];
            if(source.effect!=handle||source.type!=VfxSourceType::Direct||
               !ValidRange(source.outputs,p.outputs.size())||!ValidRange(source.parameters,p.parameters.size())||
               source.parameters.count||source.knot_count!=2||!std::isfinite(source.knots[0])||
               !std::isfinite(source.knots[1])||source.knots[0]<0||source.knots[1]<=source.knots[0]||source.knots[1]>1)
            {valid=false;break;}
            for(std::uint32_t oi=source.outputs.first;oi<source.outputs.first+source.outputs.count;++oi)
            {
                const auto &output=p.outputs[oi];
                if(output.profile!=VfxOutputProfile::ScreenOverlay)continue;
                const bool vignette=output.shape==ShapeId("screen_vignette");
                const bool arc=output.shape==ShapeId("directional_arc");
                if((!vignette&&!arc)||output.source!=si||output.shape_component_kind!=ShapeId("screen")||
                   output.coverage_type!=ShapeId("analytic_screen")||
                   output.motion!=(vignette?VfxMotionKind::SlowPulseStateLocked:VfxMotionKind::OptionalTransientArc)||
                   output.min_quality>2||!ValidRange(output.parameters,p.parameters.size())||!texture_ok(output)||
                   !std::isfinite(output.hdr)||output.hdr<0||
                   !std::all_of(output.rgba.begin(),output.rgba.end(),[](float x){return std::isfinite(x)&&x>=0;})||output.rgba[3]>1)
                {valid=false;break;}
                VfxScreenRecipe row;row.source_index=si;row.output_index=oi;row.min_quality=output.min_quality;
                row.gradient_row=output.gradient_row;row.shape=vignette?VfxScreenShape::Vignette:VfxScreenShape::DirectionalArc;
                row.knot_start=source.knots[0];row.knot_end=source.knots[1];
                row.rgb={output.rgba[0],output.rgba[1],output.rgba[2]};row.alpha=output.rgba[3];row.hdr=output.hdr;
                bool center=false,edge=false,direction=false,parameters_valid=true;
                for(const auto &parameter:p.parameters.subspan(output.parameters.first,output.parameters.count))
                {
                    if(parameter.key==ShapeId("center_clear")&&!center&&parameter.type==VfxParameterType::Float)
                    {row.center_clear=std::bit_cast<float>(parameter.bits);center=true;}
                    else if(parameter.key==ShapeId("edge_power")&&!edge&&parameter.type==VfxParameterType::Float)
                    {row.edge_power=std::bit_cast<float>(parameter.bits);edge=true;}
                    else if(parameter.key==ShapeId("angle_from_hit")&&!direction&&parameter.type==VfxParameterType::Bool&&parameter.bits==1)
                        direction=true;
                    else {parameters_valid=false;break;}
                }
                parameters_valid=parameters_valid&&(vignette?center&&edge&&!direction:direction&&!center&&!edge)&&
                    std::isfinite(row.center_clear)&&row.center_clear>=0&&row.center_clear<1&&
                    std::isfinite(row.edge_power)&&row.edge_power>0;
                if(!parameters_valid){valid=false;break;}
                outputs.push_back(row);
            }
            if(!valid)break;
        }
        recipe.outputs.count=static_cast<std::uint32_t>(outputs.size()-recipe.outputs.first);
        recipe.route_valid=valid;
        if(!valid)return Result::Failure(ErrorCode::InvalidArgument,"hs_vfx_screen",
            "Unsupported ScreenOverlay contract for effect "+std::to_string(recipe.effect_id)+".");
    }
    return Result::Success();
}

Result PrepareVfxVolumeRecipes(VfxProgramView p, std::vector<VfxVolumeEffectRecipe> &effects,
                             std::vector<VfxVolumeRecipe> &outputs)
{
    constexpr auto phase=HashEffectName("particle.boss.phase_change"),spawn=HashEffectName("particle.boss.spawn"),
        death=HashEffectName("particle.boss.death"),aura=HashEffectName("persistent.boss.phase2_aura");
    constexpr auto point=ShapeId("PointEventPayload"),context=ShapeId("PresentationContextPayload"),
        entity=ShapeId("EntityAttachmentPayload");
    const auto texture_path=[&](const VfxTextureBindingRecord &binding)->std::optional<std::string_view>{
        const auto resource=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),
            [&](const auto &row){return row.slot==binding.catalog_slot;});
        if(resource==p.texture_resources.end()||!ValidRange(resource->asset_path_bytes,p.strings.size()))return {};
        return std::string_view(reinterpret_cast<const char *>(p.strings.data())+resource->asset_path_bytes.first,
            resource->asset_path_bytes.count);
    };
    effects.assign(p.effects.size(),{});outputs.clear();
    for(std::size_t ei=0;ei<p.effects.size();++ei)
    {
        const auto handle=static_cast<std::uint32_t>(ei+1);
        const auto &effect=p.effects[ei];auto &recipe=effects[ei];
        const auto lookup=std::find_if(p.effect_lookup.begin(),p.effect_lookup.end(),
            [&](const auto &row){return row.handle==handle;});
        recipe.effect_id=lookup==p.effect_lookup.end()?0:lookup->effect_id;
        const auto id=recipe.effect_id;const bool crown=id==aura;
        if(id!=phase&&id!=spawn&&id!=death&&!crown)continue;
        recipe.seconds=effect.seconds;recipe.outputs.first=static_cast<std::uint32_t>(outputs.size());
        bool valid=effect.handle==handle&&ValidRange(effect.sources,p.sources.size());
        if(crown)valid=valid&&effect.input_mode==1&&effect.timing_kind==1&&effect.payload_kind==entity;
        else valid=valid&&effect.input_mode==0&&effect.timing_kind==0&&std::isfinite(effect.seconds)&&effect.seconds>0&&
            effect.payload_kind==(id==spawn?point:context);
        bool found=false;
        if(valid)
        for(std::uint32_t si=effect.sources.first;si<effect.sources.first+effect.sources.count;++si)
        {
            const auto &source=p.sources[si];if(source.type!=VfxSourceType::VolumeLocal)continue;
            if(found||source.effect!=handle||source.stable_id!=ShapeId(crown?"phase_wisps":"smoke_volume")||
               source.authoritative!=0||source.parameters.count!=0||!ValidRange(source.parameters,p.parameters.size())||
               source.knot_count!=2||!std::isfinite(source.knots[0])||!std::isfinite(source.knots[1])||
               source.knots[0]<0||source.knots[1]<=source.knots[0]||source.knots[1]>1||
               !ValidRange(source.outputs,p.outputs.size())||source.outputs.count!=1)
            {valid=false;break;}
            found=true;const auto oi=source.outputs.first;const auto &output=p.outputs[oi];
            const auto expected_shape=ShapeId(crown?"bounded_crown_volume":"half_res_sphere_volume");
            if(output.source!=si||output.profile!=VfxOutputProfile::LocalVolume||output.min_quality!=2||
               output.shape!=expected_shape||output.shape_domain!=ShapeId("volume")||
               output.shape_scale_rule!=ShapeId("component_bounds")||output.shape_component_kind!=ShapeId("volume")||
               output.coverage_type!=ShapeId("volume_density")||output.coverage_ref!=0||output.surface_target!=0||
               output.motion_rate_hz!=0||output.motion_amplitude!=0||output.motion_inset_fraction!=0||
               output.motion!=(crown?VfxMotionKind::SlowInwardCurl:VfxMotionKind::BuoyantTurbulentSmoke)||
               !ValidRange(output.parameters,p.parameters.size())||output.parameters.count!=3||
               !ValidRange(output.textures,p.texture_bindings.size())||output.textures.count!=3||
               !std::isfinite(output.hdr)||output.hdr<0||
               !std::all_of(output.rgba.begin(),output.rgba.end(),[](float x){return std::isfinite(x)&&x>=0;})||output.rgba[3]>1)
            {valid=false;break;}
            VfxVolumeRecipe row;row.source_index=si;row.output_index=oi;row.source_id=source.stable_id;
            row.min_quality=output.min_quality;row.gradient_row=output.gradient_row;row.shape=crown?VfxVolumeShape::BoundedCrown:VfxVolumeShape::NoisySphere;
            row.knot_start=source.knots[0];row.knot_end=source.knots[1];row.alpha=output.rgba[3];row.hdr=output.hdr;
            row.rgb={output.rgba[0],output.rgba[1],output.rgba[2]};
            bool steps=false,density=false,anisotropy=false,noise=false,half=false;
            for(const auto &parameter:p.parameters.subspan(output.parameters.first,output.parameters.count))
            {
                if(parameter.key==ShapeId("steps")&&!steps&&parameter.type==VfxParameterType::Int)
                {steps=true;row.steps=parameter.bits;if(row.steps!=(crown?18u:12u)){valid=false;break;}}
                else if(crown&&parameter.key==ShapeId("density")&&!density&&parameter.type==VfxParameterType::Float)
                {density=true;row.density=std::bit_cast<float>(parameter.bits);if(!std::isfinite(row.density)||row.density<0){valid=false;break;}}
                else if(crown&&parameter.key==ShapeId("anisotropy")&&!anisotropy&&parameter.type==VfxParameterType::Float)
                {anisotropy=true;row.anisotropy=std::bit_cast<float>(parameter.bits);if(!std::isfinite(row.anisotropy)||std::abs(row.anisotropy)>=1){valid=false;break;}}
                else if(!crown&&parameter.key==ShapeId("noise")&&!noise&&parameter.type==VfxParameterType::Enum&&parameter.bits==ShapeId("3d_fbm"))
                {noise=true;row.noise=VfxVolumeNoise::Fbm3D;}
                else if(!crown&&parameter.key==ShapeId("half_res")&&!half&&parameter.type==VfxParameterType::Bool&&parameter.bits==1)
                {half=true;row.half_resolution=1;}
                else {valid=false;break;}
            }
            if(!valid||!steps||(crown?!(density&&anisotropy):!(noise&&half))){valid=false;break;}
            bool gradient=false,stbn=false,noise_texture=false;
            for(const auto &binding:p.texture_bindings.subspan(output.textures.first,output.textures.count))
            {
                const auto path=texture_path(binding);
                if(!path||binding.selection||binding.min_quality!=0||binding.strength!=1||binding.slices.count||
                   !ValidRange(binding.slices,p.slice_indices.size())||binding.first_frame||binding.frame_count||binding.fps!=0)
                {valid=false;break;}
                if(*path=="Content/Textures/VFX/vfx_gradient_lut.dds"&&!gradient&&binding.role==ShapeId("profile.global"))gradient=true;
                else if(*path=="Content/Textures/VFX/vfx_stbn_scalar.dds"&&!stbn&&binding.role==ShapeId("profile.global"))stbn=true;
                else if(*path=="Content/Textures/VFX/vfx_noise_volume_64.dds"&&!noise_texture&&binding.role==ShapeId("profile.authored"))noise_texture=true;
                else {valid=false;break;}
            }
            if(!valid||!(gradient&&stbn&&noise_texture)){valid=false;break;}
            outputs.push_back(row);
        }
        recipe.outputs.count=static_cast<std::uint32_t>(outputs.size()-recipe.outputs.first);
        recipe.route_valid=valid&&found;
        if(!recipe.route_valid)return Result::Failure(ErrorCode::InvalidArgument,"hs_vfx_volume",
            "Unsupported LocalVolume contract for effect "+std::to_string(recipe.effect_id)+".");
    }
    return Result::Success();
}

void PrepareVfxEntitySurfaceRecipes(VfxProgramView p,
    std::vector<VfxEntitySurfaceEffectRecipe> &effects,
    std::vector<VfxEntitySurfaceRecipe> &outputs,
    VfxEntitySurfaceRouteHandles &routes)
{
    constexpr auto phase=HashEffectName("particle.boss.phase_change"),death=HashEffectName("particle.boss.death"),
        persistent=HashEffectName("persistent.boss.phase_transition_invulnerable");
    constexpr auto payload=ShapeId("PresentationContextPayload");
    const auto texture_contract=[&](const VfxOutputRecord &output) {
        if(!ValidRange(output.textures,p.texture_bindings.size())||output.textures.count!=3)return false;
        std::uint32_t seen=0;
        for(const auto &binding:p.texture_bindings.subspan(output.textures.first,output.textures.count))
        {
            const auto resource=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),
                [&](const auto &row){return row.slot==binding.catalog_slot;});
            if(resource==p.texture_resources.end()||!ValidRange(resource->asset_path_bytes,p.strings.size())||
               resource->asset_path_bytes.count==0||binding.min_quality>2||binding.selection!=0||
               binding.slices.count!=0||binding.strength!=1.0f)return false;
            const std::string_view path(reinterpret_cast<const char *>(p.strings.data())+
                resource->asset_path_bytes.first,resource->asset_path_bytes.count);
            const auto role=binding.role;
            const std::uint32_t bit=role==ShapeId("profile.global")&&path=="Content/Textures/VFX/vfx_gradient_lut.dds"?1u:
                role==ShapeId("profile.global")&&path=="Content/Textures/VFX/vfx_stbn_scalar.dds"?2u:
                role==ShapeId("profile.authored")&&path=="Content/Textures/VFX/vfx_noise_volume_64.dds"?4u:0u;
            if(!bit||(seen&bit))return false;
            seen|=bit;
        }
        return seen==7;
    };
    effects.assign(p.effects.size(),{});outputs.clear();routes={};
    for(const auto &lookup:p.effect_lookup)
    {
        auto *target=lookup.effect_id==phase?&routes.phase_effect:
            lookup.effect_id==death?&routes.death_effect:
            lookup.effect_id==HashEffectName("particle.skill.trap")?&routes.trap_point_rate_effect:nullptr;
        if(target&&!*target) *target=lookup.handle;
    }
    for(std::size_t ei=0;ei<p.effects.size();++ei)
    {
        const auto handle=static_cast<std::uint32_t>(ei+1);
        const auto &effect=p.effects[ei];auto &recipe=effects[ei];
        std::size_t matches=0;
        for(const auto &lookup:p.effect_lookup)if(lookup.handle==handle)
        {if(matches++==0)recipe.effect_id=lookup.effect_id;}
        recipe.identity_unique=matches==1&&recipe.effect_id!=0;
        if(!recipe.identity_unique)continue;
        const bool is_persistent=recipe.effect_id==persistent;
        const bool is_death=recipe.effect_id==death;
        if(recipe.effect_id!=phase&&!is_death&&!is_persistent)continue;
        recipe.importance=effect.importance;recipe.payload_kind=effect.payload_kind;
        recipe.input_mode=effect.input_mode;recipe.timing_kind=effect.timing_kind;recipe.seconds=effect.seconds;
        recipe.outputs.first=static_cast<std::uint32_t>(outputs.size());
        if(effect.handle!=handle||!ValidRange(effect.sources,p.sources.size()))continue;
        for(std::uint32_t si=effect.sources.first;si<effect.sources.first+effect.sources.count;++si)
        {
            const auto &source=p.sources[si];
            const float begin=is_persistent?.65f:.08f;
            const float end=is_persistent||is_death?1.0f:.88f;
            if(source.effect!=handle||source.stable_id!=ShapeId(is_persistent?"surface_dissolve_edge":"entity_surface_transition")||
               source.type!=VfxSourceType::Direct||source.knot_count!=2||source.knots[0]!=begin||source.knots[1]!=end||
               source.parameters.count!=0||!ValidRange(source.parameters,p.parameters.size())||
               !ValidRange(source.outputs,p.outputs.size()))continue;
            for(std::uint32_t oi=source.outputs.first;oi<source.outputs.first+source.outputs.count;++oi)
            {
                const auto &output=p.outputs[oi];
                const auto expected_quality=static_cast<std::uint32_t>(is_persistent?VfxQuality::Low:VfxQuality::Medium);
                const float expected_hdr=is_persistent?3.8f:is_death?5.5f:4.8f;
                const float expected_alpha=is_persistent?.5f:.72f;
                if(output.source!=si||output.profile!=VfxOutputProfile::EntitySurface||output.shape!=ShapeId("")||
                   output.coverage_type!=ShapeId("mesh_surface")||
                   output.surface_target!=ShapeId(is_persistent?"boss_material_hook":"boss_surface")||
                   output.motion!=(is_persistent?VfxMotionKind::NoiseEdgeFlowsUpward:VfxMotionKind::SurfaceNoiseThroughBoss)||
                   output.min_quality>2||output.min_quality!=expected_quality||!std::isfinite(output.hdr)||output.hdr!=expected_hdr||
                   output.rgba[3]!=expected_alpha||
                   std::any_of(output.rgba.begin(),output.rgba.end(),[](float value){return !std::isfinite(value)||value<0;})||
                   output.rgba[3]>1||!texture_contract(output)||!ValidRange(output.parameters,p.parameters.size())||
                   output.parameters.count!=3)continue;
                bool noise=false,edge=false,direction=false,valid=true;
                float noise_scale=0,edge_width=0;
                VfxEntitySurfaceDirection facing{};
                for(const auto &parameter:p.parameters.subspan(output.parameters.first,output.parameters.count))
                {
                    if(parameter.key==ShapeId("noise_scale")&&!noise&&parameter.type==VfxParameterType::Float)
                    {noise=true;noise_scale=std::bit_cast<float>(parameter.bits);}
                    else if(parameter.key==ShapeId("edge_width")&&!edge&&parameter.type==VfxParameterType::Float)
                    {edge=true;edge_width=std::bit_cast<float>(parameter.bits);}
                    else if(parameter.key==ShapeId("direction")&&!direction&&parameter.type==VfxParameterType::Enum&&
                        parameter.bits==ShapeId(is_persistent?"up":"up_and_out"))
                    {direction=true;facing=is_persistent?VfxEntitySurfaceDirection::Up:VfxEntitySurfaceDirection::UpAndOut;}
                    else {valid=false;break;}
                }
                if(!valid||!noise||!edge||!direction||!std::isfinite(noise_scale)||noise_scale!=(is_persistent?2.6f:2.4f)||
                   !std::isfinite(edge_width)||edge_width!=(is_persistent?.08f:.07f))continue;
                outputs.push_back({handle,si,oi,output.min_quality,output.gradient_row,
                    is_persistent?VfxEntitySurfaceShape::DissolveEdge:VfxEntitySurfaceShape::Transition,
                    facing,begin,end,noise_scale,edge_width,
                    {output.rgba[0],output.rgba[1],output.rgba[2],output.rgba[3]},
                    output.hdr,output.rgba[3],is_persistent,is_death});
            }
        }
        recipe.outputs.count=static_cast<std::uint32_t>(outputs.size()-recipe.outputs.first);
    }
}

void PrepareVfxRibbonRecipes(VfxProgramView p,std::vector<VfxRibbonEffectRecipe> &effects,
    std::vector<VfxRibbonSourceRecipe> &sources,std::vector<VfxRibbonOutputRecipe> &outputs,
    std::vector<VfxRibbonTextureRecipe> &textures,std::vector<std::uint32_t> &slice_indices)
{
    constexpr auto history=ShapeId("history_seconds"),ground=ShapeId("ground_lock"),
        arc=ShapeId("arc_deg"),head=ShapeId("width_head"),tail=ShapeId("width_tail"),
        noise=ShapeId("noise_warp"),phase=ShapeId("phase_offset"),width=ShapeId("width"),
        taper=ShapeId("taper"),multiplier=ShapeId("width_multiplier"),reverse=ShapeId("reverse_flow"),
        segments=ShapeId("segments"),pulse=ShapeId("travel_pulse");
    const auto set_float=[](VfxRibbonOutputRecipe &row,std::uint32_t key,VfxParameterType type,
        std::uint32_t bits,bool &present,bool &valid,float &value) {
        present=true;valid=type==VfxParameterType::Float;
        if(valid){value=std::bit_cast<float>(bits);valid=std::isfinite(value);}
        (void)key;
    };
    effects.assign(p.effects.size(),{});sources.assign(p.sources.size(),{});
    outputs.assign(p.outputs.size(),{});textures.assign(p.texture_bindings.size(),{});
    slice_indices.assign(p.slice_indices.begin(),p.slice_indices.end());
    for(std::size_t i=0;i<p.effects.size();++i)
    {
        const auto &effect=p.effects[i];auto &row=effects[i];
        row.handle=effect.handle;row.importance=effect.importance;row.payload_kind=effect.payload_kind;
        row.input_mode=effect.input_mode;row.timing_kind=effect.timing_kind;row.seconds=effect.seconds;row.sources=effect.sources;
        row.valid_handle=effect.handle==i+1;
        for(const auto &lookup:p.effect_lookup)
        {
            if(lookup.handle==effect.handle){++row.lookup_count;row.effect_id=lookup.effect_id;}
        }
        if(row.lookup_count!=1)row.effect_id=0;
        if(row.effect_id)for(const auto &lookup:p.effect_lookup)if(lookup.effect_id==row.effect_id)++row.identity_count;
    }
    for(std::size_t i=0;i<p.sources.size();++i)
    {
        const auto &source=p.sources[i];auto &row=sources[i];
        row.effect=source.effect;row.source_index=static_cast<std::uint32_t>(i);
        row.stable_id=source.stable_id;row.type=source.type;row.knot_count=source.knot_count;row.knots=source.knots;
        row.outputs=source.outputs;row.authoritative=source.authoritative;row.parameter_count=source.parameters.count;
          row.knots_valid=source.knot_count<=row.knots.size();
          for(std::uint32_t knot=0;row.knots_valid&&knot<source.knot_count;++knot)
              if(!std::isfinite(source.knots[knot])||source.knots[knot]<0.0f||source.knots[knot]>1.0f||
                  (knot!=0&&source.knots[knot]<=source.knots[knot-1]))row.knots_valid=false;
        if(ValidRange(source.parameters,p.parameters.size()))
            for(const auto &parameter:p.parameters.subspan(source.parameters.first,source.parameters.count))
            {
                if(parameter.key==history&&!row.history_present)
                {row.history_present=true;row.history_valid=parameter.type==VfxParameterType::Float;
                    if(row.history_valid){row.history_seconds=std::bit_cast<float>(parameter.bits);row.history_valid=std::isfinite(row.history_seconds);}}
                if(parameter.key==ground&&!row.ground_lock_present)
                {row.ground_lock_present=true;row.ground_lock_valid=parameter.type==VfxParameterType::Int;
                    if(row.ground_lock_valid)row.ground_lock_bits=parameter.bits;}
            }
    }
    for(std::size_t i=0;i<p.outputs.size();++i)
    {
        const auto &output=p.outputs[i];auto &row=outputs[i];
        row.source=output.source;row.profile=output.profile;
        row.shape=output.shape;row.shape_domain=output.shape_domain;row.shape_scale_rule=output.shape_scale_rule;
        row.shape_component_kind=output.shape_component_kind;row.motion=output.motion;
        row.coverage_type=output.coverage_type;row.coverage_ref=output.coverage_ref;row.min_quality=output.min_quality;
        row.surface_target=output.surface_target;row.gradient_row=output.gradient_row;row.textures=output.textures;
        row.rgba={output.rgba[0],output.rgba[1],output.rgba[2],output.rgba[3]};
        row.hdr=output.hdr;row.motion_rate_hz=output.motion_rate_hz;
        row.color_contract_valid=std::all_of(output.rgba.begin(),output.rgba.end(),
            [](float value){return std::isfinite(value);})&&output.rgba[3]>=0.0f&&output.rgba[3]<=1.0f&&
            std::isfinite(output.hdr)&&output.hdr>0.0f;
        row.motion_amplitude=output.motion_amplitude;row.motion_inset_fraction=output.motion_inset_fraction;
        row.parameter_count=output.parameters.count;
        if(!ValidRange(output.parameters,p.parameters.size()))continue;
        if(output.parameters.count>0)row.first_parameter_key=p.parameters[output.parameters.first].key;
        if(output.parameters.count>1)row.second_parameter_key=p.parameters[output.parameters.first+1].key;
        const auto parse_float=[&](std::uint32_t key,bool &present,bool &valid,float &value) {
            for(const auto &parameter:p.parameters.subspan(output.parameters.first,output.parameters.count))
                if(parameter.key==key){set_float(row,key,parameter.type,parameter.bits,present,valid,value);return;}
        };
        parse_float(arc,row.arc_present,row.arc_valid,row.arc_degrees);
        parse_float(head,row.width_head_present,row.width_head_valid,row.width_head);
        parse_float(tail,row.width_tail_present,row.width_tail_valid,row.width_tail);
        parse_float(noise,row.noise_warp_present,row.noise_warp_valid,row.noise_warp);
        parse_float(phase,row.phase_offset_present,row.phase_offset_valid,row.phase_offset);
        parse_float(width,row.width_present,row.width_valid,row.width);
        parse_float(taper,row.taper_present,row.taper_valid,row.taper);
        parse_float(multiplier,row.width_multiplier_present,row.width_multiplier_valid,row.width_multiplier);
        parse_float(reverse,row.reverse_flow_present,row.reverse_flow_valid,row.reverse_flow);
        for(const auto &parameter:p.parameters.subspan(output.parameters.first,output.parameters.count))
        {
            if(parameter.key==segments&&!row.segments_present)
            {row.segments_present=true;row.segments_valid=parameter.type==VfxParameterType::Int;row.segments=parameter.bits;}
            if(parameter.key==pulse&&!row.travel_pulse_present)
            {row.travel_pulse_present=true;row.travel_pulse_valid=parameter.type==VfxParameterType::Bool&&parameter.bits==1;}
        }
    }
    for(std::size_t i=0;i<p.texture_bindings.size();++i)
    {
        const auto &binding=p.texture_bindings[i];auto &row=textures[i];
        row.role=binding.role;row.selection=binding.selection;row.min_quality=binding.min_quality;
        row.strength=binding.strength;row.slices=binding.slices;row.first_frame=binding.first_frame;
        row.frame_count=binding.frame_count;row.fps=binding.fps;
        const auto resource=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),
            [&](const auto &entry){return entry.slot==binding.catalog_slot;});
        if(resource==p.texture_resources.end()||!ValidRange(resource->asset_path_bytes,p.strings.size()))continue;
        const std::string_view path(reinterpret_cast<const char *>(p.strings.data())+resource->asset_path_bytes.first,
            resource->asset_path_bytes.count);
        row.valid_path=true;
        if(path=="Content/Textures/VFX/vfx_gradient_lut.dds")row.asset=VfxRibbonTextureAsset::Gradient;
        else if(path=="Content/Textures/VFX/vfx_curve_lut.dds")row.asset=VfxRibbonTextureAsset::Curve;
        else if(path=="Content/Textures/VFX/vfx_flow_curl_2d.dds")row.asset=VfxRibbonTextureAsset::FlowCurl;
        else if(path=="Content/Textures/VFX/vfx_ribbon_detail_array.dds")row.asset=
            binding.role==ShapeId("profile.optional_detail")?VfxRibbonTextureAsset::OptionalDetail:VfxRibbonTextureAsset::DetailArray;
        else if(path=="Content/Textures/VFX/vfx_stbn_scalar.dds")row.asset=VfxRibbonTextureAsset::Stbn;
        else if(path=="Content/Textures/VFX/vfx_noise_volume_64.dds")row.asset=VfxRibbonTextureAsset::NoiseVolume;
        else if(path.find("ribbon_detail_array")!=std::string_view::npos)row.asset=VfxRibbonTextureAsset::DetailArray;
        else row.asset=VfxRibbonTextureAsset::Other;
    }
}

void PrepareVfxRibbonWakeRecipes(std::span<const VfxRibbonEffectRecipe> effects,
    std::span<const VfxRibbonSourceRecipe> sources,
    std::span<const VfxRibbonOutputRecipe> outputs,
    std::span<const VfxRibbonTextureRecipe> textures,
    std::span<const std::uint32_t> slice_indices,
    std::vector<VfxRange> &ranges, std::vector<VfxRibbonWakeRecipe> &recipes)
{
    constexpr auto effect_id = HashEffectName("persistent.boss.dash_wake");
    constexpr auto detail_role = ShapeId("ribbon_detail_array");
    constexpr auto seed_selection = ShapeId("stable_seed_mod_group_size");
    constexpr auto line_payload = ShapeId("LineAreaPayload");
    const auto valid_range = [](VfxRange range, std::size_t size) {
        return range.first <= size && range.count <= size - range.first;
    };
    ranges.assign(effects.size(), {});
    recipes.clear();
    for (std::size_t effect_index = 0; effect_index < effects.size(); ++effect_index)
    {
        const auto &effect = effects[effect_index];
        if (!effect.valid_handle || effect.handle != effect_index + 1 || effect.effect_id != effect_id ||
            effect.lookup_count != 1 || effect.input_mode != 1 || effect.timing_kind != 1 ||
            effect.payload_kind != line_payload || !valid_range(effect.sources, sources.size()))
            continue;
        auto &range = ranges[effect_index];
        range.first = static_cast<std::uint32_t>(recipes.size());
        for (std::uint32_t source_index = effect.sources.first;
             source_index < effect.sources.first + effect.sources.count; ++source_index)
        {
            const auto &source = sources[source_index];
            if (source.effect != effect.handle || source.source_index != source_index ||
                source.stable_id != ShapeId("wake_rails") || source.type != VfxSourceType::HistoryRibbon ||
                source.knot_count != 2 || source.knots[0] != 0.0f || source.knots[1] != 1.0f ||
                source.parameter_count != 2 || !valid_range(source.outputs, outputs.size()) ||
                source.outputs.count != 1 || !source.history_present || !source.history_valid ||
                source.history_seconds != .24f || !source.ground_lock_present ||
                !source.ground_lock_valid || source.ground_lock_bits != 1u)
                continue;
            const auto &output = outputs[source.outputs.first];
            if (output.source != source_index || output.profile != VfxOutputProfile::RibbonAdd ||
                output.shape != ShapeId("paired_ground_ribbons") ||
                output.shape_domain != ShapeId("ribbon") ||
                output.shape_scale_rule != ShapeId("component_width_and_history") ||
                output.shape_component_kind != ShapeId("ribbon") ||
                output.motion != VfxMotionKind::HistoryRibbonTearsBackward ||
                output.coverage_type != ShapeId("analytic") ||
                output.coverage_ref != ShapeId("ribbon_energy_core") ||
                output.parameter_count != 2 || !valid_range(output.textures, textures.size()) ||
                !output.color_contract_valid || output.gradient_row >= 68u ||
                !output.width_head_present || !output.width_head_valid || output.width_head <= 0.0f ||
                !output.width_tail_present || !output.width_tail_valid || output.width_tail < 0.0f ||
                output.first_parameter_key == output.second_parameter_key)
                continue;
            const VfxRibbonTextureRecipe *detail{};
            for (const auto &binding : textures.subspan(output.textures.first, output.textures.count))
            {
                if (binding.role != detail_role) continue;
                if (detail) { detail = nullptr; break; }
                detail = &binding;
            }
            if (!detail || !detail->valid_path || detail->asset != VfxRibbonTextureAsset::DetailArray ||
                detail->selection != seed_selection || !std::isfinite(detail->strength) ||
                detail->strength < 0.0f || detail->strength > 1.0f ||
                !valid_range(detail->slices, slice_indices.size()) || detail->slices.count != 2)
                continue;
            const auto slices = slice_indices.subspan(detail->slices.first, detail->slices.count);
            if (std::ranges::any_of(slices, [](std::uint32_t slice) { return slice >= 8u; })) continue;
            recipes.push_back({effect.handle, effect.importance, output.min_quality, detail->min_quality,
                output.gradient_row, detail->slices, output.rgba, output.width_head,
                output.width_tail, output.hdr, detail->strength});
        }
        range.count = static_cast<std::uint32_t>(recipes.size()) - range.first;
    }
}

void PrepareVfxRibbonFixedRecipes(std::span<const VfxRibbonEffectRecipe> effects,
    std::span<const VfxRibbonSourceRecipe> sources,
    std::span<const VfxRibbonOutputRecipe> outputs,
    std::span<const VfxRibbonTextureRecipe> textures,
    std::span<const std::uint32_t> slice_indices,
    std::vector<VfxRange> &ranges, std::vector<VfxRibbonFixedRecipe> &recipes)
{
    constexpr auto common_id = HashEffectName("particle.common.pull");
    constexpr auto retreat_id = HashEffectName("particle.skill.retreat_shot.move");
    constexpr auto pre_pull_id = HashEffectName("particle.upgrade.explosive.pre_pull");
    constexpr auto point_payload = ShapeId("PointEventPayload");
    constexpr auto circle_payload = ShapeId("CircleAreaPayload");
    constexpr auto core_source = ShapeId("link_core");
    constexpr auto outer_source = ShapeId("link_outer");
    const auto valid_range = [](VfxRange range, std::size_t size) {
        return range.first <= size && range.count <= size - range.first;
    };
    const auto valid_textures = [&](VfxRange range) {
        if (!valid_range(range, textures.size())) return false;
        for (const auto &binding : textures.subspan(range.first, range.count))
        {
            if (!binding.valid_path || binding.min_quality > 2 ||
                !std::isfinite(binding.strength) || binding.strength != 1.0f ||
                binding.selection != 0 || !valid_range(binding.slices, slice_indices.size()) ||
                binding.slices.count != 0 || binding.first_frame != 0 ||
                binding.frame_count != 0 || binding.fps != 0.0f)
                return false;
            const bool global = binding.role == ShapeId("profile.global") &&
                (binding.asset == VfxRibbonTextureAsset::Gradient ||
                 binding.asset == VfxRibbonTextureAsset::Curve);
            const bool authored = binding.role == ShapeId("profile.authored") &&
                binding.asset == VfxRibbonTextureAsset::FlowCurl;
            const bool optional = binding.role == ShapeId("profile.optional_detail") &&
                binding.asset == VfxRibbonTextureAsset::OptionalDetail;
            if (!global && !authored && !optional) return false;
        }
        return true;
    };
    ranges.assign(effects.size(), {});
    recipes.clear();
    for (std::size_t effect_index = 0; effect_index < effects.size(); ++effect_index)
    {
        const auto &effect = effects[effect_index];
        VfxRibbonFixedKind kind{};
        const auto payload_kind = effect.effect_id == common_id ? point_payload :
            (effect.effect_id == retreat_id ? point_payload : circle_payload);
        if (effect.effect_id == common_id) kind = VfxRibbonFixedKind::CommonPull;
        else if (effect.effect_id == retreat_id) kind = VfxRibbonFixedKind::RetreatMove;
        else if (effect.effect_id == pre_pull_id) kind = VfxRibbonFixedKind::ExplosivePrePull;
        else continue;
        if (!effect.valid_handle || effect.handle != effect_index + 1 ||
            effect.lookup_count != 1 || effect.identity_count != 1 ||
            effect.input_mode != 0 || effect.timing_kind != 0 ||
            effect.payload_kind != payload_kind || effect.sources.count != 2 ||
            !std::isfinite(effect.seconds) || effect.seconds != .45f ||
            !valid_range(effect.sources, sources.size()))
            continue;
        const auto range = effect.sources;
        const VfxRibbonSourceRecipe *core{};
        const VfxRibbonSourceRecipe *outer{};
        std::uint32_t core_index{}, outer_index{};
        bool valid_sources = true;
        for (std::uint32_t source_index = range.first;
             source_index < range.first + range.count; ++source_index)
        {
            const auto &source = sources[source_index];
            const bool is_core = source.stable_id == core_source;
            const bool is_outer = source.stable_id == outer_source;
            if (source.effect != effect.handle || source.source_index != source_index ||
                (!is_core && !is_outer) || (is_core && core) || (is_outer && outer) ||
                source.type != VfxSourceType::HistoryRibbon || source.knot_count != 2 ||
                source.knots[0] != 0.0f || source.knots[1] != 1.0f ||
                source.parameter_count != 0 || source.outputs.count != 1 ||
                !valid_range(source.outputs, outputs.size()) ||
                source.authoritative != (kind == VfxRibbonFixedKind::ExplosivePrePull && is_core ? 1u : 0u))
            {
                valid_sources = false;
                break;
            }
            if (is_core) { core = &source; core_index = source_index; }
            else { outer = &source; outer_index = source_index; }
        }
        if (!valid_sources || !core || !outer) continue;
        const auto &core_record = outputs[core->outputs.first];
        const auto &outer_record = outputs[outer->outputs.first];
        const auto valid_output = [&](const VfxRibbonOutputRecipe &output, std::uint32_t source_index,
                                      bool is_core) {
            const auto profile = is_core ? VfxOutputProfile::RibbonAdd : VfxOutputProfile::RibbonOit;
            const auto motion = is_core ? VfxMotionKind::CurvedLinkTravelingHead :
                                          VfxMotionKind::SoftWiderSheath;
            if (output.source != source_index || output.profile != profile ||
                output.shape != ShapeId("bezier_ribbon") || output.motion != motion ||
                output.coverage_type != ShapeId("analytic") ||
                output.coverage_ref != ShapeId("ribbon_energy_core") ||
                output.min_quality > 2 || output.parameter_count != (is_core ? 3u : 2u) ||
                !output.color_contract_valid || output.gradient_row >= 68u ||
                !valid_textures(output.textures))
                return false;
            if (is_core)
                return output.segments_present && output.segments_valid && output.segments >= 2 &&
                    output.segments <= 63 && output.travel_pulse_present && output.travel_pulse_valid &&
                    output.taper_present && output.taper_valid && output.taper >= 0.0f && output.taper <= 1.0f;
            return output.width_multiplier_present && output.width_multiplier_valid &&
                output.width_multiplier > 0.0f && output.noise_warp_present &&
                output.noise_warp_valid && output.noise_warp >= 0.0f;
        };
        if (!valid_output(core_record, core_index, true) ||
            !valid_output(outer_record, outer_index, false))
            continue;
        VfxRibbonFixedRecipe recipe{};
        recipe.effect_handle = effect.handle;
        recipe.importance = effect.importance;
        recipe.core_min_quality = core_record.min_quality;
        recipe.outer_min_quality = outer_record.min_quality;
        recipe.segments = core_record.segments;
        recipe.kind = kind;
        recipe.seconds = effect.seconds;
        recipe.taper = core_record.taper;
        recipe.outer_width_multiplier = outer_record.width_multiplier;
        recipe.core_output = {core_record.profile, core_record.motion, core_record.rgba,
            0.0f, 0.0f, 0.0f, 0.0f, core_record.hdr, 0.0f,
            core_record.gradient_row, 0xffffffffu, false, true};
        recipe.outer_output = {outer_record.profile, outer_record.motion, outer_record.rgba,
            0.0f, 0.0f, outer_record.noise_warp, 0.0f, outer_record.hdr, 0.0f,
            outer_record.gradient_row, 0xffffffffu, false, false};
        ranges[effect_index] = {static_cast<std::uint32_t>(recipes.size()), 1};
        recipes.push_back(recipe);
    }
}

void PrepareVfxRibbonProjectileRecipes(std::span<const VfxRibbonEffectRecipe> effects,
    std::span<const VfxRibbonSourceRecipe> sources,
    std::span<const VfxRibbonOutputRecipe> outputs,
    std::span<const VfxRibbonTextureRecipe> textures,
    std::span<const std::uint32_t> slice_indices,
    std::vector<VfxRange> &effect_ranges,
    std::vector<VfxRibbonProjectileSourceRecipe> &source_recipes,
    std::vector<VfxRibbonProjectileOutputRecipe> &output_recipes)
{
    const auto valid_range = [](VfxRange range, std::size_t size) {
        return range.first <= size && range.count <= size - range.first;
    };
    const auto accepted_motion = [](VfxMotionKind motion) {
        return motion == VfxMotionKind::HistoryRibbonTaperedFlow ||
            motion == VfxMotionKind::ShortTurbulentWake ||
            motion == VfxMotionKind::TwoPhaseNarrowWakes;
    };
    const auto detail_binding = [](const VfxRibbonTextureRecipe &binding) {
        return binding.valid_path && binding.asset == VfxRibbonTextureAsset::DetailArray;
    };
    effect_ranges.assign(effects.size(), {});
    source_recipes.clear();
    output_recipes.clear();
    for (std::size_t effect_index = 0; effect_index < effects.size(); ++effect_index)
    {
        const auto &effect = effects[effect_index];
        auto &effect_range = effect_ranges[effect_index];
        effect_range.first = static_cast<std::uint32_t>(source_recipes.size());
        if (!effect.valid_handle || effect.handle != effect_index + 1 ||
            !valid_range(effect.sources, sources.size()))
            continue;
        for (std::uint32_t source_index = effect.sources.first;
             source_index < effect.sources.first + effect.sources.count; ++source_index)
        {
            const auto &source = sources[source_index];
            if (source.effect != effect.handle || source.source_index != source_index ||
                source.type != VfxSourceType::HistoryRibbon || !source.history_present ||
                !source.history_valid || source.history_seconds <= 0.0f ||
                source.history_seconds > 62.0f / 60.0f ||
                !valid_range(source.outputs, outputs.size()))
                continue;
            VfxRibbonProjectileSourceRecipe prepared_source{};
            prepared_source.effect_handle = effect.handle;
            prepared_source.source_id = source.stable_id;
            prepared_source.knot_count = source.knot_count;
            prepared_source.history_seconds = source.history_seconds;
            prepared_source.knots = source.knots;
            prepared_source.outputs.first = static_cast<std::uint32_t>(output_recipes.size());
            for (std::uint32_t output_index = source.outputs.first;
                 output_index < source.outputs.first + source.outputs.count; ++output_index)
            {
                const auto &output = outputs[output_index];
                if (output.source != source_index ||
                    (output.profile != VfxOutputProfile::RibbonAdd &&
                     output.profile != VfxOutputProfile::RibbonOit) ||
                    !accepted_motion(output.motion) ||
                    (output.shape != ShapeId("camera_facing_ribbon") &&
                     output.shape != ShapeId("dual_camera_ribbon")) ||
                    !output.width_head_present || !output.width_head_valid ||
                    !output.width_tail_present || !output.width_tail_valid ||
                    (output.noise_warp_present && !output.noise_warp_valid) ||
                    (output.phase_offset_present && !output.phase_offset_valid) ||
                    !valid_range(output.textures, textures.size()))
                    continue;
                VfxRibbonProjectileOutputRecipe prepared_output{};
                prepared_output.min_quality = output.min_quality;
                prepared_output.output.profile = output.profile;
                prepared_output.output.motion = output.motion;
                prepared_output.output.color = output.rgba;
                prepared_output.output.hdr = output.hdr;
                prepared_output.output.gradient_row = output.gradient_row;
                prepared_output.output.width_head = output.width_head;
                prepared_output.output.width_tail = output.width_tail;
                prepared_output.output.noise_warp = output.noise_warp_present ? output.noise_warp : 0.0f;
                prepared_output.output.phase_offset = output.phase_offset_present ? output.phase_offset : 0.0f;
                prepared_output.output.dashed = output.coverage_ref == ShapeId("ribbon_dash_repeat");
                const auto texture_rows = textures.subspan(output.textures.first, output.textures.count);
                const auto detail = std::find_if(texture_rows.begin(), texture_rows.end(),
                    [&](const auto &binding) { return detail_binding(binding) && binding.slices.count != 0; });
                if (detail != texture_rows.end())
                {
                    if (!valid_range(detail->slices, slice_indices.size())) continue;
                    const auto slices = slice_indices.subspan(detail->slices.first, detail->slices.count);
                    if (std::ranges::any_of(slices, [](std::uint32_t slice) { return slice >= 8; })) continue;
                    prepared_output.detail_present = true;
                    prepared_output.detail_slices = detail->slices;
                    prepared_output.detail_strength = detail->strength;
                }
                if (!std::isfinite(prepared_output.output.width_head) ||
                    !std::isfinite(prepared_output.output.width_tail) ||
                    prepared_output.output.width_head <= 0.0f ||
                    prepared_output.output.width_tail < 0.0f)
                    continue;
                output_recipes.push_back(prepared_output);
            }
            prepared_source.outputs.count = static_cast<std::uint32_t>(output_recipes.size()) -
                prepared_source.outputs.first;
            if (prepared_source.outputs.count != 0) source_recipes.push_back(prepared_source);
        }
        effect_range.count = static_cast<std::uint32_t>(source_recipes.size()) - effect_range.first;
    }
}

void PrepareVfxRibbonCancelRecipes(std::span<const VfxRibbonEffectRecipe> effects,
    std::span<const VfxRibbonSourceRecipe> sources,
    std::span<const VfxRibbonOutputRecipe> outputs,
    std::span<const VfxRibbonTextureRecipe> textures,
    std::vector<VfxRange> &ranges, std::vector<VfxRibbonCancelRecipe> &recipes)
{
    constexpr auto effect_id = HashEffectName("particle.skill.charged_shot.cancel");
    constexpr auto payload_kind = ShapeId("EntityAttachmentPayload");
    constexpr auto source_id = ShapeId("retract_ribbon");
    constexpr auto shape_id = ShapeId("dual_retract_ribbon");
    const auto valid_range = [](VfxRange range, std::size_t size) {
        return range.first <= size && range.count <= size - range.first;
    };
    ranges.assign(effects.size(), {});
    recipes.clear();
    for (std::size_t effect_index = 0; effect_index < effects.size(); ++effect_index)
    {
        const auto &effect = effects[effect_index];
        auto &range = ranges[effect_index];
        range.first = static_cast<std::uint32_t>(recipes.size());
        if (effect.effect_id != effect_id || !effect.valid_handle || effect.handle != effect_index + 1 ||
            effect.input_mode != 0 || effect.timing_kind != 0 || effect.payload_kind != payload_kind ||
            !std::isfinite(effect.seconds) || effect.seconds <= 0.0f ||
            !valid_range(effect.sources, sources.size()))
            continue;
        for (std::uint32_t source_index = effect.sources.first;
             source_index < effect.sources.first + effect.sources.count; ++source_index)
        {
            const auto &source = sources[source_index];
            if (source.effect != effect.handle || source.source_index != source_index ||
                source.type != VfxSourceType::HistoryRibbon || source.stable_id != source_id ||
                source.knot_count != 2 || source.knots[0] != 0.0f || source.knots[1] != 1.0f ||
                source.outputs.count != 1 || !source.history_present || !source.history_valid ||
                source.history_seconds <= 0.0f || source.history_seconds > 62.0f / 60.0f ||
                !valid_range(source.outputs, outputs.size()))
                continue;
            const auto &output = outputs[source.outputs.first];
            if (output.source != source_index || output.profile != VfxOutputProfile::RibbonAdd ||
                output.motion != VfxMotionKind::ReverseFlowBow || output.shape != shape_id ||
                output.parameter_count != 3 || !output.color_contract_valid ||
                output.rgba.x < 0.0f || output.rgba.y < 0.0f || output.rgba.z < 0.0f ||
                !output.reverse_flow_present || !output.reverse_flow_valid || output.reverse_flow != 1.0f ||
                !output.width_head_present || !output.width_head_valid || output.width_head <= 0.0f ||
                !output.width_tail_present || !output.width_tail_valid || output.width_tail < 0.0f ||
                output.width_tail > output.width_head ||
                !valid_range(output.textures, textures.size()))
                continue;
            recipes.push_back({effect.handle, effect.importance, source.stable_id,
                output.min_quality, effect.seconds, source.history_seconds,
                output.width_head, output.width_tail, output.rgba, output.profile,
                output.motion, output.hdr, output.gradient_row});
        }
        range.count = static_cast<std::uint32_t>(recipes.size()) - range.first;
    }
}

void PrepareVfxRibbonLinkRecipes(std::span<const VfxRibbonEffectRecipe> effects,
    std::span<const VfxRibbonSourceRecipe> sources,
    std::span<const VfxRibbonOutputRecipe> outputs,
    std::span<const VfxRibbonTextureRecipe> textures,
    std::span<const std::uint32_t> slice_indices,
    std::vector<VfxRibbonLinkEffectRecipe> &effect_recipes,
    std::vector<VfxRibbonLinkOutputRecipe> &output_recipes)
{
    constexpr auto source_target = ShapeId("SourceTargetPayload");
    constexpr auto point_path = ShapeId("ProjectilePathPayload");
    constexpr auto line_payload = ShapeId("LineAreaPayload");
    constexpr auto context_payload = ShapeId("PresentationContextPayload");
    constexpr auto core_id = ShapeId("link_core");
    constexpr auto outer_id = ShapeId("link_outer");
    constexpr auto bezier = ShapeId("bezier_ribbon");
    constexpr auto energy = ShapeId("ribbon_energy_core");
    constexpr auto analytic = ShapeId("analytic");
    constexpr auto detail_role = ShapeId("ribbon_detail_array");
    constexpr auto global_role = ShapeId("profile.global");
    constexpr auto authored_role = ShapeId("profile.authored");
    constexpr auto optional_role = ShapeId("profile.optional_detail");
    constexpr auto detail_selection = ShapeId("stable_seed_mod_group_size");
    const auto valid_range = [](VfxRange range, std::size_t size) {
        return range.first <= size && range.count <= size - range.first;
    };
    const auto detail_facts = [&](VfxRange texture_range, bool strict_roles) {
        std::array<VfxRibbonLinkDetailRecipe,3> details{};
        if (!valid_range(texture_range, textures.size())) {
            for (auto &detail : details) detail.valid = false;
            return details;
        }
        const auto bindings = textures.subspan(texture_range.first, texture_range.count);
        for (std::uint32_t quality = 0; quality < details.size(); ++quality)
        {
            auto &detail = details[quality];
            for (const auto &binding : bindings)
            {
                if (!strict_roles && binding.role != detail_role) continue;
                if (!binding.valid_path || binding.min_quality > 2 ||
                    !std::isfinite(binding.strength) || binding.strength < 0.0f ||
                    binding.strength > 1.0f) { detail.valid = false; break; }
                const bool global = binding.role == global_role &&
                    (binding.asset == VfxRibbonTextureAsset::Gradient ||
                     binding.asset == VfxRibbonTextureAsset::Curve);
                const bool authored = binding.role == authored_role &&
                    binding.asset == VfxRibbonTextureAsset::FlowCurl;
                const bool optional = binding.role == optional_role &&
                    binding.asset == VfxRibbonTextureAsset::OptionalDetail;
                if (binding.role == global_role || binding.role == authored_role ||
                    binding.role == optional_role)
                {
                    if ((!global && !authored && !optional) || binding.min_quality != 0 ||
                        binding.selection != 0 || binding.slices.count != 0 || binding.strength != 1.0f)
                        detail.valid = false;
                    if (!detail.valid) break;
                    continue;
                }
                if (binding.role != detail_role) { detail.valid = false; break; }
                if (binding.min_quality > quality) continue;
                if (binding.asset != VfxRibbonTextureAsset::DetailArray ||
                    !valid_range(binding.slices, slice_indices.size()) || binding.slices.count == 0 ||
                    binding.selection != detail_selection || detail.present)
                { detail.valid = false; break; }
                const auto slices = slice_indices.subspan(binding.slices.first, binding.slices.count);
                if (std::ranges::any_of(slices, [](std::uint32_t slice) { return slice >= 8; }))
                { detail.valid = false; break; }
                detail.present = true;
                detail.slices = binding.slices;
                detail.strength = binding.strength;
            }
        }
        return details;
    };
    effect_recipes.assign(effects.size(), {});
    output_recipes.clear();
    for (std::size_t effect_index = 0; effect_index < effects.size(); ++effect_index)
    {
        const auto &effect = effects[effect_index];
        VfxRibbonLinkKind kind{};
        std::uint32_t payload{};
        std::uint32_t input_mode{}, timing_kind{};
        if (effect.effect_id == HashEffectName("particle.upgrade.arrow.return"))
        { kind = VfxRibbonLinkKind::CapturedPath; payload = point_path; input_mode = 1; }
        else if (effect.effect_id == HashEffectName("particle.upgrade.retreat.second_dash"))
        { kind = VfxRibbonLinkKind::CapturedLine; payload = line_payload; input_mode = 1; }
        else if (effect.effect_id == HashEffectName("particle.upgrade.retreat.afterimage"))
        { kind = VfxRibbonLinkKind::CapturedContext; payload = context_payload; input_mode = 1; }
        else if (effect.effect_id == HashEffectName("particle.pickup.xp.magnet_trail"))
        { kind = VfxRibbonLinkKind::PickupMotion; payload = source_target; input_mode = 0; timing_kind = 1; }
        else if (effect.effect_id == HashEffectName("particle.line.ricochet") ||
                 effect.effect_id == HashEffectName("particle.line.burn_transfer") ||
                 effect.effect_id == HashEffectName("particle.line.relic_chain") ||
                 effect.effect_id == HashEffectName("particle.upgrade.ricochet.return"))
        { kind = VfxRibbonLinkKind::SourceTarget; payload = source_target; input_mode = 1; timing_kind = 1; }
        else if (effect.effect_id == HashEffectName("particle.upgrade.arrow_rain.pull"))
        { kind = VfxRibbonLinkKind::ArrowRain; payload = source_target; input_mode = 0; }
        else if (effect.effect_id == HashEffectName("particle.pickup.xp_collect") ||
                 effect.effect_id == HashEffectName("particle.pickup.heal_collect") ||
                 effect.effect_id == HashEffectName("particle.pickup.magnet_collect") ||
                 effect.effect_id == HashEffectName("particle.pickup.relic_collect"))
        { kind = VfxRibbonLinkKind::PickupCollect; payload = source_target; input_mode = 0; }
        else continue;
        if (!effect.valid_handle || effect.handle != effect_index + 1 ||
            effect.input_mode != input_mode || effect.timing_kind != timing_kind ||
            effect.payload_kind != payload || !valid_range(effect.sources, sources.size()) ||
            (kind == VfxRibbonLinkKind::PickupCollect && effect.identity_count != 1))
            continue;
        if ((kind == VfxRibbonLinkKind::CapturedPath || kind == VfxRibbonLinkKind::CapturedLine ||
             kind == VfxRibbonLinkKind::CapturedContext || kind == VfxRibbonLinkKind::PickupCollect) &&
            (!std::isfinite(effect.seconds) || effect.seconds <= 0.0f))
            continue;
        auto &recipe = effect_recipes[effect_index];
        recipe.effect_handle = effect.handle;
        recipe.importance = effect.importance;
        recipe.kind = kind;
        recipe.seconds = effect.seconds;
        recipe.window_start = kind == VfxRibbonLinkKind::PickupCollect ? .1f : 0.0f;
        recipe.window_end = 1.0f;
        recipe.outputs.first = static_cast<std::uint32_t>(output_recipes.size());
        bool core_found = false;
        if (kind != VfxRibbonLinkKind::PickupCollect)
        {
            for (std::uint32_t si = effect.sources.first; si < effect.sources.first + effect.sources.count; ++si)
            {
                const auto &source = sources[si];
                if (source.effect != effect.handle || source.type != VfxSourceType::HistoryRibbon ||
                    source.stable_id != core_id || source.parameter_count != 0 ||
                    !valid_range(source.outputs, outputs.size())) continue;
                for (const auto &output : outputs.subspan(source.outputs.first, source.outputs.count))
                    if (output.source == si && output.profile == VfxOutputProfile::RibbonAdd &&
                        output.shape == bezier && output.motion == VfxMotionKind::CurvedLinkTravelingHead &&
                        output.parameter_count == 3 && output.taper_present && output.taper_valid &&
                        output.taper >= 0.0f && output.taper <= 1.0f && output.segments_present &&
                        output.segments_valid && output.segments >= 2 && output.segments <= 63 &&
                        output.travel_pulse_present && output.travel_pulse_valid)
                    {
                        if (core_found) { core_found = false; recipe.kind = VfxRibbonLinkKind::Unsupported; break; }
                        core_found = true; recipe.taper = output.taper; recipe.segments = output.segments;
                    }
                if (recipe.kind == VfxRibbonLinkKind::Unsupported) break;
            }
            if (!core_found) recipe.kind = VfxRibbonLinkKind::Unsupported;
        }
        if (recipe.kind != VfxRibbonLinkKind::Unsupported)
        {
            for (std::uint32_t si = effect.sources.first; si < effect.sources.first + effect.sources.count; ++si)
            {
                const auto &source = sources[si];
                const bool core = source.stable_id == core_id;
                const bool outer = source.stable_id == outer_id;
                const bool pickup = kind == VfxRibbonLinkKind::PickupCollect;
                if (source.effect != effect.handle || source.type != VfxSourceType::HistoryRibbon ||
                    source.parameter_count != 0 || (!pickup && !core && !outer) ||
                    (pickup && (source.stable_id != ShapeId("absorb_ribbon") || source.knot_count != 2 ||
                        source.knots[0] != .1f || source.knots[1] != 1.0f)) ||
                    (!pickup && (source.knot_count != 2 || source.knots[0] != 0.0f || source.knots[1] != 1.0f)) ||
                    (pickup && source.outputs.count != 1) || !valid_range(source.outputs, outputs.size())) continue;
                for (const auto &output : outputs.subspan(source.outputs.first, source.outputs.count))
                {
                    if (output.source != si || !valid_range(output.textures, textures.size())) continue;
                    VfxRibbonLinkOutputRecipe prepared{};
                    prepared.source_id = source.stable_id;
                    prepared.core = core;
                    prepared.min_quality = output.min_quality;
                    prepared.output.profile = output.profile;
                    prepared.output.motion = output.motion;
                    prepared.output.color = output.rgba;
                    prepared.output.hdr = output.hdr;
                    prepared.output.gradient_row = output.gradient_row;
                    prepared.output.dashed = output.coverage_ref == ShapeId("ribbon_dash_repeat");
                    if (pickup)
                    {
                        if (output.profile != VfxOutputProfile::RibbonAdd ||
                            output.shape != ShapeId("short_bezier_to_player") ||
                            output.motion != VfxMotionKind::CurvesTowardCollector ||
                            output.coverage_type != analytic || output.coverage_ref != energy ||
                            output.parameter_count != 2 || !output.color_contract_valid ||
                            output.gradient_row >= 68 || !output.segments_present ||
                            !output.segments_valid || output.segments < 2 || output.segments > 63 ||
                            !output.width_present || !output.width_valid || output.width <= 0.0f) continue;
                        prepared.width_multiplier = output.width;
                        prepared.output.width_head = output.width;
                        prepared.output.width_tail = output.width;
                        recipe.segments = output.segments;
                    }
                    else
                    {
                        if (output.profile != (core ? VfxOutputProfile::RibbonAdd : VfxOutputProfile::RibbonOit) ||
                            output.shape != bezier || output.motion != (core ?
                                VfxMotionKind::CurvedLinkTravelingHead : VfxMotionKind::SoftWiderSheath) ||
                            output.parameter_count != (core ? 3u : 2u)) continue;
                        if (!core)
                        {
                            if (!output.width_multiplier_present || !output.width_multiplier_valid ||
                                output.width_multiplier <= 0.0f || !output.noise_warp_present ||
                                !output.noise_warp_valid || output.noise_warp < 0.0f) continue;
                            prepared.width_multiplier = output.width_multiplier;
                            prepared.output.noise_warp = output.noise_warp;
                        }
                        else prepared.output.travel_pulse = true;
                    }
                    if (!std::isfinite(prepared.output.color.x) || !std::isfinite(prepared.output.color.y) ||
                        !std::isfinite(prepared.output.color.z) || !std::isfinite(prepared.output.color.w) ||
                        prepared.output.color.w < 0.0f || prepared.output.color.w > 1.0f ||
                        !std::isfinite(prepared.output.hdr) || prepared.output.hdr <= 0.0f) continue;
                    prepared.details = detail_facts(output.textures, pickup);
                    output_recipes.push_back(prepared);
                }
            }
        }
        recipe.outputs.count = static_cast<std::uint32_t>(output_recipes.size()) - recipe.outputs.first;
        if (recipe.kind != VfxRibbonLinkKind::Unsupported && recipe.outputs.count == 0)
            recipe.kind = VfxRibbonLinkKind::Unsupported;
    }
}

void PrepareVfxRibbonEventRecipes(std::span<const VfxRibbonEffectRecipe> effects,
    std::span<const VfxRibbonSourceRecipe> sources,
    std::span<const VfxRibbonOutputRecipe> outputs,
    std::span<const VfxRibbonTextureRecipe> textures,
    std::span<const std::uint32_t> slice_indices,
    std::vector<VfxRibbonEventEffectRecipe> &effect_recipes,
    std::vector<VfxRibbonEventSourceRecipe> &source_recipes,
    std::vector<VfxRibbonEventOutputRecipe> &output_recipes)
{
    constexpr auto axial_shape = ShapeId("impact_axis_ribbon");
    constexpr auto recoil_shape = ShapeId("short_arc_ribbon");
    constexpr auto detail_asset = VfxRibbonTextureAsset::DetailArray;
    const auto valid_range = [](VfxRange range, std::size_t size) {
        return range.first <= size && range.count <= size - range.first;
    };
    effect_recipes.assign(effects.size(), {});
    source_recipes.clear();
    output_recipes.clear();
    for (std::size_t effect_index = 0; effect_index < effects.size(); ++effect_index)
    {
        const auto &effect = effects[effect_index];
        auto &effect_recipe = effect_recipes[effect_index];
        effect_recipe.sources.first = static_cast<std::uint32_t>(source_recipes.size());
        if (!effect.valid_handle || effect.handle != effect_index + 1 || effect.input_mode != 0 ||
            effect.timing_kind != 0 || !std::isfinite(effect.seconds) || effect.seconds <= 0.0f ||
            !valid_range(effect.sources, sources.size()))
            continue;
        effect_recipe.effect_id = effect.effect_id;
        effect_recipe.effect_handle = effect.handle;
        effect_recipe.importance = effect.importance;
        effect_recipe.payload_kind = effect.payload_kind;
        effect_recipe.seconds = effect.seconds;
        for (std::uint32_t source_index = effect.sources.first;
             source_index < effect.sources.first + effect.sources.count; ++source_index)
        {
            const auto &source = sources[source_index];
            if (source.effect != effect.handle || source.source_index != source_index ||
                source.type != VfxSourceType::HistoryRibbon ||
                !source.history_present || !source.history_valid || source.history_seconds <= 0.0f ||
                source.history_seconds > 62.0f / 60.0f ||
                !source.knots_valid || (source.knot_count != 2 && source.knot_count != 4) ||
                !valid_range(source.outputs, outputs.size()))
                continue;
            VfxRibbonEventSourceRecipe prepared_source{};
            prepared_source.effect_handle = effect.handle;
            prepared_source.source_id = source.stable_id;
            prepared_source.knot_count = source.knot_count;
            prepared_source.axial_source = source.stable_id == ShapeId("axial_afterline");
            prepared_source.knots = source.knots;
            prepared_source.history_seconds = source.history_seconds;
            for (std::uint32_t quality = 0; quality < 3; ++quality)
                for (const auto &output : outputs.subspan(source.outputs.first, source.outputs.count))
                {
                    const bool axial = output.motion == VfxMotionKind::ShortLineIncomingVelocity &&
                        output.shape == axial_shape;
                    const bool recoil = output.motion == VfxMotionKind::CurvedRecoilArc &&
                        output.shape == recoil_shape;
                    if (output.source != source_index ||
                        (output.profile != VfxOutputProfile::RibbonAdd &&
                         output.profile != VfxOutputProfile::RibbonOit) ||
                        (!axial && !recoil) || (axial && output.profile != VfxOutputProfile::RibbonAdd) ||
                        output.min_quality > quality ||
                        (output.arc_present && (!output.arc_valid || output.arc_degrees <= 0.0f ||
                                                 output.arc_degrees > 360.0f)))
                        continue;
                    if (axial && (!output.width_head_present || !output.width_head_valid ||
                        output.width_head <= 0.0f || !output.width_tail_present ||
                        !output.width_tail_valid || output.width_tail < 0.0f ||
                        !std::isfinite(output.rgba.x) || !std::isfinite(output.rgba.y) ||
                        !std::isfinite(output.rgba.z) || !std::isfinite(output.rgba.w) ||
                        output.rgba.w < 0.0f || output.rgba.w > 1.0f ||
                        !std::isfinite(output.hdr) || output.hdr <= 0.0f || output.gradient_row >= 68u))
                        continue;
                    if (!output.arc_present) continue;
                    (axial ? prepared_source.axial_arc_by_quality[quality] :
                             prepared_source.recoil_arc_by_quality[quality]) = output.arc_degrees;
                }
            prepared_source.outputs.first = static_cast<std::uint32_t>(output_recipes.size());
            for (std::uint32_t output_index = source.outputs.first;
                 output_index < source.outputs.first + source.outputs.count; ++output_index)
            {
                const auto &output = outputs[output_index];
                const bool axial = output.motion == VfxMotionKind::ShortLineIncomingVelocity &&
                    output.shape == axial_shape;
                if (output.source != source_index ||
                    (output.profile != VfxOutputProfile::RibbonAdd &&
                     output.profile != VfxOutputProfile::RibbonOit) ||
                    (!axial && (output.motion != VfxMotionKind::CurvedRecoilArc ||
                                output.shape != recoil_shape)) ||
                    (axial && output.profile != VfxOutputProfile::RibbonAdd) ||
                    !valid_range(output.textures, textures.size()) ||
                    (output.arc_present && (!output.arc_valid || output.arc_degrees <= 0.0f ||
                                             output.arc_degrees > 360.0f)))
                    continue;
                if (axial && (!output.width_head_present || !output.width_head_valid ||
                    output.width_head <= 0.0f || !output.width_tail_present ||
                    !output.width_tail_valid || output.width_tail < 0.0f ||
                    !std::isfinite(output.rgba.x) || !std::isfinite(output.rgba.y) ||
                    !std::isfinite(output.rgba.z) || !std::isfinite(output.rgba.w) ||
                    output.rgba.w < 0.0f || output.rgba.w > 1.0f ||
                    !std::isfinite(output.hdr) || output.hdr <= 0.0f || output.gradient_row >= 68u))
                    continue;
                VfxRibbonEventOutputRecipe prepared_output{};
                prepared_output.min_quality = output.min_quality;
                prepared_output.axial = axial;
                prepared_output.arc_present = output.arc_present;
                prepared_output.arc_degrees = output.arc_degrees;
                prepared_output.output.profile = output.profile;
                prepared_output.output.motion = output.motion;
                prepared_output.output.color = output.rgba;
                prepared_output.output.hdr = output.hdr;
                prepared_output.output.gradient_row = output.gradient_row;
                prepared_output.output.width_head = axial ? output.width_head : 0.0f;
                prepared_output.output.width_tail = axial ? output.width_tail : 0.0f;
                prepared_output.output.dashed = output.coverage_ref == ShapeId("ribbon_dash_repeat");
                const auto bindings = textures.subspan(output.textures.first, output.textures.count);
                const auto detail = std::find_if(bindings.begin(), bindings.end(), [&](const auto &binding) {
                    return binding.valid_path && binding.asset == detail_asset && binding.slices.count != 0;
                });
                if (detail != bindings.end())
                {
                    if (!valid_range(detail->slices, slice_indices.size()) ||
                        !std::isfinite(detail->strength) || detail->strength < 0.0f || detail->strength > 1.0f)
                        continue;
                    const auto slices = slice_indices.subspan(detail->slices.first, detail->slices.count);
                    if (std::ranges::any_of(slices, [](std::uint32_t slice) { return slice >= 8; })) continue;
                    prepared_output.detail_present = true;
                    prepared_output.detail_slices = detail->slices;
                    prepared_output.detail_strength = detail->strength;
                }
                output_recipes.push_back(prepared_output);
            }
            prepared_source.outputs.count = static_cast<std::uint32_t>(output_recipes.size()) -
                prepared_source.outputs.first;
            if (prepared_source.outputs.count != 0) source_recipes.push_back(prepared_source);
        }
        effect_recipe.sources.count = static_cast<std::uint32_t>(source_recipes.size()) -
            effect_recipe.sources.first;
    }
}

void PrepareVfxEffectLookupById(VfxProgramView program,
    std::vector<VfxEffectLookupRecord> &lookup_by_id)
{
    lookup_by_id.assign(program.effect_lookup.begin(), program.effect_lookup.end());
    std::stable_sort(lookup_by_id.begin(), lookup_by_id.end(),
        [](const auto &left, const auto &right) { return left.effect_id < right.effect_id; });
}

void PrepareVfxDistortionRecipes(VfxProgramView p, std::vector<VfxRange> &ranges,
                                std::vector<VfxDistortionRecipe> &recipes)
{
    struct Shape { std::uint32_t id; VfxMotionKind motion; VfxDistortionShape kind; float radius; };
    constexpr Shape shapes[]{
        {ShapeId("screen_space_radial_normal"),VfxMotionKind::SingleOutwardRefractivePulse,VfxDistortionShape::RadialRing,.8f},
        {ShapeId("radial_shock"),VfxMotionKind::QuickHeatShock,VfxDistortionShape::HeatShock,.75f},
        {ShapeId("radial_shock"),VfxMotionKind::ShortRefractiveShock,VfxDistortionShape::ShortShock,.75f},
        {ShapeId("lance_heat_envelope"),VfxMotionKind::TightHeadDistortion,VfxDistortionShape::HeadEnvelope,.35f},
        {ShapeId("small_shock_disc"),VfxMotionKind::OneTightPulse,VfxDistortionShape::TightDisc,.7f}};
    const auto valid=[](VfxRange r,std::size_t n){return r.first<=n&&r.count<=n-r.first;};
    ranges.assign(p.effects.size(),{});recipes.clear();
    constexpr std::string_view global_path="Content/Textures/VFX/vfx_stbn_scalar.dds";
    constexpr std::string_view flow_path="Content/Textures/VFX/vfx_flow_curl_2d.dds";
    constexpr std::string_view noise_path="Content/Textures/VFX/vfx_noise_basis_2d.dds";
    const auto texture_path=[&](std::uint32_t slot)->std::optional<std::string_view>{
        const auto resource=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),[&](const auto&r){return r.slot==slot;});
        if(resource==p.texture_resources.end()||!valid(resource->asset_path_bytes,p.strings.size()))return {};
        return std::string_view(reinterpret_cast<const char*>(p.strings.data())+resource->asset_path_bytes.first,resource->asset_path_bytes.count);
    };
    for(std::size_t ei=0;ei<p.effects.size();++ei)
    {
        auto &range=ranges[ei];range.first=static_cast<std::uint32_t>(recipes.size());
        const auto&e=p.effects[ei];
        if(e.handle!=ei+1||!valid(e.sources,p.sources.size()))continue;
        const auto lookup=std::find_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto&v){return v.handle==e.handle;});
        if(lookup==p.effect_lookup.end()||!lookup->effect_id)continue;
        for(std::uint32_t si=e.sources.first;si<e.sources.first+e.sources.count;++si)
        {
            const auto&s=p.sources[si];
            if(s.effect!=e.handle||s.type!=VfxSourceType::Direct||s.parameters.count||!valid(s.parameters,p.parameters.size())||
               !valid(s.outputs,p.outputs.size())||(s.knot_count!=2&&s.knot_count!=4))continue;
            bool knots_valid=true;
            for(std::uint32_t k=0;k<s.knot_count;++k)
                if(!std::isfinite(s.knots[k])||s.knots[k]<0||s.knots[k]>1||(k&&s.knots[k]<s.knots[k-1]))knots_valid=false;
            if(!knots_valid||s.knots[s.knot_count-1]<=s.knots[0])continue;
            for(const auto&o:p.outputs.subspan(s.outputs.first,s.outputs.count))
            {
                const auto shape=std::find_if(std::begin(shapes),std::end(shapes),[&](const auto&v){return v.id==o.shape&&v.motion==o.motion;});
                if(shape==std::end(shapes)||o.source!=si||o.profile!=VfxOutputProfile::Distortion||!valid(o.parameters,p.parameters.size())||
                   !valid(o.textures,p.texture_bindings.size())||o.min_quality>2||o.coverage_type!=ShapeId("analytic_mask")||
                   o.coverage_ref!=shape->id||!std::isfinite(o.rgba[3])||o.rgba[3]<0||o.rgba[3]>1)continue;
                if((e.input_mode==0&&(e.timing_kind!=0||!std::isfinite(e.seconds)||e.seconds<=0))||
                   (e.input_mode==1&&(e.timing_kind!=1||e.payload_kind!=ShapeId("ProjectilePayload")||!std::isfinite(e.seconds)||e.seconds<0))||e.input_mode>1)continue;
                bool textures_ok=true;
                for(const auto&binding:p.texture_bindings.subspan(o.textures.first,o.textures.count))
                {
                    const auto path=texture_path(binding.catalog_slot);
                    if(!path|| (binding.role==ShapeId("profile.global")?*path!=global_path:
                        binding.role==ShapeId("profile.authored")?(*path!=flow_path&&*path!=noise_path):true))
                    {textures_ok=false;break;}
                }
                if(!textures_ok)continue;
                const bool radial=shape->kind==VfxDistortionShape::RadialRing;
                const bool explicit_radius=shape->kind==VfxDistortionShape::HeadEnvelope||shape->kind==VfxDistortionShape::TightDisc;
                if(o.parameters.count!=(radial||explicit_radius?2u:1u))continue;
                float strength{},radius=shape->radius;bool has_strength=false,has_radius=false,has_falloff=false,params_ok=true;
                for(const auto&v:p.parameters.subspan(o.parameters.first,o.parameters.count))
                {
                    if(v.key==ShapeId("strength")&&!has_strength&&v.type==VfxParameterType::Float){strength=std::bit_cast<float>(v.bits);has_strength=true;}
                    else if(v.key==ShapeId("radius")&&explicit_radius&&!has_radius&&v.type==VfxParameterType::Float){radius=std::bit_cast<float>(v.bits);has_radius=true;}
                    else if(v.key==ShapeId("falloff")&&radial&&!has_falloff&&v.type==VfxParameterType::Enum&&v.bits==ShapeId("ring"))has_falloff=true;
                    else {params_ok=false;break;}
                }
                if(!params_ok||!has_strength||!std::isfinite(strength)||strength<=0||!std::isfinite(radius)||radius<=0||
                   (explicit_radius&&!has_radius)||(radial&&!has_falloff))continue;
                recipes.push_back({lookup->effect_id,e.handle,s.stable_id,e.payload_kind,e.importance,e.input_mode,e.timing_kind,
                    o.min_quality,s.knot_count,shape->kind,o.motion,s.knots,e.seconds,radius,strength,o.rgba[3]});
            }
        }
        range.count=static_cast<std::uint32_t>(recipes.size())-range.first;
    }
}

void PrepareVfxDecalRecipes(VfxProgramView p, std::vector<VfxRange> &ranges,
                            std::vector<VfxDecalRecipe> &recipes)
{
    const auto valid=[](VfxRange r,std::size_t n){return r.first<=n&&r.count<=n-r.first;};
    ranges.assign(p.effects.size(),{});recipes.clear();
    constexpr std::string_view gradient="Content/Textures/VFX/vfx_gradient_lut.dds";
    constexpr std::string_view fracture="Content/Textures/VFX/vfx_fracture_decal_array.dds";
    constexpr std::string_view organic="Content/Textures/VFX/vfx_organic_decal_array.dds";
    const auto texture_path=[&](std::uint32_t slot)->std::optional<std::string_view>{
        const auto resource=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),[&](const auto&r){return r.slot==slot;});
        if(resource==p.texture_resources.end()||!valid(resource->asset_path_bytes,p.strings.size()))return {};
        return std::string_view(reinterpret_cast<const char*>(p.strings.data())+resource->asset_path_bytes.first,resource->asset_path_bytes.count);
    };
    for(std::size_t ei=0;ei<p.effects.size();++ei)
    {
        auto &range=ranges[ei];range.first=static_cast<std::uint32_t>(recipes.size());
        const auto&e=p.effects[ei];
        if(e.handle!=ei+1||!valid(e.sources,p.sources.size()))continue;
        const auto lookup=std::find_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto&v){return v.handle==e.handle;});
        if(lookup==p.effect_lookup.end()||!lookup->effect_id)continue;
        for(std::uint32_t si=e.sources.first;si<e.sources.first+e.sources.count;++si)
        {
            const auto&s=p.sources[si];
            if(s.effect!=e.handle||s.type!=VfxSourceType::Direct||s.parameters.count||!valid(s.parameters,p.parameters.size())||
               !valid(s.outputs,p.outputs.size())||(s.knot_count!=2&&s.knot_count!=4))continue;
            bool knots_valid=true;
            for(std::uint32_t k=0;k<s.knot_count;++k)
                if(!std::isfinite(s.knots[k])||s.knots[k]<0||s.knots[k]>1||(k&&s.knots[k]<s.knots[k-1]))knots_valid=false;
            if(!knots_valid||s.knots[s.knot_count-1]<=s.knots[0])continue;
            for(const auto&o:p.outputs.subspan(s.outputs.first,s.outputs.count))
            {
                VfxDecalKind kind;std::uint32_t base_cells{};float edge_glow{};
                const bool is_fracture=o.shape==ShapeId("voronoi_crack_decal")&&o.motion==VfxMotionKind::FractureAtPeak&&
                    o.coverage_type==ShapeId("analytic_or_projected")&&o.coverage_ref==o.shape;
                const bool is_organic=o.shape==ShapeId("organic_ground_decal")&&o.motion==VfxMotionKind::StaticProjectedResidue&&
                    o.coverage_type==ShapeId("projected_texture_mask");
                if(is_fracture)kind=VfxDecalKind::Fracture;else if(is_organic)kind=VfxDecalKind::Organic;else continue;
                if(o.source!=si||o.profile!=VfxOutputProfile::Decal||!valid(o.parameters,p.parameters.size())||!valid(o.textures,p.texture_bindings.size())||
                   o.min_quality>2||!std::isfinite(o.hdr)||o.hdr<0||std::any_of(o.rgba.begin(),o.rgba.end(),[](float v){return !std::isfinite(v)||v<0;})||o.rgba[3]>1)continue;
                if((e.input_mode==0&&(e.timing_kind!=0||!std::isfinite(e.seconds)||e.seconds<=0))||
                   (e.input_mode==1&&(e.timing_kind!=1||!std::isfinite(e.seconds)||e.seconds<0))||e.input_mode>1)continue;
                bool params_ok=true;
                if(kind==VfxDecalKind::Organic){if(o.parameters.count!=0)continue;}
                else
                {
                    if(o.parameters.count!=2)continue;bool cells=false,glow=false;
                    for(const auto&v:p.parameters.subspan(o.parameters.first,o.parameters.count))
                    {
                        if(v.key==ShapeId("voronoi_cells")&&v.type==VfxParameterType::Int&&!cells){base_cells=v.bits;cells=true;}
                        else if(v.key==ShapeId("edge_glow")&&v.type==VfxParameterType::Float&&!glow){edge_glow=std::bit_cast<float>(v.bits);glow=true;}
                        else {params_ok=false;break;}
                    }
                    if(!params_ok||!cells||!glow||base_cells==0||base_cells>0x7fffffff||!std::isfinite(edge_glow)||edge_glow<0||edge_glow>1)continue;
                }
                bool detail=false,coverage=false,global=false,texture_ok=true;std::uint32_t slice_base{},texture_min_quality{};
                const auto required=kind==VfxDecalKind::Fracture?fracture:organic;
                for(const auto&b:p.texture_bindings.subspan(o.textures.first,o.textures.count))
                {
                    const auto path=texture_path(b.catalog_slot);
                    if(!path||!std::isfinite(b.strength)||b.strength!=1||b.min_quality>2||!valid(b.slices,p.slice_indices.size())){texture_ok=false;break;}
                    texture_min_quality=std::max(texture_min_quality,b.min_quality);
                    if(b.role==ShapeId("profile.global")){if(*path!=gradient||b.slices.count!=0){texture_ok=false;break;}global=true;}
                    else if(b.role==ShapeId("profile.authored")){if((*path!=fracture&&*path!=organic)||b.slices.count!=0){texture_ok=false;break;}}
                    else if(b.role==ShapeId("coverage")){if(kind!=VfxDecalKind::Organic||coverage||*path!=organic||b.slices.count!=0){texture_ok=false;break;}coverage=true;}
                    else if(b.role==ShapeId("texture_array"))
                    {
                        if(detail||*path!=required||b.selection!=ShapeId("stable_seed_mod_4")||b.slices.count!=4){texture_ok=false;break;}
                        const auto slices=p.slice_indices.subspan(b.slices.first,4);slice_base=slices[0];
                        if(slice_base!=0&&(kind!=VfxDecalKind::Organic||slice_base!=4)){texture_ok=false;break;}
                        for(std::uint32_t k=0;k<4;++k)if(slices[k]!=slice_base+k)texture_ok=false;
                        detail=true;
                    }
                    else {texture_ok=false;break;}
                }
                if(!texture_ok||!global||!detail||(kind==VfxDecalKind::Organic&&!coverage))continue;
                recipes.push_back({lookup->effect_id,e.handle,s.stable_id,e.payload_kind,e.importance,e.input_mode,e.timing_kind,
                    o.min_quality,s.knot_count,kind,s.knots,e.seconds,o.hdr,o.rgba[3],edge_glow,
                    {o.rgba[0],o.rgba[1],o.rgba[2]},o.gradient_row,base_cells,slice_base,texture_min_quality});
            }
        }
        range.count=static_cast<std::uint32_t>(recipes.size())-range.first;
    }
}

void PrepareVfxLightRecipes(VfxProgramView p, std::vector<VfxRange> &ranges,
                            std::vector<VfxLightRecipe> &recipes)
{
    ranges.assign(p.effects.size(), {}); recipes.clear();
    const auto valid_range=[](VfxRange r,std::size_t n){return r.first<=n && r.count<=n-r.first;};
    const auto mix=[](std::uint64_t v){v^=v>>30;v*=0xbf58476d1ce4e5b9ull;v^=v>>27;v*=0x94d049bb133111ebull;return v^(v>>31);};
    for(std::size_t ei=0;ei<p.effects.size();++ei)
    {
        auto &range=ranges[ei]; range.first=static_cast<std::uint32_t>(recipes.size());
        const auto &e=p.effects[ei];
        if(e.handle!=ei+1 || e.input_mode!=0 || e.timing_kind!=0 || !std::isfinite(e.seconds) || e.seconds<=0 || !valid_range(e.sources,p.sources.size()))continue;
        const auto id=std::find_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto &v){return v.handle==e.handle;});
        if(id==p.effect_lookup.end() || !id->effect_id)continue;
        for(std::uint32_t si=e.sources.first;si<e.sources.first+e.sources.count;++si)
        {
            const auto &source=p.sources[si];
            if(source.effect!=e.handle || source.type!=VfxSourceType::Direct || source.parameters.count ||
                !valid_range(source.parameters,p.parameters.size()) || !valid_range(source.outputs,p.outputs.size()) ||
                (source.knot_count!=2 && source.knot_count!=4))continue;
            bool valid=true;
            for(std::uint32_t k=0;k<source.knot_count;++k)
                if(!std::isfinite(source.knots[k]) || source.knots[k]<0 || source.knots[k]>1 || (k && source.knots[k]<source.knots[k-1]))valid=false;
            if(!valid || source.knots[source.knot_count-1]<=source.knots[0])continue;
            for(const auto &o:p.outputs.subspan(source.outputs.first,source.outputs.count))
            {
                const bool hero=o.motion==VfxMotionKind::SingleHighPriorityLightPulse;
                if(o.source!=si || o.profile!=VfxOutputProfile::Light || (!hero && o.motion!=VfxMotionKind::SingleCappedLightPulse && o.motion!=VfxMotionKind::GentleGreenPulse) ||
                    !valid_range(o.parameters,p.parameters.size()) || o.parameters.count!=(hero?1u:2u))continue;
                const VfxParameterRecord *radius=nullptr,*curve=nullptr; valid=true;
                for(const auto &a:p.parameters.subspan(o.parameters.first,o.parameters.count))
                    if(a.key==ShapeId("radius") && !radius)radius=&a;
                    else if(a.key==ShapeId("intensity_curve_ref") && !curve)curve=&a;
                    else valid=false;
                if(!valid || !radius || radius->type!=VfxParameterType::Float)continue;
                const float metres=std::bit_cast<float>(radius->bits);
                if(!std::isfinite(metres) || metres<=0)continue;
                VfxRange keys{};std::uint32_t interpolation{};
                if(!hero)
                {
                    if(!curve || curve->type!=VfxParameterType::CurveRow || curve->bits>=p.curves.size())continue;
                    const auto &c=p.curves[curve->bits];
                    if(c.interpolation>1 || c.domain!=ShapeId("normalized_age") || !valid_range(c.keys,p.curve_keys.size()) || c.keys.count<2)continue;
                    auto values=p.curve_keys.subspan(c.keys.first,c.keys.count);
                    if(values.front().time!=0 || values.back().time!=1)continue;
                    valid=true;
                    for(std::size_t k=0;k<values.size();++k)
                        if(!std::isfinite(values[k].time)||!std::isfinite(values[k].value)||values[k].value<0||(k&&values[k].time<=values[k-1].time))valid=false;
                    if(!valid)continue;keys=c.keys;interpolation=c.interpolation;
                }
                if(!std::isfinite(o.rgba[0])||!std::isfinite(o.rgba[1])||!std::isfinite(o.rgba[2])||o.rgba[0]<0||o.rgba[1]<0||o.rgba[2]<0||
                    !std::isfinite(o.hdr)||o.hdr<=0||!std::isfinite(o.rgba[3])||o.rgba[3]<0||o.rgba[3]>1)continue;
                const auto output_id=(static_cast<std::uint64_t>(o.motion)<<32)|o.gradient_row;
                recipes.push_back({mix(id->effect_id)^mix(source.stable_id)^mix(output_id),e.payload_kind,e.importance,o.min_quality,source.knot_count,
                    e.seconds,metres,source.knots,{o.rgba[0],o.rgba[1],o.rgba[2]},o.hdr,o.rgba[3],keys,interpolation,hero});
            }
        }
        range.count=static_cast<std::uint32_t>(recipes.size())-range.first;
    }
}

void PrepareVfxGroundRecipes(VfxProgramView p, std::vector<VfxRange> &ranges,
                             std::vector<VfxGroundRecipe> &recipes)
{
    const auto valid=[](VfxRange r,std::size_t n){return r.first<=n&&r.count<=n-r.first;};
    ranges.assign(p.effects.size(),{}); recipes.clear();
    for(std::size_t ei=0;ei<p.effects.size();++ei)
    {
        const auto &effect=p.effects[ei]; auto &range=ranges[ei];
        range.first=static_cast<std::uint32_t>(recipes.size());
        if(effect.handle!=ei+1 || !valid(effect.sources,p.sources.size())) continue;
        const auto identity=std::find_if(p.effect_lookup.begin(),p.effect_lookup.end(),
            [&](const auto &entry){return entry.handle==effect.handle;});
        const auto identity_count=std::count_if(p.effect_lookup.begin(),p.effect_lookup.end(),
            [&](const auto &entry){return entry.handle==effect.handle;});
        const auto base_recipe=[&](std::uint32_t si, std::uint32_t oi, const VfxSourceRecord &source,
                                   const VfxOutputRecord &output, VfxGroundRecipeKind kind){
            VfxGroundRecipe recipe{};
            recipe.effect_id=identity==p.effect_lookup.end()?0:identity->effect_id;
            recipe.effect_handle=effect.handle; recipe.source_index=si; recipe.source_stable_id=source.stable_id;
            recipe.output_index=oi; recipe.gradient_row=output.gradient_row; recipe.payload_kind=effect.payload_kind;
            recipe.timing_kind=effect.timing_kind; recipe.min_quality=output.min_quality;
            recipe.source_type=source.type; recipe.profile=output.profile; recipe.motion=output.motion; recipe.kind=kind;
            recipe.knot_count=source.knot_count; recipe.knots=source.knots; recipe.seconds=effect.seconds;
            recipe.rgb={output.rgba[0],output.rgba[1],output.rgba[2]}; recipe.alpha=output.rgba[3]; recipe.hdr=output.hdr;
            recipe.motion_rate_hz=output.motion_rate_hz; recipe.motion_amplitude=output.motion_amplitude;
            recipe.motion_inset_fraction=output.motion_inset_fraction;
            recipe.source_output_count=source.outputs.count;
            recipe.effect_lookup_count=static_cast<std::uint32_t>(identity_count);
            recipe.source_parameters_empty=source.parameters.count==0 && valid(source.parameters,p.parameters.size());
            return recipe;
        };
        const auto parameter_bits=[&](const VfxOutputRecord &output, std::uint32_t key,
                                      VfxParameterType type)->std::optional<std::uint32_t>{
            if(!valid(output.parameters,p.parameters.size())) return std::nullopt;
            const VfxParameterRecord *found=nullptr;
            for(const auto &v:p.parameters.subspan(output.parameters.first,output.parameters.count))
                if(v.key==key)
                {
                    if(found || v.type!=type) return std::nullopt;
                    found=&v;
                }
            return found?std::optional<std::uint32_t>{found->bits}:std::nullopt;
        };
        for(std::uint32_t si=effect.sources.first;si<effect.sources.first+effect.sources.count;++si)
        {
            const auto &source=p.sources[si];
            if(source.effect!=effect.handle || source.type!=VfxSourceType::Direct ||
                (source.knot_count!=2 && source.knot_count!=4) ||
                !std::all_of(source.knots.begin(), source.knots.begin() + source.knot_count,
                    [](float knot) { return std::isfinite(knot) && knot >= 0.0f && knot <= 1.0f; }) ||
                source.knots[source.knot_count - 1] <= source.knots[0] ||
                !std::is_sorted(source.knots.begin(), source.knots.begin() + source.knot_count) ||
                !valid(source.outputs,p.outputs.size())) continue;
            for(std::uint32_t oi=source.outputs.first;oi<source.outputs.first+source.outputs.count;++oi)
            {
                const auto &output=p.outputs[oi];
                if(output.profile==VfxOutputProfile::GroundSdfAdd && output.shape==ShapeId("expanding_ring_sdf") &&
                    output.motion==VfxMotionKind::FastExpansionSurface)
                {
                    if(source.knot_count!=2 || !(source.knots[1]>source.knots[0]) || output.source!=si ||
                        output.min_quality>2 || !valid(output.parameters,p.parameters.size()) ||
                        !std::isfinite(output.hdr) || output.hdr<=0 ||
                        !std::all_of(output.rgba.begin(),output.rgba.end(),[](float v){return std::isfinite(v);}) ||
                        !std::isfinite(output.motion_rate_hz) || output.motion_rate_hz<0 ||
                        !std::isfinite(output.motion_amplitude) || output.motion_amplitude<0 || output.motion_amplitude>1 ||
                        !std::isfinite(output.motion_inset_fraction) || output.motion_inset_fraction<0 || output.motion_inset_fraction>1)
                        continue;
                    const auto parameter=[&](std::uint32_t key)->std::optional<float>{
                        const VfxParameterRecord *found=nullptr;
                        for(const auto &v:p.parameters.subspan(output.parameters.first,output.parameters.count))
                            if(v.key==key)
                            {
                                if(found || v.type!=VfxParameterType::Float) return std::nullopt;
                                found=&v;
                            }
                        if(!found) return std::nullopt;
                        const float value=std::bit_cast<float>(found->bits);
                        return std::isfinite(value)?std::optional<float>{value}:std::nullopt;
                    };
                    const auto start=parameter(ShapeId("start_radius"));
                    const auto end=parameter(ShapeId("end_radius"));
                    const auto edge=parameter(ShapeId("edge_width"));
                    if(!start || !end || !edge || *start<=0 || *end<=*start || *edge<=0) continue;
                    VfxGroundRecipe recipe{};
                    recipe=base_recipe(si,oi,source,output,VfxGroundRecipeKind::ExpandingRing);
                    recipe.primary=*start; recipe.secondary=*end; recipe.tertiary=*edge;
                    recipes.push_back(recipe);
                    continue;
                }
                if(identity_count==1 && identity!=p.effect_lookup.end() &&
                    identity->effect_id==HashEffectName("particle.boss.shockwave.safe_gap_marker") &&
                    source.stable_id==ShapeId("main_rune") && output.profile==VfxOutputProfile::GroundSdfSoft &&
                    output.shape==ShapeId("polar_rune_sdf") && output.motion==VfxMotionKind::SlowCounterRotationStableFootprint &&
                    output.source==si && output.min_quality<=2 && valid(output.parameters,p.parameters.size()) &&
                    std::isfinite(output.hdr) && output.hdr>0 &&
                    std::all_of(output.rgba.begin(),output.rgba.end(),[](float v){return std::isfinite(v);}))
                {
                    const auto spokes=parameter_bits(output,ShapeId("spokes"),VfxParameterType::Int);
                    const auto rings=parameter_bits(output,ShapeId("ring_count"),VfxParameterType::Int);
                    const auto speed=parameter_bits(output,ShapeId("rotation_speed"),VfxParameterType::Float);
                    if(!spokes || !rings || !speed || !*spokes || !*rings || *spokes>0x7fffffffu || *rings>0x7fffffffu ||
                        !std::isfinite(std::bit_cast<float>(*speed))) continue;
                    auto recipe=base_recipe(si,oi,source,output,VfxGroundRecipeKind::SafeSector);
                    recipe.tick_count=*spokes; recipe.pulse_curve_interpolation=*rings;
                    recipe.primary=std::bit_cast<float>(*speed);
                    recipes.push_back(recipe);
                    continue;
                }
                if(identity_count==1 && identity->effect_id==HashEffectName("particle.skill.charged_shot.pulse") &&
                    effect.input_mode==0 && effect.sources.count==2 && effect.anchor==ShapeId("presentation_context") &&
                    effect.orientation==ShapeId("effect_defined") && effect.scale==ShapeId("authored") &&
                    effect.binding_timing==ShapeId("charge_ratio") && effect.geometry==ShapeId("cosmetic_only") &&
                    effect.must_match_logic==0 && source.type==VfxSourceType::Direct &&
                    source.parameters.count==0 && valid(source.parameters,p.parameters.size()) &&
                    (source.knot_count==2 || source.knot_count==4) &&
                    source.knots[source.knot_count-1]>source.knots[0] && source.outputs.count==1 &&
                    valid(source.outputs,p.outputs.size()) && source.outputs.first==oi)
                {
                    const bool ring=source.stable_id==ShapeId("gameplay_boundary");
                    if((!ring && source.stable_id!=ShapeId("interior_secondary")) || output.source!=si ||
                        output.profile!=(ring?VfxOutputProfile::GroundSdfSoft:VfxOutputProfile::GroundSdfOit) ||
                        output.shape!=(ring?ShapeId("exact_hitbox_ring"):ShapeId("low_frequency_fill")) ||
                        output.shape_domain!=(ring?ShapeId("sdf"):ShapeId("sprite")) ||
                        output.shape_scale_rule!=(ring?ShapeId("gameplay_geometry_if_bound_else_component_radius"):ShapeId("component_size")) ||
                        output.shape_component_kind!=ShapeId("ground_sdf") || output.coverage_type!=ShapeId("analytic") ||
                        output.coverage_ref!=output.shape || output.motion!=(ring?VfxMotionKind::StableBoundaryInwardPulse:VfxMotionKind::SlowRadialFlow) ||
                        output.min_quality!=0 || output.parameters.count!=(ring?2u:1u) ||
                        !PrepareStateRingTextureContract(p,output,true) || !std::isfinite(output.hdr) || output.hdr<=0 ||
                        !std::isfinite(output.motion_rate_hz) || output.motion_rate_hz<0 ||
                        !std::isfinite(output.motion_amplitude) || output.motion_amplitude<0 || output.motion_amplitude>1 ||
                        !std::isfinite(output.motion_inset_fraction) || output.motion_inset_fraction<0 || output.motion_inset_fraction>1 ||
                        !std::all_of(output.rgba.begin(),output.rgba.end(),[](float v){return std::isfinite(v)&&v>=0;}) || output.rgba[3]>1) continue;
                    const auto primary=parameter_bits(output,ring?ShapeId("edge_width_world"):ShapeId("flow_speed"),VfxParameterType::Float);
                    const auto aa=ring?parameter_bits(output,ShapeId("anti_alias"),VfxParameterType::Enum):std::optional<std::uint32_t>{};
                    if(!primary || (ring&&(!aa || *aa!=ShapeId("fwidth")))) continue;
                    const float value=std::bit_cast<float>(*primary);
                    if(!std::isfinite(value)||value<=0||(ring&&value>=.65f)) continue;
                    auto recipe=base_recipe(si,oi,source,output,ring?VfxGroundRecipeKind::ChargedBoundary:VfxGroundRecipeKind::ChargedInterior);
                    recipe.primary=value;
                    recipes.push_back(recipe);
                    continue;
                }
                if(identity_count==1 && source.knot_count==2 && source.knots[1]>source.knots[0] &&
                    source.parameters.count==0 && valid(source.parameters,p.parameters.size()) &&
                    output.source==si && output.min_quality<=2 && valid(output.parameters,p.parameters.size()) &&
                    std::isfinite(output.hdr) && output.hdr>0 &&
                    std::isfinite(output.motion_rate_hz) && output.motion_rate_hz>=0 &&
                    std::isfinite(output.motion_amplitude) && output.motion_amplitude>=0 && output.motion_amplitude<=1 &&
                    std::isfinite(output.motion_inset_fraction) && output.motion_inset_fraction>=0 && output.motion_inset_fraction<=1 &&
                    std::all_of(output.rgba.begin(),output.rgba.end(),[](float v){return std::isfinite(v);}))
                {
                    const auto effect_id=identity->effect_id;
                    const bool persistent_entity_contract=effect.input_mode==1 && effect.timing_kind==1 &&
                        effect.payload_kind==ShapeId("EntityAttachmentPayload");
                    VfxGroundRecipeKind kind{};
                    if(persistent_entity_contract && effect_id==HashEffectName("particle.player.invulnerable_loop") && source.stable_id==ShapeId("ground_state") &&
                        output.profile==VfxOutputProfile::GroundSdfSoft && output.shape==ShapeId("state_ring") &&
                        output.shape_domain==ShapeId("sdf") && output.shape_scale_rule==ShapeId("gameplay_geometry_if_bound_else_component_radius") &&
                        output.shape_component_kind==ShapeId("ground_sdf") && output.coverage_type==ShapeId("analytic") &&
                        output.coverage_ref==ShapeId("state_ring") && output.motion==VfxMotionKind::SlowStableRotation && output.parameters.count==1)
                        kind=VfxGroundRecipeKind::StateRing;
                    const bool polar_effect=effect_id==HashEffectName("persistent.status.slow") || effect_id==HashEffectName("persistent.status.mark") ||
                        effect_id==HashEffectName("particle.pickup.xp.idle") || effect_id==HashEffectName("particle.pickup.heal.idle") ||
                        effect_id==HashEffectName("particle.pickup.magnet.idle") || effect_id==HashEffectName("particle.pickup.relic_chest.idle") ||
                        effect_id==HashEffectName("particle.upgrade.charged.overcharge_loop") ||
                        effect_id==HashEffectName("particle.enemy.suicide.countdown_loop");
                    if(persistent_entity_contract && polar_effect && source.stable_id==ShapeId("main_rune") && output.profile==VfxOutputProfile::GroundSdfSoft &&
                        output.shape==ShapeId("polar_rune_sdf") && output.shape_domain==ShapeId("sdf") &&
                        output.shape_scale_rule==ShapeId("gameplay_geometry_if_bound_else_component_radius") &&
                        output.shape_component_kind==ShapeId("ground_sdf") && output.coverage_type==ShapeId("analytic") &&
                        output.coverage_ref==ShapeId("polar_rune_sdf") &&
                        output.motion==VfxMotionKind::SlowCounterRotationStableFootprint && output.parameters.count==3)
                        kind=VfxGroundRecipeKind::PolarRune;
                    const bool phase2=effect_id==HashEffectName("persistent.boss.phase2_aura");
                    const bool transition=effect_id==HashEffectName("persistent.boss.phase_transition_invulnerable");
                    if(persistent_entity_contract && phase2 && output.profile==VfxOutputProfile::GroundSdfAdd && output.shape==ShapeId("broken_crown_sdf") &&
                        output.motion==VfxMotionKind::InwardTeethPulse) kind=VfxGroundRecipeKind::BrokenCrown;
                    if(persistent_entity_contract && transition && output.profile==VfxOutputProfile::GroundSdfAdd && output.shape==ShapeId("closed_crown_ring_sdf") &&
                        output.motion==VfxMotionKind::LockedSteadyRingPulse) kind=VfxGroundRecipeKind::ClosedCrownRing;
                    if(kind==VfxGroundRecipeKind::StateRing || kind==VfxGroundRecipeKind::PolarRune ||
                       kind==VfxGroundRecipeKind::BrokenCrown || kind==VfxGroundRecipeKind::ClosedCrownRing)
                    {
                        if(output.rgba[3]<0.0f || output.rgba[3]>1.0f) continue;
                        VfxGroundRecipe recipe=base_recipe(si,oi,source,output,kind);
                        if(kind==VfxGroundRecipeKind::StateRing)
                        {
                            recipe.texture_contract_valid=PrepareStateRingTextureContract(p,output,false);
                            if(!recipe.texture_contract_valid) continue;
                            const auto segments=parameter_bits(output,ShapeId("segments"),VfxParameterType::Int);
                            if(!segments || *segments<3 || *segments>64 || output.rgba[3]<0 || output.rgba[3]>1) continue;
                            recipe.tick_count=*segments;
                        }
                        else if(kind==VfxGroundRecipeKind::PolarRune)
                        {
                            if(effect_id==HashEffectName("particle.pickup.relic_chest.idle"))
                            {
                                PrepareRelicMaskVariants(p,output,recipe);
                                bool any_quality=false;
                                for(std::uint32_t q=output.min_quality;q<3;++q) any_quality=any_quality||recipe.mask_variants[q].valid;
                                if(!any_quality) continue;
                            }
                            else
                            {
                                recipe.texture_contract_valid=PrepareStateRingTextureContract(p,output,false);
                                if(!recipe.texture_contract_valid) continue;
                            }
                            const auto spokes=parameter_bits(output,ShapeId("spokes"),VfxParameterType::Int);
                            const auto rings=parameter_bits(output,ShapeId("ring_count"),VfxParameterType::Int);
                            const auto speed=parameter_bits(output,ShapeId("rotation_speed"),VfxParameterType::Float);
                            if(!spokes || !rings || !speed || *spokes!=6 || *rings!=2 || output.rgba[3]<0 || output.rgba[3]>1) continue;
                            recipe.tick_count=*spokes; recipe.pulse_curve_interpolation=*rings; recipe.primary=std::bit_cast<float>(*speed);
                            if(!std::isfinite(recipe.primary) || recipe.primary<=0) continue;
                        }
                        else
                        {
                            const auto pulse=parameter_bits(output,ShapeId("pulse_hz"),VfxParameterType::Float);
                            if(!pulse) continue;
                            recipe.primary=std::bit_cast<float>(*pulse);
                            if(!std::isfinite(recipe.primary) || recipe.primary<=0) continue;
                            if(kind==VfxGroundRecipeKind::BrokenCrown)
                            {
                                const auto inner=parameter_bits(output,ShapeId("inner_radius"),VfxParameterType::Float);
                                if(!inner) continue;
                                recipe.secondary=std::bit_cast<float>(*inner);
                                if(!std::isfinite(recipe.secondary) || recipe.secondary<=0) continue;
                            }
                            else
                            {
                                const auto width=parameter_bits(output,ShapeId("edge_width"),VfxParameterType::Float);
                                if(!width) continue;
                                recipe.secondary=std::bit_cast<float>(*width);
                                if(!std::isfinite(recipe.secondary) || recipe.secondary<=0) continue;
                            }
                        }
                        recipes.push_back(recipe);
                        continue;
                    }
                }
                VfxGroundRecipeKind authored_kind{};
                if(output.profile==VfxOutputProfile::GroundSdfAdd && output.shape==ShapeId("hex_constellation_sdf") &&
                    output.motion==VfxMotionKind::HexSegmentsCollapse) authored_kind=VfxGroundRecipeKind::HexConstellation;
                else if(output.profile==VfxOutputProfile::GroundSdfAdd && output.shape==ShapeId("large_signature_sdf") &&
                    output.motion==VfxMotionKind::StrongSnapGameplayResolve) authored_kind=VfxGroundRecipeKind::BossSignature;
                else if(output.profile==VfxOutputProfile::GroundSdfAdd && output.shape==ShapeId("cross_ring_sdf") &&
                    output.motion==VfxMotionKind::RingContractsReleases) authored_kind=VfxGroundRecipeKind::CrossRing;
                else if(output.profile==VfxOutputProfile::GroundSdfAdd && output.shape==ShapeId("broken_hex_sdf") &&
                    output.motion==VfxMotionKind::QuickInwardFracture) authored_kind=VfxGroundRecipeKind::BrokenHex;
                else if(output.profile==VfxOutputProfile::GroundSdfAdd && output.shape==ShapeId("axial_fracture_sdf") &&
                    output.motion==VfxMotionKind::StretchesProjectileDirection) authored_kind=VfxGroundRecipeKind::AxialFracture;
                else if(output.profile==VfxOutputProfile::GroundSdfAdd && output.shape==ShapeId("repeating_chevron_sdf") &&
                    output.motion==VfxMotionKind::ChevronsDissipate) authored_kind=VfxGroundRecipeKind::RepeatingChevron;
                if(authored_kind==VfxGroundRecipeKind::HexConstellation || authored_kind==VfxGroundRecipeKind::BossSignature ||
                   authored_kind==VfxGroundRecipeKind::CrossRing || authored_kind==VfxGroundRecipeKind::BrokenHex ||
                   authored_kind==VfxGroundRecipeKind::AxialFracture || authored_kind==VfxGroundRecipeKind::RepeatingChevron)
                {
                    if(source.knot_count!=2 || !(source.knots[1]>source.knots[0]) || output.source!=si || output.min_quality>2 ||
                        !valid(output.parameters,p.parameters.size()) || !std::isfinite(output.hdr) || output.hdr<=0 ||
                        !std::isfinite(output.motion_rate_hz) || output.motion_rate_hz<0 ||
                        !std::isfinite(output.motion_amplitude) || output.motion_amplitude<0 || output.motion_amplitude>1 ||
                        !std::isfinite(output.motion_inset_fraction) || output.motion_inset_fraction<0 || output.motion_inset_fraction>1 ||
                        !std::all_of(output.rgba.begin(),output.rgba.end(),[](float v){return std::isfinite(v);})) continue;
                    VfxGroundRecipe recipe=base_recipe(si,oi,source,output,authored_kind);
                    if(authored_kind==VfxGroundRecipeKind::HexConstellation)
                    {
                        const auto segments=parameter_bits(output,ShapeId("segments"),VfxParameterType::Int);
                        const auto sequence=parameter_bits(output,ShapeId("sequence"),VfxParameterType::Bool);
                        if(!segments || *segments!=6 || !sequence || *sequence>1) continue;
                        recipe.tick_count=*segments; recipe.pulse_curve_interpolation=*sequence;
                        PrepareHexMaskVariants(p,output,recipe);
                        if(std::none_of(recipe.mask_variants.begin(),recipe.mask_variants.end(),[](const auto &v){return v.valid;})) continue;
                    }
                    else if(authored_kind==VfxGroundRecipeKind::BossSignature)
                    {
                        const auto scale=parameter_bits(output,ShapeId("signature_scale"),VfxParameterType::Float);
                        if(!scale || std::bit_cast<float>(*scale)!=1.0f) continue;
                        recipe.primary=std::bit_cast<float>(*scale);
                    }
                    else if(authored_kind==VfxGroundRecipeKind::CrossRing)
                    {
                        const auto sdf=parameter_bits(output,ShapeId("sdf"),VfxParameterType::Enum);
                        if(!sdf || *sdf!=ShapeId("cross_plus_ring")) continue;
                    }
                    else if(authored_kind==VfxGroundRecipeKind::BrokenHex)
                    {
                        const auto width=parameter_bits(output,ShapeId("edge_width"),VfxParameterType::Float);
                        const auto sector=parameter_bits(output,ShapeId("break_sector_deg"),VfxParameterType::Float);
                        if(!width || !sector) continue;
                        recipe.primary=std::bit_cast<float>(*width); recipe.secondary=std::bit_cast<float>(*sector);
                        if(!std::isfinite(recipe.primary) || recipe.primary<=0 || !std::isfinite(recipe.secondary) ||
                            recipe.secondary<=0 || recipe.secondary>=360) continue;
                    }
                    else if(authored_kind==VfxGroundRecipeKind::AxialFracture)
                    {
                        const auto length=parameter_bits(output,ShapeId("length"),VfxParameterType::Float);
                        const auto width=parameter_bits(output,ShapeId("width"),VfxParameterType::Float);
                        if(!length || !width) continue;
                        recipe.primary=std::bit_cast<float>(*length); recipe.secondary=std::bit_cast<float>(*width);
                        if(!std::isfinite(recipe.primary) || recipe.primary<=0 || !std::isfinite(recipe.secondary) || recipe.secondary<=0) continue;
                    }
                    else
                    {
                        const auto spacing=parameter_bits(output,ShapeId("spacing"),VfxParameterType::Float);
                        const auto count=parameter_bits(output,ShapeId("count"),VfxParameterType::Int);
                        if(!spacing || !count || *count!=7) continue;
                        recipe.primary=std::bit_cast<float>(*spacing); recipe.tick_count=*count;
                        if(!std::isfinite(recipe.primary) || recipe.primary<=0) continue;
                    }
                    recipes.push_back(recipe);
                    continue;
                }
                VfxGroundRecipeKind kind{}; std::uint32_t primary_key{}; std::uint32_t secondary_key{};
                bool supported=true;
                if(output.profile==VfxOutputProfile::GroundSdfSoft && output.shape==ShapeId("exact_hitbox_ring") &&
                    output.motion==VfxMotionKind::StableBoundaryInwardPulse)
                { kind=VfxGroundRecipeKind::Ring; primary_key=ShapeId("edge_width_world"); }
                else if(output.profile==VfxOutputProfile::GroundSdfOit && output.shape==ShapeId("low_frequency_fill") &&
                    output.motion==VfxMotionKind::SlowRadialFlow)
                { kind=VfxGroundRecipeKind::Fill; primary_key=ShapeId("flow_speed"); }
                else if(output.profile==VfxOutputProfile::GroundSdfSoft && output.shape==ShapeId("dual_parallel_lines") &&
                    output.motion==VfxMotionKind::LockedChevronsTowardImpact)
                { kind=VfxGroundRecipeKind::Border; primary_key=ShapeId("edge_width_world"); secondary_key=ShapeId("chevron_spacing"); }
                else if(output.profile==VfxOutputProfile::GroundSdfOit && output.shape==ShapeId("diagonal_hatch") &&
                    output.motion==VfxMotionKind::ScrollsAttackDirection)
                { kind=VfxGroundRecipeKind::Hatch; primary_key=ShapeId("repeat"); secondary_key=ShapeId("scroll"); }
                else if(output.profile==VfxOutputProfile::GroundSdfSoft && output.shape==ShapeId("exact_circle_ring") &&
                    output.motion==VfxMotionKind::BoundaryPulseAccelerates)
                { kind=VfxGroundRecipeKind::Preview; primary_key=ShapeId("edge_width_world"); }
                else if(output.profile==VfxOutputProfile::GroundSdfOit && output.shape==ShapeId("radial_ticks_sdf") &&
                    output.motion==VfxMotionKind::TicksRotateLock)
                { kind=VfxGroundRecipeKind::Ticks; }
                else supported=false;
                if(!supported || output.source!=si || output.min_quality>2 || !valid(output.parameters,p.parameters.size())) continue;
                bool finite_parameters=true;
                for(const auto &v:p.parameters.subspan(output.parameters.first,output.parameters.count))
                    if(v.type==VfxParameterType::Float && !std::isfinite(std::bit_cast<float>(v.bits))) finite_parameters=false;
                if(!finite_parameters) continue;
                const auto parameter=[&](std::uint32_t key,VfxParameterType type,bool required)->std::optional<std::uint32_t>{
                    if(!key) return std::nullopt;
                    const VfxParameterRecord *found=nullptr;
                    for(const auto &v:p.parameters.subspan(output.parameters.first,output.parameters.count))
                        if(v.key==key)
                        {
                            if(found || v.type!=type) return std::nullopt;
                            found=&v;
                        }
                    return found ? std::optional<std::uint32_t>{found->bits} :
                        (required ? std::nullopt : std::optional<std::uint32_t>{});
                };
                const auto primary=parameter(primary_key,VfxParameterType::Float,kind!=VfxGroundRecipeKind::Ticks);
                const auto secondary=parameter(secondary_key,VfxParameterType::Float,secondary_key!=0);
                const auto tick_count=parameter(kind==VfxGroundRecipeKind::Ticks?ShapeId("tick_count"):0,
                    VfxParameterType::Int,kind==VfxGroundRecipeKind::Ticks);
                if((kind!=VfxGroundRecipeKind::Ticks && !primary) || (secondary_key && !secondary) ||
                    (kind==VfxGroundRecipeKind::Ticks && (!tick_count || !*tick_count || *tick_count>0x7fffffffu))) continue;
                const float a=primary?std::bit_cast<float>(*primary):0.0f;
                const float b=secondary?std::bit_cast<float>(*secondary):0.0f;
                if((primary && !std::isfinite(a)) || (secondary && !std::isfinite(b)) ||
                    ((kind==VfxGroundRecipeKind::Ring || kind==VfxGroundRecipeKind::Border || kind==VfxGroundRecipeKind::Hatch || kind==VfxGroundRecipeKind::Preview) && a<=0) ||
                    (kind==VfxGroundRecipeKind::Border && b<=0) || (kind==VfxGroundRecipeKind::Hatch && !std::isfinite(b))) continue;
                VfxRange pulse_keys{}; std::uint32_t pulse_interpolation{};
                if(kind==VfxGroundRecipeKind::Preview)
                {
                    const auto curve=std::find_if(p.parameters.begin()+output.parameters.first,
                        p.parameters.begin()+output.parameters.first+output.parameters.count,
                        [](const auto &v){return v.key==ShapeId("pulse_curve_ref") && v.type==VfxParameterType::CurveRow;});
                    if(curve==p.parameters.begin()+output.parameters.first+output.parameters.count || curve->bits>=p.curves.size()) continue;
                    const auto &row=p.curves[curve->bits];
                    if(row.interpolation!=0 || row.keys.count<2 || !valid(row.keys,p.curve_keys.size())) continue;
                    const auto keys=p.curve_keys.subspan(row.keys.first,row.keys.count);
                    bool curve_ok=keys.front().time==0 && keys.back().time==1;
                    for(std::size_t k=0;k<keys.size();++k)
                        if(!std::isfinite(keys[k].time)||!std::isfinite(keys[k].value)||keys[k].value<0||
                            (k&&keys[k].time<=keys[k-1].time)) curve_ok=false;
                    if(!curve_ok) continue;
                    pulse_keys=row.keys; pulse_interpolation=row.interpolation;
                }
                if(!std::isfinite(output.hdr)||output.hdr<=0 ||
                    !std::all_of(output.rgba.begin(),output.rgba.end(),[](float v){return std::isfinite(v);}) ||
                    !std::isfinite(output.motion_rate_hz)||!std::isfinite(output.motion_amplitude)||
                    !std::isfinite(output.motion_inset_fraction)) continue;
                auto recipe=base_recipe(si,oi,source,output,kind);
                recipe.pulse_curve_keys=pulse_keys; recipe.pulse_curve_interpolation=pulse_interpolation;
                recipe.tick_count=tick_count.value_or(0); recipe.primary=a; recipe.secondary=b;
                recipes.push_back(recipe);
            }
        }
        range.count=static_cast<std::uint32_t>(recipes.size())-range.first;
        if(range.count)
        {
            std::uint32_t profiles{};
            for(const auto &recipe:std::span(recipes).subspan(range.first,range.count))
            {
                if(recipe.source_type!=VfxSourceType::Direct)continue;
                if(recipe.profile==VfxOutputProfile::GroundSdfAdd)profiles|=kVfxGroundEventDirectAdd;
                if(recipe.profile==VfxOutputProfile::GroundSdfSoft)profiles|=kVfxGroundEventDirectSoft;
            }
            recipes[range.first].event_admission_profiles=profiles;
        }
    }
}

void PrepareVfxFlashRecipes(VfxProgramView p, std::vector<VfxRange> &ranges,
                            std::vector<VfxFlashRecipe> &recipes)
{
    const auto valid=[](VfxRange r,std::size_t n){return r.first<=n&&r.count<=n-r.first;};
    struct Shape { std::uint32_t id; VfxMotionKind motion; VfxImpactShape shape; float size; };
    constexpr Shape shapes[]{
        {ShapeId("soft_disc"),VfxMotionKind::InstantPointFlash,VfxImpactShape::SoftDisc,.35f},
        {ShapeId("directional_slash_sdf"),VfxMotionKind::TwoFrameSnapPerpendicular,VfxImpactShape::CrossSlash,.22f},
        {ShapeId("soft_disc_noise"),VfxMotionKind::InstantExpansionCollapse,VfxImpactShape::NoiseDisc,.32f},
        {ShapeId("status_symbol_sdf"),VfxMotionKind::FastStampBreakout,VfxImpactShape::StatusStamp,.25f},
        {ShapeId("radial_slash_sdf"),VfxMotionKind::SnapsOutwardAim,VfxImpactShape::RadialSlash,.30f},
        {ShapeId("small_pulse"),VfxMotionKind::SingleShortPulse,VfxImpactShape::Pulse,.25f},
        {ShapeId("small_star_ring"),VfxMotionKind::SnapsInwardUpward,VfxImpactShape::StarRing,.4f},
        {ShapeId("warped_flame_disc"),VfxMotionKind::RadialLobesUpward,VfxImpactShape::Flame,.45f},
        {ShapeId("smoke_billboard_6way"),VfxMotionKind::BakedSixWayPlume,VfxImpactShape::Smoke6Way,.65f},
        {ShapeId("radial_energy"),VfxMotionKind::DirectionalTurbulence,VfxImpactShape::RadialEnergy,.4f},
        {ShapeId("three_prong_bite_sdf"),VfxMotionKind::CompressSnapOutward,VfxImpactShape::ThreeProngBite,1},
        {ShapeId("boss_crest_contact_sdf"),VfxMotionKind::SingleFrameSnapShrink,VfxImpactShape::BossCrestContact,1},
        {ShapeId("needle_star_sdf"),VfxMotionKind::InstantCompressionAxialSplit,VfxImpactShape::NeedleStar,1}};
    ranges.assign(p.effects.size(),{}); recipes.clear();
    for(std::size_t ei=0;ei<p.effects.size();++ei)
    {
        const auto &effect=p.effects[ei]; auto &range=ranges[ei]; range.first=static_cast<std::uint32_t>(recipes.size());
        if(effect.handle!=ei+1 || effect.input_mode!=0 || effect.timing_kind!=0 ||
            !std::isfinite(effect.seconds)||effect.seconds<=0 || !valid(effect.sources,p.sources.size())) continue;
        const auto id=std::find_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto &v){return v.handle==effect.handle;});
        if(id!=p.effect_lookup.end()&&!id->effect_id) continue;
        const auto unique_identity=id!=p.effect_lookup.end() &&
            std::count_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto &v){return v.handle==effect.handle&&v.effect_id==id->effect_id;})==1 &&
            std::count_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto &v){return v.handle==effect.handle;})==1 &&
            std::count_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto &v){return v.effect_id==id->effect_id;})==1;
        for(std::uint32_t si=effect.sources.first;si<effect.sources.first+effect.sources.count;++si)
        {
            const auto &source=p.sources[si];
            if(source.effect!=effect.handle || source.type!=VfxSourceType::ImpactSprite || source.knot_count!=2 ||
                !std::isfinite(source.knots[0])||!std::isfinite(source.knots[1])||source.knots[0]<0||
                source.knots[1]<=source.knots[0]||source.knots[1]>1 || !valid(source.outputs,p.outputs.size()) ||
                !valid(source.parameters,p.parameters.size()) || source.parameters.count!=0) continue;
            for(std::uint32_t oi=source.outputs.first;oi<source.outputs.first+source.outputs.count;++oi)
            {
                const auto &output=p.outputs[oi];
                const auto shape=std::find_if(std::begin(shapes),std::end(shapes),[&](const auto &v){return v.id==output.shape&&v.motion==output.motion;});
                if(shape==std::end(shapes)||output.source!=si||output.min_quality>2||!valid(output.parameters,p.parameters.size())||
                    !std::isfinite(output.hdr)||output.hdr<=0||!std::all_of(output.rgba.begin(),output.rgba.end(),[](float v){return std::isfinite(v);})||
                    output.rgba[3]<0||output.rgba[3]>1) continue;
                const bool oit=shape->shape==VfxImpactShape::Flame||shape->shape==VfxImpactShape::Smoke6Way||shape->shape==VfxImpactShape::RadialEnergy;
                const auto expected_profile=shape->shape==VfxImpactShape::Flame?VfxOutputProfile::SpriteFlameOit:
                    shape->shape==VfxImpactShape::Smoke6Way?VfxOutputProfile::SpriteSmoke6WayOit:
                    shape->shape==VfxImpactShape::RadialEnergy?VfxOutputProfile::SpriteNoiseOit:
                    shape->shape==VfxImpactShape::SoftDisc?VfxOutputProfile::SpriteAdd:VfxOutputProfile::SpriteSdfAdd;
                if(output.profile!=expected_profile) continue;
                VfxFlashSpawnInput spawn; spawn.shape=shape->shape; spawn.size=shape->size;
                const auto find=[&](std::string_view key)->const VfxParameterRecord*{
                    const auto hash=ShapeId(key); const VfxParameterRecord *found=nullptr;
                    for(const auto &v:p.parameters.subspan(output.parameters.first,output.parameters.count))
                        if(v.key==hash){if(found)return nullptr;found=&v;} return found;};
                const auto scalar=[&](std::string_view key,float &dst,bool required){
                    const auto *v=find(key); if(!v)return !required;
                    if(v->type!=VfxParameterType::Float)return false; dst=std::bit_cast<float>(v->bits);
                    return std::isfinite(dst)&&(dst>0);};
                const auto nonnegative=[&](std::string_view key,float &dst,bool required){
                    const auto *v=find(key); if(!v)return !required;
                    if(v->type!=VfxParameterType::Float)return false; dst=std::bit_cast<float>(v->bits);
                    return std::isfinite(dst)&&(dst>=0);};
                const auto enumeration=[&](std::string_view key,std::string_view value){const auto *v=find(key);return v&&v->type==VfxParameterType::Enum&&v->bits==ShapeId(value);};
                const bool needs_curve=shape->shape==VfxImpactShape::NoiseDisc||shape->shape==VfxImpactShape::StatusStamp||
                    shape->shape==VfxImpactShape::RadialSlash||shape->shape==VfxImpactShape::Pulse||
                    shape->shape==VfxImpactShape::ThreeProngBite||shape->shape==VfxImpactShape::BossCrestContact||shape->shape==VfxImpactShape::NeedleStar;
                bool supported=scalar("size",spawn.size,shape->shape==VfxImpactShape::SoftDisc) &&
                    scalar("aspect",spawn.aspect,shape->shape==VfxImpactShape::CrossSlash);
                if(const auto *curve=find("scale_curve_ref")){if(curve->type!=VfxParameterType::CurveRow||curve->bits>=7)supported=false;else spawn.curve_row=curve->bits;}
                else if(needs_curve)supported=false;
                if(shape->shape==VfxImpactShape::CrossSlash)supported&=enumeration("sdf","cross_slash");
                if(shape->shape==VfxImpactShape::NoiseDisc)supported&=enumeration("noise","fbm");
                if(shape->shape==VfxImpactShape::RadialSlash)supported&=enumeration("sdf","asymmetric_star_slash");
                if(shape->shape==VfxImpactShape::StarRing)supported&=enumeration("sdf","star_ring");
                if(oit)
                {
                    const auto *octaves=find("fbm_octaves");
                    if(shape->shape!=VfxImpactShape::Smoke6Way)
                    {
                        const auto *flow=shape->shape==VfxImpactShape::Flame?find("flow_speed"):nullptr;
                        supported &= output.parameters.count==(shape->shape==VfxImpactShape::Flame?(flow?3u:2u):1u) &&
                            octaves&&octaves->type==VfxParameterType::Int&&octaves->bits>=1&&octaves->bits<=8;
                        if(supported){spawn.fbm_octaves=octaves->bits; supported &= shape->shape!=VfxImpactShape::Flame ||
                            (nonnegative("domain_warp",spawn.domain_warp,true)&&(!flow||nonnegative("flow_speed",spawn.procedural_flow_speed,true)));}
                    }
                    else
                    {
                        const auto *clip=find("clip_index"),*blend=find("motion_blend"); float strength{};
                        supported &= output.parameters.count==5 && clip&&clip->type==VfxParameterType::Int&&clip->bits<=1&&
                            blend&&blend->type==VfxParameterType::Enum&&blend->bits==ShapeId("high_only")&&
                            scalar("frame_rate",spawn.smoke_fps,true)&&scalar("opacity_scale",spawn.opacity_scale,true)&&
                            nonnegative("motion_blend_strength",strength,true)&&strength<=1&&output.coverage_type==ShapeId("flipbook_6way")&&
                            valid(output.textures,p.texture_bindings.size());
                        const VfxTextureBindingRecord *positive=nullptr,*negative=nullptr,*motion=nullptr;
                        if(supported) for(const auto &binding:p.texture_bindings.subspan(output.textures.first,output.textures.count))
                        {
                            if(binding.role!=ShapeId("coverage")&&binding.role!=ShapeId("flipbook_motion_vectors"))continue;
                            const auto resource=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),[&](const auto &r){return r.slot==binding.catalog_slot;});
                            if(resource==p.texture_resources.end()||!valid(resource->asset_path_bytes,p.strings.size())){supported=false;break;}
                            const std::string_view path(reinterpret_cast<const char*>(p.strings.data())+resource->asset_path_bytes.first,resource->asset_path_bytes.count);
                            const VfxTextureBindingRecord **target=nullptr;
                            if(binding.role==ShapeId("coverage")){if(path=="Content/Textures/VFX/vfx_smoke6way_pos.dds")target=&positive;else if(path=="Content/Textures/VFX/vfx_smoke6way_neg.dds")target=&negative;else{supported=false;break;}}
                            else{if(path!="Content/Textures/VFX/vfx_smoke_motion_vectors.dds"||binding.min_quality!=2){supported=false;break;}target=&motion;}
                            if(*target){supported=false;break;}*target=&binding;
                        }
                        if(supported&&(!positive||!negative||!motion||!positive->frame_count||positive->first_frame>=64||
                            positive->frame_count>64-positive->first_frame||positive->first_frame!=negative->first_frame||
                            positive->frame_count!=negative->frame_count||!std::isfinite(positive->fps)||positive->fps!=negative->fps||
                            positive->fps!=spawn.smoke_fps||positive->first_frame!=clip->bits*32||positive->frame_count!=32)) supported=false;
                        if(supported){spawn.smoke_first_frame=positive->first_frame;spawn.smoke_frame_count=positive->frame_count;spawn.motion_strength=strength;}
                    }
                }
                const bool contact=shape->shape==VfxImpactShape::ThreeProngBite||shape->shape==VfxImpactShape::BossCrestContact||shape->shape==VfxImpactShape::NeedleStar;
                if(contact && output.parameters.count!=1) supported=false;
                if(shape->shape==VfxImpactShape::SoftDisc && !scalar("size",spawn.size,true)) supported=false;
                if(shape->shape==VfxImpactShape::CrossSlash && !scalar("aspect",spawn.aspect,true)) supported=false;
                if(!supported)continue;
                if(!oit) for(const auto &v:p.parameters.subspan(output.parameters.first,output.parameters.count))
                    if(v.type==VfxParameterType::Float&&!std::isfinite(std::bit_cast<float>(v.bits))) supported=false;
                if(!supported)continue;
                spawn.oit=oit; spawn.color={output.rgba[0]*output.hdr,output.rgba[1]*output.hdr,output.rgba[2]*output.hdr,output.rgba[3]};
                spawn.hdr=output.hdr; spawn.gradient_row=output.gradient_row;
                spawn.delay=effect.seconds*source.knots[0];
                spawn.lifetime=effect.seconds*(source.knots[1]-source.knots[0]);
                if(!std::isfinite(spawn.delay)||!std::isfinite(spawn.lifetime)||spawn.lifetime<=0 ||
                    !std::isfinite(spawn.hdr)||spawn.hdr<=0 ||
                    !std::isfinite(spawn.color.x)||!std::isfinite(spawn.color.y)||!std::isfinite(spawn.color.z)||
                    !std::isfinite(spawn.color.w)||spawn.color.w<0||spawn.color.w>1) continue;
                const auto texture_range=output.textures;
                if(!valid(texture_range,p.texture_bindings.size()))continue;
                for(const auto &binding:p.texture_bindings.subspan(texture_range.first,texture_range.count))
                {
                    if(binding.role!=ShapeId("authored_mask_array"))continue;
                    if(binding.min_quality>2||!std::isfinite(binding.strength)||binding.strength<0||binding.strength>1||
                        binding.selection!=ShapeId("stable_seed_mod_group_size")||!valid(binding.slices,p.slice_indices.size())||!binding.slices.count) {supported=false;break;}
                    const auto resource=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),[&](const auto&r){return r.slot==binding.catalog_slot;});
                    if(resource==p.texture_resources.end()||!valid(resource->asset_path_bytes,p.strings.size())){supported=false;break;}
                    const std::string_view path(reinterpret_cast<const char*>(p.strings.data())+resource->asset_path_bytes.first,resource->asset_path_bytes.count);
                    if(path!="Content/Textures/VFX/vfx_authored_mask_array.dds"){supported=false;break;}
                    for(const auto slice:p.slice_indices.subspan(binding.slices.first,binding.slices.count))if(slice>=12)supported=false;
                    if(!supported)break;
                }
                if(!supported)continue;
                VfxFlashRecipe recipe; recipe.effect_id=id==p.effect_lookup.end()?0:id->effect_id; recipe.effect_handle=effect.handle;
                recipe.source_index=si; recipe.source_stable_id=source.stable_id; recipe.output_index=oi;
                recipe.payload_kind=effect.payload_kind; recipe.min_quality=output.min_quality; recipe.knots={source.knots[0],source.knots[1]};
                recipe.seconds=effect.seconds; recipe.spawn=spawn; recipe.unique_effect_identity=unique_identity;
                std::uint32_t coverage_quality{};
                if(shape->shape==VfxImpactShape::Smoke6Way)
                    for(const auto &binding:p.texture_bindings.subspan(texture_range.first,texture_range.count))
                        if(binding.role==ShapeId("coverage"))
                        {
                            if(binding.min_quality>2){supported=false;break;}
                            coverage_quality=std::max(coverage_quality,binding.min_quality);
                        }
                if(!supported)continue;
                recipe.coverage_quality=coverage_quality;
                for(std::uint32_t q=0;q<3;++q)
                {
                    if(q<output.min_quality || q<coverage_quality)continue;
                    bool mask_seen=false,quality_ok=true;
                    for(const auto &binding:p.texture_bindings.subspan(texture_range.first,texture_range.count))
                    {
                        if(binding.role!=ShapeId("authored_mask_array"))continue;
                        if(binding.min_quality>q)continue;
                        if(mask_seen){quality_ok=false;break;}
                        mask_seen=true;
                        recipe.mask_slice_ranges[q]=binding.slices;
                        recipe.mask_strengths[q]=binding.strength;
                    }
                    if(quality_ok)
                    {
                        recipe.supported_quality_mask |= static_cast<std::uint8_t>(1u<<q);
                        if(mask_seen)recipe.mask_present_mask |= static_cast<std::uint8_t>(1u<<q);
                    }
                }
                if(!recipe.supported_quality_mask)continue;
                recipes.push_back(recipe);
            }
        }
        range.count=static_cast<std::uint32_t>(recipes.size())-range.first;
    }
}

void PrepareVfxPersistentFlashRecipes(VfxProgramView p,
    std::vector<VfxRange> &stamp_ranges, std::vector<VfxPersistentStampRecipe> &stamps,
    std::vector<VfxRange> &fire_ranges, std::vector<VfxPersistentFireRecipe> &fire,
    std::vector<VfxRange> &tick_ranges, std::vector<VfxOwnedStatusTickRecipe> &ticks)
{
    const auto valid = [](VfxRange range, std::size_t size) {
        return range.first <= size && range.count <= size - range.first;
    };
    const auto unique_id = [&](std::uint32_t handle, std::uint64_t id) {
        return id && std::count_if(p.effect_lookup.begin(), p.effect_lookup.end(),
                   [=](const auto &row) { return row.handle == handle && row.effect_id == id; }) == 1 &&
            std::count_if(p.effect_lookup.begin(), p.effect_lookup.end(),
                   [=](const auto &row) { return row.handle == handle; }) == 1 &&
            std::count_if(p.effect_lookup.begin(), p.effect_lookup.end(),
                   [=](const auto &row) { return row.effect_id == id; }) == 1;
    };
    const auto texture_path = [&](const VfxTextureBindingRecord &binding) -> std::string_view {
        const auto resource = std::find_if(p.texture_resources.begin(), p.texture_resources.end(),
            [&](const auto &row) { return row.slot == binding.catalog_slot; });
        if (resource == p.texture_resources.end() || !valid(resource->asset_path_bytes, p.strings.size()))
            return {};
        return {reinterpret_cast<const char *>(p.strings.data() + resource->asset_path_bytes.first),
                resource->asset_path_bytes.count};
    };
    const auto stamp_curve = [&](const VfxOutputRecord &output, std::uint32_t row_id) {
        if (!valid(output.parameters, p.parameters.size()) || output.parameters.count != 1) return false;
        const auto &parameter = p.parameters[output.parameters.first];
        if (parameter.key != ShapeId("scale_curve_ref") || parameter.type != VfxParameterType::CurveRow ||
            parameter.bits != row_id || parameter.bits >= p.curves.size()) return false;
        const auto &curve = p.curves[parameter.bits];
        if (curve.domain != ShapeId("normalized_age") || curve.interpolation != 2 ||
            !valid(curve.keys, p.curve_keys.size()) || curve.keys.count < 2) return false;
        float previous = -1.0f;
        for (const auto &key : p.curve_keys.subspan(curve.keys.first, curve.keys.count))
        {
            if (!std::isfinite(key.time) || !std::isfinite(key.value) || key.time <= previous ||
                key.time > 1.0f || key.value < 0.0f) return false;
            previous = key.time;
        }
        return p.curve_keys[curve.keys.first].time == 0.0f && previous == 1.0f;
    };
    const auto stamp_textures = [&](const VfxOutputRecord &output, bool bleed) {
        if (!valid(output.textures, p.texture_bindings.size()) ||
            (bleed ? output.textures.count != 4 :
                (output.textures.count != 2 && output.textures.count != 3))) return false;
        bool gradient = false, curve = false, optional_mask = false, authored_mask = false;
        for (const auto &binding : p.texture_bindings.subspan(output.textures.first, output.textures.count))
        {
            const auto path = texture_path(binding);
            if (binding.role == ShapeId("profile.optional_detail"))
            {
                if (optional_mask || binding.min_quality != 0 || binding.selection != 0 ||
                    binding.slices.count != 0 || !std::isfinite(binding.strength) || binding.strength != 1.0f ||
                    path != "Content/Textures/VFX/vfx_authored_mask_array.dds") return false;
                optional_mask = true;
                continue;
            }
            if (bleed && binding.role == ShapeId("authored_mask_array"))
            {
                if (authored_mask || binding.min_quality != 0 || !std::isfinite(binding.strength) ||
                    binding.strength < 0.0f || binding.strength > 1.0f ||
                    binding.selection != ShapeId("stable_seed_mod_group_size") ||
                    !valid(binding.slices, p.slice_indices.size()) || binding.slices.count != 3 ||
                    path != "Content/Textures/VFX/vfx_authored_mask_array.dds") return false;
                const auto slices = p.slice_indices.subspan(binding.slices.first, binding.slices.count);
                if (slices[0] != 3 || slices[1] != 4 || slices[2] != 5) return false;
                authored_mask = true;
                continue;
            }
            if (binding.role != ShapeId("profile.global") || binding.min_quality != 0 ||
                binding.selection != 0 || binding.slices.count != 0 ||
                !std::isfinite(binding.strength) || binding.strength != 1.0f) return false;
            if (path == "Content/Textures/VFX/vfx_gradient_lut.dds" && !gradient) gradient = true;
            else if (path == "Content/Textures/VFX/vfx_curve_lut.dds" && !curve) curve = true;
            else return false;
        }
        return gradient && curve && (!bleed || (optional_mask && authored_mask));
    };
    stamp_ranges.assign(p.effects.size(), {});
    stamps.clear();
    struct StampRoute { std::uint64_t id; PersistentVfxKind kind; std::uint32_t payload; bool fixed, bleed; };
    constexpr StampRoute stamp_routes[]{
        {HashEffectName("particle.player.bow_draw"), PersistentVfxKind::PlayerBowDraw, ShapeId("EntityAttachmentPayload"), false, false},
        {HashEffectName("particle.upgrade.charged.full_ready"), PersistentVfxKind::ChargedFullReady, ShapeId("EntityAttachmentPayload"), true, false},
        {HashEffectName("particle.upgrade.empowered_ready"), PersistentVfxKind::EmpoweredReady, ShapeId("EntityAttachmentPayload"), false, false},
        {HashEffectName("particle.upgrade.retarget"), PersistentVfxKind::MultishotRetarget, ShapeId("ProjectilePathPayload"), true, false},
        {HashEffectName("particle.upgrade.ricochet.bleed_extend"), PersistentVfxKind::RicochetBleedExtend, ShapeId("EntityAttachmentPayload"), true, true},
    };
    for (std::size_t ei = 0; ei < p.effects.size(); ++ei)
    {
        const auto &effect = p.effects[ei];
        auto &range = stamp_ranges[ei];
        range.first = static_cast<std::uint32_t>(stamps.size());
        if (effect.handle != ei + 1 || effect.input_mode != 1 || !valid(effect.sources, p.sources.size())) continue;
        for (const auto &route : stamp_routes)
        {
            if (!unique_id(effect.handle, route.id) || effect.payload_kind != route.payload ||
                effect.timing_kind != (route.fixed ? 0u : 1u) ||
                (route.fixed && (!std::isfinite(effect.seconds) || effect.seconds <= 0.0f))) continue;
            for (std::uint32_t si = effect.sources.first; si < effect.sources.first + effect.sources.count; ++si)
            {
                const auto &source = p.sources[si];
                if (source.effect != effect.handle || source.stable_id != ShapeId("status_stamp") ||
                    source.authoritative != 1 || source.type != VfxSourceType::ImpactSprite ||
                    source.knot_count != 2 || source.knots[0] != 0.0f || !std::isfinite(source.knots[1]) ||
                    source.knots[1] <= 0.0f || source.knots[1] > 1.0f ||
                    !valid(source.parameters, p.parameters.size()) || source.parameters.count != 0 ||
                    !valid(source.outputs, p.outputs.size()) || source.outputs.count != 1) continue;
                const auto oi = source.outputs.first;
                const auto &output = p.outputs[oi];
                if (output.source != si || output.profile != VfxOutputProfile::SpriteSdfAdd ||
                    output.shape != ShapeId("status_symbol_sdf") || output.shape_domain != ShapeId("sdf") ||
                    output.shape_scale_rule != ShapeId("gameplay_geometry_if_bound_else_component_radius") ||
                    output.shape_component_kind != ShapeId("particle") || output.coverage_type != ShapeId("analytic") ||
                    output.coverage_ref != ShapeId("status_symbol_sdf") ||
                    output.motion != VfxMotionKind::FastStampBreakout || output.motion_rate_hz != 0.0f ||
                    output.motion_amplitude != 0.0f || output.motion_inset_fraction != 0.0f ||
                    output.min_quality != 0 || !stamp_curve(output, 4) || !stamp_textures(output, route.bleed) ||
                    !std::isfinite(output.hdr) || output.hdr <= 0.0f || !std::isfinite(output.rgba[3]) ||
                    output.rgba[3] < 0.0f || output.rgba[3] > 1.0f ||
                    !std::isfinite(output.rgba[0] * output.hdr) ||
                    !std::isfinite(output.rgba[1] * output.hdr) ||
                    !std::isfinite(output.rgba[2] * output.hdr)) continue;
                VfxPersistentStampRecipe recipe{};
                const auto identity = std::find_if(p.effect_lookup.begin(), p.effect_lookup.end(),
                    [&](const auto &entry) { return entry.handle == effect.handle && entry.effect_id == route.id; });
                recipe.effect_id = identity->effect_id;
                recipe.effect_handle = effect.handle;
                recipe.source_visual_kind = static_cast<std::uint32_t>(route.kind);
                recipe.payload_kind = route.payload;
                recipe.source_stable_id = source.stable_id;
                recipe.timing_kind = effect.timing_kind;
                recipe.fixed_seconds = effect.seconds;
                recipe.source_end = source.knots[1];
                recipe.spawn.shape = VfxImpactShape::StatusStamp;
                recipe.spawn.curve_row = 4;
                recipe.spawn.gradient_row = output.gradient_row;
                recipe.spawn.hdr = output.hdr;
                recipe.spawn.color = {output.rgba[0] * output.hdr, output.rgba[1] * output.hdr,
                    output.rgba[2] * output.hdr, output.rgba[3]};
                recipe.bleed_extend = route.bleed;
                recipe.supported_quality_mask = 0x7;
                for (const auto &binding : p.texture_bindings.subspan(output.textures.first, output.textures.count))
                    if (route.bleed && binding.role == ShapeId("authored_mask_array"))
                        for (std::size_t quality = 0; quality < 3; ++quality)
                        {
                            recipe.mask_slices[quality] = binding.slices;
                            recipe.mask_strengths[quality] = binding.strength;
                            recipe.mask_present_mask |= static_cast<std::uint8_t>(1u << quality);
                        }
                stamps.push_back(recipe);
            }
        }
        range.count = static_cast<std::uint32_t>(stamps.size()) - range.first;
    }

    fire_ranges.assign(p.effects.size(), {});
    fire.clear();
    const auto fire_id = HashEffectName("persistent.area.fire");
    for (std::size_t ei = 0; ei < p.effects.size(); ++ei)
    {
        const auto &effect = p.effects[ei];
        auto &range = fire_ranges[ei];
        range.first = static_cast<std::uint32_t>(fire.size());
        if (effect.handle != ei + 1 || effect.input_mode != 1 || effect.timing_kind != 1 ||
            effect.payload_kind != ShapeId("CircleAreaPayload") || !unique_id(effect.handle, fire_id) ||
            !valid(effect.sources, p.sources.size())) continue;
        for (std::uint32_t si = effect.sources.first; si < effect.sources.first + effect.sources.count; ++si)
        {
            const auto &source = p.sources[si];
            if (source.effect != effect.handle || source.type != VfxSourceType::CurlMotes ||
                source.stable_id != ShapeId("high_quality_flame_tongues") || source.knot_count != 2 ||
                source.knots[0] != 0.0f || source.knots[1] != 1.0f ||
                !valid(source.parameters, p.parameters.size()) || source.parameters.count != 3 ||
                !valid(source.outputs, p.outputs.size())) continue;
            std::uint32_t rate{}; float radius_scale{}; bool domain{}; bool valid_parameters = true;
            for (const auto &parameter : p.parameters.subspan(source.parameters.first, source.parameters.count))
            {
                if (parameter.key == ShapeId("count_per_second") && !rate && parameter.type == VfxParameterType::Int)
                    rate = parameter.bits;
                else if (parameter.key == ShapeId("spawn_domain") && !domain && parameter.type == VfxParameterType::Enum &&
                         parameter.bits == ShapeId("inside_binding_radius")) domain = true;
                else if (parameter.key == ShapeId("spawn_radius_scale") && radius_scale == 0 && parameter.type == VfxParameterType::Float)
                    radius_scale = std::bit_cast<float>(parameter.bits);
                else valid_parameters = false;
            }
            if (!valid_parameters || rate != 8 || !domain || !std::isfinite(radius_scale) ||
                radius_scale <= 0.0f || radius_scale > 1.0f) continue;
            for (const auto &output : p.outputs.subspan(source.outputs.first, source.outputs.count))
            {
                if (output.source != si || output.profile != VfxOutputProfile::SpriteFlameOit ||
                    output.min_quality != 2 || output.motion != VfxMotionKind::ProceduralFlameTongue ||
                    output.shape != ShapeId("warped_flame_disc") || output.coverage_type != ShapeId("analytic") ||
                    output.coverage_ref != ShapeId("warped_flame_disc") ||
                    !valid(output.parameters, p.parameters.size()) || output.parameters.count != 4 ||
                    !valid(output.textures, p.texture_bindings.size())) continue;
                VfxFlashSpawnInput spawn{};
                spawn.shape = VfxImpactShape::Flame;
                spawn.oit = true;
                bool output_valid = true; std::uint32_t seen{};
                for (const auto &parameter : p.parameters.subspan(output.parameters.first, output.parameters.count))
                {
                    std::uint32_t bit{};
                    if (parameter.key == ShapeId("fbm_octaves") && parameter.type == VfxParameterType::Int)
                    { bit = 1; spawn.fbm_octaves = parameter.bits; }
                    else if (parameter.type == VfxParameterType::Float)
                    {
                        const auto value = std::bit_cast<float>(parameter.bits);
                        if (parameter.key == ShapeId("size")) { bit = 2; spawn.size = value; }
                        else if (parameter.key == ShapeId("domain_warp")) { bit = 4; spawn.domain_warp = value; }
                        else if (parameter.key == ShapeId("flow_speed")) { bit = 8; spawn.procedural_flow_speed = value; }
                    }
                    if (!bit || (seen & bit)) { output_valid = false; break; }
                    seen |= bit;
                }
                if (!output_valid || seen != 15 || spawn.fbm_octaves < 1 || spawn.fbm_octaves > 4 ||
                    !std::isfinite(spawn.size) || spawn.size <= 0.0f || !std::isfinite(spawn.domain_warp) ||
                    spawn.domain_warp < 0.0f || !std::isfinite(spawn.procedural_flow_speed) ||
                    spawn.procedural_flow_speed < 0.0f || !std::isfinite(output.hdr) || output.hdr <= 0.0f ||
                    std::any_of(output.rgba.begin(), output.rgba.end(), [](float v) { return !std::isfinite(v) || v < 0; }) ||
                    output.rgba[3] > 1.0f) continue;
                bool mask_seen = false; VfxRange mask_slices{}; float mask_strength{};
                for (const auto &binding : p.texture_bindings.subspan(output.textures.first, output.textures.count))
                {
                    if (binding.role != ShapeId("authored_mask_array")) continue;
                    const auto path = texture_path(binding);
                    if (mask_seen || binding.min_quality > 2 || !std::isfinite(binding.strength) ||
                        binding.strength < 0.0f || binding.strength > 1.0f ||
                        binding.selection != ShapeId("stable_seed_mod_group_size") ||
                        !valid(binding.slices, p.slice_indices.size()) || !binding.slices.count ||
                        path != "Content/Textures/VFX/vfx_authored_mask_array.dds")
                    { output_valid = false; break; }
                    for (const auto slice : p.slice_indices.subspan(binding.slices.first, binding.slices.count))
                        if (slice >= 12) output_valid = false;
                    mask_seen = true; mask_slices = binding.slices; mask_strength = binding.strength;
                }
                if (!output_valid) continue;
                spawn.gradient_row = output.gradient_row;
                spawn.hdr = output.hdr;
                spawn.color = {output.rgba[0] * output.hdr, output.rgba[1] * output.hdr,
                    output.rgba[2] * output.hdr, output.rgba[3]};
                spawn.ground_base_anchor = true;
                fire.push_back({fire_id, effect.handle, source.stable_id, rate, radius_scale,
                    spawn, mask_slices, mask_strength, source.authoritative != 0});
            }
        }
        range.count = static_cast<std::uint32_t>(fire.size()) - range.first;
    }

    tick_ranges.assign(p.effects.size(), {});
    ticks.clear();
    struct TickRoute { std::uint64_t tick_id, status_id; bool bleed; };
    constexpr TickRoute tick_routes[]{
        {HashEffectName("particle.status.bleed_tick"), HashEffectName("persistent.status.bleed"), true},
        {HashEffectName("particle.status.burn_tick"), HashEffectName("persistent.status.burn"), false},
    };
    for (std::size_t ei = 0; ei < p.effects.size(); ++ei)
    {
        const auto &tick = p.effects[ei];
        auto &range = tick_ranges[ei];
        range.first = static_cast<std::uint32_t>(ticks.size());
        if (tick.handle != ei + 1 || tick.input_mode != 0 || tick.timing_kind != 0 ||
            tick.payload_kind != ShapeId("PointEventPayload") || !std::isfinite(tick.seconds) ||
            tick.seconds <= 0.0f) continue;
        for (const auto &route : tick_routes)
        {
            if (!unique_id(tick.handle, route.tick_id)) continue;
            for (std::size_t status_index = 0; status_index < p.effects.size(); ++status_index)
            {
                const auto &status = p.effects[status_index];
                if (status.handle != status_index + 1 || status.input_mode != 1 || status.timing_kind != 1 ||
                    status.payload_kind != ShapeId("EntityAttachmentPayload") ||
                    !unique_id(status.handle, route.status_id) || !valid(status.sources, p.sources.size())) continue;
                for (std::uint32_t si = status.sources.first; si < status.sources.first + status.sources.count; ++si)
                {
                    const auto &source = p.sources[si];
                    if (source.effect != status.handle || source.stable_id != ShapeId("tick_core") ||
                        source.authoritative != 1 || source.type != VfxSourceType::ImpactSprite ||
                        source.knot_count != 2 || source.knots[0] != 0.0f || source.knots[1] != 0.5f ||
                        !valid(source.outputs, p.outputs.size()) || source.outputs.count != 1 ||
                        !valid(source.parameters, p.parameters.size()) || source.parameters.count != 0) continue;
                    const auto &output = p.outputs[source.outputs.first];
                    if (output.source != si || output.profile != VfxOutputProfile::SpriteSdfAdd ||
                        output.shape != ShapeId("small_pulse") || output.shape_domain != ShapeId("procedural_sprite") ||
                        output.shape_scale_rule != ShapeId("component_size_or_bound_radius") ||
                        output.shape_component_kind != ShapeId("particle") || output.coverage_type != ShapeId("analytic") ||
                        output.coverage_ref != ShapeId("small_pulse") ||
                        output.motion != VfxMotionKind::SingleShortPulse || output.motion_rate_hz != 0.0f ||
                        output.motion_amplitude != 0.0f || output.motion_inset_fraction != 0.0f ||
                        output.min_quality > 2 || !stamp_curve(output, 6) ||
                        !valid(output.textures, p.texture_bindings.size()) ||
                        output.textures.count != (route.bleed ? 3u : 2u) ||
                        !std::isfinite(output.hdr) || output.hdr <= 0.0f ||
                        !std::isfinite(output.rgba[0] * output.hdr) ||
                        !std::isfinite(output.rgba[1] * output.hdr) ||
                        !std::isfinite(output.rgba[2] * output.hdr) ||
                        !std::isfinite(output.rgba[3]) || output.rgba[3] < 0.0f || output.rgba[3] > 1.0f) continue;
                    bool gradient = false, curve = false, mask = false, texture_valid = true;
                    std::array<std::uint32_t,3> mask_slices{}; float mask_strength{};
                    for (const auto &binding : p.texture_bindings.subspan(output.textures.first, output.textures.count))
                    {
                        const auto path = texture_path(binding);
                        if (binding.role == ShapeId("profile.global") && binding.min_quality == 0 &&
                            binding.selection == 0 && binding.slices.count == 0 &&
                            std::isfinite(binding.strength) && binding.strength == 1.0f &&
                            path == "Content/Textures/VFX/vfx_gradient_lut.dds" && !gradient)
                            gradient = true;
                        else if (binding.role == ShapeId("profile.global") && binding.min_quality == 0 &&
                            binding.selection == 0 && binding.slices.count == 0 &&
                            std::isfinite(binding.strength) && binding.strength == 1.0f &&
                            path == "Content/Textures/VFX/vfx_curve_lut.dds" && !curve)
                            curve = true;
                        else if (route.bleed && binding.role == ShapeId("authored_mask_array") &&
                            path == "Content/Textures/VFX/vfx_authored_mask_array.dds" && !mask &&
                            binding.selection == ShapeId("stable_seed_mod_group_size") && binding.min_quality == 0 &&
                            std::isfinite(binding.strength) && binding.strength >= 0.0f && binding.strength <= 1.0f &&
                            valid(binding.slices, p.slice_indices.size()) && binding.slices.count == 3)
                        {
                            const auto slices = p.slice_indices.subspan(binding.slices.first, 3);
                            if (slices[0] != 3 || slices[1] != 4 || slices[2] != 5)
                            { texture_valid = false; break; }
                            mask = true; mask_slices = {slices[0], slices[1], slices[2]};
                            mask_strength = binding.strength;
                        }
                        else { texture_valid = false; break; }
                    }
                    if (!texture_valid || !gradient || !curve || mask != route.bleed) continue;
                    VfxFlashSpawnInput spawn{};
                    spawn.shape = VfxImpactShape::Pulse;
                    spawn.curve_row = 6;
                    spawn.gradient_row = output.gradient_row;
                    spawn.hdr = output.hdr;
                    spawn.color = {output.rgba[0] * output.hdr, output.rgba[1] * output.hdr,
                        output.rgba[2] * output.hdr, output.rgba[3]};
                    ticks.push_back({route.tick_id, route.status_id, tick.handle, status.handle,
                        output.min_quality, tick.seconds, source.knots[0],
                        source.knots[1] - source.knots[0], spawn, mask_slices, mask_strength, route.bleed});
                    break;
                }
            }
        }
        range.count = static_cast<std::uint32_t>(ticks.size()) - range.first;
    }
}

void PrepareVfxBallisticRecipes(VfxProgramView p, std::vector<VfxRange> &ranges,
                                std::vector<VfxBallisticRecipe> &recipes)
{
    const auto hash=[](std::string_view s){std::uint32_t h=2166136261u;for(unsigned char c:s){h^=c;h*=16777619u;}return h;};
    const auto hash64=[](std::string_view s){std::uint64_t h=14695981039346656037ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}return h;};
    const auto valid=[](VfxRange r,std::size_t n){return r.first<=n&&r.count<=n-r.first;};
    struct Shape { std::uint32_t id; VfxMotionKind motion; std::uint8_t mesh; float min_size,max_size,min_speed,max_speed,gravity,cone,min_spin,max_spin; };
    const Shape shapes[]{
        {hash("small_shards"),VfxMotionKind::BriefRadialResponse,4,.035f,.065f,.55f,1,-3,75,3,7},
        {hash("debris_shard_mesh"),VfxMotionKind::BallisticBurstGravitySpin,3,.06f,.12f,1.5f,2.6f,-9.81f,75,5,11},
        {hash("shard_mesh"),VfxMotionKind::ShortBallisticFan,2,.04f,.09f,1.4f,2.2f,-5,55,4,9},
        {hash("small_shard_mesh"),VfxMotionKind::ShardsLiftFall,4,.025f,.05f,2,2,-8,30,3,7},
        {hash("small_shard_mesh"),VfxMotionKind::ShortForwardShardFan,4,.035f,.065f,2.4f,2.4f,-5,70,4,8},
        {hash("needle_shard_mesh"),VfxMotionKind::ThinShardsForwardCone,7,.07f,.12f,4.2f,4.2f,-3,32,2,5}
    };
    ranges.assign(p.effects.size(),{}); recipes.clear();
    for(std::size_t ei=0;ei<p.effects.size();++ei)
    {
        const auto &effect=p.effects[ei]; auto &range=ranges[ei]; range.first=static_cast<std::uint32_t>(recipes.size());
        if(effect.handle!=ei+1||!valid(effect.sources,p.sources.size()))continue;
        const auto lookup=std::find_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto &e){return e.handle==effect.handle;});
        if(lookup==p.effect_lookup.end()||!lookup->effect_id)continue;
        for(std::uint32_t si=effect.sources.first;si<effect.sources.first+effect.sources.count;++si)
        {
            const auto &source=p.sources[si];
            const bool persistent=effect.input_mode==1&&source.stable_id==hash("status_shards")&&
                (lookup->effect_id==hash64("particle.player.bow_draw")||lookup->effect_id==hash64("particle.upgrade.charged.full_ready")||
                 lookup->effect_id==hash64("particle.upgrade.empowered_ready")||lookup->effect_id==hash64("particle.upgrade.retarget")||
                 lookup->effect_id==hash64("particle.upgrade.ricochet.bleed_extend"));
            if(source.effect!=effect.handle||source.type!=VfxSourceType::BallisticCollision||source.knot_count!=2||
               !std::isfinite(source.knots[0])||!std::isfinite(source.knots[1])||source.knots[0]<0||source.knots[1]<=source.knots[0]||source.knots[1]>1||
               !valid(source.parameters,p.parameters.size())||!valid(source.outputs,p.outputs.size())||
               (persistent&&(source.knots[0]!=0||source.authoritative!=0||source.stable_id!=hash("status_shards"))))continue;
            if((persistent&&(source.outputs.count!=1||source.parameters.count!=1||
                std::count_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto &e){return e.handle==effect.handle;})!=1||
                std::count_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto &e){return e.effect_id==lookup->effect_id;})!=1))||
               (!persistent&&(effect.input_mode!=0||effect.timing_kind!=0)))continue;
            const double delay=static_cast<double>(effect.seconds)*source.knots[0]*60.;
            if(!persistent&&(!std::isfinite(effect.seconds)||effect.seconds<=0||!std::isfinite(delay)))continue;
            std::uint32_t count{};float gravity{},bounce{},drag=.8f,speed=-1,cone{};std::uint32_t seen{};bool params_ok=true;
            for(const auto &v:p.parameters.subspan(source.parameters.first,source.parameters.count))
            {
                std::uint32_t bit{};float *target=nullptr;
                if(v.key==hash("count")){bit=1;if(v.type!=VfxParameterType::Int||v.bits==0||v.bits>1024){params_ok=false;break;}count=v.bits;}
                else if(v.key==hash("gravity")){bit=2;target=&gravity;}
                else if(v.key==hash("bounce")){bit=4;target=&bounce;}
                else if(v.key==hash("drag")){bit=8;target=&drag;}
                else if(v.key==hash("speed")){bit=16;target=&speed;}
                else if(v.key==hash("cone_deg")){bit=32;target=&cone;}
                else {params_ok=false;break;}
                if(seen&bit){params_ok=false;break;}seen|=bit;
                if(target){if(v.type!=VfxParameterType::Float){params_ok=false;break;}*target=std::bit_cast<float>(v.bits);if(!std::isfinite(*target)){params_ok=false;break;}}
            }
            if(!params_ok||!(seen&1)||bounce<0||bounce>1||drag<0||cone<0||cone>180||((seen&16)&&speed<=0))continue;
            for(const auto &output:p.outputs.subspan(source.outputs.first,source.outputs.count))
            {
                const auto shape=std::find_if(std::begin(shapes),std::end(shapes),[&](const auto &s){return s.id==output.shape&&s.motion==output.motion;});
                if(shape==std::end(shapes)||output.source!=si||output.profile!=VfxOutputProfile::MeshEmissiveOit||
                   output.min_quality>2||!valid(output.parameters,p.parameters.size())||output.parameters.count||
                   !std::isfinite(output.hdr)||output.hdr<=0||!std::isfinite(output.rgba[0])||!std::isfinite(output.rgba[1])||
                   !std::isfinite(output.rgba[2])||!std::isfinite(output.rgba[3])||output.rgba[3]<0||output.rgba[3]>1)continue;
                const bool forward=output.motion==VfxMotionKind::ShortForwardShardFan||output.motion==VfxMotionKind::ThinShardsForwardCone;
                if(forward&&lookup->effect_id!=(output.motion==VfxMotionKind::ShortForwardShardFan?
                    hash64("particle.enemy.ranged.impact"):hash64("particle.skill.charged_shot.impact")))continue;
                bool textures_ok=true;
                if(!valid(output.textures,p.texture_bindings.size()))continue;
                for(const auto &binding:p.texture_bindings.subspan(output.textures.first,output.textures.count))
                {
                    if(binding.role!=hash("profile.global")){textures_ok=false;break;}
                    const auto resource=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),[&](const auto &r){return r.slot==binding.catalog_slot;});
                    if(resource==p.texture_resources.end()||!valid(resource->asset_path_bytes,p.strings.size())){textures_ok=false;break;}
                    const std::string_view path(reinterpret_cast<const char*>(p.strings.data())+resource->asset_path_bytes.first,resource->asset_path_bytes.count);
                    if(path!="Content/Textures/VFX/vfx_gradient_lut.dds"&&path!="Content/Textures/VFX/vfx_curve_lut.dds"){textures_ok=false;break;}
                }
                if(!textures_ok)continue;
                if(persistent&&(effect.payload_kind!=(lookup->effect_id==hash64("particle.upgrade.retarget")?hash("ProjectilePathPayload"):hash("EntityAttachmentPayload"))||
                    effect.timing_kind!=((lookup->effect_id==hash64("particle.upgrade.charged.full_ready")||
                    lookup->effect_id==hash64("particle.upgrade.retarget")||lookup->effect_id==hash64("particle.upgrade.ricochet.bleed_extend"))?0u:1u)||
                    (effect.timing_kind==0&&(!std::isfinite(effect.seconds)||effect.seconds<=0))||output.shape!=hash("small_shards")||
                    output.shape_domain!=hash("sprite")||output.shape_scale_rule!=hash("component_size")||output.shape_component_kind!=hash("particle")||
                    output.coverage_type!=hash("analytic")||output.coverage_ref!=hash("small_shards")||output.motion_rate_hz!=0||
                    output.motion_amplitude!=0||output.motion_inset_fraction!=0||output.min_quality!=1||output.textures.count!=2))continue;
                if(persistent)
                {
                    bool g=false,c=false,ok=true;
                    for(const auto &binding:p.texture_bindings.subspan(output.textures.first,output.textures.count))
                    {
                        const auto resource=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),[&](const auto &r){return r.slot==binding.catalog_slot;});
                        if(binding.role!=hash("profile.global")||binding.min_quality!=0||binding.selection!=0||binding.slices.count||!std::isfinite(binding.strength)||binding.strength!=1||resource==p.texture_resources.end()||!valid(resource->asset_path_bytes,p.strings.size())){ok=false;break;}
                        const std::string_view path(reinterpret_cast<const char*>(p.strings.data())+resource->asset_path_bytes.first,resource->asset_path_bytes.count);
                        if(path=="Content/Textures/VFX/vfx_gradient_lut.dds"&&!g)g=true;else if(path=="Content/Textures/VFX/vfx_curve_lut.dds"&&!c)c=true;else{ok=false;break;}
                    }
                    if(!ok||!g||!c||count!=5)continue;
                }
                recipes.push_back({lookup->effect_id,effect.handle,si,source.stable_id,effect.payload_kind,effect.importance,effect.timing_kind,
                    output.shape,output.min_quality,output.gradient_row,shape->mesh,count,source.knot_count,source.type,output.motion,
                    source.knots,effect.seconds,shape->min_size,shape->max_size,shape->min_speed,shape->max_speed,
                    (seen&2)?gravity:shape->gravity,(seen&4)?bounce:0,(seen&8)?drag:.8f,(seen&16)?speed:-1,
                    (seen&32)?cone:shape->cone,shape->min_spin,shape->max_spin,
                    {output.rgba[0],output.rgba[1],output.rgba[2],output.rgba[3]},output.hdr,persistent});
            }
        }
        range.count=static_cast<std::uint32_t>(recipes.size())-range.first;
    }
}
void PrepareVfxFresnelRecipes(VfxProgramView p,
    std::vector<VfxFresnelEffectRecipe> &effects, std::vector<VfxFresnelRecipe> &outputs)
{
    constexpr auto phase2=HashEffectName("persistent.boss.phase2_aura");
    constexpr auto transition=HashEffectName("persistent.boss.phase_transition_invulnerable");
    constexpr auto player=HashEffectName("particle.player.invulnerable_loop");
    constexpr auto entity_payload=ShapeId("EntityAttachmentPayload");
    constexpr auto boss_shell=ShapeId("boss_shell"),entity_shell=ShapeId("entity_shell");
    constexpr auto mesh_shell=ShapeId("mesh_shell"),owner_bounds=ShapeId("owner_bounds");
    constexpr auto particle=ShapeId("particle"),analytic=ShapeId("analytic");
    constexpr auto global=ShapeId("profile.global"),authored=ShapeId("profile.authored");
    constexpr auto sector=ShapeId("sector_count"),rotation=ShapeId("rotation_hz");
    constexpr auto power=ShapeId("fresnel_power"),noise=ShapeId("noise"),end_crack=ShapeId("end_crack");
    const auto valid_range=[](VfxRange range,std::size_t size){return ValidRange(range,size);};
    effects.assign(p.effects.size(),{});
    outputs.clear();
    const auto exact_textures=[&](const VfxOutputRecord &output) {
        if(!valid_range(output.textures,p.texture_bindings.size())||output.textures.count!=2)return false;
        bool has_gradient=false,has_noise=false;
        for(std::uint32_t i=output.textures.first;i<output.textures.first+output.textures.count;++i)
        {
            const auto &binding=p.texture_bindings[i];
            const auto resource=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),
                [&](const auto &candidate){return candidate.slot==binding.catalog_slot;});
            if(resource==p.texture_resources.end()||resource->asset_path_bytes.count==0||
                !valid_range(resource->asset_path_bytes,p.strings.size()))return false;
            const std::string_view path(reinterpret_cast<const char *>(p.strings.data())+
                resource->asset_path_bytes.first,resource->asset_path_bytes.count);
            if(binding.role==global&&path=="Content/Textures/VFX/vfx_gradient_lut.dds"&&!has_gradient)
                has_gradient=true;
            else if(binding.role==authored&&path=="Content/Textures/VFX/vfx_noise_volume_64.dds"&&!has_noise)
                has_noise=true;
            else return false;
        }
        return has_gradient&&has_noise;
    };
    const auto parameters=[&](const VfxOutputRecord &output,VfxFresnelShellKind kind,
                              VfxFresnelRecipe &row) {
        if(!valid_range(output.parameters,p.parameters.size())||output.parameters.count!=2)return false;
        bool first=false,second=false;
        for(std::uint32_t i=output.parameters.first;i<output.parameters.first+output.parameters.count;++i)
        {
            const auto &parameter=p.parameters[i];
            if(kind==VfxFresnelShellKind::BossCrown)
            {
                if(parameter.key==sector&&parameter.type==VfxParameterType::Int&&!first)
                {row.sector_count=parameter.bits;first=true;}
                else if(parameter.key==rotation&&parameter.type==VfxParameterType::Float&&!second)
                {row.rotation_hz=std::bit_cast<float>(parameter.bits);second=true;}
                else return false;
            }
            else if(kind==VfxFresnelShellKind::BossTransition)
            {
                if(parameter.key==power&&parameter.type==VfxParameterType::Float&&!first)
                {row.fresnel_power=std::bit_cast<float>(parameter.bits);first=true;}
                else if(parameter.key==end_crack&&parameter.type==VfxParameterType::Int&&!second)
                {row.end_crack=parameter.bits;second=true;}
                else return false;
            }
            else
            {
                if(parameter.key==power&&parameter.type==VfxParameterType::Float&&!first)
                {row.fresnel_power=std::bit_cast<float>(parameter.bits);first=true;}
                else if(parameter.key==noise&&parameter.type==VfxParameterType::Float&&!second)
                {row.noise_amount=std::bit_cast<float>(parameter.bits);second=true;}
                else return false;
            }
        }
        if(!first||!second)return false;
        if(kind==VfxFresnelShellKind::BossCrown)
            return row.sector_count>0&&row.sector_count<=64&&std::isfinite(row.rotation_hz);
        if(kind==VfxFresnelShellKind::BossTransition)
            return std::isfinite(row.fresnel_power)&&row.fresnel_power>0&&row.end_crack<=1;
        return std::isfinite(row.fresnel_power)&&row.fresnel_power>0&&
            std::isfinite(row.noise_amount)&&row.noise_amount>=0&&row.noise_amount<=1;
    };
    for(std::size_t ei=0;ei<p.effects.size();++ei)
    {
        const auto &effect=p.effects[ei];auto &effect_row=effects[ei];
        effect_row.effect_handle=effect.handle;
        effect_row.outputs.first=static_cast<std::uint32_t>(outputs.size());
        std::uint64_t identity{};bool duplicate_identity=false;
        for(const auto &lookup:p.effect_lookup)
            if(lookup.handle==effect.handle)
            {
                if(identity!=0){duplicate_identity=true;break;}
                identity=lookup.effect_id;
            }
        effect_row.effect_id=identity;
        effect_row.kind=identity==phase2?VfxFresnelShellKind::BossCrown:
            identity==transition?VfxFresnelShellKind::BossTransition:VfxFresnelShellKind::PlayerInvulnerable;
        const bool known=identity==phase2||identity==transition||identity==player;
        if(effect.handle!=ei+1||duplicate_identity||!known||effect.input_mode!=1||effect.timing_kind!=1||
            effect.payload_kind!=entity_payload||!valid_range(effect.sources,p.sources.size()))continue;
        effect_row.route_valid=true;
        for(std::uint32_t si=effect.sources.first;si<effect.sources.first+effect.sources.count;++si)
        {
            const auto &source=p.sources[si];
            if(source.effect!=effect.handle||source.type!=VfxSourceType::Direct||
                (effect_row.kind==VfxFresnelShellKind::PlayerInvulnerable&&source.stable_id!=ShapeId("body_shell"))||
                !valid_range(source.parameters,p.parameters.size())||source.parameters.count!=0||
                !valid_range(source.outputs,p.outputs.size())||source.knot_count!=2||
                !std::isfinite(source.knots[0])||!std::isfinite(source.knots[1])||
                source.knots[0]<0||source.knots[1]>1||source.knots[1]<=source.knots[0])continue;
            for(std::uint32_t oi=source.outputs.first;oi<source.outputs.first+source.outputs.count;++oi)
            {
                const auto &output=p.outputs[oi];const auto kind=effect_row.kind;
                const bool is_player=kind==VfxFresnelShellKind::PlayerInvulnerable;
                const auto shell=is_player?entity_shell:boss_shell;
                const auto motion=kind==VfxFresnelShellKind::BossCrown?
                    VfxMotionKind::SlowCounterRotationContraction:
                    kind==VfxFresnelShellKind::BossTransition?VfxMotionKind::FresnelShellContracts:
                    VfxMotionKind::SubtleFresnelPulse;
                if(output.source!=si||output.profile!=(is_player?VfxOutputProfile::FresnelShellEmissive:
                    VfxOutputProfile::FresnelShellOit)||output.shape!=shell||output.shape_domain!=mesh_shell||
                    output.shape_scale_rule!=owner_bounds||output.shape_component_kind!=particle||
                    output.coverage_type!=analytic||output.coverage_ref!=shell||output.motion!=motion||
                    output.min_quality>2||!exact_textures(output)||!std::isfinite(output.hdr)||output.hdr<=0||
                    !std::isfinite(output.motion_rate_hz)||output.motion_rate_hz<0||
                    !std::isfinite(output.motion_amplitude)||output.motion_amplitude<0||output.motion_amplitude>1||
                    !std::isfinite(output.motion_inset_fraction)||output.motion_inset_fraction<0||
                    output.motion_inset_fraction>1||
                    std::any_of(output.rgba.begin(),output.rgba.end(),[](float v){return !std::isfinite(v);})||
                    output.rgba[3]<0||output.rgba[3]>1)continue;
                VfxFresnelRecipe row{};row.effect_handle=effect.handle;row.source_index=si;
                row.source_stable_id=source.stable_id;row.output_index=oi;row.min_quality=output.min_quality;
                row.kind=kind;row.knot_start=source.knots[0];row.knot_end=source.knots[1];
                if(!parameters(output,kind,row))continue;
                row.color={output.rgba[0],output.rgba[1],output.rgba[2],output.rgba[3]};
                row.hdr=output.hdr;row.gradient_row=output.gradient_row;
                row.motion_rate_hz=output.motion_rate_hz;row.motion_amplitude=output.motion_amplitude;
                row.motion_inset_fraction=output.motion_inset_fraction;outputs.push_back(row);
            }
        }
        effect_row.outputs.count=static_cast<std::uint32_t>(outputs.size())-effect_row.outputs.first;
    }
}
void PrepareVfxMeshRecipes(VfxProgramView p,
    std::vector<VfxMeshEffectRecipe> &effects, std::vector<VfxMeshRecipe> &outputs)
{
    constexpr auto projectile=ShapeId("ProjectilePayload");
    constexpr auto fresnel_key=ShapeId("fresnel"),ratio_key=ShapeId("head_to_tail_ratio");
    constexpr auto shell_key=ShapeId("shell_thickness");
    constexpr auto arrow=ShapeId("arrowhead_mesh"),thorn=ShapeId("enemy_thorn_mesh");
    constexpr auto lance=ShapeId("boss_crest_lance_mesh");
    effects.assign(p.effects.size(),{});outputs.clear();
    for(std::size_t ei=0;ei<p.effects.size();++ei)
    {
        const auto &effect=p.effects[ei];auto &effect_row=effects[ei];
        effect_row.effect_handle=effect.handle;
        effect_row.outputs.first=static_cast<std::uint32_t>(outputs.size());
        if(effect.handle!=ei+1||effect.input_mode!=1||effect.payload_kind!=projectile||
            !ValidRange(effect.sources,p.sources.size()))continue;
        effect_row.route_valid=true;
        for(std::uint32_t si=effect.sources.first;si<effect.sources.first+effect.sources.count;++si)
        {
            const auto &source=p.sources[si];
            if(source.effect!=effect.handle||!ValidRange(source.outputs,p.outputs.size())||
                (source.type!=VfxSourceType::ProjectileFollow&&source.type!=VfxSourceType::BallisticCollision))continue;
            for(std::uint32_t oi=source.outputs.first;oi<source.outputs.first+source.outputs.count;++oi)
            {
                const auto &output=p.outputs[oi];
                std::uint32_t mesh{};
                if(output.shape==arrow&&output.motion==VfxMotionKind::VelocityAlignedSharpHead)mesh=1;
                else if(output.shape==thorn&&output.motion==VfxMotionKind::VelocityAligned)mesh=5;
                else if(output.shape==lance&&output.motion==VfxMotionKind::VelocityAlignedHeavyLance)mesh=6;
                if(output.source!=si||output.profile!=VfxOutputProfile::MeshEmissiveOit||mesh==0||
                    !std::isfinite(output.hdr)||output.hdr<=0||
                    !ValidRange(output.parameters,p.parameters.size()))continue;
                float fresnel{},ratio{},shell{};std::uint32_t seen{};bool valid=true;
                for(std::uint32_t pi=output.parameters.first;pi<output.parameters.first+output.parameters.count;++pi)
                {
                    const auto &parameter=p.parameters[pi];
                    if(parameter.type!=VfxParameterType::Float){valid=false;break;}
                    const auto value=std::bit_cast<float>(parameter.bits);std::uint32_t bit{};
                    if(parameter.key==fresnel_key){bit=1;fresnel=value;}
                    else if(parameter.key==ratio_key&&mesh!=6){bit=2;ratio=value;}
                    else if(parameter.key==shell_key&&mesh==6){bit=4;shell=value;}
                    if(bit==0||(seen&bit)){valid=false;break;}
                    seen|=bit;
                }
                if(!valid||(seen&(mesh==6?4u:2u))==0||!std::isfinite(ratio)||!std::isfinite(shell)||
                    (mesh==6?shell!=.22f:ratio!=(mesh==5?.5f:.42f))||
                    !std::isfinite(fresnel)||fresnel<0||fresnel>1||
                    !std::all_of(output.rgba.begin(),output.rgba.end(),[](float value){return std::isfinite(value);}))continue;
                outputs.push_back({effect.handle,si,oi,source.stable_id,mesh,output.min_quality,
                    output.gradient_row,{output.rgba[0],output.rgba[1],output.rgba[2],output.rgba[3]},
                    output.hdr,fresnel,ratio,shell});
            }
        }
        effect_row.outputs.count=static_cast<std::uint32_t>(outputs.size())-effect_row.outputs.first;
    }
}
Result DecodeVfxImage(std::span<const std::byte> legacy, std::span<const std::byte> program_bytes,
                      VfxImageData &output)
{
    if (legacy.size() < sizeof(CookedParticleEffectsHeader))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle catalog header is missing.");
    CookedParticleEffectsHeader header;
    std::memcpy(&header, legacy.data(), sizeof(header));
    const char magic[8]{'H', 'S', 'P', 'F', 'X', '\0', '\0', '\0'};
    if (std::memcmp(header.magic, magic, sizeof(magic)) != 0 ||
        header.format_version != 4 || header.sprite_count == 0 || header.schema_hash != kParticleEffectsSchemaHash)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle catalog header is incompatible.");
    const auto payload_size = static_cast<std::uint64_t>(header.effect_count) * sizeof(CookedVfxDefinition) +
        static_cast<std::uint64_t>(header.emitter_count) * sizeof(CookedParticleEmitter) +
        static_cast<std::uint64_t>(header.sprite_count) * sizeof(CookedParticleSprite);
    if (payload_size > std::numeric_limits<std::uint32_t>::max() || legacy.size() != sizeof(header) + payload_size)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle catalog size is invalid.");
    const auto payload = legacy.subspan(sizeof(header));
    if (Fnv1a64(payload) != header.payload_hash)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle catalog payload is corrupt.");
    VfxImageData loaded;
    loaded.definitions.resize(header.effect_count);
    loaded.emitters.resize(header.emitter_count);
    loaded.sprites.resize(header.sprite_count);
    std::memcpy(loaded.definitions.data(), payload.data(), loaded.definitions.size() * sizeof(CookedVfxDefinition));
    std::memcpy(loaded.emitters.data(), payload.data() + loaded.definitions.size() * sizeof(CookedVfxDefinition),
                loaded.emitters.size() * sizeof(CookedParticleEmitter));
    std::memcpy(loaded.sprites.data(), payload.data() +
                    loaded.definitions.size() * sizeof(CookedVfxDefinition) +
                    loaded.emitters.size() * sizeof(CookedParticleEmitter),
                loaded.sprites.size() * sizeof(CookedParticleSprite));
    std::uint64_t previous{};
    for (std::size_t index = 0; index < loaded.definitions.size(); ++index)
    {
        const auto &definition = loaded.definitions[index];
        if (definition.effect_id == 0 || (index && definition.effect_id <= previous) || definition.reserved != 0 ||
            static_cast<std::uint64_t>(definition.first_emitter) + definition.emitter_count > loaded.emitters.size())
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle definition table is invalid.");
        if ((definition.kind == VfxDefinitionKind::Particles) != (definition.emitter_count != 0))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle definition kind is invalid.");
        if (definition.kind == VfxDefinitionKind::Line &&
            (definition.line_primitive < VfxPrimitive::SolidTrail ||
             definition.line_primitive > VfxPrimitive::DashWake))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX line primitive is invalid.");
        if (definition.line_sprite >= header.sprite_count || definition.line_frame_columns == 0 ||
            definition.line_frame_rows == 0)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle definition sprite is invalid.");
        previous = definition.effect_id;
    }
    for (const auto &emitter : loaded.emitters)
    {
        const auto valid_primitive =
            (emitter.renderer == VfxRenderer::Sprite &&
             (emitter.primitive == VfxPrimitive::Soft || emitter.primitive == VfxPrimitive::Flame)) ||
            (emitter.renderer == VfxRenderer::Ground &&
             ((emitter.primitive >= VfxPrimitive::Disc && emitter.primitive <= VfxPrimitive::Cracks) ||
              emitter.primitive == VfxPrimitive::Flame)) ||
            (emitter.renderer == VfxRenderer::Segment &&
             emitter.primitive >= VfxPrimitive::SolidTrail && emitter.primitive <= VfxPrimitive::DashWake) ||
            (emitter.renderer == VfxRenderer::Mesh && emitter.primitive >= VfxPrimitive::Arrow &&
             emitter.primitive <= VfxPrimitive::ShockShell);
        if (emitter.sprite >= header.sprite_count || emitter.frame_columns == 0 ||
            emitter.frame_rows == 0 || !valid_primitive)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle emitter contract is invalid.");
    }
    previous = 0;
    for (std::size_t index = 0; index < loaded.sprites.size(); ++index)
    {
        const auto &sprite = loaded.sprites[index];
        if (sprite.sprite_id == 0 || (index && sprite.sprite_id <= previous) ||
            sprite.index >= header.sprite_count || sprite.frame_columns == 0 ||
            sprite.frame_rows == 0)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle sprite table is invalid.");
        previous = sprite.sprite_id;
    }
    if (program_bytes.empty() || program_bytes.size() > std::numeric_limits<std::uint32_t>::max())
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX program size is invalid.");
    VfxProgramData program;
    std::string program_error;
    if (!LoadVfxProgram(program_bytes, program, program_error))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX v4 program: " + program_error);
    loaded.program = std::move(program);
    const auto &cooked_program = loaded.program;
    loaded.authored_mesh_indices.resize(loaded.emitters.size());
    loaded.decorative_definitions.reserve(loaded.definitions.size());
    loaded.has_typed_wavefront = std::ranges::any_of(cooked_program.effect_lookup, [](const auto &effect) {
        return effect.effect_id == MakeAssetId("particle.boss.shockwave.wavefront").value;
    });
    for (const auto &definition : loaded.definitions)
    {
        const auto lookup = std::find_if(cooked_program.effect_lookup.begin(), cooked_program.effect_lookup.end(),
            [&](const auto &entry) { return entry.effect_id == definition.effect_id; });
        const auto *policy = lookup != cooked_program.effect_lookup.end()
            ? &cooked_program.effects[lookup->handle - 1] : nullptr;
        loaded.decorative_definitions.push_back(policy && !policy->must_match_logic && std::ranges::none_of(
            std::span(cooked_program.sources).subspan(policy->sources.first, policy->sources.count), [](const auto &source) {
                return source.authoritative || source.type == VfxSourceType::ProjectileFollow;
            }));
        for (std::uint32_t ei = definition.first_emitter; ei < definition.first_emitter + definition.emitter_count; ++ei)
        {
            const auto &emitter = loaded.emitters[ei];
            if (emitter.renderer != VfxRenderer::Mesh) continue;
            std::uint8_t selected{};
            bool ambiguous = false;
            if (lookup != cooked_program.effect_lookup.end())
            {
                const auto &effect = cooked_program.effects[lookup->handle - 1];
                for (std::uint32_t si = effect.sources.first; si < effect.sources.first + effect.sources.count; ++si)
                {
                    const auto &source = cooked_program.sources[si];
                    for (std::uint32_t oi = source.outputs.first; oi < source.outputs.first + source.outputs.count; ++oi)
                    {
                        const auto &output = cooked_program.outputs[oi];
                        if (output.profile != VfxOutputProfile::MeshEmissiveOit) continue;
                        const auto mesh = MeshIndex(output.shape);
                        if (!mesh) continue;
                        const bool compatible =
                            (emitter.primitive == VfxPrimitive::Arrow && mesh == 1) ||
                            (emitter.primitive == VfxPrimitive::Shard && (mesh == 2 || mesh == 3 || mesh == 4)) ||
                            (emitter.primitive == VfxPrimitive::Ember && (mesh == 2 || mesh == 3 || mesh == 4)) ||
                            (emitter.primitive == VfxPrimitive::Spike && (mesh == 5 || mesh == 6 || mesh == 7));
                        if (!compatible) continue;
                        if (selected && selected != mesh) ambiguous = true;
                        selected = mesh;
                    }
                }
            }
            if (selected && !ambiguous) loaded.authored_mesh_indices[ei] = selected;
        }
    }
    loaded.sprite_count = header.sprite_count;
    loaded.payload_hash = header.payload_hash;
    loaded.has_required_sprite_bindings = true;
    for (const auto &[id, target] : {
             std::pair<std::string_view, ParticleSpriteBinding *>("particle_sprite.blood", &loaded.sprite_bindings.bleed_status_sprite),
             {"particle_sprite.fire", &loaded.sprite_bindings.burn_status_sprite},
             {"particle_sprite.slow_rune", &loaded.sprite_bindings.slow_status_sprite},
             {"particle_sprite.mark_target", &loaded.sprite_bindings.mark_status_sprite},
             {"particle_sprite.trap_rune", &loaded.sprite_bindings.trap_sprite},
             {"particle_sprite.fire_ground", &loaded.sprite_bindings.fire_area_sprite}})
    {
        const auto identity = MakeAssetId(id).value;
        const auto found = std::lower_bound(loaded.sprites.begin(), loaded.sprites.end(), identity,
            [](const CookedParticleSprite &sprite, std::uint64_t key) { return sprite.sprite_id < key; });
        if (found == loaded.sprites.end() || found->sprite_id != identity)
            loaded.has_required_sprite_bindings = false;
        else *target = {found->index, found->frame_columns, found->frame_rows};
    }
    PrepareVfxLightRecipes(loaded.program.View(), loaded.light_effect_ranges, loaded.light_recipes);
    PrepareVfxBallisticRecipes(loaded.program.View(), loaded.ballistic_effect_ranges, loaded.ballistic_recipes);
    PrepareVfxDistortionRecipes(loaded.program.View(), loaded.distortion_effect_ranges, loaded.distortion_recipes);
    PrepareVfxDecalRecipes(loaded.program.View(), loaded.decal_effect_ranges, loaded.decal_recipes);
    PrepareVfxGroundRecipes(loaded.program.View(), loaded.ground_effect_ranges, loaded.ground_recipes);
    PrepareVfxFlashRecipes(loaded.program.View(), loaded.flash_effect_ranges, loaded.flash_recipes);
    PrepareVfxPersistentFlashRecipes(loaded.program.View(),
        loaded.persistent_stamp_effect_ranges, loaded.persistent_stamp_recipes,
        loaded.persistent_fire_effect_ranges, loaded.persistent_fire_recipes,
        loaded.owned_status_tick_effect_ranges, loaded.owned_status_tick_recipes);
    PrepareVfxMoteRecipes(loaded.program.View(), loaded.mote_source_recipes, loaded.mote_output_recipes);
    if(auto result=PrepareVfxScreenRecipes(loaded.program.View(), loaded.screen_effect_recipes, loaded.screen_recipes);!result)
        return result;
    if(auto result=PrepareVfxVolumeRecipes(loaded.program.View(), loaded.volume_effect_recipes, loaded.volume_recipes);!result)
        return result;
    PrepareVfxEntitySurfaceRecipes(loaded.program.View(),loaded.entity_surface_effect_recipes,
        loaded.entity_surface_recipes,loaded.entity_surface_routes);
    PrepareVfxRibbonRecipes(loaded.program.View(),loaded.ribbon_effect_recipes,loaded.ribbon_source_recipes,
        loaded.ribbon_output_recipes,loaded.ribbon_texture_recipes,loaded.ribbon_slice_indices);
    PrepareVfxRibbonWakeRecipes(loaded.ribbon_effect_recipes,
        loaded.ribbon_source_recipes, loaded.ribbon_output_recipes,
        loaded.ribbon_texture_recipes, loaded.ribbon_slice_indices,
        loaded.ribbon_wake_effect_ranges, loaded.ribbon_wake_recipes);
    PrepareVfxRibbonFixedRecipes(loaded.ribbon_effect_recipes, loaded.ribbon_source_recipes,
        loaded.ribbon_output_recipes, loaded.ribbon_texture_recipes, loaded.ribbon_slice_indices,
        loaded.ribbon_fixed_effect_ranges, loaded.ribbon_fixed_recipes);
    PrepareVfxRibbonProjectileRecipes(loaded.ribbon_effect_recipes, loaded.ribbon_source_recipes,
        loaded.ribbon_output_recipes, loaded.ribbon_texture_recipes, loaded.ribbon_slice_indices,
        loaded.ribbon_projectile_effect_ranges, loaded.ribbon_projectile_sources,
        loaded.ribbon_projectile_outputs);
    PrepareVfxRibbonCancelRecipes(loaded.ribbon_effect_recipes, loaded.ribbon_source_recipes,
        loaded.ribbon_output_recipes, loaded.ribbon_texture_recipes,
        loaded.ribbon_cancel_effect_ranges, loaded.ribbon_cancel_recipes);
    PrepareVfxRibbonLinkRecipes(loaded.ribbon_effect_recipes, loaded.ribbon_source_recipes,
        loaded.ribbon_output_recipes, loaded.ribbon_texture_recipes, loaded.ribbon_slice_indices,
        loaded.ribbon_link_effect_recipes, loaded.ribbon_link_output_recipes);
    PrepareVfxRibbonEventRecipes(loaded.ribbon_effect_recipes, loaded.ribbon_source_recipes,
        loaded.ribbon_output_recipes, loaded.ribbon_texture_recipes, loaded.ribbon_slice_indices,
        loaded.ribbon_event_effect_recipes, loaded.ribbon_event_sources, loaded.ribbon_event_outputs);
    PrepareVfxEffectLookupById(loaded.program.View(), loaded.effect_lookup_by_id);
    PrepareVfxFresnelRecipes(loaded.program.View(), loaded.fresnel_effect_recipes,
        loaded.fresnel_recipes);
    PrepareVfxMeshRecipes(loaded.program.View(), loaded.mesh_effect_recipes, loaded.mesh_recipes);
    PrepareVfxOutputProfiles(loaded.program.View(), loaded.effect_output_profiles);
    output = std::move(loaded);
    return Result::Success();
}
Result LoadVfxImage(const std::filesystem::path &legacy_path,
                    const std::filesystem::path &program_path, VfxImageData &output)
{
    const auto read = [](const std::filesystem::path &path, std::vector<std::byte> &bytes) -> Result {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX image file is missing.");
        const auto length = file.tellg();
        if (length < 0 || static_cast<std::uint64_t>(length) > std::numeric_limits<std::uint32_t>::max() + std::uint64_t(sizeof(CookedParticleEffectsHeader)))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX image file size is invalid.");
        bytes.resize(static_cast<std::size_t>(length));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char *>(bytes.data()), length))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX image file is truncated.");
        return Result::Success();
    };
    std::vector<std::byte> legacy, program;
    if (auto result = read(legacy_path, legacy); !result) return result;
    if (auto result = read(program_path, program); !result) return result;
    return DecodeVfxImage(legacy, program, output);
}
}
