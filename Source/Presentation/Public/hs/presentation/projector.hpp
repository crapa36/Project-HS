#pragma once

#include <hs/core/input.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/render_snapshot.hpp>
#include <hs/core/settings.hpp>
#include <hs/game_domain/domain_signal.hpp>
#include <hs/game_domain/game_read_model.hpp>
#include <hs/presentation/presentation_catalog.hpp>

#include <optional>
#include <span>
#include <cstdint>
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
    struct PlayerHitPose
    {
        Tick started{};
        CharacterAnimationClip clip{CharacterAnimationClip::HitBack};
    };

    void Update(const GameReadModel &model, std::span<const DomainSignal> signals);
    [[nodiscard]] std::optional<Tick> RecoilStart(std::uint64_t id) const;
    [[nodiscard]] std::optional<Tick> ReleaseTick(std::uint64_t id) const;
    [[nodiscard]] std::optional<Float3> ReleaseDirection(std::uint64_t id) const;
    [[nodiscard]] std::optional<Tick> PlayerStopStart() const noexcept
    {
        return player_stop_;
    }
    [[nodiscard]] std::optional<PlayerHitPose> PlayerHit() const noexcept
    {
        return player_hit_;
    }
    [[nodiscard]] std::span<const DeathPose> DeathPoses() const { return deaths_; }

  private:
    struct LivingPose { std::int32_t health{}; std::optional<Tick> recoil; std::optional<Tick> release; Float3 release_direction{}; bool seen{}; };
    std::unordered_map<std::uint64_t, LivingPose> living_;
    std::vector<DeathPose> deaths_;
    std::optional<Tick> player_stop_;
    std::optional<PlayerHitPose> player_hit_;
    float player_locomotion_blend_{};
    bool player_was_forced_{};
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
