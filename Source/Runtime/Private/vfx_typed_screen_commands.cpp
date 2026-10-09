#include "vfx_typed_screen_commands.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <string_view>
#include <utility>

namespace hs::runtime_detail
{
namespace
{
constexpr std::uint32_t Hash32(std::string_view s)
{ std::uint32_t h=2166136261u; for(unsigned char c:s){h^=c;h*=16777619u;} return h; }
constexpr std::uint64_t Hash64(std::string_view s)
{ std::uint64_t h=14695981039346656037ull; for(unsigned char c:s){h^=c;h*=1099511628211ull;} return h; }
constexpr auto kHit=Hash64("particle.player.hit_direction"), kHealth=Hash64("post.player.low_health_vignette");
template<class T> bool Range(VfxRange r,const std::vector<T> &v)
{ return r.first<=v.size()&&r.count<=v.size()-r.first; }
std::uint64_t EffectId(const VfxProgramData &p,VfxEffectHandle handle)
{
    const auto i=std::find_if(p.effect_lookup.begin(),p.effect_lookup.end(),[=](const auto &x){return x.handle==handle;});
    return i==p.effect_lookup.end()?0:i->effect_id;
}
Result Invalid(const char *message)
{ return Result::Failure(ErrorCode::InvalidArgument,"hs_vfx_screen",message); }
bool Parameters(const VfxProgramData &p,const VfxOutputRecord &o,VfxScreenInput &v)
{
    if(!Range(o.parameters,p.parameters))return false;
    bool center=false,edge=false,direction=false;
    for(const auto &x:std::span(p.parameters).subspan(o.parameters.first,o.parameters.count))
    {
        if(x.key==Hash32("center_clear")&&!center&&x.type==VfxParameterType::Float)
        { center=true;v.center_clear=std::bit_cast<float>(x.bits); }
        else if(x.key==Hash32("edge_power")&&!edge&&x.type==VfxParameterType::Float)
        { edge=true;v.edge_power=std::bit_cast<float>(x.bits); }
        else if(x.key==Hash32("angle_from_hit")&&!direction&&x.type==VfxParameterType::Bool&&x.bits==1)
            direction=true;
        else return false;
    }
    return (v.shape==VfxScreenShape::Vignette ? center&&edge&&!direction : direction&&!center&&!edge)&&
        std::isfinite(v.center_clear)&&v.center_clear>=0&&v.center_clear<1&&
        std::isfinite(v.edge_power)&&v.edge_power>0;
}
bool Textures(const VfxProgramData &p,const VfxOutputRecord &o)
{
    if(!Range(o.textures,p.texture_bindings)||o.textures.count!=1)return false;
    const auto &b=p.texture_bindings[o.textures.first];
    const auto i=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),[&](const auto &r){return r.slot==b.catalog_slot;});
    if(i==p.texture_resources.end()||!Range(i->asset_path_bytes,p.strings)||b.role!=Hash32("profile.global")||
       b.selection||b.slices.count||b.first_frame||b.frame_count||b.fps!=0||b.strength!=1)return false;
    return std::string_view(reinterpret_cast<const char *>(p.strings.data())+i->asset_path_bytes.first,
        i->asset_path_bytes.count)=="Content/Textures/VFX/vfx_gradient_lut.dds";
}
Result Append(const VfxProgramData &p,const VfxEffectRecord &effect,const VfxContextPayload &context,
    float elapsed,float age,bool persistent,VfxQuality admission,std::uint64_t sequence,
    std::uint64_t owner,std::vector<VfxScreenInput> &commands)
{
    if(!Range(effect.sources,p.sources))return Invalid("Screen overlay source range is invalid.");
    for(auto si=effect.sources.first;si<effect.sources.first+effect.sources.count;++si)
    {
        const auto &s=p.sources[si];
        if(s.effect!=effect.handle||s.type!=VfxSourceType::Direct||!Range(s.outputs,p.outputs)||
           !Range(s.parameters,p.parameters)||s.parameters.count||s.knot_count!=2||
           !std::isfinite(s.knots[0])||!std::isfinite(s.knots[1])||s.knots[0]<0||
           s.knots[1]<=s.knots[0]||s.knots[1]>1)return Invalid("Screen overlay requires a valid direct source interval.");
        if(!persistent&&(age<s.knots[0]||age>=s.knots[1]))continue;
        const float source_age=persistent?age:(age-s.knots[0])/(s.knots[1]-s.knots[0]);
        for(auto oi=s.outputs.first;oi<s.outputs.first+s.outputs.count;++oi)
        {
            const auto &o=p.outputs[oi];if(o.profile!=VfxOutputProfile::ScreenOverlay)continue;
            VfxScreenInput v;v.event_sequence=sequence;v.owner_id=owner;v.output_index=oi;
            if(o.shape==Hash32("screen_vignette"))v.shape=VfxScreenShape::Vignette;
            else if(o.shape==Hash32("directional_arc"))v.shape=VfxScreenShape::DirectionalArc;
            else return Invalid("Screen overlay shape is unsupported.");
            if(o.source!=si||o.shape_component_kind!=Hash32("screen")||o.coverage_type!=Hash32("analytic_screen")||
               o.motion!=(v.shape==VfxScreenShape::Vignette?VfxMotionKind::SlowPulseStateLocked:VfxMotionKind::OptionalTransientArc)||
               o.min_quality>2||!Parameters(p,o,v)||!Textures(p,o)||!std::isfinite(o.hdr)||o.hdr<0||
               !std::all_of(o.rgba.begin(),o.rgba.end(),[](float x){return std::isfinite(x)&&x>=0;})||o.rgba[3]>1)
                return Invalid("Screen overlay output violates its cooked analytic coverage or gradient contract.");
            if(o.min_quality>std::uint32_t(admission))continue;
            v.color={o.rgba[0],o.rgba[1],o.rgba[2],1};v.alpha=o.rgba[3];v.hdr=o.hdr;v.gradient_row=o.gradient_row;
            // Damage carries travel direction toward the victim; the arc points back to its source.
            v.source_direction={-context.direction.x,-context.direction.y,-context.direction.z};
            v.gradient_age=std::clamp(source_age,0.0f,1.0f);
            if(persistent)
            {
                // The owner's tick clock sets phase. Lower HP changes period only.
                const float frequency=1.0f+(1.0f-context.ratio01/0.35f);
                const float phase=elapsed*frequency;
                v.gradient_age=phase-std::floor(phase);
                v.envelope=0.75f+0.25f*std::cos(2.0f*std::numbers::pi_v<float>*phase);
            }
            commands.push_back(v); // Authored alpha zero remains zero, including optional arcs.
        }
    }
    return Result::Success();
}
}
bool IsVfxTypedScreenEffect(const VfxProgramData &p,VfxEffectHandle handle) noexcept
{ const auto id=EffectId(p,handle);return id==kHit||id==kHealth; }
Result BuildVfxTypedScreenCommands(const VfxProgramData &p,std::span<const VfxEventInput> events,
    std::span<const VfxPersistentInput> persistent,Tick tick,VfxQuality admission,std::vector<VfxScreenInput> &output)
{
    output.clear();std::vector<VfxScreenInput> commands;
    for(const auto &v:events)
    {
        if(EffectId(p,v.effect_handle)!=kHit)continue;
        if(!v.effect_handle||v.effect_handle>p.effects.size())return Invalid("Screen hit effect handle is invalid.");
        const auto &e=p.effects[v.effect_handle-1];const auto *c=std::get_if<VfxContextPayload>(&v.payload);
        if(!c||!v.sequence||e.input_mode!=0||e.timing_kind!=0||e.payload_kind!=Hash32("PresentationContextPayload")||
           !std::isfinite(e.seconds)||e.seconds<=0||!std::isfinite(c->direction.x)||!std::isfinite(c->direction.y)||
           !std::isfinite(c->direction.z))return Invalid("Screen hit requires an actual timed damage context.");
        if(tick<v.event_tick)continue;
        const float elapsed=float(tick-v.event_tick)/60.0f;if(elapsed>=e.seconds)continue;
        if(auto r=Append(p,e,*c,elapsed,elapsed/e.seconds,false,admission,v.sequence,0,commands);!r)return r;
    }
    for(const auto &v:persistent)
    {
        if(EffectId(p,v.effect_handle)!=kHealth)continue;
        if(!v.effect_handle||v.effect_handle>p.effects.size())return Invalid("Screen health effect handle is invalid.");
        const auto &e=p.effects[v.effect_handle-1];const auto *c=std::get_if<VfxContextPayload>(&v.payload);
        if(!c||!v.stable_id||e.input_mode!=1||e.timing_kind!=1||e.payload_kind!=Hash32("PresentationContextPayload")||
           !std::isfinite(c->ratio01)||c->ratio01<0||c->ratio01>0.35f||
           !std::isfinite(v.elapsed_seconds)||v.elapsed_seconds<0)return Invalid("Screen health requires its live low-health owner and tick clock.");
        if(auto r=Append(p,e,*c,v.elapsed_seconds,v.normalized_age,true,admission,0,v.stable_id,commands);!r)return r;
    }
    output=std::move(commands);return Result::Success();
}
}
