#include "vfx_runtime_cooker.hpp"
#include "vfx_spec_validation.hpp"
#include "vfx_texture_validation.hpp"
#include "vfx_gradient_cooker.hpp"
#include "vfx_program_cooker.hpp"
#include <fstream>
namespace hs::content {
void CookVfxRuntimeContent(const std::filesystem::path &root,const std::filesystem::path &output) {
 std::ifstream stream(root/"ContentSource/VFX/production_spec.v4.json");const auto spec=nlohmann::json::parse(stream);
 ValidateVfxSpecification(spec);ValidateVfxTextures(spec,root);
 for(const auto &[id,texture]:spec.at("libraries").at("texture_catalog").items()) {
  const std::filesystem::path asset=texture.at("runtime_asset").get<std::string>();
  const auto destination=output/asset;std::filesystem::create_directories(destination.parent_path());
  std::filesystem::copy_file(root/"ContentSource/Textures/VFX"/asset.filename(),destination,std::filesystem::copy_options::overwrite_existing);
 }
 const auto rows=CookVfxGradientDefaults(spec,root/"ContentSource/Textures/VFX/vfx_gradient_lut.dds",output/"Content/Textures/VFX/vfx_gradient_lut.dds");
 CookVfxProgram(spec,rows,output/"vfx_program.hsbin");
}
}
