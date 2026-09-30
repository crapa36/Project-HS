#include "vfx_typed_distortion_commands.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>
namespace hs::runtime_detail
{
namespace
{
constexpr std::uint32_t Hash(std::string_view s){std::uint32_t h=2166136261u;for(unsigned char c:s){h^=c;h*=16777619u;}return h;}
std::uint64_t Mix(std::uint64_t v){v^=v>>30;v*=0xbf58476d1ce4e5b9ull;v^=v>>27;v*=0x94d049bb133111ebull;return v^(v>>31);}
bool Finite(Float3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
template<class T>bool Range(VfxRange r,const std::vector<T>&v){return r.first<=v.size()&&r.count<=v.size()-r.first;}
struct Recipe{std::uint32_t shape;VfxMotionKind motion;VfxDistortionShape kind;float radius;};
constexpr Recipe recipes[]{
 {Hash("screen_space_radial_normal"),VfxMotionKind::SingleOutwardRefractivePulse,VfxDistortionShape::RadialRing,.8f},
 {Hash("radial_shock"),VfxMotionKind::QuickHeatShock,VfxDistortionShape::HeatShock,.75f},
 {Hash("radial_shock"),VfxMotionKind::ShortRefractiveShock,VfxDistortionShape::ShortShock,.75f},
 {Hash("lance_heat_envelope"),VfxMotionKind::TightHeadDistortion,VfxDistortionShape::HeadEnvelope,.35f},
 {Hash("small_shock_disc"),VfxMotionKind::OneTightPulse,VfxDistortionShape::TightDisc,.7f}
};
struct Anchor{Float3 position{},direction{};float scale{1},circle_radius{},inner_radius{},half_angle_degrees{},gap_half_width_degrees{};std::vector<float> gap_angles_degrees;bool projectile{},cone{},ring_gaps{};};
std::optional<Anchor> EventAnchor(const VfxEventInput &in,std::uint32_t schema)
{
 Anchor a;
 if(const auto*v=std::get_if<VfxPointPayload>(&in.payload)){if(schema!=Hash("PointEventPayload"))return {};a.position=v->position;a.direction=v->direction;a.scale=v->authored_scale;}
 else if(const auto*v=std::get_if<VfxContextPayload>(&in.payload)){if(schema!=Hash("PresentationContextPayload"))return {};a.position={in.world_transform[12],in.world_transform[13],in.world_transform[14]};a.direction=v->direction;}
 else if(const auto*v=std::get_if<VfxCirclePayload>(&in.payload)){if(schema!=Hash("CircleAreaPayload")||v->inner_radius!=0||!std::isfinite(v->radius)||v->radius<=0)return {};a.position=v->center;a.direction=v->direction;a.circle_radius=v->radius;}
 else if(const auto*v=std::get_if<VfxConePayload>(&in.payload)){
  if(schema!=Hash("ConePayload")||!Finite(v->origin)||!Finite(v->direction)||!std::isfinite(v->range)||v->range<=0||
     !std::isfinite(v->half_angle_degrees)||v->half_angle_degrees<=0||v->half_angle_degrees>180||
     std::hypot(v->direction.x,v->direction.z)<=.0001f)return {};
  a.position=v->origin;a.direction=v->direction;a.circle_radius=v->range;a.half_angle_degrees=v->half_angle_degrees;a.cone=true;
 }
 else if(const auto*v=std::get_if<VfxRingGapsPayload>(&in.payload)){
  if(schema!=Hash("RingWithGapsPayload")||!Finite(v->center)||!std::isfinite(v->inner_radius)||
     !std::isfinite(v->outer_radius)||v->inner_radius<0||v->outer_radius<=v->inner_radius||
     !std::isfinite(v->gap_half_width_degrees)||v->gap_half_width_degrees<0||v->gap_half_width_degrees>180||
     (!v->gap_angles_degrees.empty()&&v->gap_half_width_degrees==0)||
     std::any_of(v->gap_angles_degrees.begin(),v->gap_angles_degrees.end(),[](float angle){return !std::isfinite(angle);}))return {};
  a.position=v->center;a.circle_radius=v->outer_radius;a.inner_radius=v->inner_radius;
  a.gap_half_width_degrees=v->gap_half_width_degrees;a.gap_angles_degrees=v->gap_angles_degrees;a.ring_gaps=true;
 }
 else if(const auto*v=std::get_if<VfxProjectilePayload>(&in.payload)){if(schema!=Hash("ProjectilePayload")||!std::isfinite(v->hitbox_radius)||v->hitbox_radius<=0)return {};a.position={in.world_transform[12],in.world_transform[13],in.world_transform[14]};a.direction=v->velocity;a.projectile=true;}
 else return {};
 if(!Finite(a.position)||!Finite(a.direction)||!std::isfinite(a.scale)||a.scale<=0)return {};
 const float length=std::hypot(a.direction.x,a.direction.y,a.direction.z);if(!std::isfinite(length)||(a.projectile&&length<=.0001f))return {};
 a.direction=length>.0001f?Float3{a.direction.x/length,a.direction.y/length,a.direction.z/length}:Float3{0,0,1};return a;
}
bool Textures(const VfxProgramData&p,const VfxOutputRecord&o)
{
 if(!Range(o.textures,p.texture_bindings))return false;
 for(const auto&b:std::span(p.texture_bindings).subspan(o.textures.first,o.textures.count))
 {
  if(b.role!=Hash("profile.global")&&b.role!=Hash("profile.authored"))return false;
  const auto r=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),[&](const auto&v){return v.slot==b.catalog_slot;});
  if(r==p.texture_resources.end()||!Range(r->asset_path_bytes,p.strings))return false;
  const std::string_view path(reinterpret_cast<const char*>(p.strings.data())+r->asset_path_bytes.first,r->asset_path_bytes.count);
  if(b.role==Hash("profile.global")){if(path!="Content/Textures/VFX/vfx_stbn_scalar.dds")return false;}
  else if(path!="Content/Textures/VFX/vfx_flow_curl_2d.dds"&&path!="Content/Textures/VFX/vfx_noise_basis_2d.dds")return false;
 }
 return true;
}
bool Params(const VfxProgramData&p,const VfxOutputRecord&o,const Recipe&r,float&strength,float&radius)
{
 if(!Range(o.parameters,p.parameters))return false;const bool radial=r.kind==VfxDistortionShape::RadialRing;
 const bool explicit_radius=r.kind==VfxDistortionShape::HeadEnvelope||r.kind==VfxDistortionShape::TightDisc;
 if(o.parameters.count!=(radial||explicit_radius?2u:1u))return false;
 bool has_strength=false,has_radius=false,has_falloff=false;
 for(const auto&v:std::span(p.parameters).subspan(o.parameters.first,o.parameters.count))
 {
  if(v.key==Hash("strength")&&!has_strength&&v.type==VfxParameterType::Float){strength=std::bit_cast<float>(v.bits);has_strength=true;}
  else if(v.key==Hash("radius")&&explicit_radius&&!has_radius&&v.type==VfxParameterType::Float){radius=std::bit_cast<float>(v.bits);has_radius=true;}
  else if(v.key==Hash("falloff")&&radial&&!has_falloff&&v.type==VfxParameterType::Enum&&v.bits==Hash("ring"))has_falloff=true;
  else return false;
 }
 return has_strength&&std::isfinite(strength)&&strength>0&&std::isfinite(radius)&&radius>0&&(!explicit_radius||has_radius)&&(!radial||has_falloff);
}
std::optional<float> Envelope(const VfxSourceRecord&s,float age)
{
 if(s.knot_count!=2&&s.knot_count!=4)return {};
 for(std::uint32_t i=0;i<s.knot_count;++i)if(!std::isfinite(s.knots[i])||s.knots[i]<0||s.knots[i]>1||(i&&s.knots[i]<s.knots[i-1]))return {};
 const float end=s.knots[s.knot_count-1];if(end<=s.knots[0]||age<s.knots[0]||age>=end)return {};
 float alpha=1;if(s.knot_count==4){if(age<s.knots[1]&&s.knots[1]>s.knots[0])alpha=(age-s.knots[0])/(s.knots[1]-s.knots[0]);else if(age>s.knots[2]&&end>s.knots[2])alpha=(end-age)/(end-s.knots[2]);}return alpha;
}
void Append(const VfxProgramData&p,const VfxEffectRecord&e,const Anchor&a,float age,std::uint64_t owner,
            std::uint32_t seed,VfxQuality quality,std::uint32_t importance,bool persistent,std::vector<VfxDistortionInput>&result)
{
 if(!std::isfinite(age)||age<0||age>=1||static_cast<std::uint32_t>(quality)>2||quality==VfxQuality::Low||!Range(e.sources,p.sources))return;
 const auto identity=std::find_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto&v){return v.handle==e.handle;});if(identity==p.effect_lookup.end()||identity->effect_id==0)return;
 for(std::uint32_t si=e.sources.first;si<e.sources.first+e.sources.count;++si)
 {
  const auto&s=p.sources[si];if(s.effect!=e.handle||s.type!=VfxSourceType::Direct||!Range(s.parameters,p.parameters)||s.parameters.count!=0||!Range(s.outputs,p.outputs))continue;
  const auto envelope=Envelope(s,age);if(!envelope||*envelope<=0)continue;
  for(const auto&o:std::span(p.outputs).subspan(s.outputs.first,s.outputs.count))
  {
   const auto r=std::find_if(std::begin(recipes),std::end(recipes),[&](const auto&v){return v.shape==o.shape&&v.motion==o.motion;});
   if(r==std::end(recipes)||o.source!=si||o.profile!=VfxOutputProfile::Distortion||o.min_quality>static_cast<std::uint32_t>(quality)||
      persistent!=(r->kind==VfxDistortionShape::HeadEnvelope)||a.projectile!=(r->kind==VfxDistortionShape::HeadEnvelope||r->kind==VfxDistortionShape::TightDisc)||
      o.coverage_type!=Hash("analytic_mask")||o.coverage_ref!=r->shape||!Textures(p,o)||!std::isfinite(o.rgba[3])||o.rgba[3]<0||o.rgba[3]>1)continue;
   float radius=r->radius,strength{};if(!Params(p,o,*r,strength,radius))continue;
   if((a.cone||a.ring_gaps)&&r->kind!=VfxDistortionShape::ShortShock)continue;
   if(!a.projectile)radius=a.circle_radius>0?a.circle_radius:radius*a.scale;
   const float local_age=(age-s.knots[0])/(s.knots[s.knot_count-1]-s.knots[0]);
   if(!std::isfinite(radius)||radius<=0)continue;
   auto stable=Mix(owner^Mix(identity->effect_id)^Mix(s.stable_id)^Mix(static_cast<std::uint64_t>(o.motion)));if(stable==0)stable=1;
   VfxDistortionInput command{stable,a.position,radius,a.direction,strength,local_age,o.rgba[3]*(*envelope),
                              a.cone?VfxDistortionShape::ConeSector:a.ring_gaps?VfxDistortionShape::GappedAnnulus:r->kind,
                              quality,importance,seed};
   command.inner_radius=a.inner_radius;command.half_angle_degrees=a.half_angle_degrees;
   command.gap_half_width_degrees=a.gap_half_width_degrees;command.gap_angles_degrees=a.gap_angles_degrees;
   result.push_back(std::move(command));
  }
 }
}
}
std::vector<VfxDistortionInput> BuildVfxTypedDistortionCommands(const VfxProgramData&p,std::span<const VfxEventInput>events,std::span<const VfxPersistentInput>persistent,Tick current_tick)
{
 std::vector<VfxDistortionInput> result;
 for(const auto&in:events)
 {
  if(in.effect_handle==0||in.effect_handle>p.effects.size()||current_tick<in.event_tick)continue;const auto&e=p.effects[in.effect_handle-1];
  if(e.handle!=in.effect_handle||e.input_mode!=0||e.timing_kind!=0||!std::isfinite(e.seconds)||e.seconds<=0)continue;
  const auto a=EventAnchor(in,e.payload_kind);if(!a)continue;
  const auto start=in.geometry_owner_id?in.geometry_start_tick:in.event_tick;
  if(current_tick<start||(in.geometry_owner_id&&in.geometry_end_tick<=start))continue;
  const double duration=in.geometry_owner_id?static_cast<double>(in.geometry_end_tick-start):static_cast<double>(e.seconds)*60;
  const float age=static_cast<float>(static_cast<double>(current_tick-start)/duration);
  Append(p,e,*a,age,in.sequence,in.stable_seed,in.quality,in.importance_override?in.importance_override:e.importance,false,result);
 }
 for(const auto&in:persistent)
 {
  if(in.effect_handle==0||in.effect_handle>p.effects.size()||in.stable_id==0)continue;const auto&e=p.effects[in.effect_handle-1];
  const auto*projectile=std::get_if<VfxProjectilePayload>(&in.payload);
  if(!projectile||e.handle!=in.effect_handle||e.input_mode!=1||e.timing_kind!=1||e.payload_kind!=Hash("ProjectilePayload")||
     !Finite(projectile->velocity)||!std::isfinite(projectile->hitbox_radius)||projectile->hitbox_radius<=0||
     !std::isfinite(e.seconds)||e.seconds<0||!std::isfinite(in.elapsed_seconds)||in.elapsed_seconds<0||!std::isfinite(in.normalized_age)||in.normalized_age<0||in.normalized_age>=1)continue;
  Anchor a;a.position={in.current_transform[12],in.current_transform[13],in.current_transform[14]};a.projectile=true;
  const float speed=std::hypot(projectile->velocity.x,projectile->velocity.y,projectile->velocity.z);
  if(!Finite(a.position)||!std::isfinite(speed)||speed<=.0001f)continue;
  a.direction={projectile->velocity.x/speed,projectile->velocity.y/speed,projectile->velocity.z/speed};
  // Live projectiles have no predicted expiry. A zero owner age holds the cosmetic
  // source at its nominal midpoint when gameplay timing supplies no seconds.
  // Presence controls start/stop; no expiry or fade is forecast. Authored seconds,
  // when available, gate source birth before holding its cosmetic midpoint.
  const float phase=in.normalized_age>0?in.normalized_age:(e.seconds>0?std::min(in.elapsed_seconds/e.seconds,.5f):.5f);
  Append(p,e,a,phase,in.stable_id,in.stable_seed,in.quality,e.importance,true,result);
 }
 return result;
}
}
