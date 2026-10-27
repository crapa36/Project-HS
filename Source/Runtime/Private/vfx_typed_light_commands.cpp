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

}
std::vector<VfxLightInput> BuildVfxTypedLightCommands(const VfxImage &image,VfxEventView inputs,double current_tick)
{
 std::vector<VfxLightInput> result;
 for(const auto &input:inputs)
 {
  if(static_cast<std::uint32_t>(input.quality)>2 || !input.effect_handle || input.effect_handle>image.light_effect_ranges.size() || current_tick<input.event_tick)continue;
  const auto range=image.light_effect_ranges[input.effect_handle-1];
  for(const auto &r:image.light_recipes.subspan(range.first,range.count))
  {
   const auto center=Center(input,r.payload_kind);if(!center)continue;
   const auto start=input.geometry_owner_id?input.geometry_start_tick:input.event_tick;
   if(current_tick<start || (input.geometry_owner_id && input.geometry_end_tick<=start))continue;
   const double ticks=input.geometry_owner_id?static_cast<double>(input.geometry_end_tick-start):static_cast<double>(r.seconds)*60;
   const float age=static_cast<float>(static_cast<double>(current_tick-start)/ticks);
   if(!std::isfinite(age)||age<0||age>=1)continue;
   const float end=r.knots[r.knot_count-1];
   if(age<r.knots[0] || age>=end)continue;
   float envelope=1;
   if(r.knot_count==4)
   {
    if(age<r.knots[1]&&r.knots[1]>r.knots[0])envelope=(age-r.knots[0])/(r.knots[1]-r.knots[0]);
    else if(age>r.knots[2]&&end>r.knots[2])envelope=(end-age)/(end-r.knots[2]);
   }
   if(envelope<=0 || r.min_quality>static_cast<std::uint32_t>(input.quality))continue;
   const float local_age=(age-r.knots[0])/(end-r.knots[0]);
   float pulse{};
   if(r.hero){const float fade=1-std::clamp((local_age-.1f)/.9f,0.0f,1.0f);pulse=fade*fade;}
   else
   {
    const auto keys=image.program.curve_keys.subspan(r.curve_keys.first,r.curve_keys.count);
    pulse=keys.back().value;
    for(std::size_t k=1;k<keys.size();++k)if(local_age<=keys[k].time)
    {
     float t=std::clamp((local_age-keys[k-1].time)/(keys[k].time-keys[k-1].time),0.0f,1.0f);
     if(r.interpolation==1)t=t*t*(3-2*t);
     pulse=std::lerp(keys[k-1].value,keys[k].value,t);break;
    }
   }
   const float intensity=r.hdr*r.alpha*pulse*envelope;if(!std::isfinite(intensity)||intensity<=0)continue;
   auto stable=Mix(input.sequence^r.seed_xor);if(stable==0)stable=1;
   result.push_back({stable,*center,r.radius,r.rgb,intensity,input.importance_override?input.importance_override:r.importance,input.quality});
  }
 }
 return result;
}
}
