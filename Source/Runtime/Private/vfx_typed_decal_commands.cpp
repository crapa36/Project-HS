#include "vfx_typed_decal_commands.hpp"
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
std::uint64_t Mix(std::uint64_t v){v^=v>>30;v*=0xbf58476d1ce4e5b9ull;v^=v>>27;v*=0x94d049bb133111ebull;return v^(v>>31);}
bool Finite(Float3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
template<class T>bool Range(VfxRange r,const std::vector<T>&v){return r.first<=v.size()&&r.count<=v.size()-r.first;}

struct Anchor{Float3 position{},direction{0,0,1};float radius{1.25f};};
std::optional<Anchor> GetAnchor(const VfxPayload&payload,const VfxTransform&transform,std::uint32_t schema)
{
 Anchor a;
 if(const auto*v=std::get_if<VfxPointPayload>(&payload)){if(schema!=Hash("PointEventPayload")||!std::isfinite(v->authored_scale)||v->authored_scale<=0)return {};a.position=v->position;a.direction=v->direction;a.radius*=v->authored_scale;}
 else if(const auto*v=std::get_if<VfxContextPayload>(&payload)){if(schema!=Hash("PresentationContextPayload"))return {};a.position={transform[12],transform[13],transform[14]};a.direction=v->direction;}
 else if(const auto*v=std::get_if<VfxCirclePayload>(&payload)){if(schema!=Hash("CircleAreaPayload")||v->inner_radius!=0)return {};a.position=v->center;a.direction=v->direction;a.radius=v->radius;}
 else return {};
 if(!Finite(a.position)||!Finite(a.direction)||!std::isfinite(a.radius)||a.radius<=0)return {};
 const float length=std::hypot(a.direction.x,a.direction.z);if(!std::isfinite(length))return {};
 a.direction=length>.0001f?Float3{a.direction.x/length,0,a.direction.z/length}:Float3{0,0,1};return a;
}
std::optional<std::string_view> Path(const VfxProgramData&p,std::uint32_t slot)
{
 const auto r=std::find_if(p.texture_resources.begin(),p.texture_resources.end(),[&](const auto&v){return v.slot==slot;});
 if(r==p.texture_resources.end()||!Range(r->asset_path_bytes,p.strings))return {};
 return std::string_view(reinterpret_cast<const char*>(p.strings.data())+r->asset_path_bytes.first,r->asset_path_bytes.count);
}
bool Textures(const VfxProgramData&p,const VfxOutputRecord&o,VfxDecalInput&v)
{
 if(!Range(o.textures,p.texture_bindings))return false;
 constexpr std::string_view fracture="Content/Textures/VFX/vfx_fracture_decal_array.dds",organic="Content/Textures/VFX/vfx_organic_decal_array.dds",gradient="Content/Textures/VFX/vfx_gradient_lut.dds";
 const auto required=v.kind==VfxDecalKind::Fracture?fracture:organic;
 bool detail=false,coverage=false,global=false;
 for(const auto&b:std::span(p.texture_bindings).subspan(o.textures.first,o.textures.count))
 {
  const auto path=Path(p,b.catalog_slot);if(!path||!std::isfinite(b.strength)||b.strength!=1||b.min_quality>static_cast<std::uint32_t>(v.quality)||!Range(b.slices,p.slice_indices))return false;
  if(b.role==Hash("profile.global")){if(*path!=gradient||b.slices.count!=0)return false;global=true;}
  else if(b.role==Hash("profile.authored")){if((*path!=fracture&&*path!=organic)||b.slices.count!=0)return false;}
  else if(b.role==Hash("coverage")){if(v.kind!=VfxDecalKind::Organic||coverage||*path!=organic||b.slices.count!=0)return false;coverage=true;}
  else if(b.role==Hash("texture_array"))
  {
   if(detail||*path!=required||b.selection!=Hash("stable_seed_mod_4")||b.slices.count!=4)return false;
   const auto slices=std::span(p.slice_indices).subspan(b.slices.first,4);const auto base=slices[0];
   if(base!=0&&(v.kind!=VfxDecalKind::Organic||base!=4))return false;
   for(std::uint32_t k=0;k<4;++k)if(slices[k]!=base+k)return false;
   v.texture_slice=slices[v.stable_seed%4];detail=true;
  }
  else return false;
 }
 return global&&detail&&(v.kind==VfxDecalKind::Fracture||coverage);
}
bool Params(const VfxProgramData&p,const VfxOutputRecord&o,VfxDecalInput&v)
{
 if(!Range(o.parameters,p.parameters))return false;
 if(v.kind==VfxDecalKind::Organic)return o.parameters.count==0;
 if(o.parameters.count!=2)return false;bool cells=false,glow=false;
 for(const auto&a:std::span(p.parameters).subspan(o.parameters.first,o.parameters.count))
 {
  if(a.key==Hash("voronoi_cells")&&a.type==VfxParameterType::Int&&!cells){v.voronoi_cells=a.bits;cells=true;}
  else if(a.key==Hash("edge_glow")&&a.type==VfxParameterType::Float&&!glow){v.edge_glow=std::bit_cast<float>(a.bits);glow=true;}
  else return false;
 }
 return cells&&glow&&v.voronoi_cells>0&&v.voronoi_cells<=0x7fffffff&&std::isfinite(v.edge_glow)&&v.edge_glow>=0&&v.edge_glow<=1;
}
std::optional<float> Envelope(const VfxSourceRecord&s,float age)
{
 if(s.knot_count!=2&&s.knot_count!=4)return {};
 for(std::uint32_t i=0;i<s.knot_count;++i)if(!std::isfinite(s.knots[i])||s.knots[i]<0||s.knots[i]>1||(i&&s.knots[i]<s.knots[i-1]))return {};
 const float end=s.knots[s.knot_count-1];if(end<=s.knots[0]||age<s.knots[0]||age>=end)return {};
 float alpha=1;if(s.knot_count==4){if(age<s.knots[1]&&s.knots[1]>s.knots[0])alpha=(age-s.knots[0])/(s.knots[1]-s.knots[0]);else if(age>s.knots[2]&&end>s.knots[2])alpha=(end-age)/(end-s.knots[2]);}return alpha;
}

void Append(const VfxProgramData&p,const VfxEffectRecord&e,const Anchor&a,float age,std::uint64_t owner,std::uint32_t seed,VfxQuality quality,std::uint32_t importance,std::vector<VfxDecalInput>&out)
{
 if(!std::isfinite(age)||age<0||age>=1||static_cast<std::uint32_t>(quality)>2||!Range(e.sources,p.sources))return;
 const auto id=std::find_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto&v){return v.handle==e.handle;});if(id==p.effect_lookup.end()||!id->effect_id)return;
 for(std::uint32_t si=e.sources.first;si<e.sources.first+e.sources.count;++si)
 {
  const auto&s=p.sources[si];if(s.effect!=e.handle||s.type!=VfxSourceType::Direct||s.parameters.count||!Range(s.parameters,p.parameters)||!Range(s.outputs,p.outputs))continue;
  const auto env=Envelope(s,age);if(!env||*env<=0)continue;
  for(const auto&o:std::span(p.outputs).subspan(s.outputs.first,s.outputs.count))
  {
   if(o.source!=si||o.profile!=VfxOutputProfile::Decal||o.min_quality>static_cast<std::uint32_t>(quality)||!std::isfinite(o.hdr)||o.hdr<0||std::any_of(o.rgba.begin(),o.rgba.end(),[](float v){return !std::isfinite(v)||v<0;})||o.rgba[3]>1)continue;
   VfxDecalInput v;v.position=a.position;v.radius=a.radius;v.direction=a.direction;v.linear_rgb={o.rgba[0],o.rgba[1],o.rgba[2]};v.hdr=o.hdr;v.alpha=o.rgba[3]*(*env);v.normalized_age=(age-s.knots[0])/(s.knots[s.knot_count-1]-s.knots[0]);v.gradient_row=o.gradient_row;v.quality=quality;v.importance=importance;v.stable_seed=seed;
   if(o.shape==Hash("voronoi_crack_decal")&&o.motion==VfxMotionKind::FractureAtPeak&&o.coverage_type==Hash("analytic_or_projected")&&o.coverage_ref==o.shape)v.kind=VfxDecalKind::Fracture;
   else if(o.shape==Hash("organic_ground_decal")&&o.motion==VfxMotionKind::StaticProjectedResidue&&o.coverage_type==Hash("projected_texture_mask"))v.kind=VfxDecalKind::Organic;
   else continue;
   if(!Params(p,o,v)||!Textures(p,o,v))continue;
   // Fracture cools over its source interval. Residue uses its authored four-knot fade.
   if(v.kind==VfxDecalKind::Fracture)v.alpha*=1-v.normalized_age;
   v.stable_id=Mix(owner^Mix(id->effect_id)^Mix(s.stable_id)^Mix(static_cast<std::uint64_t>(o.motion)));if(!v.stable_id)v.stable_id=1;
   out.push_back(v);
  }
 }
}
}
std::vector<VfxDecalInput> BuildVfxTypedDecalCommands(const VfxProgramData&p,std::span<const VfxEventInput>events,std::span<const VfxPersistentInput>persistent,Tick tick)
{
 std::vector<VfxDecalInput> result;
 for(const auto&i:events)
 {
  if(!i.effect_handle||i.effect_handle>p.effects.size()||tick<i.event_tick)continue;const auto&e=p.effects[i.effect_handle-1];
  if(e.handle!=i.effect_handle||e.input_mode!=0||e.timing_kind!=0||!std::isfinite(e.seconds)||e.seconds<=0)continue;
  const auto a=GetAnchor(i.payload,i.world_transform,e.payload_kind);if(!a)continue;
  const auto start=i.geometry_owner_id?i.geometry_start_tick:i.event_tick;if(tick<start||(i.geometry_owner_id&&i.geometry_end_tick<=start))continue;
  const double duration=i.geometry_owner_id?static_cast<double>(i.geometry_end_tick-start):static_cast<double>(e.seconds)*60;
  Append(p,e,*a,static_cast<float>(static_cast<double>(tick-start)/duration),i.sequence,i.stable_seed,i.quality,i.importance_override?i.importance_override:e.importance,result);
 }
 for(const auto&i:persistent)
 {
  if(!i.effect_handle||i.effect_handle>p.effects.size()||!i.stable_id||!std::holds_alternative<VfxCirclePayload>(i.payload))continue;const auto&e=p.effects[i.effect_handle-1];
  if(e.handle!=i.effect_handle||e.input_mode!=1||e.timing_kind!=1||!std::isfinite(i.elapsed_seconds)||i.elapsed_seconds<0)continue;
  const auto a=GetAnchor(i.payload,i.current_transform,e.payload_kind);if(!a)continue;
  Append(p,e,*a,i.normalized_age,i.stable_id,i.stable_seed,i.quality,e.importance,result);
 }
 return result;
}
}
