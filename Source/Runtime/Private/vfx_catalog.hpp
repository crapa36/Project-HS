#pragma once

#include <hs/core/cooked_particle_effects.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/result.hpp>
#include <hs/render_data/render_snapshot_values.hpp>
#include <hs/render_data/vfx_spawn_command.hpp>
#include <hs/runtime/vfx_image.hpp>
#include <array>
#include <cstddef>

#include <optional>
#include <span>
#include <vector>

namespace hs::runtime_detail
{
struct PreparedVfxFrameBindings
{
    struct Persistent
    {
        std::uint64_t effect_id{};
        const VfxEffectRecord *effect{};
        std::uint32_t payload_kind{};
        bool fixed_stamp{};
    };
    struct SlowAreaAlias
    {
        std::uint8_t skill{}, source_upgrade{};
        Persistent binding;
        const VfxEffectRecord *base_supplement{};
    };
    struct AreaSupplement
    {
        PersistentVfxKind kind{};
        std::uint8_t skill{}, source_upgrade{};
        std::uint64_t effect_id{};
        const VfxEffectRecord *effect{};
    };
    struct DamageSupplement
    {
        std::uint8_t skill{}, source_upgrade{};
        std::uint64_t effect_id{};
        const VfxEffectRecord *effect{};
        bool fixed_duration{};
    };
    struct ArrowRain
    {
        const VfxEffectRecord *effect{};
        float head_radius{};
        Tick window_ticks{};
    };
    std::vector<Persistent> persistent;
    std::vector<SlowAreaAlias> slow_area_aliases;
    std::vector<AreaSupplement> area_supplements;
    std::vector<DamageSupplement> damage_supplements;
    // Stable gameplay ABI: nine skill ordinals, eight upgrades per skill.
    static constexpr std::size_t skill_count = 9;
    std::vector<std::array<std::uint8_t, skill_count>> upgrade_memberships;
    const VfxEffectRecord *wave_timer{};
    Tick wave_timer_window{};
    std::optional<ArrowRain> arrow_rain;

    [[nodiscard]] bool ContainsUpgrade(VfxEffectHandle handle, std::uint8_t skill,
                                       std::uint32_t ordinal) const noexcept;
};

[[nodiscard]] PreparedVfxFrameBindings PrepareVfxFrameBindings(const VfxImage &image);
}

namespace hs
{
struct VfxLegacyGroundSuppression
{
    bool all_particles{};
    bool ground_ring{};
    bool ground_disc{};
    bool segment_solid_trail{};
};

// Borrows the event and immutable catalog through the current activation only.
// Emitter offsets address caller-owned shared scratch, which may grow during preparation.
struct VfxPreparedLegacyEvent
{
    const PresentationEvent *event{};
    const CookedVfxDefinition *definition{};
    VfxEventParameters parameters{};
    Float3 forward{}, right{};
    std::size_t first_index{}, index_count{}, maximum_required_count{}, required_count{}, count{}, admitted_count{};
    std::uint32_t importance{};
    float decorative_fraction{1};
    bool decorative{}, particles_replaced{}, mesh_replaced{}, replacement_counts_ready{};
};

class VfxCatalog
{
  public:
    VfxCatalog() = default;
    VfxCatalog(const VfxCatalog &) = delete;
    VfxCatalog &operator=(const VfxCatalog &) = delete;
    // External immutable backing must outlive the catalog and its users.
    [[nodiscard]] Result Bind(const VfxImage &image);
    [[nodiscard]] const VfxImage &Image() const noexcept { return image_; }
    [[nodiscard]] const runtime_detail::PreparedVfxFrameBindings &FrameBindings() const noexcept
    { return frame_bindings_; }
    [[nodiscard]] const PreparedVfxFresnelRecipeView &FresnelRecipeView() const noexcept
    { return fresnel_recipe_view_; }
    [[nodiscard]] const PreparedVfxMeshRecipeView &MeshRecipeView() const noexcept
    { return mesh_recipe_view_; }
    [[nodiscard]] const CookedVfxDefinition *FindEffect(EffectId id) const noexcept;
    // A failed activation is terminal; discard its emitter scratch.
    [[nodiscard]] Result PrepareEvent(const PresentationEvent &event, float decorative_fraction,
        VfxPreparedLegacyEvent &prepared, std::vector<std::uint32_t> &emitter_indices,
        bool particles_replaced = false, bool mesh_replaced = false,
        VfxLegacyGroundSuppression ground_suppression = {}) const;
    [[nodiscard]] Result SetPreparedReplacements(VfxPreparedLegacyEvent &prepared,
        std::span<const std::uint32_t> emitter_indices,
        bool particles_replaced, bool mesh_replaced) const;
    [[nodiscard]] Result AppendPreparedEvent(const VfxPreparedLegacyEvent &prepared,
        std::span<const std::uint32_t> emitter_indices,
        std::vector<ParticleSpawnCommand> &particles, std::vector<VfxLineSpawnCommand> &lines) const;
    [[nodiscard]] std::uint64_t PayloadHash() const noexcept { return image_.payload_hash; }
    [[nodiscard]] const VfxProgramView &Program() const noexcept { return image_.program; }
  private:
    VfxImage image_{};
    runtime_detail::PreparedVfxFrameBindings frame_bindings_;
    PreparedVfxFresnelRecipeView fresnel_recipe_view_{};
    PreparedVfxMeshRecipeView mesh_recipe_view_{};
};

} // namespace hs
