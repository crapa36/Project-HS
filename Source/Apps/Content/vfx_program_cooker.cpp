#include "vfx_program_cooker.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace hs::content {
namespace {
using Json=nlohmann::json;
using U32=std::uint32_t;
U32 Narrow(std::size_t n) { if(n>std::numeric_limits<U32>::max()) throw std::runtime_error("VFX program exceeds uint32 range"); return static_cast<U32>(n); }
U32 Hash(const std::string& s) { U32 h=2166136261u; for(unsigned char c:s) { h^=c; h*=16777619u; } return h; }
std::uint64_t AssetHash(const std::string& s) { std::uint64_t h=14695981039346656037ull; for(unsigned char c:s) { h^=c; h*=1099511628211ull; } return h; }
float Float(const Json& v) { const double d=v.get<double>(); if(!std::isfinite(d)||std::abs(d)>std::numeric_limits<float>::max()) throw std::runtime_error("VFX nonfinite float"); return static_cast<float>(d); }
U32 Enum(const std::string& s,std::initializer_list<const char*> values) { U32 i=0; for(auto v:values) {if(s==v)return i; ++i;} throw std::runtime_error("VFX unsupported enum: "+s); }
U32 Quality(const std::string& s) {return Enum(s,{"low","medium","high"});}
struct MotionContract { VfxMotionKind kind; float rate_hz; float amplitude; float inset_fraction; };
MotionContract Motion(const std::string& s) {
 static const std::map<std::string,MotionContract> motions = {
  {"3-5 shards lift and fall; never explosion-scale",{VfxMotionKind::ShardsLiftFall,0,0,0}},
  {"baked 6-way plume with optional MV frame interpolation",{VfxMotionKind::BakedSixWayPlume,0,0,0}},
  {"ballistic burst with gravity and spin",{VfxMotionKind::BallisticBurstGravitySpin,0,0,0}},
  {"boundary stays exact while pulse frequency accelerates toward resolve",{VfxMotionKind::BoundaryPulseAccelerates,0,0,0}},
  {"brief radial response",{VfxMotionKind::BriefRadialResponse,0,0,0}},
  {"buoyant turbulent smoke; low frequency only",{VfxMotionKind::BuoyantTurbulentSmoke,0,0,0}},
  {"chevrons appear behind motion and dissipate",{VfxMotionKind::ChevronsDissipate,0,0,0}},
  {"compress then snap outward",{VfxMotionKind::CompressSnapOutward,0,0,0}},
  {"curved link with bright traveling head",{VfxMotionKind::CurvedLinkTravelingHead,0,0,0}},
  {"curved recoil arc from bow hand",{VfxMotionKind::CurvedRecoilArc,0,0,0}},
  {"curves toward collector",{VfxMotionKind::CurvesTowardCollector,0,0,0}},
  {"directional turbulence away from boss",{VfxMotionKind::DirectionalTurbulence,0,0,0}},
  {"fast expansion at surface",{VfxMotionKind::FastExpansionSurface,0,0,0}},
  {"fast stamp-in then break-out",{VfxMotionKind::FastStampBreakout,0,0,0}},
  {"few particles pulled inward",{VfxMotionKind::ParticlesPulledInward,0,0,0}},
  {"fracture appears at peak then cools",{VfxMotionKind::FractureAtPeak,0,0,0}},
  {"fresnel shell contracts at end",{VfxMotionKind::FresnelShellContracts,0,0,0}},
  {"gameplay radius 내부에 희박하게 분산된 procedural flame tongue. +Y 상승 후 안쪽으로 휘며 boundary보다 낮은 시각 우선순위를 유지.",{VfxMotionKind::ProceduralFlameTongue,0,0,0}},
  {"gentle green pulse",{VfxMotionKind::GentleGreenPulse,0,0,0}},
  {"hex segments light in sequence then collapse",{VfxMotionKind::HexSegmentsCollapse,0,0,0}},
  {"history ribbon tears backward from boss",{VfxMotionKind::HistoryRibbonTearsBackward,0,0,0}},
  {"history-based tapered ribbon; width decays toward tail",{VfxMotionKind::HistoryRibbonTaperedFlow,0,0,0}},
  {"instant compression then axial split",{VfxMotionKind::InstantCompressionAxialSplit,0,0,0}},
  {"instant expansion then collapse",{VfxMotionKind::InstantExpansionCollapse,0,0,0}},
  {"instant point flash",{VfxMotionKind::InstantPointFlash,0,0,0}},
  {"inward-moving teeth pulse",{VfxMotionKind::InwardTeethPulse,0,0,0}},
  {"locked line with direction chevrons moving toward impact",{VfxMotionKind::LockedChevronsTowardImpact,0,0,0}},
  {"locked steady ring with subtle pulse",{VfxMotionKind::LockedSteadyRingPulse,0,0,0}},
  {"noise edge flows upward and opens at end",{VfxMotionKind::NoiseEdgeFlowsUpward,0,0,0}},
  {"one tight pulse",{VfxMotionKind::OneTightPulse,0,0,0}},
  {"optional transient directional arc",{VfxMotionKind::OptionalTransientArc,0,0,0}},
  {"quick heat shock",{VfxMotionKind::QuickHeatShock,0,0,0}},
  {"quick inward snap then edge fracture",{VfxMotionKind::QuickInwardFracture,0,0,0}},
  {"radial lobes with upward bias",{VfxMotionKind::RadialLobesUpward,0,0,0}},
  {"rare inward drift",{VfxMotionKind::RareInwardDrift,0,0,0}},
  {"reverse flow toward bow; width collapses",{VfxMotionKind::ReverseFlowBow,0,0,0}},
  {"ring contracts into subject then releases",{VfxMotionKind::RingContractsReleases,0,0,0}},
  {"scrolls in attack direction",{VfxMotionKind::ScrollsAttackDirection,0,0,0}},
  {"short ballistic fan away from normal",{VfxMotionKind::ShortBallisticFan,0,0,0}},
  {"short forward-biased shard fan",{VfxMotionKind::ShortForwardShardFan,0,0,0}},
  {"short line extends along incoming velocity",{VfxMotionKind::ShortLineIncomingVelocity,0,0,0}},
  {"short refractive shock",{VfxMotionKind::ShortRefractiveShock,0,0,0}},
  {"short turbulent rear wake",{VfxMotionKind::ShortTurbulentWake,0,0,0}},
  {"short upward spiral",{VfxMotionKind::ShortUpwardSpiral,0,0,0}},
  {"single capped light pulse",{VfxMotionKind::SingleCappedLightPulse,0,0,0}},
  {"single high-priority light pulse",{VfxMotionKind::SingleHighPriorityLightPulse,0,0,0}},
  {"single outward refractive pulse",{VfxMotionKind::SingleOutwardRefractivePulse,0,0,0}},
  {"single short pulse",{VfxMotionKind::SingleShortPulse,0,0,0}},
  {"single-frame snap then shrink",{VfxMotionKind::SingleFrameSnapShrink,0,0,0}},
  {"slow counter-rotation; contraction pulse",{VfxMotionKind::SlowCounterRotationContraction,0,0,0}},
  {"slow counter-rotation; stable gameplay footprint",{VfxMotionKind::SlowCounterRotationStableFootprint,0,0,0}},
  {"slow inward curl around torso",{VfxMotionKind::SlowInwardCurl,0,0,0}},
  {"slow pulse locked to state, not camera motion",{VfxMotionKind::SlowPulseStateLocked,0,0,0}},
  {"slow radial flow; never brighter than edge",{VfxMotionKind::SlowRadialFlow,0,0,0}},
  {"slow stable rotation",{VfxMotionKind::SlowStableRotation,0,0,0}},
  {"slow upward helix",{VfxMotionKind::SlowUpwardHelix,0,0,0}},
  {"snaps inward then upward",{VfxMotionKind::SnapsInwardUpward,0,0,0}},
  {"snaps outward along aim direction",{VfxMotionKind::SnapsOutwardAim,0,0,0}},
  {"soft wider sheath",{VfxMotionKind::SoftWiderSheath,0,0,0}},
  {"sparse rearward sparks",{VfxMotionKind::SparseRearwardSparks,0,0,0}},
  {"stable boundary with subtle inward pulse",{VfxMotionKind::StableBoundaryInwardPulse,.8f,.22f,.12f}},
  {"static projected residue; fades with effect lifetime",{VfxMotionKind::StaticProjectedResidue,0,0,0}},
  {"stretches briefly along projectile direction",{VfxMotionKind::StretchesProjectileDirection,0,0,0}},
  {"strong snap synchronized to gameplay resolve",{VfxMotionKind::StrongSnapGameplayResolve,0,0,0}},
  {"subtle fresnel pulse",{VfxMotionKind::SubtleFresnelPulse,0,0,0}},
  {"surface noise edge travels through boss silhouette; synchronized to transition/death",{VfxMotionKind::SurfaceNoiseThroughBoss,0,0,0}},
  {"thin shards continue forward cone",{VfxMotionKind::ThinShardsForwardCone,0,0,0}},
  {"ticks rotate slightly then lock at final 20%",{VfxMotionKind::TicksRotateLock,0,0,0}},
  {"tight distortion around head only",{VfxMotionKind::TightHeadDistortion,0,0,0}},
  {"two phase-offset narrow wakes",{VfxMotionKind::TwoPhaseNarrowWakes,0,0,0}},
  {"two-frame snap perpendicular to incoming velocity",{VfxMotionKind::TwoFrameSnapPerpendicular,0,0,0}},
  {"velocity aligned",{VfxMotionKind::VelocityAligned,0,0,0}},
  {"velocity aligned heavy lance",{VfxMotionKind::VelocityAlignedHeavyLance,0,0,0}},
  {"velocity-aligned; sharp head, narrow tail",{VfxMotionKind::VelocityAlignedSharpHead,0,0,0}},
  {"very short local drift",{VfxMotionKind::VeryShortLocalDrift,0,0,0}}
 };
 const auto it=motions.find(s); if(it==motions.end()) throw std::runtime_error("VFX unsupported motion: "+s); return it->second;
}
struct Bytes {
 std::vector<char> data;
 void u(U32 v){for(unsigned i=0;i<4;++i)data.push_back(static_cast<char>((v>>(8*i))&255));}
 void f(float v){u(std::bit_cast<U32>(v));}
 void range(VfxRange r){u(r.first);u(r.count);}
};
struct Compiler {
 const Json& spec; const Json& lib; const Json& rows;
 std::map<U32,std::string> registry;
 std::map<std::string,U32> slots,curve_rows;
 std::vector<VfxEffectRecord> effects; std::vector<VfxSourceRecord> sources;
 std::vector<VfxOutputRecord> outputs; std::vector<VfxParameterRecord> params;
 std::vector<VfxTextureBindingRecord> textures; std::vector<U32> slices;
 std::vector<VfxCurveRecord> curves; std::vector<VfxCurveKey> keys;
 Json debug={{"effects",Json::array()},{"unresolved_execution_semantics",Json::array()}};
 U32 id(const std::string& s) { if(s.empty())return 0;auto h=Hash(s);if(!h)throw std::runtime_error("VFX reserved zero hash");auto [it,ok]=registry.emplace(h,s);if(!ok&&it->second!=s)throw std::runtime_error("VFX identifier hash collision: "+s);return h; }
 VfxRange parameters(const Json& object) {
  VfxRange r{Narrow(params.size()),Narrow(object.size())};
  for(const auto& [key,v]:object.items()) {
   const auto type=spec.at("schemas").at("parameter_catalog").at(key).at("type").get<std::string>();
   VfxParameterRecord p{}; p.key=id(key);
   if(type=="float"||(type=="float_or_enum"&&v.is_number())){p.type=VfxParameterType::Float;p.bits=std::bit_cast<U32>(Float(v));}
   else if(type=="int") {const auto n=v.get<std::int64_t>();if(n<INT32_MIN||n>INT32_MAX)throw std::runtime_error("VFX integer overflow");p.type=VfxParameterType::Int;p.bits=std::bit_cast<U32>(static_cast<std::int32_t>(n));}
   else if(type=="bool"){p.type=VfxParameterType::Bool;p.bits=v.get<bool>()?1u:0u;}
   else if(type=="curve_ref"){p.type=VfxParameterType::CurveRow;p.bits=curve_rows.at(v.get<std::string>());}
   else if(type=="enum"||type=="enum_or_id"||type=="float_or_enum"){p.type=VfxParameterType::Enum;p.bits=id(v.get<std::string>());}
   else throw std::runtime_error("VFX unsupported parameter type: "+type);
   params.push_back(p);
  }return r;
 }
 void texture(const std::string& resource,const std::string& role,const Json& object) {
  VfxTextureBindingRecord t{};t.role=id(role);t.catalog_slot=slots.at(resource);
  t.selection=id(object.value("selection",std::string{}));t.min_quality=Quality(object.value("quality",std::string("low")));t.strength=Float(object.value("strength",Json(1.0)));
  const auto& catalog=lib.at("texture_catalog").at(resource);
  t.slices.first=Narrow(slices.size());
  if(object.contains("slice_group"))for(const auto& name:object.at("slice_group")) {
   bool found=false;for(const auto& slice:catalog.at("slice_map"))if(slice.at("name")==name){slices.push_back(slice.at("slice").get<U32>());found=true;break;}
   if(!found)throw std::runtime_error("VFX unresolved texture slice");
  }
  t.slices.count=Narrow(slices.size())-t.slices.first;
  if(object.contains("clip")){const auto& clip=catalog.at("clip_map").at(object.at("clip").get<std::string>());t.first_frame=clip.at("first_slice").get<U32>();t.frame_count=clip.at("frame_count").get<U32>();t.fps=Float(clip.at("fps"));}
  textures.push_back(t);
 }
 void build() {
  if(spec.at("schema_version")!=4)throw std::runtime_error("VFX compiler requires schema v4");
  for(const auto& [name,unused]:lib.at("texture_catalog").items()){(void)unused;slots.emplace(name,Narrow(slots.size()));}
  for(const auto& [name,c]:lib.at("curve_library").items()){
   curve_rows.emplace(name,Narrow(curves.size()));VfxCurveRecord r{};r.interpolation=Enum(c.at("interpolation"),{"linear","smoothstep","cubic"});r.domain=id(c.at("domain"));r.keys={Narrow(keys.size()),Narrow(c.at("keys").size())};
   for(const auto& k:c.at("keys"))keys.push_back({Float(k.at(0)),Float(k.at(1))});curves.push_back(r);
  }
  std::vector<const Json*> sorted;for(const auto& e:spec.at("effects"))sorted.push_back(&e);
  std::sort(sorted.begin(),sorted.end(),[](auto a,auto b){return a->at("id").template get<std::string>()<b->at("id").template get<std::string>();});
  std::string previous;
  for(const auto* ep:sorted){const auto& e=*ep;const auto name=e.at("id").get<std::string>();if(name==previous)throw std::runtime_error("VFX duplicate effect");previous=name;
   VfxEffectRecord er{};er.handle=Narrow(effects.size())+1;er.input_mode=Enum(e.at("input_mode"),{"event_stream","persistent_snapshot"});er.importance=e.at("importance").get<U32>();er.role=id(e.at("role"));er.style=id(e.at("visual").at("style"));er.readability=id(e.at("readability"));
   const auto& gameplay=e.at("gameplay");const auto& binding=gameplay.contains("binding")?gameplay.at("binding"):lib.at("binding_profiles").at(gameplay.at("binding_ref").get<std::string>());
   er.payload_kind=id(binding.at("payload_schema"));er.anchor=id(binding.at("anchor_source"));er.orientation=id(binding.at("orientation_source"));er.scale=id(binding.at("scale_source"));er.binding_timing=id(binding.at("timing_source"));er.geometry=id(binding.at("geometry"));er.must_match_logic=binding.at("must_match_logic").get<bool>()?1u:0u;
   const auto& timing=e.at("timing");er.timing_kind=Enum(timing.at("source"),{"fixed","gameplay"});if(er.timing_kind==0)er.seconds=Float(timing.at("seconds"));else if(timing.contains("fallback_seconds")&&!timing.at("fallback_seconds").is_null())throw std::runtime_error("VFX gameplay fallback timing requires explicit contract");
   er.sources.first=Narrow(sources.size());debug["effects"].push_back({{"id",name},{"handle",er.handle},{"visual",e.at("visual")}});
   for(const auto& v:e.at("visual_elements")){
    VfxSourceRecord sr{};sr.effect=er.handle;sr.stable_id=id(v.at("id"));sr.type=static_cast<VfxSourceType>(Enum(v.at("source").at("type"),{"direct","projectile_follow","ballistic_collision","curl_motes","impact_sprite","history_ribbon","volume_local"}));
    const auto knots=v.value("t",Json::array({0.0,1.0}));if(knots.size()!=2&&knots.size()!=4)throw std::runtime_error("VFX requires two or four lifecycle knots");sr.knot_count=Narrow(knots.size());for(std::size_t i=0;i<knots.size();++i)sr.knots[i]=Float(knots[i]);
    sr.parameters=parameters(v.at("source").value("params",Json::object()));sr.outputs.first=Narrow(outputs.size());
    for(const auto& a:gameplay.value("authoritative",Json::array()))if(a==v.at("id"))sr.authoritative=1;
    for(const auto& o:v.at("outputs")){
     VfxOutputRecord out{};out.source=Narrow(sources.size());out.profile=static_cast<VfxOutputProfile>(Enum(o.at("profile"),{"sprite_sdf_add","mesh_emissive_oit","sprite_add","ribbon_add","ground_sdf_soft","ground_sdf_add","distortion","ground_sdf_oit","sprite_flame_oit","light","ribbon_oit","local_volume","decal","sprite_noise_oit","screen_overlay","entity_surface","fresnel_shell_oit","fresnel_shell_emissive","sprite_smoke_6way_oit"}));
     const Json empty_shape={{"id",""},{"domain",""},{"scale_rule",""},{"component_kind",""},{"construction",""}};
     const auto& shape=!o.contains("shape_inline")&&!o.contains("shape")?empty_shape:o.contains("shape_inline")?o.at("shape_inline"):lib.at("shape_library").at(o.at("shape").get<std::string>());
     out.shape=id(o.contains("shape_inline")?shape.at("id").get<std::string>():o.value("shape",std::string{}));out.shape_domain=id(shape.at("domain"));out.shape_scale_rule=id(shape.at("scale_rule"));out.shape_component_kind=id(shape.at("component_kind"));
     const auto style=e.at("visual").at("style").get<std::string>();const auto& rgb=lib.at("style_families").at(style).at("colors").at(o.at("color").get<std::string>()).at("linear_rgb");for(std::size_t i=0;i<3;++i)out.rgba[i]=Float(rgb.at(i));out.rgba[3]=Float(o.at("alpha"));out.hdr=Float(o.at("hdr"));
     const auto gradient=o.value("gradient",std::string("energy_clean"));bool found=false;for(const auto& row:rows)if(row.at("style")==style&&row.at("gradient")==gradient){out.gradient_row=row.at("row").get<U32>();found=true;break;}if(!found)throw std::runtime_error("VFX gradient row missing");
     out.min_quality=Quality(o.value("min_quality",std::string("low")));out.surface_target=id(o.value("surface_target",std::string{}));out.parameters=parameters(o.value("params",Json::object()));out.textures.first=Narrow(textures.size());
     const auto& profile=lib.at("render_profiles").at(o.at("profile").get<std::string>());for(const auto& [kind,resources]:profile.at("texture_bindings").items())if(resources.is_array())for(const auto& resource:resources)texture(resource,"profile."+kind,Json::object());
     if(o.contains("coverage_source")){const auto& c=o.at("coverage_source");out.coverage_type=id(c.at("type"));out.coverage_ref=id(c.value("function",c.value("shape_ref",std::string{})));if(c.contains("resource"))texture(c.at("resource"),"coverage",c);if(c.contains("resources"))for(const auto& resource:c.at("resources"))texture(resource,"coverage",c);}
     if(o.contains("detail_sources"))for(const auto& detail:o.at("detail_sources"))texture(detail.at("resource"),detail.at("type"),detail);
     if(o.contains("frame_motion_source")){const auto& motion=o.at("frame_motion_source");texture(motion.at("resource"),motion.at("type"),motion);}
     out.textures.count=Narrow(textures.size())-out.textures.first;
     const auto motion=Motion(o.at("motion").get<std::string>());out.motion=motion.kind;out.motion_rate_hz=motion.rate_hz;out.motion_amplitude=motion.amplitude;out.motion_inset_fraction=motion.inset_fraction;
     debug["unresolved_execution_semantics"].push_back({{"effect",name},{"source",v.at("id")},{"output",outputs.size()},{"motion",o.at("motion")},{"motion_kind",static_cast<U32>(out.motion)},{"shape_construction",shape.at("construction")}});
     outputs.push_back(out);
    }sr.outputs.count=Narrow(outputs.size())-sr.outputs.first;sources.push_back(sr);
   }er.sources.count=Narrow(sources.size())-er.sources.first;effects.push_back(er);
  }
 }
};
}

