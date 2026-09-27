#include "vfx_typed_ballistic_commands.hpp"
#include <hs/core/render_snapshot.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <string_view>
namespace hs::runtime_detail
{
namespace
{
constexpr std::uint32_t Hash(std::string_view s){std::uint32_t h=2166136261u;for(unsigned char c:s){h^=c;h*=16777619u;}return h;}
constexpr std::uint64_t Hash64(std::string_view s){std::uint64_t h=14695981039346656037ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}return h;}
constexpr auto kEntityPayload=Hash("EntityAttachmentPayload");
constexpr auto kProjectilePathPayload=Hash("ProjectilePathPayload");
constexpr auto kMultishotRetargetEffect=Hash64("particle.upgrade.retarget");
constexpr auto kRicochetBleedExtendEffect=Hash64("particle.upgrade.ricochet.bleed_extend");
std::uint32_t Mix(std::uint32_t v){v^=v>>16;v*=0x7feb352du;v^=v>>15;v*=0x846ca68bu;return v^(v>>16);}
float Random(std::uint32_t seed,std::uint32_t lane){return static_cast<float>(Mix(seed^Mix(lane+0x9e3779b9u))>>8)/16777216.0f;}
bool Finite(Float3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
template<class T> bool Range(VfxRange r,const std::vector<T>&v){return r.first<=v.size()&&r.count<=v.size()-r.first;}
Float3 Normalize(Float3 v){const float length=std::hypot(v.x,v.y,v.z);return length>.0001f?Float3{v.x/length,v.y/length,v.z/length}:Float3{0,1,0};}
Float3 Cross(Float3 a,Float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
struct Recipe{std::uint32_t shape;VfxMotionKind motion;std::uint8_t mesh;float min_size,max_size,min_speed,max_speed,gravity,cone,min_spin,max_spin;};
constexpr Recipe recipes[]{
 {Hash("small_shards"),VfxMotionKind::BriefRadialResponse,4,.035f,.065f,.55f,1,-3,75,3,7},
 {Hash("debris_shard_mesh"),VfxMotionKind::BallisticBurstGravitySpin,3,.06f,.12f,1.5f,2.6f,-9.81f,75,5,11},
 {Hash("shard_mesh"),VfxMotionKind::ShortBallisticFan,2,.04f,.09f,1.4f,2.2f,-5,55,4,9},
 {Hash("small_shard_mesh"),VfxMotionKind::ShardsLiftFall,4,.025f,.05f,2,2,-8,30,3,7},
 {Hash("small_shard_mesh"),VfxMotionKind::ShortForwardShardFan,4,.035f,.065f,2.4f,2.4f,-5,70,4,8},
 {Hash("needle_shard_mesh"),VfxMotionKind::ThinShardsForwardCone,7,.07f,.12f,4.2f,4.2f,-3,32,2,5}
};
bool ProfileTextures(const VfxProgramData &p,const VfxOutputRecord &output)
{
 if(!Range(output.textures,p.texture_bindings))return false;
 for(const auto &binding:std::span(p.texture_bindings).subspan(output.textures.first,output.textures.count))
 {
  // Mesh profile always supplies these shared LUTs, even with no authored detail texture.
  if(binding.role!=Hash("profile.global"))return false;
  const auto resource=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),[&](const auto &r){return r.slot==binding.catalog_slot;});
  if(resource==p.texture_resources.end()||!Range(resource->asset_path_bytes,p.strings))return false;
  const std::string_view path(reinterpret_cast<const char*>(p.strings.data())+resource->asset_path_bytes.first,resource->asset_path_bytes.count);
  if(path!="Content/Textures/VFX/vfx_gradient_lut.dds"&&path!="Content/Textures/VFX/vfx_curve_lut.dds")return false;
 }
 return true;
}
std::uint64_t StatusEffectId(std::uint8_t kind)
{
 switch(static_cast<PersistentVfxKind>(kind))
 {
 case PersistentVfxKind::PlayerBowDraw:return Hash64("particle.player.bow_draw");
 case PersistentVfxKind::ChargedFullReady:return Hash64("particle.upgrade.charged.full_ready");
 case PersistentVfxKind::EmpoweredReady:return Hash64("particle.upgrade.empowered_ready");
 case PersistentVfxKind::MultishotRetarget:return kMultishotRetargetEffect;
 case PersistentVfxKind::RicochetBleedExtend:return kRicochetBleedExtendEffect;
 default:return 0;
 }
}
bool UniqueIdentity(const VfxProgramData &p,VfxEffectHandle handle,std::uint64_t id)
{
 return std::count_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto &e){return e.handle==handle&&e.effect_id==id;})==1&&
        std::count_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto &e){return e.handle==handle;})==1&&
        std::count_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto &e){return e.effect_id==id;})==1;
}
bool StatusTextures(const VfxProgramData &p,const VfxOutputRecord &output)
{
 if(!Range(output.textures,p.texture_bindings)||output.textures.count!=2)return false;
 bool gradient=false,curve=false;
 for(const auto &binding:std::span(p.texture_bindings).subspan(output.textures.first,output.textures.count))
 {
  if(binding.role!=Hash("profile.global")||binding.min_quality!=0||binding.selection!=0||
     binding.slices.count!=0||!std::isfinite(binding.strength)||binding.strength!=1.0f)return false;
  const auto resource=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),[&](const auto &r){return r.slot==binding.catalog_slot;});
  if(resource==p.texture_resources.end()||!Range(resource->asset_path_bytes,p.strings))return false;
  const std::string_view path(reinterpret_cast<const char*>(p.strings.data())+resource->asset_path_bytes.first,resource->asset_path_bytes.count);
  if(path=="Content/Textures/VFX/vfx_gradient_lut.dds"&&!gradient)gradient=true;
  else if(path=="Content/Textures/VFX/vfx_curve_lut.dds"&&!curve)curve=true;
  else return false;
 }
 return gradient&&curve;
}
struct Params{std::uint32_t count{};float gravity{},bounce{},drag{.8f},speed{-1},cone{};};
std::optional<Params> Parameters(const VfxProgramData &p,const VfxSourceRecord &s,const Recipe &r)
{
 if(!Range(s.parameters,p.parameters))return {};Params result;result.gravity=r.gravity;result.cone=r.cone;
 std::uint32_t seen{};
 for(const auto &v:std::span(p.parameters).subspan(s.parameters.first,s.parameters.count))
 {
  std::uint32_t bit{};float *target=nullptr;
  if(v.key==Hash("count")){bit=1;if(v.type!=VfxParameterType::Int||v.bits==0||v.bits>1024)return {};result.count=v.bits;}
  else
  {
   if(v.key==Hash("gravity")){bit=2;target=&result.gravity;}
   else if(v.key==Hash("bounce")){bit=4;target=&result.bounce;}
   else if(v.key==Hash("drag")){bit=8;target=&result.drag;}
   else if(v.key==Hash("speed")){bit=16;target=&result.speed;}
   else if(v.key==Hash("cone_deg")){bit=32;target=&result.cone;}
   else return {};
   if(v.type!=VfxParameterType::Float)return {};*target=std::bit_cast<float>(v.bits);if(!std::isfinite(*target))return {};
  }
  if(seen&bit)return {};seen|=bit;
 }
 if(!(seen&1)||result.bounce<0||result.bounce>1||result.drag<0||result.cone<0||result.cone>180||
    ((seen&16)&&result.speed<=0))return {};return result;
}
}
std::vector<ParticleSpawnCommand> BuildVfxTypedBallisticCommands(const VfxProgramData &p,std::span<const VfxEventInput> inputs)
{
 std::vector<ParticleSpawnCommand> result;
 for(const auto &input:inputs)
 {
  if(input.effect_handle==0||input.effect_handle>p.effects.size()||static_cast<std::uint32_t>(input.quality)>2)continue;
  const auto &effect=p.effects[input.effect_handle-1];
  if(effect.handle!=input.effect_handle||effect.input_mode!=0||effect.timing_kind!=0||!std::isfinite(effect.seconds)||effect.seconds<=0||!Range(effect.sources,p.sources))continue;
  const auto identity=std::find_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto &e){return e.handle==effect.handle;});
  if(identity==p.effect_lookup.end()||identity->effect_id==0)continue;
  Float3 center{},normal{0,1,0};float scale=1,radius=0;bool projectile_geometry=false;
  if(const auto *point=std::get_if<VfxPointPayload>(&input.payload))
  {if(effect.payload_kind!=Hash("PointEventPayload"))continue;center=point->position;normal=point->normal;scale=point->authored_scale;}
  else if(std::holds_alternative<VfxContextPayload>(input.payload))
  {if(effect.payload_kind!=Hash("PresentationContextPayload"))continue;center={input.world_transform[12],input.world_transform[13],input.world_transform[14]};}
  else if(const auto *projectile=std::get_if<VfxProjectilePayload>(&input.payload))
  {
   if(effect.payload_kind!=Hash("ProjectilePayload")||!Finite(projectile->velocity)||
      !std::isfinite(projectile->hitbox_radius)||projectile->hitbox_radius<=0||
      std::hypot(projectile->velocity.x,projectile->velocity.y,projectile->velocity.z)<=.0001f)continue;
   center={input.world_transform[12],input.world_transform[13],input.world_transform[14]};
   normal=projectile->velocity;projectile_geometry=true;
  }
  else if(const auto *circle=std::get_if<VfxCirclePayload>(&input.payload))
  {if(effect.payload_kind!=Hash("CircleAreaPayload")||circle->inner_radius!=0||!std::isfinite(circle->radius)||circle->radius<=0)continue;center=circle->center;radius=circle->radius;}
  else continue;
  if(!Finite(center)||!Finite(normal)||!std::isfinite(std::hypot(normal.x,normal.y,normal.z))||!std::isfinite(scale)||scale<=0)continue;
  for(std::uint32_t si=effect.sources.first;si<effect.sources.first+effect.sources.count;++si)
  {
   const auto &source=p.sources[si];
   if(source.effect!=effect.handle||source.type!=VfxSourceType::BallisticCollision||source.knot_count!=2||
      !std::isfinite(source.knots[0])||!std::isfinite(source.knots[1])||source.knots[0]<0||source.knots[1]>1||source.knots[1]<=source.knots[0]||!Range(source.outputs,p.outputs))continue;
   const double delay_ticks=static_cast<double>(effect.seconds)*source.knots[0]*60.0;
   const double floor_delay=std::floor(delay_ticks),ceil_delay=std::ceil(delay_ticks);
   if(!std::isfinite(delay_ticks)||ceil_delay>=static_cast<double>(std::numeric_limits<Tick>::max()-input.event_tick))continue;
   const float lifetime=effect.seconds*(source.knots[1]-source.knots[0]);if(!std::isfinite(lifetime)||lifetime<=0)continue;
   const auto source_seed=Mix(input.stable_seed^Mix(static_cast<std::uint32_t>(identity->effect_id))^
       Mix(static_cast<std::uint32_t>(identity->effect_id>>32))^Mix(source.stable_id));
   for(const auto &output:std::span(p.outputs).subspan(source.outputs.first,source.outputs.count))
   {
    const auto recipe=std::find_if(std::begin(recipes),std::end(recipes),[&](const auto &r){return r.shape==output.shape&&r.motion==output.motion;});
    if(recipe==std::end(recipes)||output.source!=si||output.profile!=VfxOutputProfile::MeshEmissiveOit||
       output.min_quality>static_cast<std::uint32_t>(input.quality)||!Range(output.parameters,p.parameters)||output.parameters.count!=0||
       !ProfileTextures(p,output)||!std::isfinite(output.hdr)||output.hdr<=0||
       !Finite({output.rgba[0],output.rgba[1],output.rgba[2]})||!std::isfinite(output.rgba[3])||output.rgba[3]<0||output.rgba[3]>1)continue;
    const bool forward=recipe->motion==VfxMotionKind::ShortForwardShardFan||recipe->motion==VfxMotionKind::ThinShardsForwardCone;
    if(forward!=projectile_geometry)continue;
    if(forward&&identity->effect_id!=(recipe->motion==VfxMotionKind::ShortForwardShardFan?
        Hash64("particle.enemy.ranged.impact"):Hash64("particle.skill.charged_shot.impact")))continue;
    const auto params=Parameters(p,source,*recipe);if(!params)continue;
    const Float3 axis=(recipe->motion==VfxMotionKind::ShortBallisticFan||forward)?Normalize(normal):Float3{0,1,0};
    const Float3 right=Normalize(Cross(std::abs(axis.y)<.9f?Float3{0,1,0}:Float3{1,0,0},axis));
    const Float3 up=Cross(axis,right);
    for(std::uint32_t i=0;i<params->count;++i)
    {
     ParticleSpawnCommand cmd;cmd.seed=Mix(source_seed^Mix(i+1));cmd.sequence=input.sequence;cmd.count=1;
     cmd.authored_ballistic=true;cmd.renderer=VfxRenderer::Mesh;cmd.primitive=VfxPrimitive::Shard;cmd.mesh_index=recipe->mesh;cmd.facing=ParticleFacing::Velocity;
     cmd.tick=input.event_tick+static_cast<Tick>(ceil_delay);cmd.authored_birth_tick=input.event_tick+static_cast<Tick>(floor_delay);
     cmd.authored_birth_fraction=static_cast<float>(delay_ticks-floor_delay);cmd.lifetime_min=cmd.lifetime_max=lifetime;
     cmd.position=center;
     if(radius>0){const float angle=Random(cmd.seed,0)*2*std::numbers::pi_v<float>,distance=std::sqrt(Random(cmd.seed,1))*radius;
       cmd.position.x+=std::cos(angle)*distance;cmd.position.z+=std::sin(angle)*distance;cmd.position.y+=.1f;}
     const float size=std::lerp(recipe->min_size,recipe->max_size,Random(cmd.seed,2))*scale;
     const float azimuth=Random(cmd.seed,3)*2*std::numbers::pi_v<float>;
     const float cosine=std::lerp(1.0f,std::cos(params->cone*std::numbers::pi_v<float>/180),Random(cmd.seed,4));
     const float sine=std::sqrt(std::max(0.0f,1-cosine*cosine));
     const float speed=params->speed>0?params->speed:std::lerp(recipe->min_speed,recipe->max_speed,Random(cmd.seed,5));
     cmd.authored_initial_velocity={(axis.x*cosine+(right.x*std::cos(azimuth)+up.x*std::sin(azimuth))*sine)*speed,
       (axis.y*cosine+(right.y*std::cos(azimuth)+up.y*std::sin(azimuth))*sine)*speed,
       (axis.z*cosine+(right.z*std::cos(azimuth)+up.z*std::sin(azimuth))*sine)*speed};
     cmd.direction=Normalize(cmd.authored_initial_velocity);cmd.gravity=params->gravity;cmd.authored_drag=params->drag;cmd.authored_bounce=params->bounce;
     cmd.authored_ground_y=0;cmd.authored_collision_radius=size*.5f;cmd.authored_hdr=output.hdr;cmd.authored_gradient_row=output.gradient_row;
     cmd.start_size_min=cmd.start_size_max=cmd.end_size_min=cmd.end_size_max=size;
     cmd.start_color={output.rgba[0],output.rgba[1],output.rgba[2],output.rgba[3]};cmd.end_color=cmd.start_color;cmd.end_color.w=0;
     cmd.rotation_min=cmd.rotation_max=Random(cmd.seed,6)*2*std::numbers::pi_v<float>;
     cmd.angular_velocity_min=cmd.angular_velocity_max=std::lerp(recipe->min_spin,recipe->max_spin,Random(cmd.seed,7))*(Random(cmd.seed,8)<.5f?-1.0f:1.0f);
     if(Finite(cmd.position)&&Finite(cmd.authored_initial_velocity)&&std::isfinite(size)&&size>0)result.push_back(cmd);
    }
   }
  }
 }
 return result;
}

