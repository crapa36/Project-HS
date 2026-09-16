#pragma once

#include <hs/core/input.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/render_snapshot.hpp>
#include <hs/core/settings.hpp>
#include <hs/game_domain/domain_signal.hpp>
#include <hs/game_domain/game_read_model.hpp>
#include <hs/presentation/presentation_catalog.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace hs
{

struct UiInteraction
{
    std::optional<UiAction> gameplay_action;
    std::optional<UiCommand> runtime_command;
    bool invalid{};
};

enum class UiPage : std::uint8_t
{
    Root = 0,
    Collection = 1,
    MainMenuSettings = 2,
    CharacterOverview = 3,
    CharacterSkills = 4,
    CharacterStats = 5,
    PauseSettings = 6,
};

struct PresentationUiState
{
    UiPage page{UiPage::Root};
    std::uint8_t selected_collection_skill{};
    std::uint8_t selected_character_skill{};
    std::uint8_t loadout_source_slot{0xFF};
};

// Updated once per simulation tick, independently of snapshot availability.
class EnemyAnimationState
{
  public:
    struct DeathPose
    {
        std::uint64_t id{};
        EnemyKind kind{};
        Float3 position{};
        Float3 direction{};
        Tick started{};
    };

    void Update(const GameReadModel &model, std::span<const DomainSignal> signals)
    {
        constexpr Tick kStopTicks = 24;
        constexpr Tick kHitTicks = 18;
        const bool reset = initialized_ &&
            (model.tick < tick_ || model.seed != seed_ ||
             (model.session.phase == SessionPhase::MainMenu &&
              phase_ != SessionPhase::MainMenu));
        if (reset)
        {
            player_stop_.reset();
            player_hit_.reset();
            player_locomotion_blend_ = 0.0f;
        }

        UpdateLegacy(model, signals);

        if (player_stop_ &&
            (model.tick < *player_stop_ || model.tick - *player_stop_ >= kStopTicks))
            player_stop_.reset();
        if (player_hit_ &&
            (model.tick < *player_hit_ || model.tick - *player_hit_ >= kHitTicks))
            player_hit_.reset();

        const auto blend = model.player.locomotion_blend;
        const bool forced_move = model.player.forced_move_skill != SkillKind::Count &&
                                 model.tick < model.player.retreat_until;
        if (!forced_move && !player_stop_ && player_locomotion_blend_ > 0.05f &&
            blend + 0.001f < player_locomotion_blend_)
            player_stop_ = model.tick;
        if (forced_move || blend > player_locomotion_blend_ + 0.001f)
            player_stop_.reset();
        player_locomotion_blend_ = blend;

        for (const auto &signal : signals)
        {
            if (signal.kind == DomainSignalKind::PlayerDamaged &&
                signal.tick <= model.tick && model.tick - signal.tick < kHitTicks)
                player_hit_ = signal.tick;
        }
    }

    [[nodiscard]] std::optional<Tick> RecoilStart(std::uint64_t id) const;
    [[nodiscard]] std::optional<Tick> ReleaseTick(std::uint64_t id) const;
    [[nodiscard]] std::optional<Float3> ReleaseDirection(std::uint64_t id) const;
    [[nodiscard]] std::optional<Tick> PlayerStopStart() const noexcept
    {
        return player_stop_;
    }
    [[nodiscard]] std::optional<Tick> PlayerHitStart() const noexcept
    {
        return player_hit_;
    }
    [[nodiscard]] std::span<const DeathPose> DeathPoses() const { return deaths_; }

  private:
    void UpdateLegacy(const GameReadModel &model, std::span<const DomainSignal> signals);

    struct LivingPose
    {
        std::int32_t health{};
        std::optional<Tick> recoil;
        std::optional<Tick> release;
        Float3 release_direction{};
        bool seen{};
    };
    std::unordered_map<std::uint64_t, LivingPose> living_;
    std::vector<DeathPose> deaths_;
    std::optional<Tick> player_stop_;
    std::optional<Tick> player_hit_;
    float player_locomotion_blend_{};
    Tick tick_{};
    std::uint64_t seed_{};
    SessionPhase phase_{};
    bool initialized_{};
};

[[nodiscard]] std::size_t ProjectDomainSignal(
    const DomainSignal &signal, std::span<PresentationEvent> output) noexcept;
[[nodiscard]] UiInteraction ResolveUiInteraction(const SessionProbe &session,
                                                 PresentationUiState &ui,
                                                 const SettingsData &settings,
                                                 Float2 cursor_normalized);
[[nodiscard]] bool ProjectRenderSnapshot(const GameReadModel &model,
                                         const PresentationCatalog &presentation,
                                         const PresentationUiState &ui,
                                         const SettingsData &settings,
                                         RenderSnapshotStorage &snapshot,
                                         std::uint8_t pending_rebind_slot = 0xFF,
                                         const EnemyAnimationState *enemy_animations = nullptr);

} // namespace hs
