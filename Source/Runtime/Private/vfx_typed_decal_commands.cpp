#include "vfx_typed_decal_commands.hpp"
#include <cmath>
#include <optional>
#include <string_view>
namespace hs::runtime_detail
{
namespace
{
constexpr std::uint32_t Hash(std::string_view s){std::uint32_t h=2166136261u;for(unsigned char c:s){h^=c;h*=16777619u;}return h;}
constexpr auto kPointPayload = Hash("PointEventPayload");
constexpr auto kContextPayload = Hash("PresentationContextPayload");
constexpr auto kCirclePayload = Hash("CircleAreaPayload");
std::uint64_t Mix(std::uint64_t v){v^=v>>30;v*=0xbf58476d1ce4e5b9ull;v^=v>>27;v*=0x94d049bb133111ebull;return v^(v>>31);}
bool Finite(Float3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}

struct Anchor{Float3 position{},direction{0,0,1};float radius{1.25f};};
std::optional<Anchor> GetAnchor(const VfxPayload&payload,const VfxTransform&transform,std::uint32_t schema)
{
 Anchor a;
 if(const auto*v=std::get_if<VfxPointPayload>(&payload)){if(schema!=kPointPayload||!std::isfinite(v->authored_scale)||v->authored_scale<=0)return {};a.position=v->position;a.direction=v->direction;a.radius*=v->authored_scale;}
 else if(const auto*v=std::get_if<VfxContextPayload>(&payload)){if(schema!=kContextPayload)return {};a.position={transform[12],transform[13],transform[14]};a.direction=v->direction;}
 else if(const auto*v=std::get_if<VfxCirclePayload>(&payload)){if(schema!=kCirclePayload||v->inner_radius!=0)return {};a.position=v->center;a.direction=v->direction;a.radius=v->radius;}
 else return {};
 if(!Finite(a.position)||!Finite(a.direction)||!std::isfinite(a.radius)||a.radius<=0)return {};
 const float length=std::hypot(a.direction.x,a.direction.z);if(!std::isfinite(length))return {};
 a.direction=length>.0001f?Float3{a.direction.x/length,0,a.direction.z/length}:Float3{0,0,1};return a;
}
std::optional<float> Envelope(const VfxDecalRecipe&r,float age)
{
 const auto &k=r.knots;const auto n=r.knot_count;const float end=k[n-1];if(age<k[0]||age>=end)return {};
 float alpha=1;if(n==4){if(age<k[1]&&k[1]>k[0])alpha=(age-k[0])/(k[1]-k[0]);else if(age>k[2]&&end>k[2])alpha=(end-age)/(end-k[2]);}return alpha;
}

void Append(const VfxDecalRecipe&r,const Anchor&a,float age,std::uint64_t owner,std::uint32_t seed,VfxQuality quality,std::uint32_t importance,std::vector<VfxDecalInput>&out)
{
 if(!std::isfinite(age)||age<0||age>=1||static_cast<std::uint32_t>(quality)>2||r.min_quality>static_cast<std::uint32_t>(quality)||r.texture_min_quality>static_cast<std::uint32_t>(quality))return;
 const auto env=Envelope(r,age);if(!env||*env<=0)return;
 VfxDecalInput v;v.position=a.position;v.radius=a.radius;v.direction=a.direction;v.linear_rgb=r.linear_rgb;v.hdr=r.hdr;v.alpha=r.alpha*(*env);
 v.normalized_age=(age-r.knots[0])/(r.knots[r.knot_count-1]-r.knots[0]);v.gradient_row=r.gradient_row;v.quality=quality;v.importance=importance;v.stable_seed=seed;
 v.kind=r.kind;v.voronoi_cells=r.voronoi_cells;v.edge_glow=r.edge_glow;v.texture_slice=r.texture_slice_base+(seed%4);
 if(v.kind==VfxDecalKind::Fracture)v.alpha*=1-v.normalized_age;
 v.stable_id=Mix(owner^Mix(r.effect_id)^Mix(r.source_stable_id)^Mix(static_cast<std::uint64_t>(r.kind==VfxDecalKind::Fracture?VfxMotionKind::FractureAtPeak:VfxMotionKind::StaticProjectedResidue)));if(!v.stable_id)v.stable_id=1;
 out.push_back(v);
}
}
std::vector<VfxDecalInput> BuildVfxTypedDecalCommands(const VfxImage&image,VfxEventView events,std::span<const VfxPersistentInput>persistent,double tick)
{
 std::vector<VfxDecalInput> result;
 AppendVfxTypedDecalCommands(image, events, persistent, tick, result);
 return result;
}
void AppendVfxTypedDecalCommands(const VfxImage&image,VfxEventView events,
 std::span<const VfxPersistentInput>persistent,double tick,std::vector<VfxDecalInput>&result)
{
 const auto original_size=result.size();
 try
 {
 const auto run=[&](std::uint32_t handle,const Anchor&a,float age,std::uint64_t owner,std::uint32_t seed,VfxQuality quality,std::uint32_t importance,bool is_persistent){if(!handle||handle>image.decal_effect_ranges.size())return;const auto range=image.decal_effect_ranges[handle-1];if(range.first>image.decal_recipes.size()||range.count>image.decal_recipes.size()-range.first)return;for(const auto&r:image.decal_recipes.subspan(range.first,range.count))if(r.input_mode==(is_persistent?1u:0u)&&r.timing_kind==(is_persistent?1u:0u))Append(r,a,age,owner,seed,quality,importance,result);};
 for(const auto&i:events)
 {
  if(!i.effect_handle||i.effect_handle>image.decal_effect_ranges.size()||tick<i.event_tick)continue;const auto range=image.decal_effect_ranges[i.effect_handle-1];if(range.first>=image.decal_recipes.size()||!range.count)continue;const auto&first=image.decal_recipes[range.first];
  if(first.input_mode!=0||first.timing_kind!=0||!std::isfinite(first.seconds)||first.seconds<=0)continue;const auto a=GetAnchor(i.payload,i.world_transform,first.payload_kind);if(!a)continue;
  const auto start=i.geometry_owner_id?i.geometry_start_tick:i.event_tick;if(tick<start||(i.geometry_owner_id&&i.geometry_end_tick<=start))continue;
  const double duration=i.geometry_owner_id?static_cast<double>(i.geometry_end_tick-start):static_cast<double>(first.seconds)*60;
  run(i.effect_handle,*a,static_cast<float>(static_cast<double>(tick-start)/duration),i.sequence,i.stable_seed,i.quality,i.importance_override?i.importance_override:first.importance,false);
 }
 for(const auto&i:persistent)
 {
  if(!i.effect_handle||i.effect_handle>image.decal_effect_ranges.size()||!i.stable_id||!std::holds_alternative<VfxCirclePayload>(i.payload))continue;const auto range=image.decal_effect_ranges[i.effect_handle-1];if(range.first>=image.decal_recipes.size()||!range.count)continue;const auto&first=image.decal_recipes[range.first];
  if(first.input_mode!=1||first.timing_kind!=1||!std::isfinite(i.elapsed_seconds)||i.elapsed_seconds<0)continue;
  const auto a=GetAnchor(i.payload,i.current_transform,first.payload_kind);if(!a)continue;
  run(i.effect_handle,*a,i.normalized_age,i.stable_id,i.stable_seed,i.quality,first.importance,true);
 }
 }
 catch(...){result.resize(original_size);throw;}
}
}
