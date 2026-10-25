#include "vfx_catalog.hpp"
#include <hs/runtime/vfx_image_io.hpp>

namespace hs
{
Result VfxCatalog::Load(const std::filesystem::path &path, VfxCatalog &catalog)
{
    return Load(path, path.parent_path() / "vfx_program.hsbin", catalog);
}
Result VfxCatalog::Load(const std::filesystem::path &path,
                        const std::filesystem::path &program_path, VfxCatalog &catalog)
{
    VfxCatalog prepared;
    prepared.owned_ = std::make_unique<VfxImageData>();
    if (auto result = LoadVfxImage(path, program_path, *prepared.owned_); !result) return result;
    prepared.image_ = prepared.owned_->View();
    catalog = std::move(prepared);
    return Result::Success();
}
}
