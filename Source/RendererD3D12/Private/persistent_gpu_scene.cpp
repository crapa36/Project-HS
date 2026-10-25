#include "persistent_gpu_scene.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace hs
{

bool BuildGpuSceneTableLayout(std::size_t capacity, std::size_t slot_count,
                              GpuSceneTableLayout &layout,
                              std::size_t entity_surface_bytes) noexcept
{
    layout = {};
    if (slot_count > capacity || capacity > std::numeric_limits<std::uint32_t>::max() ||
        slot_count > std::numeric_limits<std::uint32_t>::max())
        return false;

    std::size_t cursor = sizeof(GpuSceneTableHeader);
    auto append_table = [&](std::size_t stride, std::uint32_t &offset) {
        if (capacity != 0 &&
            stride > (std::numeric_limits<std::uint32_t>::max() - cursor) / capacity)
            return false;
        offset = static_cast<std::uint32_t>(cursor);
        cursor += capacity * stride;
        return cursor <= std::numeric_limits<std::uint32_t>::max();
    };

    auto &header = layout.header;
    header.capacity = static_cast<std::uint32_t>(capacity);
    header.slot_count = static_cast<std::uint32_t>(slot_count);
    header.instance_stride = sizeof(GpuSceneInstanceEntry);
    header.transform_stride = sizeof(GpuSceneTransformEntry);
    header.mesh_stride = sizeof(GpuSceneMeshEntry);
    header.material_stride = sizeof(GpuSceneMaterialEntry);
    header.bounds_stride = sizeof(GpuSceneBoundsEntry);
    header.skin_stride = sizeof(GpuSceneSkinEntry);
    if (!append_table(sizeof(GpuSceneInstanceEntry), header.instance_offset) ||
        !append_table(sizeof(GpuSceneTransformEntry), header.transform_offset) ||
        !append_table(sizeof(GpuSceneMeshEntry), header.mesh_offset) ||
        !append_table(sizeof(GpuSceneMaterialEntry), header.material_offset) ||
        !append_table(sizeof(GpuSceneBoundsEntry), header.bounds_offset) ||
        !append_table(sizeof(GpuSceneSkinEntry), header.skin_offset))
    {
        layout = {};
        return false;
    }
    if (entity_surface_bytes > std::numeric_limits<std::uint32_t>::max() - cursor ||
        (entity_surface_bytes % 16u) != 0)
    {
        layout = {};
        return false;
    }
    if (entity_surface_bytes != 0)
    {
        layout.entity_surface_offset = static_cast<std::uint32_t>(cursor);
        layout.entity_surface_bytes = static_cast<std::uint32_t>(entity_surface_bytes);
        cursor += entity_surface_bytes;
    }
    layout.total_bytes = cursor;
    return true;
}

GpuSceneSerializedSlot SerializeGpuSceneSlot(std::uint32_t slot,
                                             const GpuInstance &instance,
                                             const GpuSceneMetadata &metadata) noexcept
{
    const auto low = [](std::uint64_t value) {
        return static_cast<std::uint32_t>(value);
    };
    const auto high = [](std::uint64_t value) {
        return static_cast<std::uint32_t>(value >> 32u);
    };

    GpuSceneSerializedSlot result;
    result.instance = {
        slot, slot, slot, slot, metadata.skin_index, metadata.generation, 0u, metadata.reserved};
    result.transform = {
        {instance.position.x, instance.position.y, instance.position.z, instance.yaw},
        {instance.scale.x, instance.scale.y, instance.scale.z, instance.padding}};
    result.mesh = {
        instance.mesh, low(metadata.asset_id), high(metadata.asset_id), 0u};
    result.material = {
        instance.color, low(metadata.material_id), high(metadata.material_id), 0u};
    result.bounds = {{0.0f, 0.0f, 0.0f, instance.position.w}};
    result.skin = {metadata.skin_index, metadata.skin_flags,
                   metadata.skin_vertex_offset, metadata.previous_skin_vertex_offset,
                   metadata.skin_vertex_stride, metadata.previous_skin_vertex_stride};
    return result;
}

void PersistentGpuSceneSlots::BeginFrame(bool reset_scene)
{
    Abort();
    preparing_ = true;
    reset_scene_ = reset_scene;
    active_count_ = 0;
    anonymous_count_ = 0;
    changes_ = {};
    content_changed_ = false;
    pending_content_revision_ = content_revision_;
    dirty_slots_.clear();
    dirty_ranges_.clear();
    dirty_ranges_valid_ = true;
}

PersistentGpuSceneSlot PersistentGpuSceneSlots::Upsert(
    PersistentGpuSceneKey key, const GpuInstance &instance,
    GpuSceneMetadata metadata, GpuSceneCanonicalInputs canonical)
{
    if (key.stable_id != 0) key.fallback_ordinal = 0;
    if (key.stable_id == 0 && key.fallback_ordinal == 0)
        key.fallback_ordinal = 1;

    if (const auto pending = pending_slots_.find(key); pending != pending_slots_.end())
    {
        const auto slot = pending->second;
        const auto &entry = pending_entries_.at(slot);
        metadata.generation = entry.generation;
        if (metadata.skin_flags != 0 &&
            metadata.skin_index == std::numeric_limits<std::uint32_t>::max())
            metadata.skin_index = slot;
        const bool same =
            std::memcmp(&instances_[slot], &instance, sizeof(GpuInstance)) == 0 &&
            metadata_[slot] == metadata && entry.canonical == canonical;
        return {slot, entry.generation, !same};
    }

    const auto existing = reset_scene_ ? slots_.end() : slots_.find(key);
    std::uint32_t slot{};
    Entry next;
    if (existing == slots_.end())
    {
        if (reset_scene_)
        {
            if (pending_reset_slots_ >= std::numeric_limits<std::uint32_t>::max())
                return {0, 0, true};
            slot = static_cast<std::uint32_t>(pending_reset_slots_++);
            next.generation = 1;
            if (slot < instances_.size())
                previous_payloads_.push_back({slot, instances_[slot], metadata_[slot]});
            else
            {
                instances_.emplace_back();
                metadata_.emplace_back();
            }
        }
        else if (pending_reused_slots_ < free_slots_.size())
        {
            slot = free_slots_[free_slots_.size() - 1 - pending_reused_slots_++];
            next = entries_[slot];
            ++next.generation;
            if (next.generation == 0) ++next.generation;
            previous_payloads_.push_back({slot, instances_[slot], metadata_[slot]});
        }
        else
        {
            if (instances_.size() >= std::numeric_limits<std::uint32_t>::max())
                return {0, 0, true};
            slot = static_cast<std::uint32_t>(instances_.size());
            next.generation = 1;
            instances_.emplace_back();
            metadata_.emplace_back();
        }
        metadata.generation = next.generation;
        if (metadata.skin_flags != 0 &&
            metadata.skin_index == std::numeric_limits<std::uint32_t>::max())
            metadata.skin_index = slot;
        instances_[slot] = instance;
        metadata_[slot] = metadata;
        MarkDirty(slot);
        ++changes_.created;
        next.revisions = {1, 1, 1, 1};
        content_changed_ = true;
    }
    else
    {
        slot = existing->second;
        next = entries_[slot];
        metadata.generation = next.generation;
        if (metadata.skin_flags != 0 &&
            metadata.skin_index == std::numeric_limits<std::uint32_t>::max())
            metadata.skin_index = slot;
        const bool payload_changed =
            std::memcmp(&instances_[slot], &instance, sizeof(GpuInstance)) != 0 ||
            metadata_[slot] != metadata;
        const auto &before = instances_[slot];
        const auto &binding = metadata_[slot];
        const bool transform_changed =
            std::memcmp(&before.position, &instance.position, sizeof(instance.position)) != 0 ||
            std::memcmp(&before.scale, &instance.scale, sizeof(instance.scale)) != 0 ||
            std::bit_cast<std::uint32_t>(before.yaw) != std::bit_cast<std::uint32_t>(instance.yaw) ||
            next.canonical.supplied_transform_revision != canonical.supplied_transform_revision;
        const bool material_changed = before.color != instance.color ||
            binding.material_id != metadata.material_id || binding.reserved != metadata.reserved ||
            next.canonical.supplied_material_revision != canonical.supplied_material_revision ||
            next.canonical.status_visual_mask != canonical.status_visual_mask;
        const bool content_changed = before.mesh != instance.mesh || binding.asset_id != metadata.asset_id ||
            std::bit_cast<std::uint32_t>(before.padding) != std::bit_cast<std::uint32_t>(instance.padding) ||
            next.canonical.content != canonical.content ||
            next.canonical.geometry_identity != canonical.geometry_identity;
        const bool pose_changed = next.canonical.pose != canonical.pose ||
            next.canonical.status_visual_mask != canonical.status_visual_mask;
        const auto advance = [](std::uint64_t &revision) { if (++revision == 0) ++revision; };
        if (transform_changed) advance(next.revisions.transform);
        if (material_changed) advance(next.revisions.material);
        if (content_changed) advance(next.revisions.content);
        if (pose_changed) advance(next.revisions.pose);
        content_changed_ |= material_changed || content_changed;
        if (payload_changed || next.canonical != canonical) ++changes_.updated;
        else ++changes_.unchanged;
        // A pose-only edit changes the canonical scene without uploading
        // unchanged instance bytes. Deformation has its own physical backing.
        if (payload_changed)
        {
            previous_payloads_.push_back({slot, instances_[slot], metadata_[slot]});
            instances_[slot] = instance;
            metadata_[slot] = metadata;
            MarkDirty(slot);
        }
    }
    next.key = key;
    next.canonical = canonical;
    next.active = true;
    if (pending_entries_.size() <= slot) pending_entries_.resize(std::size_t(slot) + 1);
    pending_entries_[slot] = next;
    pending_slots_.emplace(key, slot);
    ++active_count_;
    if (key.stable_id == 0) ++anonymous_count_;
    return {slot, next.generation, false};
}

void PersistentGpuSceneSlots::EndFrame()
{
    changes_.destroyed = 0;
    if (!reset_scene_)
        for (const auto &entry : entries_)
            if (entry.active && !pending_slots_.contains(entry.key)) ++changes_.destroyed;
    pending_content_revision_ = content_revision_;
    if (content_changed_ || changes_.destroyed)
        if (++pending_content_revision_ == 0) ++pending_content_revision_;
    // Destruction and free-slot reuse are published only after submission.
}

void PersistentGpuSceneSlots::Commit()
{
    if (!preparing_) return;
    if (reset_scene_)
    {
        slots_.clear();
        free_slots_.clear();
        instances_.resize(pending_reset_slots_);
        metadata_.resize(pending_reset_slots_);
    }
    else
    {
        free_slots_.resize(free_slots_.size() - pending_reused_slots_);
        for (std::uint32_t slot = 0; slot < entries_.size(); ++slot)
        {
            auto &entry = entries_[slot];
            if (!entry.active || pending_slots_.contains(entry.key)) continue;
            slots_.erase(entry.key);
            entry.active = false;
            free_slots_.push_back(slot);
        }
    }
    entries_.resize(instances_.size());
    for (const auto &[key, slot] : pending_slots_)
    {
        static_cast<void>(key);
        entries_[slot] = pending_entries_[slot];
    }
    for (const auto &[key, slot] : pending_slots_) slots_.insert_or_assign(key, slot);
    content_revision_ = pending_content_revision_;
    pending_slots_.clear();
    previous_payloads_.clear();
    pending_reused_slots_ = 0;
    pending_reset_slots_ = 0;
    preparing_ = false;
}

void PersistentGpuSceneSlots::Abort() noexcept
{
    for (const auto &payload : previous_payloads_)
    {
        instances_[payload.slot] = payload.instance;
        metadata_[payload.slot] = payload.metadata;
    }
    instances_.resize(entries_.size());
    metadata_.resize(entries_.size());
    pending_slots_.clear();
    previous_payloads_.clear();
    pending_reused_slots_ = 0;
    pending_reset_slots_ = 0;
    preparing_ = false;
}

void PersistentGpuSceneSlots::MarkAllActiveDirty()
{
    if (preparing_)
    {
        for (const auto &[key, slot] : pending_slots_)
        {
            static_cast<void>(key);
            MarkDirty(slot);
        }
    }
    else
    {
        for (std::uint32_t slot = 0; slot < entries_.size(); ++slot)
            if (entries_[slot].active) MarkDirty(slot);
    }
}

const std::vector<PersistentGpuSceneRange> &PersistentGpuSceneSlots::DirtyRanges()
{
    if (!dirty_ranges_valid_) RebuildDirtyRanges();
    return dirty_ranges_;
}

void PersistentGpuSceneSlots::Clear() noexcept
{
    Abort();
    slots_.clear();
    entries_.clear();
    pending_entries_.clear();
    instances_.clear();
    metadata_.clear();
    free_slots_.clear();
    dirty_slots_.clear();
    dirty_ranges_.clear();
    active_count_ = 0;
    anonymous_count_ = 0;
    changes_ = {};
    content_revision_ = pending_content_revision_ = 0;
    content_changed_ = false;
    dirty_ranges_valid_ = true;
}

void PersistentGpuSceneSlots::MarkDirty(std::uint32_t slot)
{
    dirty_slots_.push_back(slot);
    dirty_ranges_valid_ = false;
}

void PersistentGpuSceneSlots::RebuildDirtyRanges()
{
    std::ranges::sort(dirty_slots_);
    const auto unique_end = std::ranges::unique(dirty_slots_).begin();
    dirty_slots_.erase(unique_end, dirty_slots_.end());
    dirty_ranges_.clear();
    for (const auto slot : dirty_slots_)
    {
        if (!dirty_ranges_.empty() &&
            dirty_ranges_.back().begin + dirty_ranges_.back().count == slot)
        {
            ++dirty_ranges_.back().count;
        }
        else
        {
            dirty_ranges_.push_back({slot, 1});
        }
    }
    dirty_ranges_valid_ = true;
}

} // namespace hs
