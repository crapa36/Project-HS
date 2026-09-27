#include "../Source/Runtime/Private/vfx_typed_decal_commands.hpp"
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
namespace
{
std::uint32_t Hash(std::string_view s){std::uint32_t h=2166136261u;for(unsigned char c:s){h^=c;h*=16777619u;}return h;}
void Check(bool c,const char*m){if(!c)throw std::runtime_error(m);}
bool Near(float a,float b){return std::abs(a-b)<.00001f;}
hs::VfxProgramData Program(bool organic=false,bool rune=false)
{
 hs::VfxProgramData p;hs::VfxEffectRecord e;e.handle=1;e.payload_kind=Hash("PointEventPayload");e.seconds=2;e.importance=5;e.sources={0,1};p.effects.push_back(e);p.effect_lookup.push_back({0xabcdef1234ull,1});
 hs::VfxSourceRecord s;s.effect=1;s.stable_id=Hash("decal");s.type=hs::VfxSourceType::Direct;s.knot_count=organic?4:2;s.knots=organic?std::array<float,4>{0,.2f,.6f,1}:std::array<float,4>{.1f,1,0,0};s.outputs={0,1};p.sources.push_back(s);
 hs::VfxOutputRecord o;o.source=0;o.profile=hs::VfxOutputProfile::Decal;o.motion=organic?hs::VfxMotionKind::StaticProjectedResidue:hs::VfxMotionKind::FractureAtPeak;o.shape=Hash(organic?"organic_ground_decal":"voronoi_crack_decal");o.coverage_type=Hash(organic?"projected_texture_mask":"analytic_or_projected");o.coverage_ref=organic?0:o.shape;o.rgba={.2f,.3f,.4f,.5f};o.hdr=1.6f;o.gradient_row=7;o.min_quality=organic?1:0;
 if(!organic){p.parameters={{Hash("voronoi_cells"),hs::VfxParameterType::Int,12},{Hash("edge_glow"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(.7f)}};o.parameters={0,2};}
 const auto add=[&](std::string_view path,std::string_view role){auto first=static_cast<std::uint32_t>(p.strings.size());for(char c:path)p.strings.push_back(static_cast<std::byte>(c));auto slot=static_cast<std::uint32_t>(p.texture_resources.size());p.texture_resources.push_back({slot,{first,static_cast<std::uint32_t>(path.size())}});hs::VfxTextureBindingRecord b;b.role=Hash(role);b.catalog_slot=slot;p.texture_bindings.push_back(b);};
 add("Content/Textures/VFX/vfx_gradient_lut.dds","profile.global");add("Content/Textures/VFX/vfx_fracture_decal_array.dds","profile.authored");add("Content/Textures/VFX/vfx_organic_decal_array.dds","profile.authored");
 if(organic)add("Content/Textures/VFX/vfx_organic_decal_array.dds","coverage");
 add(organic?"Content/Textures/VFX/vfx_organic_decal_array.dds":"Content/Textures/VFX/vfx_fracture_decal_array.dds","texture_array");p.texture_bindings.back().selection=Hash("stable_seed_mod_4");p.texture_bindings.back().slices={0,4};for(std::uint32_t k=0;k<4;++k)p.slice_indices.push_back(k+(rune?4u:0u));o.textures={0,static_cast<std::uint32_t>(p.texture_bindings.size())};p.outputs.push_back(o);return p;
}
hs::VfxEventInput Input(){hs::VfxEventInput i;i.effect_handle=1;i.event_tick=100;i.sequence=77;i.stable_seed=6;i.payload=hs::VfxPointPayload{{1,2,3},{},{1,0,0},2};return i;}
auto Run(const hs::VfxProgramData&p,const hs::VfxEventInput&i,hs::Tick tick=166){return hs::runtime_detail::BuildVfxTypedDecalCommands(p,std::span(&i,1),{},tick);}
void GeometryAndIdentity()
{
 auto p=Program();auto i=Input();auto a=Run(p,i);Check(a.size()==1&&Near(a[0].radius,2.5f)&&Near(a[0].normalized_age,.5f)&&Near(a[0].alpha,.25f)&&a[0].texture_slice==2&&a[0].voronoi_cells==12&&Near(a[0].edge_glow,.7f)&&a[0].gradient_row==7&&a[0].position.y==2,"fracture contract lost");
 Check(Run(p,i,111).empty()&&Run(p,i,220).empty(),"source interval ignored");i.importance_override=8;Check(Run(p,i)[0].importance==8&&Run(p,i)[0].stable_id==a[0].stable_id,"priority changed identity");
 p.effects.push_back(p.effects[0]);p.effects[1].handle=2;p.sources[0].effect=2;p.effect_lookup[0].handle=2;i.effect_handle=2;Check(Run(p,i)[0].stable_id==a[0].stable_id,"cooked reorder changed identity");
 p=Program();i=Input();p.effects[0].payload_kind=Hash("CircleAreaPayload");hs::VfxCirclePayload c;c.center={5,0,7};c.radius=9;i.payload=c;Check(Run(p,i)[0].radius==9&&Run(p,i)[0].position.x==5,"circle geometry replaced by default");
 p.effects[0].payload_kind=Hash("PresentationContextPayload");hs::VfxContextPayload context;context.ratio01=100;i.payload=context;i.world_transform[12]=6;Check(Near(Run(p,i)[0].radius,1.25f)&&Run(p,i)[0].position.x==6,"context ratio distorted cosmetic radius");
 i.geometry_owner_id=9;i.geometry_start_tick=100;i.geometry_end_tick=340;Check(Near(Run(p,i,232)[0].normalized_age,.5f),"owner clock ignored");
}
void ResidueAndOwner()
{
 auto p=Program(true);auto i=Input();auto a=Run(p,i,112);Check(a.size()==1&&Near(a[0].alpha,.25f)&&a[0].texture_slice==2&&a[0].kind==hs::VfxDecalKind::Organic,"scorch fade or slice lost");
 p=Program(true,true);Check(Run(p,i,196)[0].texture_slice==6&&Near(Run(p,i,196)[0].alpha,.25f),"rune slice offset or fade-out lost");
 p=Program(true);p.effects[0].payload_kind=Hash("CircleAreaPayload");p.effects[0].input_mode=1;p.effects[0].timing_kind=1;p.effects[0].seconds=0;hs::VfxPersistentInput owner;owner.effect_handle=1;owner.stable_id=23;owner.stable_seed=6;owner.normalized_age=.4f;hs::VfxCirclePayload c;c.radius=7;c.center={2,0,5};owner.payload=c;
 const auto owned=[&](){return hs::runtime_detail::BuildVfxTypedDecalCommands(p,{},std::span(&owner,1),999);};a=owned();Check(a.size()==1&&a[0].radius==7&&Near(a[0].alpha,.5f),"persistent actual circle/age lost");owner.normalized_age=.8f;Check(Near(owned()[0].alpha,.25f)&&owned()[0].stable_id==a[0].stable_id,"persistent fade or identity changed");owner.normalized_age=1;Check(owned().empty(),"expired owner retained decal");Check(hs::runtime_detail::BuildVfxTypedDecalCommands(p,{},{},999).empty(),"absent owner retained decal");
}
void Invalid()
{
 auto p=Program(true);auto i=Input();i.quality=hs::VfxQuality::Low;Check(Run(p,i).empty(),"organic minimum quality ignored");p=Program();Check(Run(p,i).size()==1,"low fracture incorrectly discarded despite authored quality");
 i=Input();p=Program();p.slice_indices[1]=3;Check(Run(p,i).empty(),"malformed slice group accepted");p=Program();p.texture_bindings.back().selection=0;Check(Run(p,i).empty(),"unknown selection accepted");p=Program(true);p.texture_bindings[3].catalog_slot=1;Check(Run(p,i).empty(),"wrong coverage resource accepted");
 p=Program();p.parameters[0].type=hs::VfxParameterType::Float;Check(Run(p,i).empty(),"wrong cells type accepted");p=Program();p.parameters[1].bits=std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());Check(Run(p,i).empty(),"invalid glow accepted");p=Program();p.outputs[0].motion=hs::VfxMotionKind::StaticProjectedResidue;Check(Run(p,i).empty(),"shape/motion mismatch accepted");
 p=Program();p.sources[0].parameters={0,1};Check(Run(p,i).empty(),"unknown source params ignored");p=Program();i.payload=hs::VfxConePayload{};Check(Run(p,i).empty(),"unsupported cone coerced");
}
}
int main(){try{GeometryAndIdentity();ResidueAndOwner();Invalid();std::cout<<"Typed decal commands passed\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
