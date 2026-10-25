#include "vfx_typed_volume_commands.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace hs::runtime_detail
{
namespace
{
constexpr std::uint32_t Hash32(std::string_view text)
{std::uint32_t h=2166136261u;for(unsigned char c:text){h^=c;h*=16777619u;}return h;}
constexpr std::uint64_t Hash64(std::string_view text)
{std::uint64_t h=14695981039346656037ull;for(unsigned char c:text){h^=c;h*=1099511628211ull;}return h;}
constexpr auto kPhase=Hash64("particle.boss.phase_change"),kSpawn=Hash64("particle.boss.spawn"),
    kDeath=Hash64("particle.boss.death"),kAura=Hash64("persistent.boss.phase2_aura");
Result Invalid(std::string message)
{return Result::Failure(ErrorCode::InvalidArgument,"hs_vfx_volume",std::move(message));}
template<class T> bool Range(VfxRange r,std::span<const T> values)
{return r.first<=values.size()&&r.count<=values.size()-r.first;}
std::uint64_t EffectId(const VfxProgramView &p,VfxEffectHandle handle)
{
    const auto i=std::find_if(p.effect_lookup.begin(),p.effect_lookup.end(),[=](const auto &x){return x.handle==handle;});
    return i==p.effect_lookup.end()?0:i->effect_id;
}
bool Allowed(std::uint64_t id){return id==kPhase||id==kSpawn||id==kDeath||id==kAura;}
bool Affine(const std::array<float,16> &m)
{return std::all_of(m.begin(),m.end(),[](float x){return std::isfinite(x);})&&m[3]==0&&m[7]==0&&m[11]==0&&m[15]==1;}
void HashWord(std::uint64_t &h,std::uint32_t word)
{for(unsigned i=0;i<4;++i){h^=(word>>(i*8))&255u;h*=1099511628211ull;}}
void HashFloat(std::uint64_t &h,float value){HashWord(h,std::bit_cast<std::uint32_t>(value==0?0.0f:value));}

// Invert the actual affine owner transform, including authored component
// scale. A crown's canonical box is lifted one footprint radius above its
// attachment origin; it is a visual component, not a gameplay hitbox.
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
    out.local_to_world=m;out.world_to_local=inverse;out.authored_scale=scale;
    const Float3 extent{std::abs(m[0])+std::abs(m[4])+std::abs(m[8]),
        std::abs(m[1])+std::abs(m[5])+std::abs(m[9]),std::abs(m[2])+std::abs(m[6])+std::abs(m[10])};
    out.bounds_min={m[12]-extent.x,m[13]-extent.y,m[14]-extent.z};
    out.bounds_max={m[12]+extent.x,m[13]+extent.y,m[14]+extent.z};
    return std::isfinite(out.bounds_min.x)&&std::isfinite(out.bounds_min.y)&&std::isfinite(out.bounds_min.z)&&
        std::isfinite(out.bounds_max.x)&&std::isfinite(out.bounds_max.y)&&std::isfinite(out.bounds_max.z);
}
bool Parameters(const VfxProgramView &p,const VfxOutputRecord &output,bool crown,VfxVolumeInput &out)
{
    if(!Range(output.parameters,p.parameters)||output.parameters.count!=3)return false;
    bool steps=false,density=false,anisotropy=false,noise=false,half=false;
    for(const auto &parameter:std::span(p.parameters).subspan(output.parameters.first,output.parameters.count))
    {
        if(parameter.key==Hash32("steps")&&!steps&&parameter.type==VfxParameterType::Int)
        {steps=true;out.steps=parameter.bits;if(out.steps!=(crown?18u:12u))return false;}
        else if(crown&&parameter.key==Hash32("density")&&!density&&parameter.type==VfxParameterType::Float)
        {density=true;out.density=std::bit_cast<float>(parameter.bits);if(!std::isfinite(out.density)||out.density<0)return false;}
        else if(crown&&parameter.key==Hash32("anisotropy")&&!anisotropy&&parameter.type==VfxParameterType::Float)
        {anisotropy=true;out.anisotropy=std::bit_cast<float>(parameter.bits);if(!std::isfinite(out.anisotropy)||std::abs(out.anisotropy)>=1)return false;}
        else if(!crown&&parameter.key==Hash32("noise")&&!noise&&parameter.type==VfxParameterType::Enum&&parameter.bits==Hash32("3d_fbm"))
        {noise=true;out.noise=VfxVolumeNoise::Fbm3D;}
        else if(!crown&&parameter.key==Hash32("half_res")&&!half&&parameter.type==VfxParameterType::Bool&&parameter.bits==1)
        {half=true;out.half_resolution=1;}
        else return false;
    }
    return steps&&(crown?(density&&anisotropy):(noise&&half));
}
bool Textures(const VfxProgramView &p,const VfxOutputRecord &o)
{
    if(!Range(o.textures,p.texture_bindings)||o.textures.count!=3)return false;
    bool gradient=false,stbn=false,noise=false;
    for(const auto &binding:std::span(p.texture_bindings).subspan(o.textures.first,o.textures.count))
    {
        const auto found=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),[&](const auto &r){return r.slot==binding.catalog_slot;});
        if(found==p.texture_resources.end()||!Range(found->asset_path_bytes,p.strings))return false;
        if(binding.selection||binding.min_quality!=0||binding.strength!=1||binding.slices.count||
           !Range(binding.slices,p.slice_indices)||binding.first_frame||binding.frame_count||binding.fps!=0)return false;
        const std::string_view path(reinterpret_cast<const char *>(p.strings.data()+found->asset_path_bytes.first),found->asset_path_bytes.count);
        if(path=="Content/Textures/VFX/vfx_gradient_lut.dds"&&!gradient&&binding.role==Hash32("profile.global"))gradient=true;
        else if(path=="Content/Textures/VFX/vfx_stbn_scalar.dds"&&!stbn&&binding.role==Hash32("profile.global"))stbn=true;
        else if(path=="Content/Textures/VFX/vfx_noise_volume_64.dds"&&!noise&&binding.role==Hash32("profile.authored"))noise=true;
        else return false;
    }
    return gradient&&stbn&&noise;
}
void Revisions(VfxVolumeInput &out)
{
    out.transform_revision=14695981039346656037ull;
    for(float v:out.local_to_world)HashFloat(out.transform_revision,v);
    out.density_revision=14695981039346656037ull;
    for(float v:{out.elapsed_seconds,out.source_age,out.source_envelope,out.density,out.alpha,out.anisotropy,out.extinction})HashFloat(out.density_revision,v);
    HashWord(out.density_revision,out.stable_seed);HashWord(out.density_revision,out.steps);
    HashWord(out.density_revision,std::uint32_t(out.shape));HashWord(out.density_revision,std::uint32_t(out.noise));
    out.appearance_revision=14695981039346656037ull;
    for(float v:{out.scattering_albedo.x,out.scattering_albedo.y,out.scattering_albedo.z,out.emission.x,out.emission.y,out.emission.z,out.hdr})HashFloat(out.appearance_revision,v);
    HashWord(out.appearance_revision,out.gradient_row);
}
Result Append(const VfxProgramView &program,const VfxEffectRecord &effect,std::uint64_t effect_id,
    VfxVolumeIdentity identity,const std::array<float,16> &transform,float scale,float elapsed,float effect_age,
    std::uint32_t seed,VfxQuality quality,std::vector<VfxVolumeInput> &out)
{
    if(!Range(effect.sources,program.sources))return Invalid("LocalVolume effect source range is invalid.");
    const bool crown=effect_id==kAura;bool found=false;
    for(std::uint32_t index=effect.sources.first;index<effect.sources.first+effect.sources.count;++index)
    {
        const auto &source=program.sources[index];if(source.type!=VfxSourceType::VolumeLocal)continue;
        if(found||source.effect!=effect.handle||source.stable_id!=Hash32(crown?"phase_wisps":"smoke_volume")||
           source.authoritative!=0||source.parameters.count!=0||!Range(source.parameters,program.parameters)||
           source.knot_count!=2||!std::isfinite(source.knots[0])||!std::isfinite(source.knots[1])||
           source.knots[0]<0||source.knots[1]<=source.knots[0]||source.knots[1]>1||
           !Range(source.outputs,program.outputs)||source.outputs.count!=1)
            return Invalid("LocalVolume source must retain its exact bounded source identity, parameters and lifecycle knots.");
        found=true;
        const auto &output=program.outputs[source.outputs.first];
        VfxVolumeInput command;command.identity=identity;command.identity.effect_id=effect_id;
        command.identity.effect_handle=effect.handle;command.identity.source_id=source.stable_id;
        command.identity.source_index=index;command.identity.output_index=source.outputs.first;
        command.shape=crown?VfxVolumeShape::BoundedCrown:VfxVolumeShape::NoisySphere;
        command.minimum_quality=output.min_quality;command.stable_seed=seed;command.elapsed_seconds=elapsed;
        command.gradient_row=output.gradient_row;command.alpha=output.rgba[3];command.hdr=output.hdr;
        if(output.source!=index||output.profile!=VfxOutputProfile::LocalVolume||output.min_quality!=2||
           output.shape!=Hash32(crown?"bounded_crown_volume":"half_res_sphere_volume")||
           output.shape_domain!=Hash32("volume")||output.shape_scale_rule!=Hash32("component_bounds")||
           output.shape_component_kind!=Hash32("volume")||output.coverage_type!=Hash32("volume_density")||
           output.coverage_ref!=0||output.surface_target!=0||output.motion_rate_hz!=0||output.motion_amplitude!=0||output.motion_inset_fraction!=0||
           output.motion!=(crown?VfxMotionKind::SlowInwardCurl:VfxMotionKind::BuoyantTurbulentSmoke)||
           !Parameters(program,output,crown,command)||!Textures(program,output)||
           !std::isfinite(output.hdr)||output.hdr<0||!std::all_of(output.rgba.begin(),output.rgba.end(),[](float v){return std::isfinite(v)&&v>=0;})||
           output.rgba[3]>1||!Transform(transform,scale,crown,command))
            return Invalid("Malformed or unsupported cooked LocalVolume output, texture contract, scale or optical coefficients.");
        if(std::uint32_t(quality)<output.min_quality)continue;
        if(!crown&&(effect_age<source.knots[0]||effect_age>=source.knots[1]))continue;
        // Gameplay lifetime never repeats a fixed smoke fade: snapshot presence
        // owns the crown, whose density flow continues with actual elapsed time.
        command.source_age=crown?effect_age:(effect_age-source.knots[0])/(source.knots[1]-source.knots[0]);
        if(!std::isfinite(command.source_age)||command.source_age<0||command.source_age>1)
            return Invalid("LocalVolume source age must be the actual bounded lifecycle phase.");
        // The cooked gradient row is sampled from the resident authored LUT on
        // GPU. Do not bake its alpha stops into Runtime or apply them twice.
        command.source_envelope=1.0f;
        command.scattering_albedo={std::clamp(output.rgba[0],0.0f,1.0f),std::clamp(output.rgba[1],0.0f,1.0f),std::clamp(output.rgba[2],0.0f,1.0f)};
        command.emission={output.rgba[0]*output.hdr,output.rgba[1]*output.hdr,output.rgba[2]*output.hdr};
        if(!std::isfinite(command.emission.x)||!std::isfinite(command.emission.y)||!std::isfinite(command.emission.z))
            return Invalid("LocalVolume HDR appearance overflowed.");
        Revisions(command);out.push_back(command);
    }
    return found?Result::Success():Invalid("Allowlisted LocalVolume effect has no executable VolumeLocal source.");
}
} // namespace
bool IsVfxTypedVolumeEffect(const VfxProgramView &program,VfxEffectHandle handle) noexcept
{return Allowed(EffectId(program,handle));}
Result BuildVfxTypedVolumeCommands(const VfxProgramView &program,std::span<const VfxEventInput> events,
    std::span<const VfxPersistentInput> persistent,Tick current_tick,VfxQuality admission,std::vector<VfxVolumeInput> &output)
{
    output.clear();if(admission!=VfxQuality::High)return Result::Success();
    std::vector<VfxVolumeInput> commands;
    for(const auto &input:events)
    {
        const auto id=EffectId(program,input.effect_handle);if(!Allowed(id)||id==kAura)continue;
        if(input.effect_handle==0||input.effect_handle>program.effects.size())return Invalid("LocalVolume event has an invalid effect handle.");
        const auto &effect=program.effects[input.effect_handle-1];
        if(effect.handle!=input.effect_handle||effect.input_mode!=0||effect.timing_kind!=0||
           !std::isfinite(effect.seconds)||effect.seconds<=0||input.sequence==0||current_tick<input.event_tick)
            return Invalid("LocalVolume event requires its actual sequence, fixed timing and source tick.");
        const float elapsed=float(current_tick-input.event_tick)/60.0f;if(elapsed>=effect.seconds)continue;
        float scale=0;
        if(id==kSpawn&&effect.payload_kind==Hash32("PointEventPayload"))
        {if(const auto *p=std::get_if<VfxPointPayload>(&input.payload))scale=p->authored_scale;}
        else if((id==kPhase||id==kDeath)&&effect.payload_kind==Hash32("PresentationContextPayload"))
        {if(const auto *p=std::get_if<VfxContextPayload>(&input.payload))scale=p->scalar0;}
        VfxVolumeIdentity identity;identity.event_sequence=input.sequence;
        if(auto result=Append(program,effect,id,identity,input.world_transform,scale,elapsed,elapsed/effect.seconds,
            input.stable_seed,input.quality,commands);!result)return result;
    }
    for(const auto &input:persistent)
    {
        if(EffectId(program,input.effect_handle)!=kAura)continue;
        if(input.effect_handle==0||input.effect_handle>program.effects.size())return Invalid("LocalVolume owner has an invalid effect handle.");
        const auto &effect=program.effects[input.effect_handle-1];const auto *entity=std::get_if<VfxEntityPayload>(&input.payload);
        if(effect.handle!=input.effect_handle||effect.input_mode!=1||effect.timing_kind!=1||effect.payload_kind!=Hash32("EntityAttachmentPayload")||
           input.stable_id==0||!entity||entity->render_instance_id==0||!std::isfinite(input.elapsed_seconds)||input.elapsed_seconds<0)
            return Invalid("LocalVolume crown requires a live entity owner and its authoritative elapsed time.");
        VfxVolumeIdentity identity;identity.owner_id=input.stable_id;identity.episode_generation=input.status_episode_generation;
        if(auto result=Append(program,effect,kAura,identity,input.current_transform,entity->footprint_radius,input.elapsed_seconds,
            input.normalized_age,input.stable_seed,input.quality,commands);!result)return result;
    }
    output=std::move(commands);return Result::Success();
}
} // namespace hs::runtime_detail
