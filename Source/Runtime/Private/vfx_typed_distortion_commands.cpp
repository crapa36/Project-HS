#include "vfx_typed_distortion_commands.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>
namespace hs::runtime_detail
{
namespace
{
constexpr std::uint32_t Hash(std::string_view s){std::uint32_t h=2166136261u;for(unsigned char c:s){h^=c;h*=16777619u;}return h;}
constexpr auto kPointPayload = Hash("PointEventPayload");
constexpr auto kContextPayload = Hash("PresentationContextPayload");
constexpr auto kCirclePayload = Hash("CircleAreaPayload");
constexpr auto kConePayload = Hash("ConePayload");
constexpr auto kRingGapsPayload = Hash("RingWithGapsPayload");
constexpr auto kProjectilePayload = Hash("ProjectilePayload");
std::uint64_t Mix(std::uint64_t v){v^=v>>30;v*=0xbf58476d1ce4e5b9ull;v^=v>>27;v*=0x94d049bb133111ebull;return v^(v>>31);}
bool Finite(Float3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
struct Anchor{Float3 position{},direction{};float scale{1},circle_radius{},inner_radius{},half_angle_degrees{},gap_half_width_degrees{};std::span<const float> gap_angles_degrees;bool projectile{},cone{},ring_gaps{};};
std::optional<Anchor> EventAnchor(const VfxEventInput &in,std::uint32_t schema)
{
 Anchor a;
 if(const auto*v=std::get_if<VfxPointPayload>(&in.payload)){if(schema!=kPointPayload)return {};a.position=v->position;a.direction=v->direction;a.scale=v->authored_scale;}
 else if(const auto*v=std::get_if<VfxContextPayload>(&in.payload)){if(schema!=kContextPayload)return {};a.position={in.world_transform[12],in.world_transform[13],in.world_transform[14]};a.direction=v->direction;}
 else if(const auto*v=std::get_if<VfxCirclePayload>(&in.payload)){if(schema!=kCirclePayload||v->inner_radius!=0||!std::isfinite(v->radius)||v->radius<=0)return {};a.position=v->center;a.direction=v->direction;a.circle_radius=v->radius;}
 else if(const auto*v=std::get_if<VfxConePayload>(&in.payload)){
  if(schema!=kConePayload||!Finite(v->origin)||!Finite(v->direction)||!std::isfinite(v->range)||v->range<=0||
     !std::isfinite(v->half_angle_degrees)||v->half_angle_degrees<=0||v->half_angle_degrees>180||
     std::hypot(v->direction.x,v->direction.z)<=.0001f)return {};
  a.position=v->origin;a.direction=v->direction;a.circle_radius=v->range;a.half_angle_degrees=v->half_angle_degrees;a.cone=true;
 }
 else if(const auto*v=std::get_if<VfxRingGapsPayload>(&in.payload)){
  if(schema!=kRingGapsPayload||!Finite(v->center)||!std::isfinite(v->inner_radius)||
     !std::isfinite(v->outer_radius)||v->inner_radius<0||v->outer_radius<=v->inner_radius||
     !std::isfinite(v->gap_half_width_degrees)||v->gap_half_width_degrees<0||v->gap_half_width_degrees>180||
     (!v->gap_angles_degrees.empty()&&v->gap_half_width_degrees==0)||
     std::any_of(v->gap_angles_degrees.begin(),v->gap_angles_degrees.end(),[](float angle){return !std::isfinite(angle);}))return {};
  a.position=v->center;a.circle_radius=v->outer_radius;a.inner_radius=v->inner_radius;
  a.gap_half_width_degrees=v->gap_half_width_degrees;a.gap_angles_degrees=v->gap_angles_degrees;a.ring_gaps=true;
 }
 else if(const auto*v=std::get_if<VfxProjectilePayload>(&in.payload)){if(schema!=kProjectilePayload||!std::isfinite(v->hitbox_radius)||v->hitbox_radius<=0)return {};a.position={in.world_transform[12],in.world_transform[13],in.world_transform[14]};a.direction=v->velocity;a.projectile=true;}
 else return {};
 if(!Finite(a.position)||!Finite(a.direction)||!std::isfinite(a.scale)||a.scale<=0)return {};
 const float length=std::hypot(a.direction.x,a.direction.y,a.direction.z);if(!std::isfinite(length)||(a.projectile&&length<=.0001f))return {};
 a.direction=length>.0001f?Float3{a.direction.x/length,a.direction.y/length,a.direction.z/length}:Float3{0,0,1};return a;
}
std::optional<float> Envelope(const VfxDistortionRecipe&r,float age)
{
 const auto &k=r.knots;const auto n=r.knot_count;const float end=k[n-1];if(age<k[0]||age>=end)return {};
 float alpha=1;if(n==4){if(age<k[1]&&k[1]>k[0])alpha=(age-k[0])/(k[1]-k[0]);else if(age>k[2]&&end>k[2])alpha=(end-age)/(end-k[2]);}return alpha;
}
void Append(const VfxDistortionRecipe&r,const Anchor&a,float age,std::uint64_t owner,
            std::uint32_t seed,VfxQuality quality,std::uint32_t importance,std::vector<VfxDistortionInput>&result)
{
 if(!std::isfinite(age)||age<0||age>=1||static_cast<std::uint32_t>(quality)>2||r.min_quality>static_cast<std::uint32_t>(quality)||
    (a.cone||a.ring_gaps)&&r.kind!=VfxDistortionShape::ShortShock)return;
 const auto envelope=Envelope(r,age);if(!envelope||*envelope<=0)return;
 const float radius=a.projectile?r.radius:(a.circle_radius>0?a.circle_radius:r.radius*a.scale);
 const float local_age=(age-r.knots[0])/(r.knots[r.knot_count-1]-r.knots[0]);if(!std::isfinite(radius)||radius<=0)return;
 auto stable=Mix(owner^Mix(r.effect_id)^Mix(r.source_stable_id)^Mix(static_cast<std::uint64_t>(r.motion)));if(!stable)stable=1;
 VfxDistortionInput command{stable,a.position,radius,a.direction,r.strength,local_age,r.alpha*(*envelope),
   a.cone?VfxDistortionShape::ConeSector:a.ring_gaps?VfxDistortionShape::GappedAnnulus:r.kind,quality,importance,seed};
 command.inner_radius=a.inner_radius;command.half_angle_degrees=a.half_angle_degrees;
 command.gap_half_width_degrees=a.gap_half_width_degrees;command.gap_angles_degrees.assign(a.gap_angles_degrees.begin(),a.gap_angles_degrees.end());result.push_back(std::move(command));
}
}
std::vector<VfxDistortionInput> BuildVfxTypedDistortionCommands(const VfxImage&image,VfxEventView events,std::span<const VfxPersistentInput>persistent,double current_tick)
{
 std::vector<VfxDistortionInput> result;
 AppendVfxTypedDistortionCommands(image, events, persistent, current_tick, result);
 return result;
}
void AppendVfxTypedDistortionCommands(const VfxImage&image,VfxEventView events,
 std::span<const VfxPersistentInput>persistent,double current_tick,std::vector<VfxDistortionInput>&result)
{
 const auto original_size=result.size();
 try
 {
 const auto run=[&](std::uint32_t handle,const Anchor&a,float age,std::uint64_t owner,std::uint32_t seed,VfxQuality quality,std::uint32_t importance,bool persistent_owner){
  if(!handle||handle>image.distortion_effect_ranges.size())return;const auto range=image.distortion_effect_ranges[handle-1];
  if(range.first>image.distortion_recipes.size()||range.count>image.distortion_recipes.size()-range.first)return;
  for(const auto&r:image.distortion_recipes.subspan(range.first,range.count))
   if(r.input_mode==(persistent_owner?1u:0u)&&r.timing_kind==(persistent_owner?1u:0u)&&
      persistent_owner==(r.kind==VfxDistortionShape::HeadEnvelope)&&
      a.projectile==(r.kind==VfxDistortionShape::HeadEnvelope||r.kind==VfxDistortionShape::TightDisc))
    Append(r,a,age,owner,seed,quality,importance,result);
 };
 for(const auto&in:events)
 {
  if(current_tick<in.event_tick)continue;
  if(in.effect_handle==0||in.effect_handle>image.distortion_effect_ranges.size())continue;
  const auto range=image.distortion_effect_ranges[in.effect_handle-1];if(range.first>=image.distortion_recipes.size()||!range.count)continue;
  const auto first=image.distortion_recipes[range.first];const auto anchor=EventAnchor(in,first.payload_kind);if(!anchor)continue;
  const auto start=in.geometry_owner_id?in.geometry_start_tick:in.event_tick;
  if(current_tick<start||(in.geometry_owner_id&&in.geometry_end_tick<=start))continue;
  const double duration=in.geometry_owner_id?static_cast<double>(in.geometry_end_tick-start):static_cast<double>(first.seconds)*60;
  const float age=static_cast<float>(static_cast<double>(current_tick-start)/duration);
  run(in.effect_handle,*anchor,age,in.sequence,in.stable_seed,in.quality,in.importance_override?in.importance_override:first.importance,false);
 }
 for(const auto&in:persistent)
 {
  if(in.effect_handle==0||in.effect_handle>image.distortion_effect_ranges.size()||in.stable_id==0)continue;
  const auto range=image.distortion_effect_ranges[in.effect_handle-1];if(range.first>=image.distortion_recipes.size()||!range.count)continue;const auto&r=image.distortion_recipes[range.first];
  const auto*projectile=std::get_if<VfxProjectilePayload>(&in.payload);
  if(!projectile||r.input_mode!=1||r.timing_kind!=1||r.payload_kind!=kProjectilePayload||
     !Finite(projectile->velocity)||!std::isfinite(projectile->hitbox_radius)||projectile->hitbox_radius<=0||
     !std::isfinite(r.seconds)||r.seconds<0||!std::isfinite(in.elapsed_seconds)||in.elapsed_seconds<0||!std::isfinite(in.normalized_age)||in.normalized_age<0||in.normalized_age>=1)continue;
  Anchor a;a.position={in.current_transform[12],in.current_transform[13],in.current_transform[14]};a.projectile=true;
  const float speed=std::hypot(projectile->velocity.x,projectile->velocity.y,projectile->velocity.z);
  if(!Finite(a.position)||!std::isfinite(speed)||speed<=.0001f)continue;
  a.direction={projectile->velocity.x/speed,projectile->velocity.y/speed,projectile->velocity.z/speed};
  // Live projectiles have no predicted expiry. A zero owner age holds the cosmetic
  // source at its nominal midpoint when gameplay timing supplies no seconds.
  // Presence controls start/stop; no expiry or fade is forecast. Authored seconds,
  // when available, gate source birth before holding its cosmetic midpoint.
  const float phase=in.normalized_age>0?in.normalized_age:(r.seconds>0?std::min(in.elapsed_seconds/r.seconds,.5f):.5f);
  run(in.effect_handle,a,phase,in.stable_id,in.stable_seed,in.quality,r.importance,true);
 }
 }
 catch(...){result.resize(original_size);throw;}
}
}
