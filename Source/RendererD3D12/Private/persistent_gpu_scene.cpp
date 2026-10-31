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
    for (auto &ranges : dirty_channel_ranges_) ranges.clear();
    dirty_ranges_valid_ = true;
}

PersistentGpuSceneSlot PersistentGpuSceneSlots::Upsert(
    PersistentGpuSceneKey key, const GpuInstance &instance,
    GpuSceneMetadata metadata, GpuSceneCanonicalInputs canonical)
{
    if (key.stable_id != 0) key.fallback_ordinal = 0;
    if (key.stable_id == 0 && key.fallback_ordinal == 0)
        key.fallback_ordinal = 1;

    const auto existing = reset_scene_ ? slots_.end() : slots_.find(key);
    auto prepared_slot = std::numeric_limits<std::uint32_t>::max();
    if (existing != slots_.end())
    {
        if (pending_seen_[existing->second]) prepared_slot = existing->second;
    }
    else if (const auto pending = pending_new_slots_.find(key); pending != pending_new_slots_.end())
        prepared_slot = pending->second;
    if (prepared_slot != std::numeric_limits<std::uint32_t>::max())
    {
        const auto slot = prepared_slot;
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
            std::uint8_t channels{};
            const auto changed = [&](GpuSceneChannel channel, bool value) {
                if (value) channels |= static_cast<std::uint8_t>(1u << static_cast<unsigned>(channel));
            };
            changed(GpuSceneChannel::Instance, binding.skin_index != metadata.skin_index ||
                binding.generation != metadata.generation || binding.reserved != metadata.reserved);
            changed(GpuSceneChannel::Transform,
                std::memcmp(&before.position, &instance.position, sizeof(float) * 3) != 0 ||
                std::memcmp(&before.scale, &instance.scale, sizeof(float) * 3) != 0 ||
                std::bit_cast<std::uint32_t>(before.yaw) != std::bit_cast<std::uint32_t>(instance.yaw) ||
                std::bit_cast<std::uint32_t>(before.padding) != std::bit_cast<std::uint32_t>(instance.padding));
            changed(GpuSceneChannel::Mesh, before.mesh != instance.mesh || binding.asset_id != metadata.asset_id);
            changed(GpuSceneChannel::Material, before.color != instance.color || binding.material_id != metadata.material_id);
            changed(GpuSceneChannel::Bounds,
                std::bit_cast<std::uint32_t>(before.position.w) != std::bit_cast<std::uint32_t>(instance.position.w));
            changed(GpuSceneChannel::Skin, binding.skin_index != metadata.skin_index ||
                binding.skin_flags != metadata.skin_flags || binding.skin_vertex_offset != metadata.skin_vertex_offset ||
                binding.previous_skin_vertex_offset != metadata.previous_skin_vertex_offset ||
                binding.skin_vertex_stride != metadata.skin_vertex_stride ||
                binding.previous_skin_vertex_stride != metadata.previous_skin_vertex_stride);
            previous_payloads_.push_back({slot, instances_[slot], metadata_[slot]});
            instances_[slot] = instance;
            metadata_[slot] = metadata;
            MarkDirty(slot, channels);
        }
    }
    next.key = key;
    next.canonical = canonical;
    next.active = true;
    if (pending_entries_.size() <= slot) pending_entries_.resize(std::size_t(slot) + 1);
    pending_entries_[slot] = next;
    if (existing == slots_.end()) pending_new_slots_.emplace(key, slot);
    if (pending_seen_.size() <= slot) pending_seen_.resize(std::size_t(slot) + 1);
    pending_active_slots_.push_back(slot);
    pending_seen_[slot] = 1;
    ++active_count_;
    if (key.stable_id == 0) ++anonymous_count_;
    return {slot, next.generation, false};
}

