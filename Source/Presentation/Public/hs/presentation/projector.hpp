#pragma once

#include <hs/core/input.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/result.hpp>
#include <hs/render_data/render_snapshot.hpp>
#include <hs/core/settings.hpp>
#include <hs/game_domain/domain_signal.hpp>
#include <hs/game_domain/tick_read_view.hpp>
#include <hs/presentation/presentation_catalog.hpp>

#include <array>
#include <optional>
#include <memory>
#include <span>
#include <cstdint>
#include <string>
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
    std::uint8_t settings_group{}; // Seven definition groups, then Audio/Input.
    std::uint8_t settings_page{};
    std::uint8_t character_details_page{};
};

// Runtime publishes immutable requested intent and actual renderer receipts.
// Presentation owns no device queries and never treats an unqueried choice as available.
struct GraphicsUiChoice
{
    SettingId setting{};
    std::string token, label, reason;
    bool available{};
};
struct GraphicsUiStatus { SettingId setting{}; std::string effective, reason; };
struct GraphicsSettingsUiModel
{
    GraphicsSettings requested;
    std::vector<GraphicsUiChoice> choices;
    std::vector<GraphicsUiStatus> resolved;
    std::string transaction_state;
    // Runtime formats native observations; Presentation owns only these bounded
    // read-only labels and never queries renderer/device state.
    std::array<std::string, 3> telemetry_status;
    std::uint64_t generation{};
    bool pending{};
    std::optional<std::uint32_t> frame_generation_maximum;
};

// One immutable owner for the displayed models and the facts that interpret clicks.
struct UiFrame
{
    Sequence generation{};
    std::uint64_t session_id{}, selection_epoch{}, dynamic_pair_revision{};
    SessionProbe session{};
    PresentationUiState ui{};
    SettingsData settings{};
    std::shared_ptr<const GraphicsSettingsUiModel> graphics_model;
    std::uint8_t modal_phase{}, pending_rebind_slot{0xFF};
    bool input_rejected{};
    std::vector<UiModel> draw_models;
};
inline constexpr std::size_t kGraphicsUiRowsPerPage = 10;
// Main-menu title + panel + tabs + rows + telemetry + footer + display modal.
inline constexpr std::size_t kGraphicsUiSnapshotReserve = 2 + 8 + 5 * kGraphicsUiRowsPerPage + 3 + 5 + 4;
inline constexpr float kGraphicsUiRowY = 250, kGraphicsUiRowStride = 56;
inline constexpr std::size_t kCharacterUpgradesPerPage = 4, kCharacterRelicsPerPage = 4;
[[nodiscard]] std::size_t CharacterDetailsPageCount(const SessionProbe &, const PresentationUiState &) noexcept;
[[nodiscard]] std::vector<const GraphicsSettingDescriptor *> GraphicsSettingsVisibleRows(
    const PresentationUiState &);
[[nodiscard]] std::size_t GraphicsSettingsPageCount(std::uint8_t group);
[[nodiscard]] std::string GraphicsSettingDisplayLabel(const GraphicsSettingDescriptor &);
[[nodiscard]] std::string GraphicsSettingDisplayValue(SettingId, std::string_view);
[[nodiscard]] Float2 ReferenceUiCursor(Float2 client_pixels, GraphicsExtent client_extent) noexcept;
[[nodiscard]] bool GraphicsSettingTokenAvailable(SettingId, std::string_view, const GraphicsSettingsUiModel *);
[[nodiscard]] std::optional<std::string> NextGraphicsSettingToken(
    const GraphicsSettings &, SettingId, int direction, const GraphicsSettingsUiModel *);