nlohmann::json CookVfxProgram(const Json& spec,const Json& gradient_rows,const std::filesystem::path& output_file) {
 Compiler c{spec,spec.at("libraries"),gradient_rows};c.build();
 std::array<Bytes,13> sections;
 for(const auto& e:c.effects){auto& b=sections[0];b.u(e.handle);b.u(e.input_mode);b.u(e.importance);b.u(e.role);b.u(e.style);b.u(e.readability);b.u(e.payload_kind);b.u(e.anchor);b.u(e.orientation);b.u(e.scale);b.u(e.binding_timing);b.u(e.geometry);b.u(e.must_match_logic);b.u(e.timing_kind);b.f(e.seconds);b.range(e.sources);}
 for(const auto& s:c.sources){auto& b=sections[1];b.u(s.effect);b.u(s.stable_id);b.u(static_cast<U32>(s.type));b.u(s.knot_count);for(auto k:s.knots)b.f(k);b.range(s.parameters);b.range(s.outputs);b.u(s.authoritative);}
 for(const auto& o:c.outputs){auto& b=sections[2];b.u(o.source);b.u(static_cast<U32>(o.profile));b.u(o.shape);b.u(o.shape_domain);b.u(o.shape_scale_rule);b.u(o.shape_component_kind);for(auto v:o.rgba)b.f(v);b.f(o.hdr);b.u(o.gradient_row);b.u(o.min_quality);b.u(o.coverage_type);b.u(o.coverage_ref);b.u(o.surface_target);b.range(o.parameters);b.range(o.textures);b.u(static_cast<U32>(o.motion));b.f(o.motion_rate_hz);b.f(o.motion_amplitude);b.f(o.motion_inset_fraction);}
 for(const auto& p:c.params){auto& b=sections[3];b.u(p.key);b.u(static_cast<U32>(p.type));b.u(p.bits);}
 for(const auto& t:c.textures){auto& b=sections[4];b.u(t.role);b.u(t.catalog_slot);b.u(t.selection);b.u(t.min_quality);b.f(t.strength);b.range(t.slices);b.u(t.first_frame);b.u(t.frame_count);b.f(t.fps);}
 for(auto s:c.slices)sections[5].u(s);
 for(const auto& curve:c.curves){auto& b=sections[6];b.u(curve.interpolation);b.u(curve.domain);b.range(curve.keys);}
 for(const auto& key:c.keys){sections[7].f(key.time);sections[7].f(key.value);}
 std::map<std::string,U32> effect_handles;
 for(const auto& e:c.debug.at("effects")){const auto name=e.at("id").get<std::string>();const auto handle=e.at("handle").get<U32>();effect_handles.emplace(name,handle);const auto id=AssetHash(name);sections[8].u(static_cast<U32>(id));sections[8].u(static_cast<U32>(id>>32));sections[8].u(handle);}
 for(const auto& [name,slot]:c.slots){const auto asset=c.lib.at("texture_catalog").at(name).at("runtime_asset").get<std::string>();sections[9].u(slot);sections[9].u(Narrow(sections[10].data.size()));sections[9].u(Narrow(asset.size()));sections[10].data.insert(sections[10].data.end(),asset.begin(),asset.end());}
 for(const auto& binding:spec.at("skill_upgrade_visual_bindings")){const auto id=AssetHash(binding.at("skill_id"));sections[11].u(static_cast<U32>(id));sections[11].u(static_cast<U32>(id>>32));sections[11].u(binding.at("upgrade_ordinal").get<U32>());sections[11].u(Narrow(sections[12].data.size()/4));sections[11].u(Narrow(binding.at("vfx_sequence").size()));for(const auto& effect:binding.at("vfx_sequence"))sections[12].u(effect_handles.at(effect.get<std::string>()));}
 const std::array<U32,13> counts{Narrow(c.effects.size()),Narrow(c.sources.size()),Narrow(c.outputs.size()),Narrow(c.params.size()),Narrow(c.textures.size()),Narrow(c.slices.size()),Narrow(c.curves.size()),Narrow(c.keys.size()),Narrow(effect_handles.size()),Narrow(c.slots.size()),Narrow(sections[10].data.size()),Narrow(spec.at("skill_upgrade_visual_bindings").size()),Narrow(sections[12].data.size()/4)};
 Bytes payload;U32 offset=13u*16u; // directory is inside checksummed payload; offsets relative to payload.
 for(std::size_t i=0;i<sections.size();++i){payload.u(static_cast<U32>(i)+1);payload.u(counts[i]);payload.u(offset);payload.u(Narrow(sections[i].data.size()));if(sections[i].data.size()>UINT32_MAX-offset)throw std::runtime_error("VFX section offset overflow");offset+=Narrow(sections[i].data.size());}
 for(const auto& section:sections)payload.data.insert(payload.data.end(),section.data.begin(),section.data.end());
 U32 checksum=2166136261u;for(unsigned char byte:payload.data){checksum^=byte;checksum*=16777619u;}
 Bytes file;file.u(0x34565348u);file.u(5);file.u(spec.at("schema_version").get<U32>());file.u(spec.at("engine_contract_version").get<U32>());file.u(spec.at("minimum_runtime_version").get<U32>());file.u(13);file.u(Narrow(payload.data.size()));file.u(checksum);file.data.insert(file.data.end(),payload.data.begin(),payload.data.end());
 if(!output_file.parent_path().empty())std::filesystem::create_directories(output_file.parent_path());
 std::ofstream output(output_file,std::ios::binary|std::ios::trunc);output.write(file.data.data(),static_cast<std::streamsize>(file.data.size()));output.close();if(!output)throw std::runtime_error("Cannot write VFX program");
 c.debug["format_version"]=5;c.debug["checksum_fnv1a32"]=checksum;c.debug["byte_count"]=file.data.size();c.debug["runtime_ready"]=false;
 c.debug["section_counts"]=counts;c.debug["section_stride_words"]={17,13,24,3,10,1,4,2,3,3,0,5,1};
 c.debug["string_section_stride_bytes"]=1;
 c.debug["registry"]=Json::array();for(const auto& [value,name]:c.registry)c.debug["registry"].push_back({{"id",value},{"name",name}});
 c.debug["texture_catalog_slots"]=c.slots;c.debug["curve_rows"]=c.curve_rows;
 c.debug["execution_contract_note"]="Shape IDs, source types and profile IDs require matching renderer implementations. Natural-language motion/shape construction is retained here, never interpreted or silently approximated at runtime.";
 return c.debug;
}
}
