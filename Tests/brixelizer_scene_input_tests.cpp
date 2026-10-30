#include "renderer_brixelizer_scene.hpp"
#include <hs/render_data/material_ir.hpp>
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
}
int main()try{Validation();DomainsAndCounts();std::cout<<"GI source validation and compact exclusion parity PASS\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
