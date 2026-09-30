#include "../Source/Runtime/Private/vfx_typed_distortion_commands.hpp"
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
hs::VfxProgramData Program(hs::VfxMotionKind motion=hs::VfxMotionKind::SingleOutwardRefractivePulse)
{
 hs::VfxProgramData p;hs::VfxEffectRecord e;e.handle=1;e.payload_kind=Hash("PointEventPayload");e.seconds=2;e.importance=5;e.sources={0,1};p.effects.push_back(e);p.effect_lookup.push_back({0xabcdef1234ull,1});
 hs::VfxSourceRecord s;s.effect=1;s.stable_id=Hash("distortion");s.type=hs::VfxSourceType::Direct;s.knot_count=2;s.knots={.25f,.75f,0,0};s.outputs={0,1};p.sources.push_back(s);
 p.parameters={{Hash("strength"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(.022f)}};
 hs::VfxOutputRecord o;o.source=0;o.profile=hs::VfxOutputProfile::Distortion;o.motion=motion;o.rgba={1,1,1,.5f};o.min_quality=1;o.coverage_type=Hash("analytic_mask");
 switch(motion){
 case hs::VfxMotionKind::SingleOutwardRefractivePulse:o.shape=Hash("screen_space_radial_normal");p.parameters.push_back({Hash("falloff"),hs::VfxParameterType::Enum,Hash("ring")});break;
 case hs::VfxMotionKind::QuickHeatShock:case hs::VfxMotionKind::ShortRefractiveShock:o.shape=Hash("radial_shock");break;
 case hs::VfxMotionKind::OneTightPulse:o.shape=Hash("small_shock_disc");p.parameters.push_back({Hash("radius"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(.7f)});p.effects[0].payload_kind=Hash("ProjectilePayload");break;
 case hs::VfxMotionKind::TightHeadDistortion:o.shape=Hash("lance_heat_envelope");p.parameters.push_back({Hash("radius"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(.35f)});p.effects[0].payload_kind=Hash("ProjectilePayload");p.effects[0].input_mode=1;p.effects[0].timing_kind=1;p.effects[0].seconds=0;p.sources[0].knots={.05f,.95f,0,0};break;
 default:break;
 }
 o.coverage_ref=o.shape;o.parameters={0,static_cast<std::uint32_t>(p.parameters.size())};p.outputs.push_back(o);return p;
}
hs::VfxEventInput Input(){hs::VfxEventInput in;in.effect_handle=1;in.event_tick=100;in.sequence=77;in.stable_seed=31;in.payload=hs::VfxPointPayload{{1,2,3},{},{0,0,2},2};return in;}
auto Run(const hs::VfxProgramData&p,const hs::VfxEventInput&i,hs::Tick t=145){return hs::runtime_detail::BuildVfxTypedDistortionCommands(p,std::span(&i,1),{},t);}
auto Owned(const hs::VfxProgramData&p,const hs::VfxPersistentInput&i){return hs::runtime_detail::BuildVfxTypedDistortionCommands(p,{},std::span(&i,1),999);}
void GeometryClockAndIdentity()
{
 auto p=Program();auto i=Input();auto a=Run(p,i);Check(a.size()==1&&Near(a[0].radius,1.6f)&&Near(a[0].normalized_age,.25f)&&Near(a[0].alpha,.5f)&&Near(a[0].strength,.022f)&&a[0].position.y==2&&a[0].direction.z==1,"radial geometry or authored source-local phase lost");
 Check(Run(p,i,129).empty()&&Run(p,i,190).empty(),"source interval ignored");
 i.importance_override=9;Check(Run(p,i)[0].importance==9&&Run(p,i)[0].stable_id==a[0].stable_id&&Run(p,i,160)[0].stable_seed==31,"priority, seed or temporal identity changed");
 p.effects.push_back(p.effects[0]);p.effects[1].handle=2;p.sources[0].effect=2;p.effect_lookup[0].handle=2;i.effect_handle=2;Check(Run(p,i)[0].stable_id==a[0].stable_id,"catalog reorder changed semantic identity");
 p=Program();i=Input();p.sources[0].knot_count=4;p.sources[0].knots={.25f,.5f,.6f,.75f};Check(Near(Run(p,i)[0].alpha,.25f),"four-knot fade-in lost");
 i.geometry_owner_id=8;i.geometry_start_tick=100;i.geometry_end_tick=340;Check(Run(p,i).empty()&&Near(Run(p,i,190)[0].alpha,.25f),"actual owner clock ignored");
 p=Program(hs::VfxMotionKind::QuickHeatShock);i=Input();hs::VfxCirclePayload c;c.center={4,0,9};c.radius=6;c.direction={1,0,0};i.payload=c;p.effects[0].payload_kind=Hash("CircleAreaPayload");Check(Run(p,i)[0].radius==6&&Run(p,i)[0].position.x==4&&Run(p,i)[0].shape==hs::VfxDistortionShape::HeatShock,"circle radius replaced by cosmetic default");
 p=Program(hs::VfxMotionKind::ShortRefractiveShock);p.effects[0].payload_kind=Hash("PresentationContextPayload");hs::VfxContextPayload context;context.ratio01=100;i.payload=context;i.world_transform[12]=7;Check(Near(Run(p,i)[0].radius,.75f)&&Run(p,i)[0].position.x==7&&Run(p,i)[0].shape==hs::VfxDistortionShape::ShortShock,"context ratio scaled distortion");
 p=Program(hs::VfxMotionKind::OneTightPulse);hs::VfxProjectilePayload v;v.velocity={3,0,4};v.hitbox_radius=9;i.payload=v;Check(Near(Run(p,i)[0].radius,.7f)&&Near(Run(p,i)[0].direction.x,.6f)&&Run(p,i)[0].shape==hs::VfxDistortionShape::TightDisc,"projectile contact lost authored radius or actual velocity");
}
void Presence()
{
 auto p=Program(hs::VfxMotionKind::TightHeadDistortion);hs::VfxPersistentInput in;in.effect_handle=1;in.stable_id=42;in.current_transform[12]=8;hs::VfxProjectilePayload v;v.velocity={2,0,0};v.hitbox_radius=3;in.payload=v;
 auto a=Owned(p,in);Check(a.size()==1&&Near(a[0].radius,.35f)&&Near(a[0].normalized_age,.5f)&&a[0].position.x==8&&a[0].shape==hs::VfxDistortionShape::HeadEnvelope,"presence did not hold nominal head phase");
 in.elapsed_seconds=100;in.current_transform[12]=10;Check(Owned(p,in)[0].position.x==10&&Owned(p,in)[0].stable_id==a[0].stable_id&&Near(Owned(p,in)[0].normalized_age,.5f),"owned head independently integrated or expired");
 in.normalized_age=.275f;Check(Near(Owned(p,in)[0].normalized_age,.25f),"authoritative owner age ignored");
 in.normalized_age=.96f;Check(Owned(p,in).empty(),"actual source expiry ignored");
 in.normalized_age=0;p.effects[0].seconds=2;in.elapsed_seconds=.09f;Check(Owned(p,in).empty(),"authored source birth knot interpreted as seconds");
 in.elapsed_seconds=.1f;Check(Owned(p,in).size()==1,"authored source birth incorrectly delayed");in.elapsed_seconds=100;Check(Near(Owned(p,in)[0].normalized_age,.5f),"presence failed to hold after midpoint");
 Check(hs::runtime_detail::BuildVfxTypedDistortionCommands(p,{},{},999).empty(),"absent owner retained distortion");
}
void ExactBossGeometry()
{
    auto p=Program(hs::VfxMotionKind::ShortRefractiveShock);
    auto i=Input();
    p.effects[0].payload_kind=Hash("ConePayload");
    hs::VfxConePayload cone;cone.origin={2,.025f,3};cone.direction={0,0,1};
    cone.range=19;cone.half_angle_degrees=37;
    i.payload=cone;
    const auto fan=Run(p,i);
    Check(fan.size()==1&&fan[0].shape==hs::VfxDistortionShape::ConeSector&&
          fan[0].radius==19&&fan[0].half_angle_degrees==37&&
          fan[0].position.x==2&&fan[0].position.z==3&&
          fan[0].direction.z==1,"boss cone distortion lost exact fan geometry");
    cone.half_angle_degrees=0;i.payload=cone;
    Check(Run(p,i).empty(),"invalid boss cone accepted");
    p.effects[0].payload_kind=Hash("RingWithGapsPayload");
    hs::VfxRingGapsPayload ring;ring.center={-1,.025f,8};ring.inner_radius=4;
    ring.outer_radius=9;ring.gap_half_width_degrees=13;
    ring.gap_angles_degrees={7,121,266};i.payload=ring;
    const auto wave=Run(p,i);
    Check(wave.size()==1&&wave[0].shape==hs::VfxDistortionShape::GappedAnnulus&&
          wave[0].radius==9&&wave[0].inner_radius==4&&
          wave[0].gap_half_width_degrees==13&&
          wave[0].gap_angles_degrees==ring.gap_angles_degrees&&
          wave[0].position.x==-1&&wave[0].position.z==8,
          "boss shockwave distortion lost annulus or explicit safe gaps");
    ring.gap_angles_degrees[1]=std::numeric_limits<float>::quiet_NaN();i.payload=ring;
    Check(Run(p,i).empty(),"nonfinite shockwave gap accepted");
    p=Program(hs::VfxMotionKind::QuickHeatShock);i=Input();
    p.effects[0].payload_kind=Hash("RingWithGapsPayload");
    ring.gap_angles_degrees[1]=121;i.payload=ring;
    Check(Run(p,i).empty(),"unsupported recipe was coerced to exact boss distortion");
}
void TextureContracts()
{
 auto p=Program();auto i=Input();
 const auto add=[&](std::string_view path,std::string_view role){const auto first=static_cast<std::uint32_t>(p.strings.size());for(char c:path)p.strings.push_back(static_cast<std::byte>(c));const auto slot=static_cast<std::uint32_t>(p.texture_resources.size()+1);p.texture_resources.push_back({slot,{first,static_cast<std::uint32_t>(path.size())}});hs::VfxTextureBindingRecord b;b.role=Hash(role);b.catalog_slot=slot;p.texture_bindings.push_back(b);};
 add("Content/Textures/VFX/vfx_stbn_scalar.dds","profile.global");add("Content/Textures/VFX/vfx_flow_curl_2d.dds","profile.authored");add("Content/Textures/VFX/vfx_noise_basis_2d.dds","profile.authored");p.outputs[0].textures={0,3};
 Check(Run(p,i).size()==1,"actual cooked distortion profile textures rejected");
 p.texture_bindings[2].role=Hash("coverage");Check(Run(p,i).empty(),"unsupported coverage texture silently ignored");p.texture_bindings[2].role=Hash("profile.authored");p.strings.back()=std::byte{'x'};Check(Run(p,i).empty(),"unknown authored resource accepted");
}
void Invalid()
{
 auto p=Program();auto i=Input();i.quality=hs::VfxQuality::Low;Check(Run(p,i).empty(),"low retained decorative distortion");i.quality=hs::VfxQuality::Medium;p.outputs[0].min_quality=2;Check(Run(p,i).empty(),"minimum quality ignored");
 i=Input();p=Program();p.parameters[1].bits=Hash("disc");Check(Run(p,i).empty(),"unsupported falloff accepted");p=Program();p.parameters[0].type=hs::VfxParameterType::Int;Check(Run(p,i).empty(),"integer strength coerced");
 p=Program();p.parameters[0].bits=std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());Check(Run(p,i).empty(),"nonfinite strength accepted");
 p=Program();p.outputs[0].shape=Hash("radial_shock");Check(Run(p,i).empty(),"unsupported shape/motion accepted");p=Program();p.outputs[0].coverage_ref=0;Check(Run(p,i).empty(),"missing coverage semantics ignored");
 p=Program();p.sources[0].parameters={0,1};Check(Run(p,i).empty(),"source params ignored");p=Program();p.outputs[0].textures={1,1};Check(Run(p,i).empty(),"malformed texture range accepted");
 p=Program();p.effects[0].payload_kind=Hash("ConePayload");i.payload=hs::VfxConePayload{};Check(Run(p,i).empty(),"unsupported cone coerced");
 p=Program(hs::VfxMotionKind::OneTightPulse);hs::VfxProjectilePayload v;v.hitbox_radius=1;i.payload=v;Check(Run(p,i).empty(),"invalid projectile velocity accepted");
 p=Program();i=Input();i.geometry_owner_id=1;i.geometry_start_tick=100;i.geometry_end_tick=100;Check(Run(p,i).empty(),"invalid owner clock accepted");
}
}
int main(){try{GeometryClockAndIdentity();Presence();ExactBossGeometry();TextureContracts();Invalid();std::cout<<"Typed distortion commands passed\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