void PersistentGpuSceneSlots::EndFrame()
{
    changes_.destroyed = 0;
    pending_removed_slots_.clear();
    if (!reset_scene_)
    {
        pending_removed_slots_.reserve(entries_.size());
        for (std::size_t slot = 0; slot < entries_.size(); ++slot)
            if (entries_[slot].active && !pending_seen_[slot])
            {
                ++changes_.destroyed;
                pending_removed_slots_.push_back(static_cast<std::uint32_t>(slot));
            }
    }
    pending_content_revision_ = content_revision_;
    if (content_changed_ || changes_.destroyed)
        if (++pending_content_revision_ == 0) ++pending_content_revision_;
    // Native submission commits these owners; any allocation must fail before it.
    entries_.reserve(instances_.size());
    payload_slot_revisions_.resize(instances_.size());
    pending_payload_channels_.resize(instances_.size());
    free_slots_.reserve(free_slots_.size() + changes_.destroyed);
    if (!pending_new_slots_.empty())
        slots_.reserve(reset_scene_ ? pending_new_slots_.size() : slots_.size() + pending_new_slots_.size());
    // Destruction and free-slot reuse are published only after submission.
}

void PersistentGpuSceneSlots::Commit()
{
    if (!preparing_) return;
    if (reset_scene_)
    {
        slots_.clear();
        free_slots_.clear();
        for (auto &revisions : payload_slot_revisions_) revisions.fill(0);
        instances_.resize(pending_reset_slots_);
        metadata_.resize(pending_reset_slots_);
        payload_slot_revisions_.resize(pending_reset_slots_);
        pending_payload_channels_.resize(pending_reset_slots_);
    }
    else
    {
        free_slots_.resize(free_slots_.size() - pending_reused_slots_);
        for (const auto slot : pending_removed_slots_)
        {
            auto &entry = entries_[slot];
            slots_.erase(entry.key);
            entry.active = false;
            free_slots_.push_back(slot);
            if (slot < payload_slot_revisions_.size()) payload_slot_revisions_[slot].fill(0);
        }
    }
    CommitPayloadChanges();
    entries_.resize(instances_.size());
    for (const auto slot : pending_active_slots_)
        entries_[slot] = pending_entries_[slot];
    slots_.merge(pending_new_slots_);
    content_revision_ = pending_content_revision_;
    for (const auto slot : pending_active_slots_) pending_seen_[slot] = 0;
    pending_active_slots_.clear();
    pending_new_slots_.clear();
    pending_removed_slots_.clear();
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
    for (const auto slot : pending_active_slots_) pending_seen_[slot] = 0;
    for (const auto dirty : dirty_slots_)
        if (dirty.slot < pending_payload_channels_.size()) pending_payload_channels_[dirty.slot] = 0;
    pending_active_slots_.clear();
    pending_new_slots_.clear();
    pending_removed_slots_.clear();
    previous_payloads_.clear();
    pending_reused_slots_ = 0;
    pending_reset_slots_ = 0;
    payload_change_pending_ = false;
    preparing_ = false;
}

