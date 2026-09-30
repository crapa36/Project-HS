#include "../Source/Runtime/Private/vfx_typed_light_commands.hpp"
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
hs::VfxProgramData Program()
{
 hs::VfxProgramData p;hs::VfxEffectRecord e;e.handle=1;e.payload_kind=Hash("PointEventPayload");e.seconds=2;e.importance=5;e.sources={0,1};p.effects.push_back(e);p.effect_lookup.push_back({0xabcdef1234ull,1});
 hs::VfxSourceRecord s;s.effect=1;s.stable_id=Hash("light_pulse");s.type=hs::VfxSourceType::Direct;s.knot_count=2;s.knots={.25f,.75f,0,0};s.outputs={0,1};p.sources.push_back(s);
 p.parameters={{Hash("radius"),hs::VfxParameterType::Float,std::bit_cast<std::uint32_t>(5.5f)},{Hash("intensity_curve_ref"),hs::VfxParameterType::CurveRow,0}};
 p.curves.push_back({1,Hash("normalized_age"),{0,2}});p.curve_keys={{0,1},{1,0}};
 hs::VfxOutputRecord o;o.source=0;o.profile=hs::VfxOutputProfile::Light;o.motion=hs::VfxMotionKind::SingleCappedLightPulse;o.parameters={0,2};o.rgba={.5f,.8f,1,.8f};o.hdr=3;p.outputs.push_back(o);return p;
}
hs::VfxEventInput Input(){hs::VfxEventInput in;in.effect_handle=1;in.event_tick=100;in.sequence=77;in.payload=hs::VfxPointPayload{{1,2,3},{},{},9};return in;}
auto Run(const hs::VfxProgramData&p,const hs::VfxEventInput&i,hs::Tick t){return hs::runtime_detail::BuildVfxTypedLightCommands(p,std::span(&i,1),t);}
void ClockCurvesAndIdentity()
{
 auto p=Program();auto i=Input();Check(Run(p,i,129).empty()&&Run(p,i,190).empty(),"source interval ignored");
 auto a=Run(p,i,145);Check(a.size()==1&&Near(a[0].intensity,2.025f)&&Near(a[0].radius,5.5f)&&a[0].linear_rgb.x==.5f&&a[0].position.y==2&&a[0].importance==5,"authored smoothstep, intensity or unscaled radius lost");
 i.importance_override=2;Check(Run(p,i,145)[0].importance==2&&Run(p,i,145)[0].stable_id==a[0].stable_id,"light importance override ignored or changed identity");
 i.importance_override=0;Check(Run(p,i,145)[0].importance==5,"zero importance override did not preserve recipe priority");
 Check(Run(p,i,145)[0].stable_id==a[0].stable_id&&Run(p,i,160)[0].stable_id==a[0].stable_id,"frame time changed light identity");
 p.curves[0].interpolation=0;Check(Near(Run(p,i,145)[0].intensity,1.8f),"linear curve not evaluated");
 p.curves[0].interpolation=1;p.outputs[0].motion=hs::VfxMotionKind::GentleGreenPulse;p.curve_keys[0].value=.7f;
 Check(Near(Run(p,i,145)[0].intensity,1.4175f),"soft light authored peak lost");
 p=Program();p.effects.push_back(p.effects[0]);p.effects[1].handle=2;p.sources[0].effect=2;p.effect_lookup[0].handle=2;i.effect_handle=2;
 Check(Run(p,i,145)[0].stable_id==a[0].stable_id,"catalog reorder changed semantic identity");
 i=Input();p=Program();i.geometry_owner_id=8;i.geometry_start_tick=100;i.geometry_end_tick=340;
 Check(Run(p,i,145).empty()&&Near(Run(p,i,190)[0].intensity,2.025f)&&Run(p,i,280).empty(),"light ignored actual owner duration");
 p.sources[0].knot_count=4;p.sources[0].knots={.25f,.4f,.6f,.75f};i=Input();
 Check(Near(Run(p,i,139)[0].intensity,1.1271f),"four-knot source envelope not multiplied with local curve");
}
void TypedCentersAndHero()
{
 auto p=Program();auto i=Input();p.outputs[0].motion=hs::VfxMotionKind::SingleHighPriorityLightPulse;p.outputs[0].parameters.count=1;
 p.parameters[0].bits=std::bit_cast<std::uint32_t>(6.5f);p.sources[0].knots={0,1,0,0};
 Check(Near(Run(p,i,100)[0].intensity,2.4f)&&Near(Run(p,i,112)[0].intensity,2.4f)&&Near(Run(p,i,166)[0].intensity,.6f),"hero plateau or quadratic fade changed");
 p.effects[0].payload_kind=Hash("PresentationContextPayload");hs::VfxContextPayload context;context.ratio01=.01f;i.payload=context;i.world_transform[12]=4;
 Check(Run(p,i,120)[0].position.x==4&&Near(Run(p,i,120)[0].radius,6.5f),"context ratio changed light radius");
 hs::VfxCirclePayload circle;circle.center={5,1,2};circle.radius=33;i.payload=circle;p.effects[0].payload_kind=Hash("CircleAreaPayload");
 Check(Run(p,i,120)[0].position.x==5&&Near(Run(p,i,120)[0].radius,6.5f),"circle geometry changed authored light radius");
 hs::VfxRingGapsPayload ring;ring.center={6,2,1};ring.inner_radius=3;ring.outer_radius=20;i.payload=ring;p.effects[0].payload_kind=Hash("RingWithGapsPayload");
 Check(Run(p,i,120)[0].position.x==6,"ring light center incorrectly coerced");
 hs::VfxConePayload cone;cone.origin={7,1,2};cone.direction={0,0,1};cone.range=10;cone.half_angle_degrees=35;i.payload=cone;p.effects[0].payload_kind=Hash("ConePayload");
 Check(Run(p,i,120)[0].position.x==7,"cone light did not use actual origin");
 p.effects[0].payload_kind=Hash("PointEventPayload");Check(Run(p,i,120).empty(),"light accepted schema mismatch");
}
void QualityAndInvalidInputs()
{
 auto p=Program();auto i=Input();i.quality=hs::VfxQuality::Low;Check(Run(p,i,145).empty(),"low quality retained decorative light");
 i.quality=hs::VfxQuality::Medium;p.outputs[0].min_quality=2;Check(Run(p,i,145).empty(),"output minimum quality ignored");
 i=Input();p=Program();p.curves[0].interpolation=2;Check(Run(p,i,145).empty(),"unsupported curve interpolation approximated");
 p=Program();p.curve_keys[1].time=0;Check(Run(p,i,145).empty(),"duplicate curve keys accepted");
 p=Program();p.curve_keys[1].value=std::numeric_limits<float>::quiet_NaN();Check(Run(p,i,145).empty(),"invalid curve value accepted");
 p=Program();p.parameters[0].type=hs::VfxParameterType::Int;Check(Run(p,i,145).empty(),"wrong radius type accepted");
 p=Program();p.sources[0].parameters={0,1};Check(Run(p,i,145).empty(),"unsupported source parameters ignored");
 p=Program();p.effects[0].input_mode=1;Check(Run(p,i,145).empty(),"persistent input mode coerced to event light");
 p=Program();i.geometry_owner_id=8;i.geometry_start_tick=100;i.geometry_end_tick=100;Check(Run(p,i,145).empty(),"invalid owner clock accepted");
 Check(hs::runtime_detail::BuildVfxTypedLightCommands(p,{},145).empty(),"cancelled event retained light");
}
}
int main(){try{ClockCurvesAndIdentity();TypedCentersAndHero();QualityAndInvalidInputs();std::cout<<"Typed light commands passed\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
