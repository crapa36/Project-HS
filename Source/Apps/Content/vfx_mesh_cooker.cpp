#include "vfx_mesh_cooker.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>
namespace hs::content {
namespace {
using Json=nlohmann::json;
std::uint32_t Hash(const std::string &s){std::uint32_t h=2166136261u;for(unsigned char c:s){h^=c;h*=16777619u;}return h;}
Json Read(const std::filesystem::path &source) {
 std::ifstream stream(source);auto doc=Json::parse(stream);
 if(doc.at("version")!=1 || doc.at("unit_meters")!=1 || doc.at("axis_forward")!="+Z") throw std::runtime_error("VFX atlas convention mismatch");
 constexpr std::array names{"arrowhead_mesh","shard_mesh","debris_shard_mesh","small_shard_mesh","enemy_thorn_mesh","boss_crest_lance_mesh","needle_shard_mesh"};
 const auto &meshes=doc.at("meshes");if(meshes.size()!=names.size())throw std::runtime_error("VFX atlas requires seven shapes");
 for(std::size_t m=0;m<meshes.size();++m){const auto &mesh=meshes[m];const auto &vertices=mesh.at("vertices");
  if(mesh.at("id")!=names[m] || vertices.empty() || vertices.size()%3 || vertices.size()>65535 || mesh.at("indices").size()!=vertices.size())throw std::runtime_error("VFX mesh table invalid");
  for(std::size_t i=0;i<vertices.size();++i){const auto &v=vertices[i];if(v.size()!=12 || mesh.at("indices")[i]!=i)throw std::runtime_error("VFX atlas vertex layout invalid");
   for(const auto &x:v)if(!x.is_number()||!std::isfinite(x.get<float>()))throw std::runtime_error("VFX atlas nonfinite vertex");
   float norm=0,tangent=0;for(unsigned j=0;j<3;++j){norm+=v[3+j].get<float>()*v[3+j].get<float>();tangent+=v[8+j].get<float>()*v[8+j].get<float>();}
   if(std::abs(norm-1)>0.01f||std::abs(tangent-1)>0.01f||std::abs(v[11].get<float>())!=1)throw std::runtime_error("VFX mesh tangent basis invalid");
  }
  for(std::size_t i=0;i<vertices.size();i+=3){std::array<float,3>a{},b{},cross{};for(unsigned j=0;j<3;++j){a[j]=vertices[i+1][j].get<float>()-vertices[i][j].get<float>();b[j]=vertices[i+2][j].get<float>()-vertices[i][j].get<float>();}cross={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};if(cross[0]*cross[0]+cross[1]*cross[1]+cross[2]*cross[2]<=1e-16f)throw std::runtime_error("VFX atlas degenerate triangle");}
 }return doc;
}
}
void ValidateVfxMeshAtlas(const std::filesystem::path &source){Read(source);}
void CookVfxMeshAtlas(const std::filesystem::path &source,const std::filesystem::path &output){const auto doc=Read(source);std::vector<char> bytes;
 const auto word=[&](std::uint32_t v){for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<char>((v>>(8*i))&255));};
 std::uint32_t count=0;for(const auto &m:doc.at("meshes"))count+=static_cast<std::uint32_t>(m.at("vertices").size());
 word(0x4d565348u);word(1);word(7);word(count);std::uint32_t first=0;
 for(const auto &m:doc.at("meshes")){word(Hash(m.at("id")));word(first);word(static_cast<std::uint32_t>(m.at("vertices").size()));first+=static_cast<std::uint32_t>(m.at("vertices").size());}
 for(const auto &m:doc.at("meshes"))for(const auto &v:m.at("vertices"))for(const auto &x:v)word(std::bit_cast<std::uint32_t>(x.get<float>()));
 std::ofstream stream(output,std::ios::binary|std::ios::trunc);stream.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));if(!stream)throw std::runtime_error("Cannot write VFX mesh atlas");
}
}
