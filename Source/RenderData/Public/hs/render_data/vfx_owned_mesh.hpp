#pragma once
#include <hs/core/result.hpp>
#include <hs/render_data/vfx_frame_input.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace hs
{
// Fixed catalog-authored material and mesh selection for an owned projectile mesh.
struct VfxMeshRecipe
{
    std::uint32_t effect_handle{}, source_index{}, output_index{}, source_stable_id{};
    std::uint32_t mesh_index{}, min_quality{}, gradient_row{};
    Float4 color{};
    float hdr{}, fresnel{}, head_to_tail_ratio{}, shell_thickness{};
};

class PreparedVfxMeshRecipeView
{
  public:
    [[nodiscard]] static Result Create(std::span<const VfxMeshRecipe> rows,
                                       PreparedVfxMeshRecipeView &output)
    {
        if (rows.size() > (std::numeric_limits<std::uint32_t>::max)())
            return Result::Failure(ErrorCode::InvalidArgument, "hs_render_data",
                                   "Owned mesh recipe table exceeds the command index domain.");
        std::uint32_t maximum_gradient_row{};
        for (const auto &row : rows)
        {
            if (!row.effect_handle || row.min_quality > 2 || row.mesh_index == 0 || row.mesh_index > 7 ||
                !std::isfinite(row.color.x) || !std::isfinite(row.color.y) ||
                !std::isfinite(row.color.z) || !std::isfinite(row.color.w) ||
                !std::isfinite(row.hdr) || row.hdr <= 0.0f ||
                !std::isfinite(row.fresnel) || row.fresnel < 0.0f || row.fresnel > 1.0f ||
                !std::isfinite(row.head_to_tail_ratio) || row.head_to_tail_ratio < 0.0f ||
                row.head_to_tail_ratio >= 1.0f || !std::isfinite(row.shell_thickness) ||
                row.shell_thickness < 0.0f)
                return Result::Failure(ErrorCode::InvalidArgument, "hs_render_data",
                                       "Invalid admitted owned mesh recipe row.");
            maximum_gradient_row = std::max(maximum_gradient_row, row.gradient_row);
        }
        output.rows_ = rows;
        output.maximum_gradient_row_ = maximum_gradient_row;
        output.admitted_ = true;
        return Result::Success();
    }

    [[nodiscard]] std::span<const VfxMeshRecipe> Rows() const noexcept { return rows_; }
    [[nodiscard]] std::uint32_t MaximumGradientRow() const noexcept { return maximum_gradient_row_; }
    [[nodiscard]] bool IsAdmitted() const noexcept { return admitted_; }
    [[nodiscard]] bool Matches(std::span<const VfxMeshRecipe> rows) const noexcept
    { return rows_.data() == rows.data() && rows_.size() == rows.size(); }

  private:
    std::span<const VfxMeshRecipe> rows_;
    std::uint32_t maximum_gradient_row_{};
    bool admitted_{};
};

struct VfxOwnedMeshInput
{
    std::uint64_t owner_id{};
    Float3 position{}, previous_position{}, velocity{};
    float radius{};
    std::uint32_t recipe_index{};
};
}
