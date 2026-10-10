#pragma once

#include <hs/core/input.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/render_snapshot.hpp>
#include <hs/core/settings.hpp>
#include <hs/game_domain/domain_signal.hpp>
#include <hs/game_domain/game_read_model.hpp>
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
inline constexpr std::size_t kGraphicsUiRowsPerPage = 10;
// Main-menu title + panel + tabs + rows + telemetry + footer + display modal.
inline constexpr std::size_t kGraphicsUiSnapshotReserve = 2 + 8 + 5 * kGraphicsUiRowsPerPage + 3 + 5 + 4;
inline constexpr float kGraphicsUiRowY = 250, kGraphicsUiRowStride = 56;
[[nodiscard]] std::vector<const GraphicsSettingDescriptor *> GraphicsSettingsVisibleRows(
    const PresentationUiState &);
[[nodiscard]] std::size_t GraphicsSettingsPageCount(std::uint8_t group);
[[nodiscard]] std::string GraphicsSettingDisplayLabel(const GraphicsSettingDescriptor &);
[[nodiscard]] std::string GraphicsSettingDisplayValue(SettingId, std::string_view);
[[nodiscard]] Float2 ReferenceUiCursor(Float2 client_pixels, GraphicsExtent client_extent) noexcept;
[[nodiscard]] bool GraphicsSettingTokenAvailable(SettingId, std::string_view, const GraphicsSettingsUiModel *);
[[nodiscard]] std::optional<std::string> NextGraphicsSettingToken(
    const GraphicsSettings &, SettingId, int direction, const GraphicsSettingsUiModel *);

// Simulation-thread-owned cache. It compares the complete copied arena layout
// before reusing an immutable scene; snapshots retain their own scene lease.
class StaticEnvironmentSceneCache
{
  public:
    explicit StaticEnvironmentSceneCache(std::string content_generation_id);
    [[nodiscard]] std::shared_ptr<const StaticRenderScene> Resolve(
        const GameReadModel &model);

  private:
    struct Layout
    {
        std::uint64_t session_id{};
        std::uint64_t seed{};
        float arena_half_extent{};
        ArenaBoundary boundary{};
        std::vector<ArenaObstacle2D> obstacles;
    };
    [[nodiscard]] static bool SameLayout(const Layout &layout,
                                         const GameReadModel &model) noexcept;

    std::string content_generation_id_;
    std::optional<Layout> layout_;
    std::shared_ptr<const StaticRenderScene> scene_;
};

// Simulation-thread-owned incremental scene publisher. Build is transactional:
// the next delta becomes the comparison base only after Commit, matching the
// snapshot exchange's publish/abandon boundary.
class RenderSceneProjectionState
{
  public:
    [[nodiscard]] bool Build(std::span<const RenderInstance> instances,
                             std::uint64_t session_id,
                             RenderSceneDelta &delta);
    void Commit() noexcept;
    void Abort() noexcept;
    void Clear() noexcept;

  private:
    struct KeyHash
    {
        [[nodiscard]] std::size_t operator()(RenderSceneInstanceKey key) const noexcept;
    };

    std::unordered_map<RenderSceneInstanceKey, RenderInstance::Revisions, KeyHash> previous_;
    std::unordered_map<RenderSceneInstanceKey, RenderInstance::Revisions, KeyHash> pending_;
    std::uint64_t committed_session_id_{};
    std::uint64_t pending_session_id_{};
    std::uint64_t generation_{};
    std::uint64_t pending_generation_{};
    bool initialized_{};
    bool pending_valid_{};
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
    void Update(const GameReadModel &model, std::span<const DomainSignal> signals);
    [[nodiscard]] std::optional<Tick> RecoilStart(std::uint64_t id) const;
    [[nodiscard]] bool IsAttackRecoil(std::uint64_t id) const;
    [[nodiscard]] std::optional<Tick> ReleaseTick(std::uint64_t id) const;
    [[nodiscard]] std::optional<Float3> ReleaseDirection(std::uint64_t id) const;
    [[nodiscard]] std::optional<float> CancelProgress(std::uint64_t id) const;
    [[nodiscard]] std::span<const DeathPose> DeathPoses() const { return deaths_; }

  private:
    struct LivingPose { std::int32_t health{}; std::optional<Tick> recoil; std::optional<Tick> release; Float3 release_direction{}; bool seen{}; bool attacking{}; Tick attack_started{}, attack_resolve{}; std::optional<Tick> cancel_started; float cancel_progress{}; bool attack_recoil{}; };
    std::unordered_map<std::uint64_t, LivingPose> living_;
    std::vector<DeathPose> deaths_;
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
                                                 Float2 cursor_normalized,
                                                 const GraphicsSettingsUiModel *graphics_ui = nullptr);
[[nodiscard]] bool ProjectRenderSnapshot(const GameReadModel &model,
                                         const PresentationCatalog &presentation,
                                         const PresentationUiState &ui,
                                         const SettingsData &settings,
                                         RenderSnapshotStorage &snapshot,
                                         std::uint8_t pending_rebind_slot = 0xFF,
                                         const EnemyAnimationState *enemy_animations = nullptr,
                                         StaticEnvironmentSceneCache *environment_cache = nullptr,
                                         RenderSceneProjectionState *scene_state = nullptr,
                                         const GraphicsSettingsUiModel *graphics_ui = nullptr);

} // namespace hs
