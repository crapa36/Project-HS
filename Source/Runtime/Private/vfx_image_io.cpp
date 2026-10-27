#include <hs/runtime/vfx_image_io.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/render_data/render_snapshot.hpp>
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
constexpr std::uint64_t EffectId(std::string_view name) noexcept
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
                (source.knot_count!=2 && source.knot_count!=4) || !valid(source.outputs,p.outputs.size())) continue;
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
                if(std::any_of(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto &entry){
                    return entry.handle==effect.handle && entry.effect_id==EffectId("particle.boss.shockwave.safe_gap_marker");}) &&
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
                if(identity_count==1 && identity->effect_id==EffectId("particle.skill.charged_shot.pulse") &&
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
                    VfxGroundRecipeKind kind{};
                    if(effect_id==EffectId("particle.player.invulnerable_loop") && source.stable_id==ShapeId("ground_state") &&
                        output.profile==VfxOutputProfile::GroundSdfSoft && output.shape==ShapeId("state_ring") &&
                        output.shape_domain==ShapeId("sdf") && output.shape_scale_rule==ShapeId("gameplay_geometry_if_bound_else_component_radius") &&
                        output.shape_component_kind==ShapeId("ground_sdf") && output.coverage_type==ShapeId("analytic") &&
                        output.coverage_ref==ShapeId("state_ring") && output.motion==VfxMotionKind::SlowStableRotation && output.parameters.count==1)
                        kind=VfxGroundRecipeKind::StateRing;
                    const bool polar_effect=effect_id==EffectId("persistent.status.slow") || effect_id==EffectId("persistent.status.mark") ||
                        effect_id==EffectId("particle.pickup.xp.idle") || effect_id==EffectId("particle.pickup.heal.idle") ||
                        effect_id==EffectId("particle.pickup.magnet.idle") || effect_id==EffectId("particle.pickup.relic_chest.idle") ||
                        effect_id==EffectId("particle.upgrade.charged.overcharge_loop") ||
                        effect_id==EffectId("particle.enemy.suicide.countdown_loop");
                    if(polar_effect && source.stable_id==ShapeId("main_rune") && output.profile==VfxOutputProfile::GroundSdfSoft &&
                        output.shape==ShapeId("polar_rune_sdf") && output.shape_domain==ShapeId("sdf") &&
                        output.shape_scale_rule==ShapeId("gameplay_geometry_if_bound_else_component_radius") &&
                        output.shape_component_kind==ShapeId("ground_sdf") && output.coverage_type==ShapeId("analytic") &&
                        output.coverage_ref==ShapeId("polar_rune_sdf") &&
                        output.motion==VfxMotionKind::SlowCounterRotationStableFootprint && output.parameters.count==3)
                        kind=VfxGroundRecipeKind::PolarRune;
                    const bool phase2=effect_id==EffectId("persistent.boss.phase2_aura");
                    const bool transition=effect_id==EffectId("persistent.boss.phase_transition_invulnerable");
                    if(phase2 && output.profile==VfxOutputProfile::GroundSdfAdd && output.shape==ShapeId("broken_crown_sdf") &&
                        output.motion==VfxMotionKind::InwardTeethPulse) kind=VfxGroundRecipeKind::BrokenCrown;
                    if(transition && output.profile==VfxOutputProfile::GroundSdfAdd && output.shape==ShapeId("closed_crown_ring_sdf") &&
                        output.motion==VfxMotionKind::LockedSteadyRingPulse) kind=VfxGroundRecipeKind::ClosedCrownRing;
                    if(kind==VfxGroundRecipeKind::StateRing || kind==VfxGroundRecipeKind::PolarRune ||
                       kind==VfxGroundRecipeKind::BrokenCrown || kind==VfxGroundRecipeKind::ClosedCrownRing)
                    {
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
                            if(effect_id==EffectId("particle.pickup.relic_chest.idle"))
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
        {EffectId("particle.player.bow_draw"), PersistentVfxKind::PlayerBowDraw, ShapeId("EntityAttachmentPayload"), false, false},
        {EffectId("particle.upgrade.charged.full_ready"), PersistentVfxKind::ChargedFullReady, ShapeId("EntityAttachmentPayload"), true, false},
        {EffectId("particle.upgrade.empowered_ready"), PersistentVfxKind::EmpoweredReady, ShapeId("EntityAttachmentPayload"), false, false},
        {EffectId("particle.upgrade.retarget"), PersistentVfxKind::MultishotRetarget, ShapeId("ProjectilePathPayload"), true, false},
        {EffectId("particle.upgrade.ricochet.bleed_extend"), PersistentVfxKind::RicochetBleedExtend, ShapeId("EntityAttachmentPayload"), true, true},
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
    const auto fire_id = EffectId("persistent.area.fire");
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
                    spawn, mask_slices, mask_strength});
            }
        }
        range.count = static_cast<std::uint32_t>(fire.size()) - range.first;
    }

    tick_ranges.assign(p.effects.size(), {});
    ticks.clear();
    struct TickRoute { std::uint64_t tick_id, status_id; bool bleed; };
    constexpr TickRoute tick_routes[]{
        {EffectId("particle.status.bleed_tick"), EffectId("persistent.status.bleed"), true},
        {EffectId("particle.status.burn_tick"), EffectId("persistent.status.burn"), false},
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
    std::uint32_t mapped{}, unmapped{};
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
            if (selected && !ambiguous) { loaded.authored_mesh_indices[ei] = selected; ++mapped; }
            else ++unmapped;
        }
    }
    std::clog << "vfx.authored_mesh mapped_emitters=" << mapped << " unmapped_emitters=" << unmapped
              << " policy=primitive_compatible_unique_shape motion=legacy\n";
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
