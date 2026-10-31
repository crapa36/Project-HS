#include "renderer_brixelizer_scene.hpp"
#include <hs/render_data/material_ir.hpp>
#include <hs/render_data/render_snapshot_values.hpp>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace hs;
namespace {
void Check(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
void Validation(){
 SceneGeometryRecord g{1,2,0,3,0,76,3,0,0,1,0,9,0,SceneGeometryDeformed};
 SceneDrawInstance i{};i.mesh=9;i.scale={1,1,1,1};
 Check(bool(ValidateSceneGeometryRecord(g,i)),"valid source rejected");
 const auto bad=[&](auto edit){auto x=g;auto y=i;edit(x,y);Check(!ValidateSceneGeometryRecord(x,y),"malformed excluded source accepted");};
 bad([](auto& x,auto&){x.vertex_descriptor=0;});
 bad([](auto& x,auto&){x.index_descriptor=render_data::kGeometryDescriptorCount;});
 bad([](auto& x,auto&){x.vertex_stride=12;});
 bad([](auto& x,auto&){x.vertex_byte_offset=1;});
 bad([](auto& x,auto&){x.vertex_count=2;});
 bad([](auto& x,auto&){x.index_count=4;});
 bad([](auto& x,auto&){x.instance_generation=0;});
 bad([](auto& x,auto&){x.environment_ordinal=16;});
 bad([](auto& x,auto&){x.flags=8;});
 bad([](auto& x,auto&){x.flags=SceneGeometryCachedSkin|SceneGeometryDeformed;});
 bad([](auto& x,auto&){x.mesh_id=0;});
 bad([](auto&,auto& y){y.yaw=std::numeric_limits<float>::quiet_NaN();});
 bad([](auto&,auto& y){y.padding=std::numeric_limits<float>::infinity();});
 bad([](auto&,auto& y){y.position.x=std::numeric_limits<float>::quiet_NaN();});
 bad([](auto&,auto& y){y.scale.z=std::numeric_limits<float>::infinity();});
 bad([](auto&,auto& y){y.scale.x=0;});
 bad([](auto&,auto& y){y.scale.y=-1;});
 bad([](auto& x,auto&){x.vertex_count=UINT32_MAX;});
 bad([](auto& x,auto&){x.first_index=UINT32_MAX;});
 g.flags=SceneGeometryCachedSkin;Check(bool(ValidateSceneGeometryRecord(g,i)),"valid skin rejected");
}
void StaticBuiltinTerrainShape(){
 const auto check=[&](RenderMesh mesh,float factor){
  SceneGeometryRecord geometry{};geometry.mesh_id=static_cast<std::uint32_t>(mesh);
  geometry.flags=SceneGeometryBuiltin|SceneGeometryDeformed;
  SceneDrawInstance instance{};instance.mesh=static_cast<std::uint32_t>(mesh);
  instance.scale={2.0f,4.0f,3.0f,1.0f};
  Check(FoldStaticBuiltinTerrainShape(geometry,instance),"static builtin terrain shape was not folded");
  Check(instance.scale.x==2.0f&&instance.scale.y==4.0f*factor&&instance.scale.z==3.0f&&
   (geometry.flags&SceneGeometryDeformed)==0&&(geometry.flags&SceneGeometryBuiltin)!=0,
   "static builtin terrain transform or flags changed");
  Check(!FoldStaticBuiltinTerrainShape(geometry,instance)&&instance.scale.y==4.0f*factor,
   "static builtin terrain shape was applied more than once");
 };
 check(RenderMesh::Ground,0.1f);check(RenderMesh::DirtPatch,0.035f);
 SceneGeometryRecord wind_geometry{};wind_geometry.mesh_id=static_cast<std::uint32_t>(RenderMesh::Grass);
 wind_geometry.flags=SceneGeometryBuiltin|SceneGeometryDeformed;
 SceneDrawInstance wind{};wind.mesh=wind_geometry.mesh_id;wind.scale={1.0f,1.0f,1.0f,1.0f};
 Check(!FoldStaticBuiltinTerrainShape(wind_geometry,wind)&&wind_geometry.flags==
  (SceneGeometryBuiltin|SceneGeometryDeformed)&&wind.scale.y==1.0f,
  "wind builtin deformation was altered");
 SceneGeometryRecord skin_geometry{};skin_geometry.mesh_id=static_cast<std::uint32_t>(RenderMesh::Ground);
 skin_geometry.flags=SceneGeometryBuiltin|SceneGeometryCachedSkin;
 SceneDrawInstance skin{};skin.mesh=skin_geometry.mesh_id;skin.scale={1.0f,1.0f,1.0f,1.0f};
 Check(!FoldStaticBuiltinTerrainShape(skin_geometry,skin)&&skin_geometry.flags==
  (SceneGeometryBuiltin|SceneGeometryCachedSkin)&&skin.scale.y==1.0f,
  "cached skin row was altered");
 SceneGeometryRecord tiny_geometry{};tiny_geometry.mesh_id=static_cast<std::uint32_t>(RenderMesh::Ground);
 tiny_geometry.flags=SceneGeometryBuiltin|SceneGeometryDeformed;
 SceneDrawInstance tiny{};tiny.mesh=tiny_geometry.mesh_id;
 tiny.scale={1.0f,std::numeric_limits<float>::denorm_min(),1.0f,1.0f};
 Check(!FoldStaticBuiltinTerrainShape(tiny_geometry,tiny)&&tiny_geometry.flags==
  (SceneGeometryBuiltin|SceneGeometryDeformed)&&tiny.scale.y==std::numeric_limits<float>::denorm_min(),
  "underflowing terrain scale did not retain the deformed fallback");
}
void DeformationSections(){
 std::array<SceneGeometryRecord,5> rows{{
  {1,2,0,3,0,76,3,91,10,1,0,9,~0u,0},
  {1,2,0,6,0,76,4,777,11,1,0,9,~0u,SceneGeometryDeformed},
  {1,2,0,3,0,76,3,888,12,1,4,9,~0u,SceneGeometryCachedSkin},
  {1,2,0,9,0,76,6,999,13,1,0,9,~0u,SceneGeometryDeformed},
  {1,2,0,3,0,76,3,92,14,1,0,9,~0u,0}}};
 std::array<SceneDrawInstance,5> draws{};
 for(auto& draw:draws){draw.mesh=9;draw.scale={1,1,1,1};}
 std::vector<std::uint32_t> deformation_rows;
 std::uint64_t position_bytes{};
 Check(bool(PrepareSceneGeometryRows(rows,draws,deformation_rows,position_bytes)),"mixed scene row preparation failed");
 Check(deformation_rows==std::vector<std::uint32_t>{1,3},"deformation source rows lost logical order");
 Check(position_bytes==120&&rows[1].position_byte_offset==0&&rows[3].position_byte_offset==48,
  "deformed position offsets are not compact-prefix offsets");
 Check(rows[0].position_byte_offset==91&&rows[2].position_byte_offset==888&&rows[4].position_byte_offset==92,
  "non-deformed static/SkinCache rows were modified");
 for(std::size_t i=0;i<rows.size();++i)
  Check(rows[i].stable_gpu_scene_slot==10u+i,"logical row order or stable identity changed");

 SceneGeometryStaticSection cached;
 const SceneGeometryRecord immutable{1,2,0,3,0,76,3,91,20,1,0,9,~0u,0};
 const SceneGeometryRecord wind{1,2,0,6,0,76,4,777,21,1,0,9,~0u,SceneGeometryDeformed};
 SceneDrawInstance immutable_draw{};immutable_draw.mesh=9;immutable_draw.scale={1,1,1,1};
 auto wind_draw=immutable_draw;
 Check(bool(AppendSceneGeometryStaticRow(cached,immutable,immutable_draw))&&
  bool(AppendSceneGeometryStaticRow(cached,wind,wind_draw)),"static section construction failed");
 Check(cached.deformation_rows==std::vector<std::uint32_t>{1}&&
  cached.deformation_rank_by_row==std::vector<std::uint32_t>{UINT32_MAX,0},
  "static deformation rank differs from section order");
 std::array<SceneGeometryRecord,2> current{{
  {1,2,0,3,0,76,3,999,30,1,0,9,~0u,SceneGeometryDeformed},
  {1,2,0,3,0,76,3,888,31,1,0,9,~0u,SceneGeometryCachedSkin}}};
 std::array<SceneDrawInstance,2> current_draws{};
 for(auto &draw:current_draws){draw.mesh=9;draw.scale={1,1,1,1};}
 std::vector<std::uint32_t> compact_rows,static_offsets;std::uint64_t section_bytes{};
 Check(bool(PrepareSceneGeometrySections(current,current_draws,cached,compact_rows,static_offsets,section_bytes)),
  "static/current section preparation failed");
 Check(compact_rows==std::vector<std::uint32_t>{0,3}&&static_offsets==std::vector<std::uint32_t>{36}&&
  section_bytes==84,"section deformation order/prefix changed");
 Check(current[0].position_byte_offset==0&&cached.geometries[1].position_byte_offset==777&&
  cached.geometries[0].position_byte_offset==91,
  "section preparation mutated current/static source offsets incorrectly");
}
void DomainsAndCounts(){
 BrixelizerSceneInputs ingress{};
 Check(ingress.excluded_masked_count==0&&ingress.excluded_thin_count==0&&ingress.excluded_translucent_count==0,"compact ingress defaults");
 ingress.excluded_masked_count=5;ingress.excluded_thin_count=7;ingress.excluded_translucent_count=11;
 Check(bool(AddBrixelizerExcludedCount(ingress.excluded_masked_count,1))&&bool(AddBrixelizerExcludedCount(ingress.excluded_masked_count,3)),"mixed masked accumulation");
 Check(bool(AddBrixelizerExcludedCount(ingress.excluded_thin_count,2))&&bool(AddBrixelizerExcludedCount(ingress.excluded_translucent_count,4)),"mixed domain accumulation");
 Check(ingress.excluded_masked_count==9&&ingress.excluded_thin_count==9&&ingress.excluded_translucent_count==15,"domain counts not independent");
 for(auto* c:{&ingress.excluded_masked_count,&ingress.excluded_thin_count,&ingress.excluded_translucent_count}){
  *c=UINT32_MAX-1;Check(bool(AddBrixelizerExcludedCount(*c,1))&&*c==UINT32_MAX,"domain exact capacity");
  Check(!AddBrixelizerExcludedCount(*c,1)&&*c==UINT32_MAX,"domain overflow mutated count");}
 using D=BrixelizerGeometryDomain;
 const auto thin=static_cast<std::uint32_t>(render_data::MaterialClosureCapability::ThinTransmission);
 const auto solid=static_cast<std::uint32_t>(render_data::MaterialClosureCapability::SolidTransmission);
 Check(ClassifyBrixelizerDomain(0,false,false)==D::Opaque,"opaque classification");
 Check(ClassifyBrixelizerDomain(thin,false,false)==D::UnsupportedThin,"thin classification");
 Check(ClassifyBrixelizerDomain(solid,false,false)==D::UnsupportedThin,"solid classification");
 Check(ClassifyBrixelizerDomain(thin,true,false)==D::UnsupportedMasked,"mask precedence");
 Check(ClassifyBrixelizerDomain(thin,true,true)==D::Translucent,"transmission precedence");
 SoftwareSceneGeometry geometry{};geometry.stable_key=1;geometry.generation=1;geometry.current_revision=7;geometry.index_count=9;
 SoftwareSceneInstance instance{};instance.generation=1;instance.material_id=9;
 std::array<render_data::GeometrySubmesh,3> parts{{{0,3,0,0,0},{3,3,0,0,1},{6,3,0,0,2}}};
 std::array<std::uint64_t,3> submesh_keys{BrixelizerSubmeshKey(geometry.stable_key,0),
  BrixelizerSubmeshKey(geometry.stable_key,1),BrixelizerSubmeshKey(geometry.stable_key,2)};
 Check(submesh_keys[0]!=0&&submesh_keys[1]!=0&&submesh_keys[2]!=0,"precomputed submesh key is zero");
 std::array<render_data::MaterialSurfaceLayout,7> layouts{};
 for(auto n:{0u,1u,3u})for(auto mode:{0u,1u,2u}){
  BrixelizerGeometryLookup lookup;std::vector<SoftwareSceneGeometry> gs;std::vector<BrixelizerSceneInstance> is;
  const bool masked=mode==0,transmissive=mode==2;
  Check(bool(AppendBrixelizerLeaf(geometry,instance,std::span(parts).first(n),std::span(submesh_keys).first(n),{},layouts,thin,masked,transmissive,{},{},3,lookup,gs,is)),"legacy excluded expansion failed");
  std::uint32_t compact=0;Check(bool(AddBrixelizerExcludedCount(compact,n)),"compact count failed");
  Check(compact==is.size()&&gs.empty(),"compact count differs from original expansion");
  for(auto& x:is)Check(x.domain==ClassifyBrixelizerDomain(thin,masked,transmissive)&&x.geometry_index==UINT32_MAX,"excluded domain parity");
 }
 std::uint32_t count=UINT32_MAX-1;Check(bool(AddBrixelizerExcludedCount(count,1))&&count==UINT32_MAX,"exact maximum count");
 Check(!AddBrixelizerExcludedCount(count,1)&&count==UINT32_MAX,"overflow must reject without mutation");
 Check(bool(AddBrixelizerExcludedCount(count,0))&&count==UINT32_MAX,"empty submesh count");
 // Mixed hero pages stay expanded: opaque, authored cutout and transmission.
 std::array<render_data::CharacterMaterialBinding,3> bindings{{{0,0,0,render_data::HeroMaterialDomain::Opaque},{1,0,2,render_data::HeroMaterialDomain::Opaque},{2,0,5,render_data::HeroMaterialDomain::Translucent}}};
 BrixelizerGeometryLookup lookup;std::vector<SoftwareSceneGeometry> gs;std::vector<BrixelizerSceneInstance> is;
 Check(bool(AppendBrixelizerLeaf(geometry,instance,parts,submesh_keys,bindings,layouts,0,false,false,{},{},3,lookup,gs,is)),"mixed hero expansion");
 Check(gs.size()==1&&gs[0].stable_key==submesh_keys[0]&&is.size()==3&&is[0].domain==D::Opaque&&is[1].domain==D::UnsupportedMasked&&is[2].domain==D::Translucent,"mixed hero domain/range/key changed");
 Check(gs[0].index_count==3&&gs[0].index_byte_offset==0,"opaque range changed");
 auto second=instance;second.stable_gpu_scene_slot=1;
 Check(bool(AppendBrixelizerLeaf(geometry,second,parts,submesh_keys,bindings,layouts,0,false,false,{},{},3,lookup,gs,is)),"second same-mesh instance failed");
 Check(gs.size()==1&&is.size()==6&&is[0].geometry_index==0&&is[3].geometry_index==0,"same mesh instances did not reuse keyed geometry");
 auto missing=std::span(bindings).first(2);BrixelizerGeometryLookup missingLookup;
 std::vector<SoftwareSceneGeometry> missing_geometries;std::vector<BrixelizerSceneInstance> missing_instances;
 Check(!AppendBrixelizerLeaf(geometry,instance,parts,submesh_keys,missing,layouts,0,false,false,{},{},3,missingLookup,missing_geometries,missing_instances),"missing hero binding accepted");
 BrixelizerGeometryLookup mismatchLookup;std::vector<SoftwareSceneGeometry> mismatch_geometries;std::vector<BrixelizerSceneInstance> mismatch_instances;
 Check(!AppendBrixelizerLeaf(geometry,instance,parts,std::span(submesh_keys).first(2),bindings,layouts,0,false,false,{},{},3,mismatchLookup,mismatch_geometries,mismatch_instances),"mismatched submesh keys accepted");
 auto zero_keys=submesh_keys;zero_keys[1]=0;BrixelizerGeometryLookup zeroKeyLookup;
 std::vector<SoftwareSceneGeometry> zero_geometries;std::vector<BrixelizerSceneInstance> zero_instances;
 Check(!AppendBrixelizerLeaf(geometry,instance,parts,zero_keys,bindings,layouts,0,false,false,{},{},3,zeroKeyLookup,zero_geometries,zero_instances),"zero submesh key accepted");
}
void OrderedInstanceRanges(){
 using D=BrixelizerGeometryDomain;
 std::array<BrixelizerSceneInstance,2> first{{
  {11,1,1,4,5,6,{}, {}, {},D::Opaque},
  {12,1,UINT32_MAX,5,6,7,{}, {}, {},D::UnsupportedMasked}}};
 std::array<BrixelizerSceneInstance,2> second{{
  {13,1,0,6,7,8,{}, {}, {},D::AuthoredCoverage},
  {14,1,UINT32_MAX,7,8,9,{}, {}, {},D::Translucent}}};
 const BrixelizerSceneInstanceRange empty{{},false};
 std::array<BrixelizerSceneInstanceRange,3> ranges{{{first,true},empty,{second,false}}};
 std::uint32_t count{},immutable_count{};
 Check(bool(CountBrixelizerSceneInstanceRanges(ranges,count,immutable_count))&&count==4&&immutable_count==2,
  "ordered spans failed count admission");
 std::array<std::uint32_t,4> expected_slots{11,12,13,14};
 std::uint32_t visited{};
 Check(bool(ForEachBrixelizerSceneInstance(ranges,false,
  [&](const BrixelizerSceneInstance &instance,std::uint32_t logical,bool immutable)->Result {
   Check(logical==visited&&instance.stable_gpu_scene_slot==expected_slots[visited]&&immutable==(visited<2),
    "ordered borrowed range traversal changed logical order/index");
   ++visited;return Result::Success();
  }))&&visited==4,"ordered borrowed range traversal failed");
 visited=0;
 Check(bool(ForEachBrixelizerSceneInstance(ranges,true,
  [&](const BrixelizerSceneInstance &instance,std::uint32_t logical,bool immutable)->Result {
   Check(!immutable&&logical==visited+2&&instance.stable_gpu_scene_slot==expected_slots[visited+2],
    "immutable warm traversal failed to skip its rows or preserve logical indices");
   ++visited;return Result::Success();
  }))&&visited==2,"immutable warm traversal changed current-row order");
 std::uint32_t total{};
 Check(!AccumulateBrixelizerSceneInstanceCount(UINT32_MAX,1,total),"instance count overflow accepted");
}
}
int main()try{Validation();StaticBuiltinTerrainShape();DeformationSections();DomainsAndCounts();OrderedInstanceRanges();std::cout<<"GI source validation and compact exclusion parity PASS\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
