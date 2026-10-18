#pragma once

#include "render_scene_extractor.hpp"

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hs
{

struct GpuInstance
{
    DirectX::XMFLOAT4 position;
    DirectX::XMFLOAT4 scale;
    std::uint32_t color{};
    std::uint32_t mesh{};
    float yaw{};
    float padding{};
};

static_assert(sizeof(GpuInstance) == 48);

constexpr std::uint32_t kGpuSceneTableMagic = 0x37475348u; // "HSG7"
constexpr std::uint32_t kGpuSceneTableVersion = 1;

struct GpuSceneTableHeader
{
    std::uint32_t magic{kGpuSceneTableMagic};
    std::uint32_t version{kGpuSceneTableVersion};
    std::uint32_t capacity{};
    std::uint32_t slot_count{};
    std::uint32_t instance_offset{};
    std::uint32_t transform_offset{};
    std::uint32_t mesh_offset{};
    std::uint32_t material_offset{};
    std::uint32_t bounds_offset{};
    std::uint32_t skin_offset{};
    std::uint32_t instance_stride{};
    std::uint32_t transform_stride{};
    std::uint32_t mesh_stride{};
    std::uint32_t material_stride{};
    std::uint32_t bounds_stride{};
    std::uint32_t skin_stride{};
};

struct GpuSceneInstanceEntry
{
    std::uint32_t transform_index{};
    std::uint32_t mesh_index{};
    std::uint32_t material_index{};
    std::uint32_t bounds_index{};
    std::uint32_t skin_index{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t generation{};
    std::uint32_t flags{};
    std::uint32_t reserved{};
};

struct GpuSceneTransformEntry
{
    DirectX::XMFLOAT4 position_yaw;
    DirectX::XMFLOAT4 scale_parameter;
};

struct GpuSceneMeshEntry
{
    std::uint32_t mesh{};
    std::uint32_t asset_id_low{};
    std::uint32_t asset_id_high{};
    std::uint32_t flags{};
};

struct GpuSceneMaterialEntry
{
    std::uint32_t color{};
    std::uint32_t material_id_low{};
    std::uint32_t material_id_high{};
    std::uint32_t flags{};
};

struct GpuSceneBoundsEntry
{
    DirectX::XMFLOAT4 center_radius;
};

struct GpuSceneSkinEntry
{
    std::uint32_t skin_index{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t flags{};
    std::uint32_t reserved0{};
    std::uint32_t reserved1{};
};

static_assert(sizeof(GpuSceneTableHeader) == 64);
static_assert(sizeof(GpuSceneInstanceEntry) == 32);
static_assert(sizeof(GpuSceneTransformEntry) == 32);
static_assert(sizeof(GpuSceneMeshEntry) == 16);
static_assert(sizeof(GpuSceneMaterialEntry) == 16);
static_assert(sizeof(GpuSceneBoundsEntry) == 16);
static_assert(sizeof(GpuSceneSkinEntry) == 16);

constexpr std::size_t kGpuSceneSlotPayloadBytes =
    sizeof(GpuSceneInstanceEntry) + sizeof(GpuSceneTransformEntry) +
    sizeof(GpuSceneMeshEntry) + sizeof(GpuSceneMaterialEntry) +
    sizeof(GpuSceneBoundsEntry) + sizeof(GpuSceneSkinEntry);

struct GpuSceneTableLayout
{
    GpuSceneTableHeader header{};
    std::size_t total_bytes{};
    std::uint32_t entity_surface_offset{};
    std::uint32_t entity_surface_bytes{};
};

struct GpuSceneMetadata
{
    std::uint64_t asset_id{};
    std::uint64_t material_id{};
    std::uint32_t skin_index{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t skin_flags{};
    std::uint32_t generation{};
    std::uint32_t reserved{};

    [[nodiscard]] bool operator==(const GpuSceneMetadata &) const noexcept = default;
};

struct GpuSceneSerializedSlot
{
    GpuSceneInstanceEntry instance{};
    GpuSceneTransformEntry transform{};
    GpuSceneMeshEntry mesh{};
    GpuSceneMaterialEntry material{};
    GpuSceneBoundsEntry bounds{};
    GpuSceneSkinEntry skin{};
};

[[nodiscard]] bool BuildGpuSceneTableLayout(std::size_t capacity,
                                            std::size_t slot_count,
                                            GpuSceneTableLayout &layout,
                                            std::size_t entity_surface_bytes = 0) noexcept;
[[nodiscard]] GpuSceneSerializedSlot SerializeGpuSceneSlot(
    std::uint32_t slot, const GpuInstance &instance,
    const GpuSceneMetadata &metadata) noexcept;

// Transitional shared draw-reference ABI for both immutable static instances
// and persistent dynamic instance slots. InstanceGeneration is not consumed by
// the current indexed frontend yet, but travels with the slot so visibility
// history can reject a reused slot instead of confusing it with the old object.
struct GpuStaticDrawRef
{
    std::uint32_t instance_index{};
    std::uint32_t mesh_flags{};
    float lod_threshold{1.0f};
    std::uint32_t instance_generation{};
};

static_assert(sizeof(GpuStaticDrawRef) == 16);

using PersistentGpuSceneKey = RenderSceneInstanceKey;

struct PersistentGpuSceneSlot
{
    std::uint32_t index{};
    std::uint32_t generation{};
    bool conflict{};
};

struct PersistentGpuSceneRange
{
    std::size_t begin{};
    std::size_t count{};
};

class PersistentGpuSceneSlots
{
  public:
    // Preparation exposes upload payloads and draw slots without publishing
    // retained identities. BeginFrame discards an unsubmitted preparation.
    void BeginFrame(bool reset_scene = false);
    [[nodiscard]] PersistentGpuSceneSlot Upsert(PersistentGpuSceneKey key,
                                                const GpuInstance &instance,
                                                RenderInstance::Revisions revisions = {},
                                                GpuSceneMetadata metadata = {});
    void EndFrame();
    void Commit();
    void Abort() noexcept;

    void MarkAllActiveDirty();

    [[nodiscard]] std::span<const GpuInstance> Instances() const noexcept
    {
        return {instances_.data(), SlotCount()};
    }
    [[nodiscard]] std::span<const GpuSceneMetadata> Metadata() const noexcept
    {
        return {metadata_.data(), SlotCount()};
    }
    [[nodiscard]] const std::vector<PersistentGpuSceneRange> &DirtyRanges();
    [[nodiscard]] std::size_t ActiveCount() const noexcept { return active_count_; }
    [[nodiscard]] std::size_t SlotCount() const noexcept
    {
        return preparing_ && reset_scene_ ? pending_reset_slots_ : instances_.size();
    }
    [[nodiscard]] std::size_t AnonymousCount() const noexcept { return anonymous_count_; }
    void Clear() noexcept;

  private:
    using KeyHash = RenderSceneInstanceKeyHash;
    struct Entry
    {
        PersistentGpuSceneKey key{};
        std::uint32_t generation{};
        RenderInstance::Revisions revisions{};
        bool active{};
    };

    void MarkDirty(std::uint32_t slot);
    void RebuildDirtyRanges();

    std::unordered_map<PersistentGpuSceneKey, std::uint32_t, KeyHash> slots_;
    std::vector<Entry> entries_;
    std::vector<GpuInstance> instances_;
    std::vector<GpuSceneMetadata> metadata_;
    std::vector<std::uint32_t> free_slots_;
    // Only touched payloads need an undo record. Retained slot identity is
    // unchanged until Commit, while Instances exposes the prepared upload.
    std::unordered_map<PersistentGpuSceneKey, std::uint32_t, KeyHash> pending_slots_;
    // Reused by slot; only keys present in pending_slots_ may read this staging.
    std::vector<Entry> pending_entries_;
    struct PreviousPayload
    {
        std::uint32_t slot{};
        GpuInstance instance{};
        GpuSceneMetadata metadata{};
    };
    std::vector<PreviousPayload> previous_payloads_;
    std::size_t pending_reused_slots_{};
    std::size_t pending_reset_slots_{};
    bool preparing_{};
    bool reset_scene_{};
    std::vector<std::uint32_t> dirty_slots_;
    std::vector<PersistentGpuSceneRange> dirty_ranges_;
    std::size_t active_count_{};
    std::size_t anonymous_count_{};
    bool dirty_ranges_valid_{true};
};

} // namespace hs
