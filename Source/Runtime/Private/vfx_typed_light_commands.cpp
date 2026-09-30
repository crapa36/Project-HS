#include "vfx_typed_light_commands.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <string_view>
namespace hs::runtime_detail
{
namespace
{
constexpr std::uint32_t Hash(std::string_view s){std::uint32_t h=2166136261u;for(unsigned char c:s){h^=c;h*=16777619u;}return h;}
bool Finite(Float3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
template<class T>bool Range(VfxRange r,const std::vector<T>&v){return r.first<=v.size()&&r.count<=v.size()-r.first;}
std::uint64_t Mix(std::uint64_t v){v^=v>>30;v*=0xbf58476d1ce4e5b9ull;v^=v>>27;v*=0x94d049bb133111ebull;return v^(v>>31);}
std::optional<Float3> Center(const VfxEventInput &input,std::uint32_t schema)
{
 if(const auto *v=std::get_if<VfxPointPayload>(&input.payload)){if(schema==Hash("PointEventPayload")&&Finite(v->position))return v->position;}
 else if(std::holds_alternative<VfxContextPayload>(input.payload))
 {const Float3 p{input.world_transform[12],input.world_transform[13],input.world_transform[14]};if(schema==Hash("PresentationContextPayload")&&Finite(p))return p;}
 else if(const auto *v=std::get_if<VfxCirclePayload>(&input.payload))
 {if(schema==Hash("CircleAreaPayload")&&Finite(v->center)&&std::isfinite(v->radius)&&v->radius>0&&v->inner_radius==0)return v->center;}
 else if(const auto *v=std::get_if<VfxRingGapsPayload>(&input.payload))
 {if(schema==Hash("RingWithGapsPayload")&&Finite(v->center)&&std::isfinite(v->inner_radius)&&std::isfinite(v->outer_radius)&&v->inner_radius>=0&&v->outer_radius>v->inner_radius)return v->center;}
 else if(const auto *v=std::get_if<VfxConePayload>(&input.payload))
 {if(schema==Hash("ConePayload")&&Finite(v->origin)&&Finite(v->direction)&&std::isfinite(v->range)&&v->range>0&&
   std::isfinite(v->half_angle_degrees)&&v->half_angle_degrees>0&&v->half_angle_degrees<=180)return v->origin;}
 return {};
}
std::optional<float> Curve(const VfxProgramData &p,std::uint32_t row,float age)
{
 if(row>=p.curves.size())return {};const auto &curve=p.curves[row];
 if(curve.interpolation>1||curve.domain!=Hash("normalized_age")||!Range(curve.keys,p.curve_keys)||curve.keys.count<2)return {};
 const auto keys=std::span(p.curve_keys).subspan(curve.keys.first,curve.keys.count);
 if(keys.front().time!=0||keys.back().time!=1)return {};
 for(std::size_t i=0;i<keys.size();++i)
  if(!std::isfinite(keys[i].time)||!std::isfinite(keys[i].value)||keys[i].value<0||
     (i&&keys[i].time<=keys[i-1].time))return {};
 for(std::size_t i=1;i<keys.size();++i)if(age<=keys[i].time)
 {
  float t=std::clamp((age-keys[i-1].time)/(keys[i].time-keys[i-1].time),0.0f,1.0f);
  if(curve.interpolation==1)t=t*t*(3-2*t);
  return std::lerp(keys[i-1].value,keys[i].value,t);
 }
 return keys.back().value;
}
std::optional<float> Envelope(const VfxSourceRecord &source,float age)
{
 if(source.knot_count!=2&&source.knot_count!=4)return {};
 for(std::uint32_t i=0;i<source.knot_count;++i)
  if(!std::isfinite(source.knots[i])||source.knots[i]<0||source.knots[i]>1||(i&&source.knots[i]<source.knots[i-1]))return {};
 const auto end=source.knots[source.knot_count-1];
 if(end<=source.knots[0]||age<source.knots[0]||age>=end)return {};
 float alpha=1;
 if(source.knot_count==4)
 {
  if(age<source.knots[1]&&source.knots[1]>source.knots[0])alpha=(age-source.knots[0])/(source.knots[1]-source.knots[0]);
  else if(age>source.knots[2]&&end>source.knots[2])alpha=(end-age)/(end-source.knots[2]);
 }
 return alpha;
}
}
std::vector<VfxLightInput> BuildVfxTypedLightCommands(const VfxProgramData &p,std::span<const VfxEventInput> inputs,Tick current_tick)
{
 std::vector<VfxLightInput> result;
 for(const auto &input:inputs)
 {
  if(input.quality==VfxQuality::Low||static_cast<std::uint32_t>(input.quality)>2||input.effect_handle==0||input.effect_handle>p.effects.size()||current_tick<input.event_tick)continue;
  const auto &effect=p.effects[input.effect_handle-1];
  if(effect.handle!=input.effect_handle||effect.input_mode!=0||effect.timing_kind!=0||!std::isfinite(effect.seconds)||effect.seconds<=0||!Range(effect.sources,p.sources))continue;
  const auto center=Center(input,effect.payload_kind);if(!center)continue;
  const auto identity=std::find_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto &v){return v.handle==effect.handle;});
  if(identity==p.effect_lookup.end()||identity->effect_id==0)continue;
  const auto start=input.geometry_owner_id?input.geometry_start_tick:input.event_tick;
  if(current_tick<start||(input.geometry_owner_id&&input.geometry_end_tick<=start))continue;
  const double ticks=input.geometry_owner_id?static_cast<double>(input.geometry_end_tick-start):static_cast<double>(effect.seconds)*60;
  const float age=static_cast<float>(static_cast<double>(current_tick-start)/ticks);
  if(!std::isfinite(age)||age<0||age>=1)continue;
  for(std::uint32_t si=effect.sources.first;si<effect.sources.first+effect.sources.count;++si)
  {
   const auto &source=p.sources[si];
   if(source.effect!=effect.handle||source.type!=VfxSourceType::Direct||!Range(source.parameters,p.parameters)||source.parameters.count!=0||!Range(source.outputs,p.outputs))continue;
   const auto envelope=Envelope(source,age);if(!envelope||*envelope<=0)continue;
   const float local_age=(age-source.knots[0])/(source.knots[source.knot_count-1]-source.knots[0]);
   for(const auto &output:std::span(p.outputs).subspan(source.outputs.first,source.outputs.count))
   {
    const bool hero=output.motion==VfxMotionKind::SingleHighPriorityLightPulse;
    if(output.source!=si||output.profile!=VfxOutputProfile::Light||(!hero&&output.motion!=VfxMotionKind::SingleCappedLightPulse&&output.motion!=VfxMotionKind::GentleGreenPulse)||
       output.min_quality>static_cast<std::uint32_t>(input.quality)||!Range(output.parameters,p.parameters)||output.parameters.count!=(hero?1u:2u))continue;
    const VfxParameterRecord *radius=nullptr,*curve=nullptr;bool valid=true;
    for(const auto &parameter:std::span(p.parameters).subspan(output.parameters.first,output.parameters.count))
    {if(parameter.key==Hash("radius")&&!radius)radius=&parameter;else if(parameter.key==Hash("intensity_curve_ref")&&!curve)curve=&parameter;else valid=false;}
    if(!valid||!radius||radius->type!=VfxParameterType::Float)continue;
    const float metres=std::bit_cast<float>(radius->bits);if(!std::isfinite(metres)||metres<=0)continue;
    float pulse{};
    if(hero)
    {
     // No authored curve for this family: brief 10% plateau, then quadratic fade.
     const float fade=1-std::clamp((local_age-.1f)/.9f,0.0f,1.0f);pulse=fade*fade;
    }
    else
    {if(!curve||curve->type!=VfxParameterType::CurveRow)continue;const auto value=Curve(p,curve->bits,local_age);if(!value)continue;pulse=*value;}
    const Float3 rgb{output.rgba[0],output.rgba[1],output.rgba[2]};
    if(!Finite(rgb)||rgb.x<0||rgb.y<0||rgb.z<0||!std::isfinite(output.hdr)||output.hdr<=0||!std::isfinite(output.rgba[3])||output.rgba[3]<0||output.rgba[3]>1)continue;
    const float intensity=output.hdr*output.rgba[3]*pulse*(*envelope);if(!std::isfinite(intensity)||intensity<=0)continue;
    const auto output_id=(static_cast<std::uint64_t>(output.motion)<<32)|output.gradient_row;
    auto stable=Mix(input.sequence^Mix(identity->effect_id)^Mix(source.stable_id)^Mix(output_id));if(stable==0)stable=1;
    result.push_back({stable,*center,metres,rgb,intensity,
        input.importance_override != 0 ? input.importance_override : effect.importance,input.quality});
   }
  }
 }
 return result;
}
}