// Updated once per simulation tick, independently of snapshot availability.
class EnemyAnimationState
{
  public:
    struct Sample
    {
        std::optional<Tick> recoil_start;
        bool attack_recoil{};
        std::optional<Tick> release_tick;
        std::optional<Float3> release_direction;
        std::optional<float> cancel_progress;
    };
    struct DeathPose
    {
        std::uint64_t id{};
        EnemyKind kind{};
        Float3 position{};
        Float3 direction{};
        Tick started{};
    };
    void Update(const TickReadView &model, std::span<const DomainSignal> signals);
    void Update(const GameReadModel &model, std::span<const DomainSignal> signals)
    { Update(development::BorrowOwnedReadModel(model), signals); }
    [[nodiscard]] std::optional<Tick> RecoilStart(std::uint64_t id) const;
    [[nodiscard]] bool IsAttackRecoil(std::uint64_t id) const;
    [[nodiscard]] std::optional<Tick> ReleaseTick(std::uint64_t id) const;
    [[nodiscard]] std::optional<Float3> ReleaseDirection(std::uint64_t id) const;
    [[nodiscard]] std::optional<float> CancelProgress(std::uint64_t id) const;
    [[nodiscard]] Sample AnimationSample(std::uint64_t id) const;
    [[nodiscard]] std::span<const DeathPose> DeathPoses() const { return deaths_; }
    [[nodiscard]] Sequence PlayerMotionEpoch() const noexcept { return player_motion_epoch_; }

  private:
    struct LivingPose { std::int32_t health{}; std::optional<Tick> recoil; std::optional<Tick> release; Float3 release_direction{}; bool seen{}; bool attacking{}; Tick attack_started{}, attack_resolve{}; std::optional<Tick> cancel_started; float cancel_progress{}; bool attack_recoil{}; };
    std::unordered_map<std::uint64_t, LivingPose> living_;
    std::vector<DeathPose> deaths_;
    Tick tick_{};
    std::uint64_t seed_{}, session_id_{};
    Sequence player_motion_epoch_{};
    SessionPhase phase_{};
    bool initialized_{};
};

// Advances only on actual simulation samples; render retries cannot emit audio.
class PlayerFootstepTimeline
{
  public:
    void Reset(const TickReadView &model, Sequence motion_epoch = 0) noexcept;
    [[nodiscard]] std::optional<PresentationEvent> Advance(
        const TickReadView &model, Sequence motion_epoch = 0) noexcept;

  private:
    Tick tick_{};
    std::uint64_t session_id_{};
    Sequence motion_epoch_{};
    bool initialized_{};
};

[[nodiscard]] std::size_t ProjectDomainSignal(
    const DomainSignal &signal, std::span<PresentationEvent> output,
    float arena_half_extent = 0.0f) noexcept;
[[nodiscard]] UiInteraction ResolveUiInteraction(const SessionProbe &session,
                                                 PresentationUiState &ui,
                                                 const SettingsData &settings,
                                                 Float2 cursor_normalized,
                                                 const GraphicsSettingsUiModel *graphics_ui = nullptr);
[[nodiscard]] Result ProjectDynamicTick(const TickReadView &model,
                                        RenderSnapshotStorage &snapshot,
                                        std::shared_ptr<const StaticRenderScene> environment_scene,
                                        const EnemyAnimationState *enemy_animations = nullptr);
void ProjectTickUi(const TickReadView &model,
                   const PresentationCatalog &presentation,
                   const PresentationUiState &ui,
                   const SettingsData &settings,
                   RenderSnapshotStorage &snapshot,
                   std::uint8_t pending_rebind_slot = 0xFF,
                   const GraphicsSettingsUiModel *graphics_ui = nullptr);
[[nodiscard]] Result ProjectTick(const TickReadView &model,
                                         const PresentationCatalog &presentation,
                                         const PresentationUiState &ui,
                                         const SettingsData &settings,
                                         RenderSnapshotStorage &snapshot,
                                         std::shared_ptr<const StaticRenderScene> environment_scene,
                                         std::uint8_t pending_rebind_slot = 0xFF,
                                         const EnemyAnimationState *enemy_animations = nullptr,
                                         const GraphicsSettingsUiModel *graphics_ui = nullptr);

// Owned-format development fixture adapter; Runtime projects a scoped TickReadView.
[[nodiscard]] Result ProjectRenderSnapshot(const GameReadModel &model,
                                         const PresentationCatalog &presentation,
                                         const PresentationUiState &ui,
                                         const SettingsData &settings,
                                         RenderSnapshotStorage &snapshot,
                                         std::uint8_t pending_rebind_slot = 0xFF,
                                         const EnemyAnimationState *enemy_animations = nullptr,
                                         std::shared_ptr<const StaticRenderScene> environment_scene = {},
                                         const GraphicsSettingsUiModel *graphics_ui = nullptr);

} // namespace hs
