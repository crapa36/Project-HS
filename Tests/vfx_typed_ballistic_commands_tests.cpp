#include "../Source/Runtime/Private/vfx_typed_ballistic_commands.hpp"
#include <hs/core/render_snapshot.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <stdexcept>
#include <span>
#include <string_view>
namespace
{
std::uint32_t Hash(std::string_view s){std::uint32_t h=2166136261u;for(unsigned char c:s){h^=c;h*=16777619u;}return h;}
std::uint64_t Hash64(std::string_view s){std::uint64_t h=14695981039346656037ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}return h;}
void Check(bool c,const char*m){if(!c)throw std::runtime_error(m);}
bool Near(float a,float b){return std::abs(a-b)<.00001f;}
hs::VfxParameterRecord Float(std::string_view n,float v){return {Hash(n),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(v)};}
hs::VfxProgramData Program()
{
 hs::VfxProgramData p;hs::VfxEffectRecord e;e.handle=1;e.payload_kind=Hash("PointEventPayload");e.seconds=.5f;e.sources={0,1};p.effects.push_back(e);p.effect_lookup.push_back({0xabc123defull,1});
 hs::VfxSourceRecord s;s.effect=1;s.stable_id=Hash("shards");s.type=hs::VfxSourceType::BallisticCollision;s.knot_count=2;s.knots={.125f,.75f,0,0};s.parameters={0,2};s.outputs={0,1};p.sources.push_back(s);
 p.parameters={{Hash("count"),hs::VfxParameterType::Int,6},Float("gravity",-5)};
 hs::VfxOutputRecord o;o.source=0;o.profile=hs::VfxOutputProfile::MeshEmissiveOit;o.shape=Hash("shard_mesh");o.motion=hs::VfxMotionKind::ShortBallisticFan;o.rgba={1,.5f,.2f,.7f};o.hdr=3;o.gradient_row=8;p.outputs.push_back(o);return p;
}
hs::VfxEventInput Input(){hs::VfxEventInput i;i.effect_handle=1;i.event_tick=100;i.sequence=99;i.stable_seed=123;hs::VfxPointPayload p;p.position={1,.2f,2};p.normal={1,0,0};i.payload=p;return i;}
auto Run(const hs::VfxProgramData&p,const hs::VfxEventInput&i){return hs::runtime_detail::BuildVfxTypedBallisticCommands(p,std::span(&i,1));}
void TimingIdentityAndScale()
{
 auto p=Program();auto i=Input();const auto a=Run(p,i);Check(a.size()==6,"authored count lost");
 Check(a[0].tick==104&&a[0].authored_birth_tick==103&&Near(a[0].authored_birth_fraction,.75f)&&Near(a[0].lifetime_min,.3125f),"fractional source birth or lifetime lost");
 Check(a[0].authored_ballistic&&a[0].count==1&&a[0].sequence==99&&a[0].mesh_index==2&&a[0].authored_gradient_row==8&&a[0].authored_hdr==3&&a[0].end_color.w==0,"mesh or authored presentation fields lost");
 Check(a[0].gravity==-5&&Near(a[0].authored_drag,.8f)&&a[0].authored_bounce==0&&Near(a[0].authored_collision_radius,a[0].start_size_min*.5f),"physics parameters lost");
 for(const auto &c:a){float v=std::hypot(c.authored_initial_velocity.x,c.authored_initial_velocity.y,c.authored_initial_velocity.z);Check(c.authored_initial_velocity.x/v>=std::cos(55*.01745329252f)-.00001f,"fan ignored impact normal");}
 const auto again=Run(p,i);Check(again.size()==a.size()&&again[0].seed==a[0].seed&&again[0].authored_initial_velocity.x==a[0].authored_initial_velocity.x,"repeated decode changed initial conditions");
 Check(a[0].seed!=a[1].seed,"emission ordinal seed collapsed");
 std::get<hs::VfxPointPayload>(i.payload).authored_scale=2;auto scaled=Run(p,i);Check(Near(scaled[0].start_size_min,2*a[0].start_size_min)&&scaled[0].authored_initial_velocity.x==a[0].authored_initial_velocity.x,"point scale altered world velocity");
 i=Input();p.effects.push_back(p.effects[0]);p.effects[1].handle=2;p.sources[0].effect=2;p.effect_lookup[0].handle=2;i.effect_handle=2;
 Check(Run(p,i)[0].seed==a[0].seed,"catalog handle changed semantic seed");
}
void GeometryAndRecipes()
{
 auto p=Program();auto i=Input();p.effects[0].payload_kind=Hash("CircleAreaPayload");hs::VfxCirclePayload c;c.center={2,0,3};c.radius=4;i.payload=c;
 p.outputs[0].shape=Hash("debris_shard_mesh");p.outputs[0].motion=hs::VfxMotionKind::BallisticBurstGravitySpin;p.parameters[0].bits=8;p.parameters[1]=Float("bounce",.2f);
 auto a=Run(p,i);Check(a.size()==8&&a[0].mesh_index==3&&Near(a[0].authored_bounce,.2f)&&Near(a[0].gravity,-9.81f),"burst authored bounce or mesh lost");
 for(const auto &v:a)Check(std::hypot(v.position.x-2,v.position.z-3)<=4&&Near(v.position.y,.1f)&&v.start_size_min<=.12f,"circle emission escaped footprint or scaled debris");
 p.outputs[0].shape=Hash("small_shard_mesh");p.outputs[0].motion=hs::VfxMotionKind::ShardsLiftFall;p.sources[0].parameters.count=3;p.parameters={{Hash("count"),hs::VfxParameterType::Int,4},Float("gravity",-8),Float("speed",2)};
 a=Run(p,i);Check(a.size()==4&&a[0].mesh_index==4&&Near(std::hypot(a[0].authored_initial_velocity.x,a[0].authored_initial_velocity.y,a[0].authored_initial_velocity.z),2),"trap shard speed lost");
 p.outputs[0].shape=Hash("small_shards");p.outputs[0].motion=hs::VfxMotionKind::BriefRadialResponse;p.sources[0].parameters.count=1;p.parameters[0].bits=5;
 p.effects[0].payload_kind=Hash("PresentationContextPayload");hs::VfxContextPayload context;context.ratio01=.01f;i.payload=context;i.world_transform[12]=9;
 a=Run(p,i);Check(a.size()==5&&a[0].position.x==9&&a[0].start_size_min>=.035f,"context ratio incorrectly scaled burst");
 p.outputs[0].min_quality=1;i.quality=hs::VfxQuality::Low;Check(Run(p,i).empty(),"quality ignored");
}
void ProjectileForwardFans()
{
 auto id=[](std::string_view name){std::uint64_t h=14695981039346656037ull;for(unsigned char c:name){h^=c;h*=1099511628211ull;}return h;};
 auto p=Program();auto input=Input();p.effects[0].payload_kind=Hash("ProjectilePayload");
 hs::VfxProjectilePayload projectile;projectile.velocity={0,0,-9};projectile.hitbox_radius=.8f;input.payload=projectile;input.world_transform[12]=4;
 p.effect_lookup[0].effect_id=id("particle.enemy.ranged.impact");p.outputs[0].shape=Hash("small_shard_mesh");p.outputs[0].motion=hs::VfxMotionKind::ShortForwardShardFan;
 p.parameters={{Hash("count"),hs::VfxParameterType::Int,5},Float("cone_deg",70),Float("speed",2.4f)};p.sources[0].parameters={0,3};
 auto out=Run(p,input);Check(out.size()==5&&out[0].mesh_index==4&&out[0].position.x==4,"forward impact fan missing");
 for(const auto &c:out){const auto &v=c.authored_initial_velocity;Check(Near(std::hypot(v.x,v.y,v.z),2.4f)&&-v.z/2.4f>=std::cos(70*.01745329252f)-.00001f&&c.start_size_min>=.035f&&c.start_size_min<=.065f,"forward fan lost world speed, half-angle or physical size");}
 auto before=out;std::get<hs::VfxProjectilePayload>(input.payload).hitbox_radius=4;
 out=Run(p,input);Check(out[0].start_size_min==before[0].start_size_min&&out[0].authored_initial_velocity.z==before[0].authored_initial_velocity.z,"hitbox radius incorrectly scaled shards");
 p.effect_lookup[0].effect_id=id("particle.skill.charged_shot.impact");p.outputs[0].shape=Hash("needle_shard_mesh");p.outputs[0].motion=hs::VfxMotionKind::ThinShardsForwardCone;
 p.parameters[0].bits=7;p.parameters[1]=Float("cone_deg",32);p.parameters[2]=Float("speed",4.2f);
 out=Run(p,input);Check(out.size()==7&&out[0].mesh_index==7&&Near(out[0].gravity,-3),"needle fan missing authored mesh or gravity default");
 for(const auto &c:out)Check(Near(std::hypot(c.authored_initial_velocity.x,c.authored_initial_velocity.y,c.authored_initial_velocity.z),4.2f)&&-c.authored_initial_velocity.z/4.2f>=std::cos(32*.01745329252f)-.00001f,"needle speed/cone ignored");
 p.effect_lookup[0].effect_id=id("particle.enemy.ranged.impact");Check(Run(p,input).empty(),"wrong contact identity accepted for needle fan");p.effect_lookup[0].effect_id=id("particle.skill.charged_shot.impact");
 std::get<hs::VfxProjectilePayload>(input.payload).velocity={};Check(Run(p,input).empty(),"forward fan invented zero-velocity axis");
}
void CookedGlobalBindings()
{
 auto p=Program();auto input=Input();
 for(const auto path:{std::string_view("Content/Textures/VFX/vfx_gradient_lut.dds"),std::string_view("Content/Textures/VFX/vfx_curve_lut.dds")})
 {
  const auto first=static_cast<std::uint32_t>(p.strings.size()),slot=static_cast<std::uint32_t>(p.texture_resources.size());
  for(char ch:path)p.strings.push_back(static_cast<std::byte>(ch));
  p.texture_resources.push_back({slot,{first,static_cast<std::uint32_t>(path.size())}});
  hs::VfxTextureBindingRecord binding;binding.role=Hash("profile.global");binding.catalog_slot=slot;p.texture_bindings.push_back(binding);
 }
 p.outputs[0].textures={0,2};Check(Run(p,input).size()==6,"shared cooked mesh LUT bindings rejected");
 p.texture_bindings[1].role=Hash("authored_mask_array");Check(Run(p,input).empty(),"unimplemented authored texture silently ignored");
 p.texture_bindings[1].role=Hash("profile.global");p.texture_bindings[1].catalog_slot=100;
 Check(Run(p,input).empty(),"missing shared resource accepted");
}
void RejectMalformed()
{
 auto p=Program();auto i=Input();p.parameters[0].type=hs::VfxParameterType::Float;Check(Run(p,i).empty(),"wrong count type accepted");
 p=Program();p.parameters[0].bits=0xffffffffu;Check(Run(p,i).empty(),"negative count accepted");
 p=Program();p.parameters[1]=Float("gravity",std::numeric_limits<float>::infinity());Check(Run(p,i).empty(),"infinite gravity accepted");
 p=Program();p.parameters[1]=Float("bounce",1.1f);Check(Run(p,i).empty(),"invalid restitution accepted");
 p=Program();p.parameters[1]=Float("unimplemented",1);Check(Run(p,i).empty(),"unknown source parameter ignored");
 p=Program();p.sources[0].knots[1]=p.sources[0].knots[0];Check(Run(p,i).empty(),"empty source window accepted");
 p=Program();i.payload=hs::VfxProjectilePayload{};Check(Run(p,i).empty(),"unsupported projectile coerced");i=Input();
 p.effects[0].input_mode=1;Check(Run(p,i).empty(),"persistent burst emitted repeatedly");
 p=Program();p.effect_lookup.clear();Check(Run(p,i).empty(),"missing semantic identity accepted");
}
hs::VfxProgramData PersistentShardProgram(std::string_view name,bool fixed,float fixed_seconds=.55f,
                                          std::uint32_t payload_kind=Hash("EntityAttachmentPayload"))
{
 auto p=Program();p.effects[0].input_mode=1;p.effects[0].timing_kind=fixed?0:1;
 p.effects[0].seconds=fixed?fixed_seconds:0;p.effects[0].payload_kind=payload_kind;
 p.effect_lookup[0].effect_id=[](std::string_view s){std::uint64_t h=14695981039346656037ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}return h;}(name);
 p.sources[0].stable_id=Hash("status_shards");p.sources[0].knots={0,.8f,0,0};
 p.sources[0].parameters={0,1};p.parameters={{Hash("count"),hs::VfxParameterType::Int,5}};
 p.outputs[0].shape=Hash("small_shards");p.outputs[0].motion=hs::VfxMotionKind::BriefRadialResponse;
 p.outputs[0].shape_domain=Hash("sprite");p.outputs[0].shape_scale_rule=Hash("component_size");
 p.outputs[0].shape_component_kind=Hash("particle");p.outputs[0].coverage_type=Hash("analytic");
 p.outputs[0].coverage_ref=Hash("small_shards");p.outputs[0].min_quality=1;
 auto texture=[&](std::string_view path,std::uint32_t slot){
  const auto first=static_cast<std::uint32_t>(p.strings.size());
  for(char ch:path)p.strings.push_back(static_cast<std::byte>(ch));
  p.texture_resources.push_back({slot,{first,static_cast<std::uint32_t>(path.size())}});
  hs::VfxTextureBindingRecord binding;binding.role=Hash("profile.global");binding.catalog_slot=slot;
  p.texture_bindings.push_back(binding);
 };
 texture("Content/Textures/VFX/vfx_gradient_lut.dds",1);
 texture("Content/Textures/VFX/vfx_curve_lut.dds",2);
 p.outputs[0].textures={0,2};return p;
}
hs::VfxPersistentInput PersistentShardOwner(hs::PersistentVfxKind kind)
{
 hs::VfxPersistentInput owner;owner.effect_handle=1;owner.stable_id=52;owner.stable_seed=123;
 owner.source_visual_kind=static_cast<std::uint8_t>(kind);
 owner.source_duration_seconds=kind==hs::PersistentVfxKind::ChargedFullReady?.55f:
   kind==hs::PersistentVfxKind::MultishotRetarget?.30f:
   kind==hs::PersistentVfxKind::RicochetBleedExtend?.35f:2.0f;
 owner.current_transform[12]=3;owner.current_transform[13]=.5f;owner.current_transform[14]=4;
 hs::VfxEntityPayload entity;entity.render_instance_id=9;entity.footprint_radius=.7f;
 owner.payload=entity;
 if(kind==hs::PersistentVfxKind::MultishotRetarget)
 {
  hs::VfxProjectilePathPayload path;
  path.current_position={8,.75f,9};path.previous_position={7.5f,.75f,8.5f};
  path.velocity={3,0,3};path.projectile_radius=.18f;path.lifetime01=owner.normalized_age;
  owner.current_transform[12]=-40;owner.current_transform[14]=-40;owner.payload=path;
 }
 return owner;
}
void PersistentStatusShards()
{
 struct Case{const char *name;hs::PersistentVfxKind kind;float seconds;bool path;};
 constexpr Case cases[]{
  {"particle.player.bow_draw",hs::PersistentVfxKind::PlayerBowDraw,0,false},
  {"particle.upgrade.charged.full_ready",hs::PersistentVfxKind::ChargedFullReady,.55f,false},
  {"particle.upgrade.empowered_ready",hs::PersistentVfxKind::EmpoweredReady,0,false},
  {"particle.upgrade.retarget",hs::PersistentVfxKind::MultishotRetarget,.30f,true},
  {"particle.upgrade.ricochet.bleed_extend",hs::PersistentVfxKind::RicochetBleedExtend,.35f,false},
 };
 for(const auto &test:cases)
 {
  const bool fixed=test.seconds>0;
  const auto payload=test.path?Hash("ProjectilePathPayload"):Hash("EntityAttachmentPayload");
  auto p=PersistentShardProgram(test.name,fixed,test.seconds,payload);auto owner=PersistentShardOwner(test.kind);
  auto run=[&]{return hs::runtime_detail::BuildVfxPersistentStatusShardCommands(p,std::span(&owner,1),77);};
  const auto commands=run();
  Check(commands.size()==5,"persistent shard count lost");
  for(const auto &cmd:commands)
   Check(cmd.authored_ballistic&&cmd.renderer==hs::VfxRenderer::Mesh&&cmd.mesh_index==4&&
     cmd.tick==77&&cmd.authored_birth_tick==77&&cmd.authored_birth_fraction==0&&
     Near(cmd.lifetime_min,owner.source_duration_seconds*.8f)&&
     Near(cmd.lifetime_max,owner.source_duration_seconds*.8f)&&
     cmd.position.x==(test.path?8.0f:3.0f)&&cmd.position.y==(test.path?.75f:.5f)&&
     cmd.position.z==(test.path?9.0f:4.0f)&&
     cmd.start_size_min>=.035f&&cmd.start_size_min<=.065f&&
     Near(cmd.gravity,-3)&&Near(cmd.authored_drag,.8f)&&cmd.authored_bounce==0,
     "persistent shard birth, physics or source lifetime changed");
  Check(commands[0].seed!=commands[1].seed&&run()[0].seed==commands[0].seed&&
    run()[0].authored_initial_velocity.x==commands[0].authored_initial_velocity.x,
    "persistent shard initial conditions are not deterministic");
  owner.quality=hs::VfxQuality::Low;Check(run().empty(),"low quality emitted medium shards");
  owner.quality=hs::VfxQuality::Medium;Check(run().size()==5,"medium quality lost authored shards");
  owner.normalized_age=.8f;
  if(test.path)std::get<hs::VfxProjectilePathPayload>(owner.payload).lifetime01=.8f;
  else std::get<hs::VfxEntityPayload>(owner.payload).lifetime01=.8f;
  Check(run().empty(),"expired source window emitted shards");
  owner=PersistentShardOwner(test.kind);owner.source_visual_kind=static_cast<std::uint8_t>(hs::PersistentVfxKind::EnemyBurnStatus);
  Check(run().empty(),"unrelated visual kind emitted shards");
  owner=PersistentShardOwner(test.kind);owner.source_duration_seconds=0;
  Check(run().empty(),"nonpositive source duration emitted shards");
  owner=PersistentShardOwner(test.kind);p.effects[0].input_mode=0;
  Check(run().empty(),"event input mode emitted persistent shards");
  p=PersistentShardProgram(test.name,fixed,test.seconds,payload);p.effects[0].timing_kind=fixed?1:0;
  Check(run().empty(),"wrong timing kind emitted persistent shards");
  p=PersistentShardProgram(test.name,fixed,test.seconds,payload);
  owner=PersistentShardOwner(test.kind);p.sources[0].stable_id=Hash("wrong_source");
  Check(run().empty(),"wrong source identity emitted shards");
  p=PersistentShardProgram(test.name,fixed,test.seconds,payload);p.parameters[0].bits=6;
  Check(run().empty(),"wrong authored count emitted shards");
  p=PersistentShardProgram(test.name,fixed,test.seconds,payload);p.outputs[0].min_quality=0;
  Check(run().empty(),"wrong authored quality emitted shards");
  p=PersistentShardProgram(test.name,fixed,test.seconds,payload);p.texture_bindings[0].catalog_slot=99;
  Check(run().empty(),"missing shared gradient texture emitted shards");
  p=PersistentShardProgram(test.name,fixed,test.seconds,payload);owner.payload=hs::VfxPointPayload{};
  Check(run().empty(),"wrong persistent payload emitted shards");
 }
}
void ProductionBowDrawStatusShards()
{
 std::ifstream file("Cooked/vfx_program.hsbin",std::ios::binary);
 Check(file.good(),"production VFX program missing");
 const std::string binary{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
 hs::VfxProgramData cooked;std::string error;
 Check(hs::LoadVfxProgram(std::as_bytes(std::span(binary)),cooked,error),"production VFX program could not be decoded");
 const auto lookup=std::find_if(cooked.effect_lookup.begin(),cooked.effect_lookup.end(),
   [](const auto &entry){return entry.effect_id==Hash64("particle.player.bow_draw");});
 Check(lookup!=cooked.effect_lookup.end(),"production bow effect missing");
 auto owner=PersistentShardOwner(hs::PersistentVfxKind::PlayerBowDraw);
 owner.effect_handle=lookup->handle;
 owner.source_duration_seconds=14.0f/60.0f;
 owner.elapsed_seconds=5.0f/60.0f;
 owner.normalized_age=5.0f/14.0f;
 std::get<hs::VfxEntityPayload>(owner.payload).lifetime01=owner.normalized_age;
 owner.quality=hs::VfxQuality::Medium;
 const auto run=[&]{return hs::runtime_detail::BuildVfxPersistentStatusShardCommands(cooked,std::span(&owner,1),77);};
 Check(run().size()==5,"production bow draw did not decode five medium status shards");
 owner.quality=hs::VfxQuality::Low;
 Check(run().empty(),"production bow draw emitted medium status shards at low quality");
}
void ProductionUpgradeStatusShards()
{
 std::ifstream file("Cooked/vfx_program.hsbin",std::ios::binary);
 Check(file.good(),"production VFX program missing");
 const std::string binary{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
 hs::VfxProgramData cooked;std::string error;
 Check(hs::LoadVfxProgram(std::as_bytes(std::span(binary)),cooked,error),"production VFX program could not be decoded");
 struct Case{const char *name;hs::PersistentVfxKind kind;float seconds;bool path;};
 constexpr Case cases[]{
  {"particle.upgrade.retarget",hs::PersistentVfxKind::MultishotRetarget,.30f,true},
  {"particle.upgrade.ricochet.bleed_extend",hs::PersistentVfxKind::RicochetBleedExtend,.35f,false},
 };
 for(const auto &test:cases)
 {
  const auto lookup=std::find_if(cooked.effect_lookup.begin(),cooked.effect_lookup.end(),
    [&](const auto &entry){return entry.effect_id==Hash64(test.name);});
  Check(lookup!=cooked.effect_lookup.end(),"production upgrade status effect missing");
  auto owner=PersistentShardOwner(test.kind);owner.effect_handle=lookup->handle;
  owner.source_duration_seconds=test.seconds;owner.elapsed_seconds=.2f*test.seconds;owner.normalized_age=.2f;
  if(test.path)std::get<hs::VfxProjectilePathPayload>(owner.payload).lifetime01=owner.normalized_age;
  else std::get<hs::VfxEntityPayload>(owner.payload).lifetime01=owner.normalized_age;
  owner.quality=hs::VfxQuality::Medium;
  const auto run=[&]{return hs::runtime_detail::BuildVfxPersistentStatusShardCommands(cooked,std::span(&owner,1),77);};
  const auto commands=run();
  Check(commands.size()==5&&commands[0].position.x==(test.path?8.0f:3.0f)&&
    commands[0].position.z==(test.path?9.0f:4.0f),"production upgrade status shards did not use authored geometry");
  owner.quality=hs::VfxQuality::Low;Check(run().empty(),"production upgrade emitted medium shards at low quality");
 }
}
}
int main(){try{TimingIdentityAndScale();GeometryAndRecipes();CookedGlobalBindings();ProjectileForwardFans();RejectMalformed();PersistentStatusShards();ProductionBowDrawStatusShards();ProductionUpgradeStatusShards();std::cout<<"Typed ballistic commands passed\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
