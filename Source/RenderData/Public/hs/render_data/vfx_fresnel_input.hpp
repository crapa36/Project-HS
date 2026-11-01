#pragma once

#include <hs/core/result.hpp>
#include <hs/render_data/vfx_frame_input.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace hs
{
enum class VfxFresnelShellKind : std::uint8_t
{
    BossCrown,
    BossTransition,
    PlayerInvulnerable,
};

// Immutable fixed attributes admitted once with a VFX catalog. Command rows
// refer to this table by index and carry only live owner/time data.
struct VfxFresnelRecipe
{
    std::uint32_t effect_handle{}, source_index{}, source_stable_id{}, output_index{}, min_quality{};
    VfxFresnelShellKind kind{};
    float knot_start{}, knot_end{};
    std::uint32_t sector_count{}, end_crack{};
    float fresnel_power{}, rotation_hz{}, noise_amount{};
    Float4 color{};
    float hdr{};
    std::uint32_t gradient_row{};
    float motion_rate_hz{}, motion_amplitude{}, motion_inset_fraction{};
};

class PreparedVfxFresnelRecipeView
{
  public:
    [[nodiscard]] static Result Create(std::span<const VfxFresnelRecipe> rows,
                                       PreparedVfxFresnelRecipeView &output)
    {
        std::uint32_t maximum_gradient_row{};
        for (const auto &row : rows)
        {
            const bool crown = row.kind == VfxFresnelShellKind::BossCrown;
            const bool transition = row.kind == VfxFresnelShellKind::BossTransition;
            const bool player = row.kind == VfxFresnelShellKind::PlayerInvulnerable;
            if (!row.effect_handle || row.min_quality > 2 ||
                (!crown && !transition && !player) ||
                !std::isfinite(row.knot_start) || !std::isfinite(row.knot_end) ||
                row.knot_start < 0.0f || row.knot_end > 1.0f || row.knot_end <= row.knot_start ||
                !std::isfinite(row.color.x) || row.color.x < 0.0f ||
                !std::isfinite(row.color.y) || row.color.y < 0.0f ||
                !std::isfinite(row.color.z) || row.color.z < 0.0f ||
                !std::isfinite(row.color.w) || row.color.w < 0.0f || row.color.w > 1.0f ||
                !std::isfinite(row.hdr) || row.hdr <= 0.0f ||
                !std::isfinite(row.motion_rate_hz) || row.motion_rate_hz < 0.0f ||
                !std::isfinite(row.motion_amplitude) || row.motion_amplitude < 0.0f || row.motion_amplitude > 1.0f ||
                !std::isfinite(row.motion_inset_fraction) || row.motion_inset_fraction < 0.0f ||
                row.motion_inset_fraction > 1.0f || !std::isfinite(row.noise_amount) ||
                row.noise_amount < 0.0f || row.noise_amount > 1.0f ||
                (crown && (row.sector_count != 6 || !std::isfinite(row.rotation_hz) ||
                           row.rotation_hz >= 0.0f || row.fresnel_power != 0.0f ||
                           row.end_crack != 0 || row.noise_amount != 0.0f)) ||
                (transition && (row.sector_count != 0 || row.rotation_hz != 0.0f ||
                                !std::isfinite(row.fresnel_power) || row.fresnel_power <= 0.0f ||
                                row.end_crack != 1 || row.noise_amount != 0.0f)) ||
                (player && (row.sector_count != 0 || row.rotation_hz != 0.0f ||
                            !std::isfinite(row.fresnel_power) || row.fresnel_power <= 0.0f ||
                            row.end_crack != 0))
                )
                return Result::Failure(ErrorCode::InvalidArgument, "hs_render_data",
                                       "Invalid admitted Fresnel recipe row.");
            maximum_gradient_row = std::max(maximum_gradient_row, row.gradient_row);
        }
        output.rows_ = rows;
        output.maximum_gradient_row_ = maximum_gradient_row;
        output.admitted_ = true;
        return Result::Success();
    }

    [[nodiscard]] std::span<const VfxFresnelRecipe> Rows() const noexcept { return rows_; }
    [[nodiscard]] std::uint32_t MaximumGradientRow() const noexcept { return maximum_gradient_row_; }
    [[nodiscard]] bool IsAdmitted() const noexcept { return admitted_; }
    [[nodiscard]] bool Matches(std::span<const VfxFresnelRecipe> rows) const noexcept
    { return rows_.data() == rows.data() && rows_.size() == rows.size(); }

  private:
    std::span<const VfxFresnelRecipe> rows_;
    std::uint32_t maximum_gradient_row_{};
    bool admitted_{};
};

struct VfxFresnelInput
{
    std::uint64_t render_instance_id{};
    std::uint64_t stable_id{};
    std::uint32_t recipe_index{};
    VfxTransform current_transform{kVfxIdentityTransform};
    VfxTransform previous_transform{kVfxIdentityTransform};
    float footprint_radius{};
    float source_progress{};
    float elapsed_seconds{};
};
}