std::vector<ParticleSpawnCommand> BuildVfxPersistentStatusShardCommands(
    const VfxProgramData &p,std::span<const VfxPersistentInput> inputs,Tick current_tick)
{
 std::vector<ParticleSpawnCommand> result;
 for(const auto &input:inputs)
 {
  const auto effect_id=StatusEffectId(input.source_visual_kind);
  const auto visual_kind=static_cast<PersistentVfxKind>(input.source_visual_kind);
  const bool retarget=visual_kind==PersistentVfxKind::MultishotRetarget;
  const bool bleed_extend=visual_kind==PersistentVfxKind::RicochetBleedExtend;
  if(effect_id==0||input.stable_id==0||input.effect_handle==0||input.effect_handle>p.effects.size()||
     static_cast<std::uint32_t>(input.quality)<1||static_cast<std::uint32_t>(input.quality)>2||
     !UniqueIdentity(p,input.effect_handle,effect_id)||
     !std::isfinite(input.source_duration_seconds)||input.source_duration_seconds<=0||
     !std::isfinite(input.elapsed_seconds)||input.elapsed_seconds<0||
     !std::isfinite(input.normalized_age)||input.normalized_age<0||input.normalized_age>=1)continue;
  const auto *entity=std::get_if<VfxEntityPayload>(&input.payload);
  const auto *path=std::get_if<VfxProjectilePathPayload>(&input.payload);
  if(retarget)
  {
   if(!path||!Finite(path->current_position)||!Finite(path->previous_position)||
      !Finite(path->velocity)||!std::isfinite(path->projectile_radius)||path->projectile_radius<=0||
      !std::isfinite(path->lifetime01)||path->lifetime01!=input.normalized_age)continue;
  }
  else if(!entity||entity->render_instance_id==0||!std::isfinite(entity->footprint_radius)||
          entity->footprint_radius<=0||!std::isfinite(entity->lifetime01)||
          entity->lifetime01!=input.normalized_age||!std::isfinite(entity->health_fraction)||
          entity->health_fraction<0||entity->health_fraction>1)continue;
  const auto &effect=p.effects[input.effect_handle-1];
  const bool fixed=static_cast<PersistentVfxKind>(input.source_visual_kind)==PersistentVfxKind::ChargedFullReady||
                   retarget||bleed_extend;
  const auto expected_payload=retarget?kProjectilePathPayload:kEntityPayload;
  if(effect.handle!=input.effect_handle||effect.input_mode!=1||
     effect.payload_kind!=expected_payload||effect.timing_kind!=(fixed?0u:1u)||
     (fixed&&(!std::isfinite(effect.seconds)||effect.seconds!=input.source_duration_seconds))||
     !Range(effect.sources,p.sources))continue;
  const Float3 center=retarget?path->current_position:
      Float3{input.current_transform[12],input.current_transform[13],input.current_transform[14]};
  if(!Finite(center))continue;
  for(std::size_t si=effect.sources.first;si<static_cast<std::size_t>(effect.sources.first)+effect.sources.count;++si)
  {
   const auto &source=p.sources[si];
   if(source.effect!=effect.handle||source.stable_id!=Hash("status_shards")||source.authoritative!=0||
      source.type!=VfxSourceType::BallisticCollision||source.knot_count!=2||source.knots[0]!=0||
      !std::isfinite(source.knots[1])||source.knots[1]<=0||source.knots[1]>1||
      input.normalized_age>=source.knots[1]||!Range(source.outputs,p.outputs)||source.outputs.count!=1||
      !Range(source.parameters,p.parameters)||source.parameters.count!=1)continue;
   const auto &output=p.outputs[source.outputs.first];
   if(output.source!=si||output.profile!=VfxOutputProfile::MeshEmissiveOit||
      output.shape!=Hash("small_shards")||output.shape_domain!=Hash("sprite")||
      output.shape_scale_rule!=Hash("component_size")||output.shape_component_kind!=Hash("particle")||
      output.coverage_type!=Hash("analytic")||output.coverage_ref!=Hash("small_shards")||
      output.motion!=VfxMotionKind::BriefRadialResponse||output.motion_rate_hz!=0||
      output.motion_amplitude!=0||output.motion_inset_fraction!=0||output.min_quality!=1||
      !Range(output.parameters,p.parameters)||output.parameters.count!=0||!StatusTextures(p,output)||
      !std::isfinite(output.hdr)||output.hdr<=0||
      !Finite({output.rgba[0],output.rgba[1],output.rgba[2]})||
      !std::isfinite(output.rgba[3])||output.rgba[3]<0||output.rgba[3]>1)continue;
   const auto recipe=std::find_if(std::begin(recipes),std::end(recipes),[&](const auto &r){
       return r.shape==output.shape&&r.motion==output.motion;});
   if(recipe==std::end(recipes))continue;
   const auto params=Parameters(p,source,*recipe);
   if(!params||params->count!=5)continue;
   const float lifetime=input.source_duration_seconds*source.knots[1];
   if(!std::isfinite(lifetime)||lifetime<=0)continue;
   const auto source_seed=Mix(input.stable_seed^Mix(static_cast<std::uint32_t>(effect_id))^
       Mix(static_cast<std::uint32_t>(effect_id>>32))^Mix(source.stable_id));
   const Float3 axis{0,1,0},right{0,0,1},up{1,0,0};
   for(std::uint32_t i=0;i<params->count;++i)
   {
    ParticleSpawnCommand cmd;cmd.seed=Mix(source_seed^Mix(i+1));cmd.count=1;
    cmd.authored_ballistic=true;cmd.renderer=VfxRenderer::Mesh;cmd.primitive=VfxPrimitive::Shard;
    cmd.mesh_index=recipe->mesh;cmd.facing=ParticleFacing::Velocity;
    cmd.tick=current_tick;cmd.authored_birth_tick=current_tick;cmd.authored_birth_fraction=0;
    cmd.lifetime_min=cmd.lifetime_max=lifetime;cmd.position=center;
    const float size=std::lerp(recipe->min_size,recipe->max_size,Random(cmd.seed,2));
    const float azimuth=Random(cmd.seed,3)*2*std::numbers::pi_v<float>;
    const float cosine=std::lerp(1.0f,std::cos(params->cone*std::numbers::pi_v<float>/180),Random(cmd.seed,4));
    const float sine=std::sqrt(std::max(0.0f,1-cosine*cosine));
    const float speed=params->speed>0?params->speed:std::lerp(recipe->min_speed,recipe->max_speed,Random(cmd.seed,5));
    cmd.authored_initial_velocity={(axis.x*cosine+(right.x*std::cos(azimuth)+up.x*std::sin(azimuth))*sine)*speed,
      (axis.y*cosine+(right.y*std::cos(azimuth)+up.y*std::sin(azimuth))*sine)*speed,
      (axis.z*cosine+(right.z*std::cos(azimuth)+up.z*std::sin(azimuth))*sine)*speed};
    cmd.direction=Normalize(cmd.authored_initial_velocity);cmd.gravity=params->gravity;
    cmd.authored_drag=params->drag;cmd.authored_bounce=params->bounce;
    cmd.authored_ground_y=0;cmd.authored_collision_radius=size*.5f;
    cmd.authored_hdr=output.hdr;cmd.authored_gradient_row=output.gradient_row;
    cmd.start_size_min=cmd.start_size_max=cmd.end_size_min=cmd.end_size_max=size;
    cmd.start_color={output.rgba[0],output.rgba[1],output.rgba[2],output.rgba[3]};
    cmd.end_color=cmd.start_color;cmd.end_color.w=0;
    cmd.rotation_min=cmd.rotation_max=Random(cmd.seed,6)*2*std::numbers::pi_v<float>;
    cmd.angular_velocity_min=cmd.angular_velocity_max=
        std::lerp(recipe->min_spin,recipe->max_spin,Random(cmd.seed,7))*(Random(cmd.seed,8)<.5f?-1.0f:1.0f);
    if(Finite(cmd.authored_initial_velocity)&&std::isfinite(size)&&size>0)result.push_back(cmd);
   }
  }
 }
 return result;
}
}
