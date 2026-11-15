#include "vfx_typed_volume_commands.hpp"

#include <algorithm>
#include <cmath>

namespace hs::runtime_detail
{
namespace
{
Result Invalid(const char *message)
{return Result::Failure(ErrorCode::InvalidArgument,"hs_vfx_volume",message);}
bool Affine(const std::array<float,16> &m)
{return std::all_of(m.begin(),m.end(),[](float x){return std::isfinite(x);})&&m[3]==0&&m[7]==0&&m[11]==0&&m[15]==1;}
bool Transform(const std::array<float,16> &owner,float scale,bool crown,VfxVolumeInput &out)
{
    if(!Affine(owner)||!std::isfinite(scale)||scale<=0)return false;
    auto m=owner;
    if(crown)for(unsigned column=0;column<3;++column)m[12+column]+=scale*owner[4+column];
    for(unsigned row=0;row<3;++row)for(unsigned column=0;column<3;++column)m[row*4+column]*=scale;
    const double a=m[0],b=m[1],c=m[2],d=m[4],e=m[5],f=m[6],g=m[8],h=m[9],i=m[10];
    const double determinant=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
    if(!std::isfinite(determinant)||determinant==0)return false;
    std::array<float,16> inverse{};inverse[15]=1;
    const double terms[]{e*i-f*h,c*h-b*i,b*f-c*e,f*g-d*i,a*i-c*g,c*d-a*f,d*h-e*g,b*g-a*h,a*e-b*d};
    for(unsigned row=0;row<3;++row)for(unsigned column=0;column<3;++column)
        inverse[row*4+column]=float(terms[row*3+column]/determinant);
    for(unsigned column=0;column<3;++column)
        inverse[12+column]=-(m[12]*inverse[column]+m[13]*inverse[4+column]+m[14]*inverse[8+column]);
    if(!Affine(m)||!Affine(inverse))return false;
    out.local_to_world=m;out.world_to_local=inverse;
    const Float3 extent{std::abs(m[0])+std::abs(m[4])+std::abs(m[8]),
        std::abs(m[1])+std::abs(m[5])+std::abs(m[9]),std::abs(m[2])+std::abs(m[6])+std::abs(m[10])};
    out.bounds_min={m[12]-extent.x,m[13]-extent.y,m[14]-extent.z};
    out.bounds_max={m[12]+extent.x,m[13]+extent.y,m[14]+extent.z};
    return std::isfinite(out.bounds_min.x)&&std::isfinite(out.bounds_min.y)&&std::isfinite(out.bounds_min.z)&&
        std::isfinite(out.bounds_max.x)&&std::isfinite(out.bounds_max.y)&&std::isfinite(out.bounds_max.z);
}
Result Append(const VfxImage &image,const VfxVolumeEffectRecipe &effect,VfxVolumeIdentity identity,
    const std::array<float,16> &transform,float scale,float elapsed,float effect_age,
    std::uint32_t seed,VfxQuality quality,std::vector<VfxVolumeInput> &out)
{
    const bool crown=effect.route==VfxVolumeRoute::PersistentEntity;
    VfxVolumeInput transformed;
    if(effect.outputs.count && !Transform(transform,scale,crown,transformed))
        return Invalid("Malformed or unsupported cooked LocalVolume transform or scale.");
    for(std::size_t i=effect.outputs.first;i<static_cast<std::size_t>(effect.outputs.first)+effect.outputs.count;++i)
    {
        const auto &recipe=image.volume_recipes[i];
        if(static_cast<std::uint32_t>(quality)<recipe.min_quality)continue;
        if(!crown&&(effect_age<recipe.knot_start||effect_age>=recipe.knot_end))continue;
        VfxVolumeInput command=transformed;command.identity=identity;command.identity.effect_id=effect.effect_id;
        command.identity.effect_handle=identity.effect_handle;command.identity.source_id=recipe.source_id;
        command.identity.source_index=recipe.source_index;command.identity.output_index=recipe.output_index;
        command.shape=recipe.shape;command.noise=recipe.noise;command.minimum_quality=recipe.min_quality;
        command.stable_seed=seed;command.elapsed_seconds=elapsed;command.gradient_row=recipe.gradient_row;
        command.alpha=recipe.alpha;command.steps=recipe.steps;
        command.half_resolution=recipe.half_resolution;command.density=recipe.density;command.anisotropy=recipe.anisotropy;
        command.source_age=crown?effect_age:(effect_age-recipe.knot_start)/(recipe.knot_end-recipe.knot_start);
        if(!std::isfinite(command.source_age)||command.source_age<0||command.source_age>1)
            return Invalid("LocalVolume source age must be the actual bounded lifecycle phase.");
        command.source_envelope=1.0f;
        command.scattering_albedo={std::clamp(recipe.rgb.x,0.0f,1.0f),std::clamp(recipe.rgb.y,0.0f,1.0f),std::clamp(recipe.rgb.z,0.0f,1.0f)};
        command.emission={recipe.rgb.x*recipe.hdr,recipe.rgb.y*recipe.hdr,recipe.rgb.z*recipe.hdr};
        if(!std::isfinite(command.emission.x)||!std::isfinite(command.emission.y)||!std::isfinite(command.emission.z))
            return Invalid("LocalVolume HDR appearance overflowed.");
        out.push_back(command);
    }
    return Result::Success();
}
}
bool IsVfxTypedVolumeEffect(const VfxImage &image,VfxEffectHandle handle) noexcept
{
    if(!handle||handle>image.volume_effect_recipes.size())return false;
    return image.volume_effect_recipes[handle-1].route!=VfxVolumeRoute::Unsupported;
}
Result BuildVfxTypedVolumeCommands(const VfxImage &image,VfxEventView events,
    std::span<const VfxPersistentInput> persistent,double current_tick,VfxQuality admission,std::vector<VfxVolumeInput> &output)
{
    output.clear();if(admission!=VfxQuality::High)return Result::Success();
    std::vector<VfxVolumeInput> commands;commands.swap(output);
    for(const auto &input:events)
    {
        if(!input.effect_handle||input.effect_handle>image.volume_effect_recipes.size())continue;
        const auto &effect=image.volume_effect_recipes[input.effect_handle-1];
        if(effect.route!=VfxVolumeRoute::EventContext&&effect.route!=VfxVolumeRoute::EventPoint)continue;
        if(input.sequence==0)return Invalid("LocalVolume event requires its actual sequence, fixed timing and source tick.");
        if(current_tick<input.event_tick)continue;
        const float elapsed=float(current_tick-input.event_tick)/60.0f;if(elapsed>=effect.seconds)continue;
        float scale=0;
        if(effect.route==VfxVolumeRoute::EventPoint)
        {if(const auto *point=std::get_if<VfxPointPayload>(&input.payload))scale=point->authored_scale;}
        else if(const auto *context=std::get_if<VfxContextPayload>(&input.payload))scale=context->scalar0;
        VfxVolumeIdentity identity;identity.event_sequence=input.sequence;identity.effect_handle=input.effect_handle;
        if(auto result=Append(image,effect,identity,input.world_transform,scale,elapsed,elapsed/effect.seconds,
            input.stable_seed,input.quality,commands);!result)return result;
    }
    for(const auto &input:persistent)
    {
        if(!input.effect_handle||input.effect_handle>image.volume_effect_recipes.size())continue;
        const auto &effect=image.volume_effect_recipes[input.effect_handle-1];if(effect.route!=VfxVolumeRoute::PersistentEntity)continue;
        const auto *entity=std::get_if<VfxEntityPayload>(&input.payload);
        if(input.stable_id==0||!entity||entity->render_instance_id==0||
           !std::isfinite(input.elapsed_seconds)||input.elapsed_seconds<0)
            return Invalid("LocalVolume crown requires a live entity owner and its authoritative elapsed time.");
        VfxVolumeIdentity identity;identity.owner_id=input.stable_id;identity.episode_generation=input.status_episode_generation;
        identity.effect_handle=input.effect_handle;
        if(auto result=Append(image,effect,identity,input.current_transform,entity->footprint_radius,input.elapsed_seconds,
            input.normalized_age,input.stable_seed,input.quality,commands);!result)return result;
    }
    output.swap(commands);return Result::Success();
}
} // namespace hs::runtime_detail