void PersistentGpuSceneSlots::MarkAllActiveDirty()
{
    if (preparing_)
    {
        for (const auto slot : pending_active_slots_) MarkDirty(slot);
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

const std::vector<PersistentGpuSceneRange> &PersistentGpuSceneSlots::DirtyRanges(GpuSceneChannel channel)
{
    if (!dirty_ranges_valid_) RebuildDirtyRanges();
    return dirty_channel_ranges_.at(static_cast<std::size_t>(channel));
}

void PersistentGpuSceneSlots::Clear() noexcept
{
    Abort();
    slots_.clear();
    entries_.clear();
    pending_entries_.clear();
    pending_seen_.clear();
    pending_removed_slots_.clear();
    instances_.clear();
    metadata_.clear();
    payload_slot_revisions_.clear();
    pending_payload_channels_.clear();
    free_slots_.clear();
    dirty_slots_.clear();
    dirty_ranges_.clear();
    for (auto &ranges : dirty_channel_ranges_) ranges.clear();
    active_count_ = 0;
    anonymous_count_ = 0;
    changes_ = {};
    content_revision_ = pending_content_revision_ = 0;
    content_changed_ = false;
    payload_change_pending_ = false;
    dirty_ranges_valid_ = true;
}

void PersistentGpuSceneSlots::MarkDirty(std::uint32_t slot, std::uint8_t channels)
{
    if (channels == 0) return;
    dirty_slots_.push_back({slot, channels});
    if (slot >= pending_payload_channels_.size())
        pending_payload_channels_.resize(std::size_t(slot) + 1);
    pending_payload_channels_[slot] |= channels;
    payload_change_pending_ = true;
    dirty_ranges_valid_ = false;
}

std::uint64_t PersistentGpuSceneSlots::PayloadRevision() const noexcept
{
    return payload_change_pending_ && payload_revision_ != std::numeric_limits<std::uint64_t>::max()
        ? payload_revision_ + 1 : payload_revision_;
}

void PersistentGpuSceneSlots::CommitPayloadChanges() noexcept
{
    if (!payload_change_pending_) return;
    if (payload_revision_ == std::numeric_limits<std::uint64_t>::max())
        payload_revision_exhausted_ = true;
    else
        ++payload_revision_;
    for (const auto dirty : dirty_slots_)
    {
        if (dirty.slot >= payload_slot_revisions_.size()) continue;
        auto &revisions = payload_slot_revisions_[dirty.slot];
        for (std::size_t channel = 0; channel < kGpuSceneChannelCount; ++channel)
            if (dirty.channels & (1u << channel)) revisions[channel] = payload_revision_;
        if (dirty.slot < pending_payload_channels_.size()) pending_payload_channels_[dirty.slot] = 0;
    }
    payload_change_pending_ = false;
}

void PersistentGpuSceneSlots::CollectDirtyRangesSince(
    std::uint64_t uploaded_revision,
    std::vector<PersistentGpuSceneRange> &union_ranges,
    std::array<std::vector<PersistentGpuSceneRange>, kGpuSceneChannelCount> &channel_ranges) const
{
    union_ranges.clear();
    for (auto &ranges : channel_ranges) ranges.clear();
    const auto pending_revision = PayloadRevision();
    const bool pending_is_newer = payload_change_pending_ && uploaded_revision < pending_revision;
    const bool all_active = uploaded_revision == 0 || payload_revision_exhausted_;
    if (!all_active && uploaded_revision == pending_revision) return;

    const auto append = [](std::vector<PersistentGpuSceneRange> &ranges, std::uint32_t slot) {
        if (!ranges.empty() && ranges.back().begin + ranges.back().count == slot)
            ++ranges.back().count;
        else
            ranges.push_back({slot, 1});
    };
    for (std::uint32_t slot = 0; slot < SlotCount(); ++slot)
    {
        const bool active = preparing_
            ? slot < pending_seen_.size() && pending_seen_[slot] != 0
            : slot < entries_.size() && entries_[slot].active;
        if (!active) continue;
        std::uint8_t changed_channels = all_active ? kGpuSceneAllChannels : 0;
        if (!all_active && slot < payload_slot_revisions_.size())
            for (std::size_t channel = 0; channel < kGpuSceneChannelCount; ++channel)
                if (payload_slot_revisions_[slot][channel] > uploaded_revision)
                    changed_channels |= static_cast<std::uint8_t>(1u << channel);
        if (!all_active && pending_is_newer && slot < pending_payload_channels_.size())
            changed_channels |= pending_payload_channels_[slot];
        changed_channels &= kGpuSceneAllChannels;
        if (changed_channels == 0) continue;

        append(union_ranges, slot);
        for (std::size_t channel = 0; channel < kGpuSceneChannelCount; ++channel)
            if (changed_channels & (1u << channel)) append(channel_ranges[channel], slot);
    }
}

void PersistentGpuSceneSlots::RebuildDirtyRanges()
{
    std::ranges::sort(dirty_slots_, {}, &DirtySlot::slot);
    std::size_t unique_count{};
    for (const auto dirty : dirty_slots_)
    {
        if (unique_count != 0 && dirty_slots_[unique_count - 1].slot == dirty.slot)
            dirty_slots_[unique_count - 1].channels |= dirty.channels;
        else dirty_slots_[unique_count++] = dirty;
    }
    dirty_slots_.resize(unique_count);
    dirty_ranges_.clear();
    for (auto &ranges : dirty_channel_ranges_) ranges.clear();
    const auto append = [](auto &ranges, std::uint32_t slot) {
        if (!ranges.empty() && ranges.back().begin + ranges.back().count == slot)
            ++ranges.back().count;
        else ranges.push_back(PersistentGpuSceneRange{slot, 1});
    };
    for (const auto dirty : dirty_slots_)
    {
        append(dirty_ranges_, dirty.slot);
        for (std::size_t channel = 0; channel < kGpuSceneChannelCount; ++channel)
            if (dirty.channels & (1u << channel)) append(dirty_channel_ranges_[channel], dirty.slot);
    }
    dirty_ranges_valid_ = true;
}

} // namespace hs
