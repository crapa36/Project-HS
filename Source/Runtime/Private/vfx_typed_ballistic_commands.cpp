#include "vfx_typed_ballistic_commands.hpp"
#include "vfx_workspace_size.hpp"
#include <hs/render_data/render_snapshot.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace hs::runtime_detail
{
namespace
{
constexpr std::uint64_t Hash64(std::string_view s){std::uint64_t h=14695981039346656037ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}return h;}
constexpr std::uint32_t Hash(std::string_view s){std::uint32_t h=2166136261u;for(unsigned char c:s){h^=c;h*=16777619u;}return h;}
constexpr auto kMultishotRetargetEffect=Hash64("particle.upgrade.retarget");
constexpr auto kRicochetBleedExtendEffect=Hash64("particle.upgrade.ricochet.bleed_extend");
std::uint32_t Mix(std::uint32_t v){v^=v>>16;v*=0x7feb352du;v^=v>>15;v*=0x846ca68bu;return v^(v>>16);}
float Random(std::uint32_t seed,std::uint32_t lane){return static_cast<float>(Mix(seed^Mix(lane+0x9e3779b9u))>>8)/16777216.0f;}
bool Finite(Float3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
Float3 Normalize(Float3 v){const float length=std::hypot(v.x,v.y,v.z);return length>.0001f?Float3{v.x/length,v.y/length,v.z/length}:Float3{0,1,0};}
Float3 Cross(Float3 a,Float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
VfxRange EffectRecipes(const VfxImage &image,VfxEffectHandle handle)
{return handle&&handle<=image.ballistic_effect_ranges.size()?image.ballistic_effect_ranges[handle-1]:VfxRange{};}
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
std::uint32_t SourceSeed(const VfxBallisticRecipe &r,std::uint32_t seed)
{return Mix(seed^Mix(static_cast<std::uint32_t>(r.effect_id))^Mix(static_cast<std::uint32_t>(r.effect_id>>32))^Mix(r.source_stable_id));}
bool Emit(const VfxBallisticRecipe &r,std::uint32_t source_seed,std::uint32_t count,Tick tick,Tick birth_tick,float birth_fraction,
 Float3 center,Float3 axis,float scale,float radius,float lifetime,Sequence sequence,
 std::vector<ParticleSpawnCommand> &result,bool reserve_output=true)
{
 if(!count)return false;
 if(reserve_output)vfx_retained_size::ReserveAppend(result,count);
 const Float3 right=Normalize(Cross(std::abs(axis.y)<.9f?Float3{0,1,0}:Float3{1,0,0},axis));const Float3 up=Cross(axis,right);
 const float cone_cosine=std::cos(r.cone*std::numbers::pi_v<float>/180);
 const auto before=result.size();
 for(std::uint32_t i=0;i<count;++i)
 {
  ParticleSpawnCommand cmd;cmd.seed=Mix(source_seed^Mix(i+1));cmd.sequence=sequence;cmd.count=1;cmd.authored_ballistic=true;cmd.renderer=VfxRenderer::Mesh;
  cmd.primitive=VfxPrimitive::Shard;cmd.mesh_index=r.mesh_index;cmd.facing=ParticleFacing::Velocity;cmd.tick=tick;cmd.authored_birth_tick=birth_tick;
  cmd.authored_birth_fraction=birth_fraction;cmd.lifetime_min=cmd.lifetime_max=lifetime;cmd.position=center;
  if(radius>0){const float angle=Random(cmd.seed,0)*2*std::numbers::pi_v<float>,distance=std::sqrt(Random(cmd.seed,1))*radius;cmd.position.x+=std::cos(angle)*distance;cmd.position.z+=std::sin(angle)*distance;cmd.position.y+=.1f;}
  const float size=std::lerp(r.min_size,r.max_size,Random(cmd.seed,2))*scale,azimuth=Random(cmd.seed,3)*2*std::numbers::pi_v<float>;
  const float azimuth_cosine=std::cos(azimuth),azimuth_sine=std::sin(azimuth);
  const float cosine=std::lerp(1.0f,cone_cosine,Random(cmd.seed,4)),sine=std::sqrt(std::max(0.0f,1-cosine*cosine));
  const float speed=r.speed>0?r.speed:std::lerp(r.min_speed,r.max_speed,Random(cmd.seed,5));
  cmd.authored_initial_velocity={(axis.x*cosine+(right.x*azimuth_cosine+up.x*azimuth_sine)*sine)*speed,(axis.y*cosine+(right.y*azimuth_cosine+up.y*azimuth_sine)*sine)*speed,(axis.z*cosine+(right.z*azimuth_cosine+up.z*azimuth_sine)*sine)*speed};
  cmd.direction=Normalize(cmd.authored_initial_velocity);cmd.gravity=r.gravity;cmd.authored_drag=r.drag;cmd.authored_bounce=r.bounce;cmd.authored_ground_y=0;cmd.authored_collision_radius=size*.5f;
  cmd.authored_hdr=r.hdr;cmd.authored_gradient_row=r.gradient_row;cmd.start_size_min=cmd.start_size_max=cmd.end_size_min=cmd.end_size_max=size;
  cmd.start_color=r.color;cmd.end_color=cmd.start_color;cmd.end_color.w=0;
  cmd.rotation_min=cmd.rotation_max=Random(cmd.seed,6)*2*std::numbers::pi_v<float>;
  cmd.angular_velocity_min=cmd.angular_velocity_max=std::lerp(r.min_spin,r.max_spin,Random(cmd.seed,7))*(Random(cmd.seed,8)<.5f?-1.0f:1.0f);
  if(Finite(cmd.position)&&Finite(cmd.authored_initial_velocity)&&std::isfinite(size)&&size>0)result.push_back(cmd);
 }
 return result.size()!=before;
}
void AppendPersistentStatusShardCommands(const VfxImage &image,
 std::span<const VfxPersistentInput> inputs,std::span<const std::size_t> selected_indices,
 bool use_selection,Tick current_tick,std::vector<ParticleSpawnCommand> &result)
{
 const auto old_result_size=result.size();
 try
 {
  const auto append_owner=[&](const VfxPersistentInput &input)
  {
   const auto id=StatusEffectId(input.source_visual_kind);const auto kind=static_cast<PersistentVfxKind>(input.source_visual_kind);
   const bool retarget=kind==PersistentVfxKind::MultishotRetarget,bleed=kind==PersistentVfxKind::RicochetBleedExtend;
   if(!id||!input.stable_id||!input.effect_handle||static_cast<std::uint32_t>(input.quality)<1||static_cast<std::uint32_t>(input.quality)>2||!std::isfinite(input.source_duration_seconds)||input.source_duration_seconds<=0||!std::isfinite(input.elapsed_seconds)||input.elapsed_seconds<0||!std::isfinite(input.normalized_age)||input.normalized_age<0||input.normalized_age>=1)return;
   const auto *entity=std::get_if<VfxEntityPayload>(&input.payload);const auto *path=std::get_if<VfxProjectilePathPayload>(&input.payload);
   if(retarget){if(!path||!Finite(path->current_position)||!Finite(path->previous_position)||!Finite(path->velocity)||!std::isfinite(path->projectile_radius)||path->projectile_radius<=0||!std::isfinite(path->lifetime01)||path->lifetime01!=input.normalized_age)return;}
   else if(!entity||!entity->render_instance_id||!std::isfinite(entity->footprint_radius)||entity->footprint_radius<=0||!std::isfinite(entity->lifetime01)||entity->lifetime01!=input.normalized_age||!std::isfinite(entity->health_fraction)||entity->health_fraction<0||entity->health_fraction>1)return;
   const auto range=EffectRecipes(image,input.effect_handle);if(range.first>image.ballistic_recipes.size()||range.count>image.ballistic_recipes.size()-range.first)return;
   const Float3 center=retarget?path->current_position:Float3{input.current_transform[12],input.current_transform[13],input.current_transform[14]};if(!Finite(center))return;
   for(const auto &r:image.ballistic_recipes.subspan(range.first,range.count))
   {
    if(!r.persistent_status||r.effect_id!=id||r.min_quality>static_cast<std::uint32_t>(input.quality)||input.normalized_age>=r.knots[1])continue;
    const bool fixed=kind==PersistentVfxKind::ChargedFullReady||retarget||bleed;
    if(fixed&&r.seconds!=input.source_duration_seconds)continue;
    const float lifetime=fixed?r.seconds*r.knots[1]:input.source_duration_seconds*r.knots[1];if(!std::isfinite(lifetime)||lifetime<=0)continue;
    Emit(r,SourceSeed(r,input.stable_seed),r.count,current_tick,current_tick,0,center,{0,1,0},1,0,lifetime,0,result);
   }
  };
  if(use_selection)
  {
   for(const auto index:selected_indices)
   {
    if(index>=inputs.size())throw std::out_of_range("persistent VFX owner index is out of range");
    append_owner(inputs[index]);
   }
  }
  else for(const auto &input:inputs)append_owner(input);
 }
 catch(...)
 {
  result.resize(old_result_size);
  throw;
 }
}
}
std::vector<ParticleSpawnCommand> BuildVfxTypedBallisticCommands(const VfxImage &image,VfxEventView inputs)
{std::vector<ParticleSpawnCommand> result;AppendVfxTypedBallisticCommands(image,inputs,result);return result;}
void PrepareVfxTypedBallisticBursts(const VfxImage &image,VfxEventView inputs,
 std::vector<VfxPreparedBallisticBurst> &plans)
{
 const auto old_plan_size=plans.size();
 try
 {
  for(std::size_t input_index=0;input_index<inputs.size();++input_index)
  {
   const auto &input=inputs[input_index];
   if(input.effect_handle==0||static_cast<std::uint32_t>(input.quality)>2)continue;const auto range=EffectRecipes(image,input.effect_handle);
   if(range.first>image.ballistic_recipes.size()||range.count>image.ballistic_recipes.size()-range.first)continue;
   Float3 center{},normal{0,1,0};float scale=1,radius=0;bool projectile_geometry=false;std::uint32_t payload_kind{};
   if(const auto *point=std::get_if<VfxPointPayload>(&input.payload)){center=point->position;normal=point->normal;scale=point->authored_scale;payload_kind=Hash("PointEventPayload");}
   else if(std::holds_alternative<VfxContextPayload>(input.payload)){center={input.world_transform[12],input.world_transform[13],input.world_transform[14]};payload_kind=Hash("PresentationContextPayload");}
   else if(const auto *projectile=std::get_if<VfxProjectilePayload>(&input.payload)){if(!Finite(projectile->velocity)||!std::isfinite(projectile->hitbox_radius)||projectile->hitbox_radius<=0||std::hypot(projectile->velocity.x,projectile->velocity.y,projectile->velocity.z)<=.0001f)continue;center={input.world_transform[12],input.world_transform[13],input.world_transform[14]};normal=projectile->velocity;projectile_geometry=true;payload_kind=Hash("ProjectilePayload");}
   else if(const auto *circle=std::get_if<VfxCirclePayload>(&input.payload)){if(circle->inner_radius!=0||!std::isfinite(circle->radius)||circle->radius<=0)continue;center=circle->center;radius=circle->radius;payload_kind=Hash("CircleAreaPayload");}
   else continue;
   if(!Finite(center)||!Finite(normal)||!std::isfinite(std::hypot(normal.x,normal.y,normal.z))||!std::isfinite(scale)||scale<=0)continue;
   for(const auto &r:image.ballistic_recipes.subspan(range.first,range.count))
   {
    if(r.persistent_status||r.effect_handle!=input.effect_handle||r.min_quality>static_cast<std::uint32_t>(input.quality)||
       r.payload_kind!=payload_kind)continue;
    const bool forward=r.motion==VfxMotionKind::ShortForwardShardFan||r.motion==VfxMotionKind::ThinShardsForwardCone;if(forward!=projectile_geometry)continue;
    const double delay=static_cast<double>(r.seconds)*r.knots[0]*60.,floor_delay=std::floor(delay),ceil_delay=std::ceil(delay);
    if(!std::isfinite(delay)||ceil_delay>=static_cast<double>(std::numeric_limits<Tick>::max()-input.event_tick))continue;
    const Tick floor_tick=input.event_tick+static_cast<Tick>(floor_delay),tick=input.event_tick+static_cast<Tick>(ceil_delay);
    const auto source_seed=SourceSeed(r,input.stable_seed);const Float3 axis=(r.motion==VfxMotionKind::ShortBallisticFan||forward)?Normalize(normal):Float3{0,1,0};
    const bool cosmetic=r.source_index<image.program.sources.size()&&
      image.program.sources[r.source_index].effect==r.effect_handle&&
      image.program.sources[r.source_index].type==VfxSourceType::BallisticCollision&&
      image.program.sources[r.source_index].authoritative==0&&r.source_type==VfxSourceType::BallisticCollision;
    plans.push_back({&r,input_index,r.count,r.count,source_seed,r.importance,tick,floor_tick,center,axis,
      static_cast<float>(delay-floor_delay),scale,radius,r.seconds*(r.knots[1]-r.knots[0]),input.sequence,cosmetic});
   }
  }
 }
 catch(...)
 {
  plans.resize(old_plan_size);
  throw;
 }
}
void AppendVfxPreparedBallisticBursts(std::span<const VfxPreparedBallisticBurst> plans,
 std::vector<ParticleSpawnCommand> &result,std::vector<std::size_t> *emitted_event_indices)
{
 const auto old_result_size=result.size();
 const auto old_index_size=emitted_event_indices?emitted_event_indices->size():0;
 try
 {
  std::size_t aggregate{};
  for(const auto &plan:plans)
  {
   if(!plan.recipe||plan.count!=plan.recipe->count||
      plan.count>std::numeric_limits<std::uint32_t>::max()||plan.admitted_count>plan.count)
    throw std::invalid_argument("prepared ballistic burst has invalid recipe or admission count");
   if(plan.admitted_count>std::numeric_limits<std::size_t>::max()-aggregate)
    throw std::length_error("prepared ballistic burst count overflow");
   aggregate+=plan.admitted_count;
  }
  vfx_retained_size::ReserveAppend(result,aggregate);

  if(emitted_event_indices)
  {
   std::size_t event_groups{};
   for(std::size_t i=0;i<plans.size();)
   {
    const auto event_index=plans[i].event_index;
    bool admitted=false;
    do{admitted|=plans[i].admitted_count!=0;++i;}
    while(i<plans.size()&&plans[i].event_index==event_index);
    if(admitted)++event_groups;
   }
   vfx_retained_size::ReserveAppend(*emitted_event_indices,event_groups);
  }

  for(std::size_t i=0;i<plans.size();)
  {
   const auto event_index=plans[i].event_index;
   bool event_emitted=false;
   do
   {
    const auto &plan=plans[i++];
    if(plan.admitted_count==0)continue;
    event_emitted|=Emit(*plan.recipe,plan.source_seed,static_cast<std::uint32_t>(plan.admitted_count),plan.tick,plan.birth_tick,
      plan.birth_fraction,plan.center,plan.axis,plan.scale,plan.radius,plan.lifetime,plan.sequence,result,false);
   }
   while(i<plans.size()&&plans[i].event_index==event_index);
   if(event_emitted&&emitted_event_indices)emitted_event_indices->push_back(event_index);
  }
 }
 catch(...)
 {
  result.resize(old_result_size);
  if(emitted_event_indices)emitted_event_indices->resize(old_index_size);
  throw;
 }
}
void AppendVfxTypedBallisticCommands(const VfxImage &image,VfxEventView inputs,
 std::vector<ParticleSpawnCommand> &result,std::vector<std::size_t> *emitted_event_indices)
{
 std::vector<VfxPreparedBallisticBurst> plans;
 PrepareVfxTypedBallisticBursts(image,inputs,plans);
 AppendVfxPreparedBallisticBursts(plans,result,emitted_event_indices);
}
void BuildVfxTypedBallisticCommands(const VfxImage &image,VfxEventView inputs,std::vector<ParticleSpawnCommand> &result)
{AppendVfxTypedBallisticCommands(image,inputs,result);}
std::vector<ParticleSpawnCommand> BuildVfxPersistentStatusShardCommands(const VfxImage &image,std::span<const VfxPersistentInput> inputs,Tick current_tick)
{std::vector<ParticleSpawnCommand> result;BuildVfxPersistentStatusShardCommands(image,inputs,current_tick,result);return result;}
void BuildVfxPersistentStatusShardCommands(const VfxImage &image,std::span<const VfxPersistentInput> inputs,Tick current_tick,std::vector<ParticleSpawnCommand> &result)
{
 AppendPersistentStatusShardCommands(image,inputs,{},false,current_tick,result);
}
void AppendVfxSelectedPersistentStatusShardCommands(const VfxImage &image,
 std::span<const VfxPersistentInput> owners,std::span<const std::size_t> indices,
 Tick current_tick,std::vector<ParticleSpawnCommand> &output)
{AppendPersistentStatusShardCommands(image,owners,indices,true,current_tick,output);}
}
