#pragma once

#include <hs/core/presentation_event.hpp>
#include <hs/render_data/render_snapshot.hpp>
#include <hs/render_data/vfx_frame_input.hpp>
#include <hs/runtime/vfx_image.hpp>

#include <cstdint>
#include <cstddef>
#include <span>
#include <unordered_map>
#include <vector>

namespace hs::runtime_detail
{

struct VfxTypedFrameInputs
{
    std::vector<VfxEventInput> events;
    std::vector<VfxPersistentInput> persistent;
    std::vector<std::uint64_t> unsupported_effect_ids;
};

// Indexes ordered row identities of the retained publication pair, never its addresses.
// Duplicate identities retain the first source-order match; unidentified inputs rebuild.
class PreparedPairFacts
{
  public:
    struct Key
    {
        std::uint64_t session_id{};
        Sequence previous_publication{}, current_publication{};
        std::size_t previous_count{}, current_count{}, instance_count{}, capture_count{};
        bool operator==(const Key &) const = default;
    };

    // Returns whether the publication indexes were rebuilt; unknown identities always rebuild.
    bool Prepare(Key key, std::span<const PersistentVfxVisual> previous,
                 std::span<const PersistentVfxVisual> current,
                 std::span<const RenderInstance> instances,
                 std::span<const CapturedRenderInstance> captures);
    [[nodiscard]] std::size_t RetainedCapacityBytes() const noexcept;
    [[nodiscard]] const PersistentVfxVisual *Previous(
        std::uint64_t stable_id, PersistentVfxKind kind, std::span<const PersistentVfxVisual> rows) const noexcept;
    [[nodiscard]] const PersistentVfxVisual *Current(
        std::uint64_t stable_id, PersistentVfxKind kind,
        std::span<const PersistentVfxVisual> rows) const noexcept;
    [[nodiscard]] const RenderInstance *Instance(
        std::uint64_t stable_id, std::span<const RenderInstance> rows) const noexcept;
    [[nodiscard]] const CapturedRenderInstance *Capture(
        std::uint64_t session_id, Sequence sequence, Tick tick, std::uint64_t entity_id,
        std::span<const CapturedRenderInstance> rows) const noexcept;

  private:
    struct VisualKey
    {
        std::uint64_t stable_id{};
        PersistentVfxKind kind{};
        bool operator==(const VisualKey &) const = default;
    };
    struct VisualHash
    {
        std::size_t operator()(const VisualKey &key) const noexcept;
    };
    struct CaptureKey
    {
        std::uint64_t session_id{};
        Sequence sequence{};
        Tick tick{};
        std::uint64_t entity_id{};
        bool operator==(const CaptureKey &) const = default;
    };
    struct CaptureHash
    {
        std::size_t operator()(const CaptureKey &key) const noexcept;
    };
    Key key_{};
    bool prepared_{};
    std::unordered_map<VisualKey, std::size_t, VisualHash> previous_;
    std::unordered_map<VisualKey, std::size_t, VisualHash> current_;
    std::unordered_map<std::uint64_t, std::size_t> instances_;
    std::unordered_map<CaptureKey, std::size_t, CaptureHash> captures_;
};

// Convert the presentation contracts into typed v4 renderer inputs. The
// function owns no state and does not create a lifetime or geometry value when
// the source contract does not provide one.
void BuildVfxTypedFrameInputs(
    const VfxImage &image,
    std::span<const PresentationEvent> events,
    std::span<const PersistentVfxVisual> current,
    std::span<const PersistentVfxVisual> previous,
    Tick current_tick,
    VfxTypedFrameInputs &result,
    const PreparedPairFacts *pair_facts = nullptr,
    float interpolation = 1.0f, VfxQuality quality = VfxQuality::High);

// Retain only the captured endpoints of a successful second retreat. The
// movement is instantaneous in gameplay; fixed authored seconds own the fade.
// Clear on session reset; catalog adoption rebinds the captured owner payloads.
class VfxUpgradeMovementState
{
  public:
    void Append(
        const VfxImage &image,
        std::span<const PresentationEvent> dispatched_events,
        Tick current_tick, std::vector<VfxPersistentInput> &output,
        float interpolation = 1.0f, VfxQuality quality = VfxQuality::High);
    void Clear() noexcept { owners_.clear(); staged_owners_.clear(); }
    [[nodiscard]] std::size_t RetainedCapacityBytes() const noexcept;
    [[nodiscard]] std::uint64_t Rebind(const VfxProgramView &previous,
                                     const VfxProgramView &next) noexcept;
    void Swap(VfxUpgradeMovementState &prepared) noexcept
    {
        owners_.swap(prepared.owners_);
        staged_owners_.swap(prepared.staged_owners_);
    }

  private:
    struct Owner
    {
        VfxPersistentInput input;
        Tick started{};
        float seconds{};
    };
    std::vector<Owner> owners_;
    std::vector<Owner> staged_owners_;
};

// Refresh one retained warning event from its authoritative persistent owner.
// False means its owner/recipe/age contract is no longer retainable.
[[nodiscard]] bool RefreshVfxGroundEventOwner(
    VfxEventInput &event, std::span<const PersistentVfxVisual> owners,
    Tick current_tick, const PreparedPairFacts *pair_facts = nullptr,
    std::span<const PersistentVfxVisual> previous = {}, float interpolation = 1.0f);

// Refresh retained warning events from their authoritative persistent owners.
// Events without owner metadata are preserved unchanged.
void RefreshVfxGroundEventOwners(std::vector<VfxEventInput> &events,
                                 std::span<const PersistentVfxVisual> owners,
                                 Tick current_tick,
                                 const PreparedPairFacts *pair_facts = nullptr,
                                 std::span<const PersistentVfxVisual> previous = {},
                                 float interpolation = 1.0f);

// Validate prepared owner-backed events against exact warning/sector recipes.
// Events without owner metadata retain their already remapped handles.
void RebindVfxGroundEventOwners(const VfxImage &image,
                                std::vector<VfxEventInput> &events);

} // namespace hs::runtime_detail
