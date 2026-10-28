#include "vfx_catalog.hpp"

#include <hs/core/cooked_format.hpp>
#include <hs/runtime/vfx_image_io.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace hs
{
namespace
{
const auto kExplosiveArrowMain = MakeAssetId("particle.skill.explosive_arrow.main");
const auto kShockwaveRelease = MakeAssetId("particle.boss.shockwave.release");

std::uint32_t SpawnSeed(Sequence sequence, std::uint32_t emitter) noexcept
{
    auto value = sequence ^ (0x9e3779b97f4a7c15ull + emitter);
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27;
    return static_cast<std::uint32_t>(value ^ (value >> 31));
}
}

const CookedVfxDefinition *VfxCatalog::FindEffect(EffectId id) const noexcept
{
    const auto found = std::lower_bound(image_.definitions.begin(), image_.definitions.end(), id.value,
        [](const CookedVfxDefinition &definition, std::uint64_t value) { return definition.effect_id < value; });
    return found != image_.definitions.end() && found->effect_id == id.value ? &*found : nullptr;
}

VfxCatalog::VfxCatalog() noexcept = default;
VfxCatalog::~VfxCatalog() = default;
VfxCatalog::VfxCatalog(VfxCatalog &&other) noexcept
    : image_(std::exchange(other.image_, {})), owned_(std::move(other.owned_)) {}
VfxCatalog &VfxCatalog::operator=(VfxCatalog &&other) noexcept
{
    if (this != &other)
    {
        image_ = std::exchange(other.image_, {});
        owned_ = std::move(other.owned_);
    }
    return *this;
}
void VfxCatalog::Bind(const VfxImage &image) noexcept
{
    if (&image == &image_) return;
    owned_.reset();
    image_ = image;
}
void VfxCatalog::Swap(VfxCatalog &prepared) noexcept
{
    using std::swap;
    swap(image_, prepared.image_);
    swap(owned_, prepared.owned_);
}

const CookedParticleSprite *VfxCatalog::FindSprite(AssetId id) const noexcept
{
    const auto found = std::lower_bound(image_.sprites.begin(), image_.sprites.end(), id.value,
        [](const CookedParticleSprite &sprite, std::uint64_t value) {
            return sprite.sprite_id < value;
        });
    return found != image_.sprites.end() && found->sprite_id == id.value ? &*found : nullptr;
}

Result VfxCatalog::ExpandEvent(const PresentationEvent &event, float decorative_fraction,
                           std::vector<ParticleSpawnCommand> &particles,
                           std::vector<VfxLineSpawnCommand> &lines,
                           bool particles_replaced, bool mesh_replaced,
                           VfxLegacyGroundSuppression ground_suppression) const
{
    VfxPreparedLegacyEvent prepared;
    std::vector<std::uint32_t> indices;
    if (auto result = PrepareEvent(event, decorative_fraction, prepared, indices,
            particles_replaced, mesh_replaced, ground_suppression); !result) return result;
    return AppendPreparedEvent(prepared, indices, particles, lines);
}

Result VfxCatalog::PrepareEvent(const PresentationEvent &event, float decorative_fraction,
    VfxPreparedLegacyEvent &prepared, std::vector<std::uint32_t> &emitter_indices,
    bool particles_replaced, bool mesh_replaced,
    VfxLegacyGroundSuppression ground_suppression) const
{
    prepared = {};
    const auto old_index_size = emitter_indices.size();
    if (event.kind != PresentationKind::Vfx)
        return Result::Success();
    const auto *definition = FindEffect(event.asset);
    if (!definition)
    {
        // The legacy emitter catalog is only the compatibility path. New v4
        // events are decoded by the typed program and need no legacy emitter.
        const auto typed = std::lower_bound(image_.effect_lookup_by_id.begin(), image_.effect_lookup_by_id.end(),
            event.asset.value, [](const auto &effect, std::uint64_t id) { return effect.effect_id < id; });
        if (typed != image_.effect_lookup_by_id.end() && typed->effect_id == event.asset.value)
            return Result::Success();
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX effect ID is missing.");
    }
    if (!std::isfinite(decorative_fraction) || decorative_fraction < 0 || decorative_fraction > 1)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Decorative particle fraction is invalid.");
    const bool decorative = image_.decorative_definitions[static_cast<std::size_t>(definition - image_.definitions.data())] != 0;
    const auto parameters = DecodeVfxParameters(event.parameters);
    const auto direction_length = std::hypot(parameters.direction.x, parameters.direction.z);
    if (!std::isfinite(parameters.scale) || parameters.scale <= 0.0f || direction_length <= 0.0001f)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX event parameters are invalid.");
    const Float3 forward{parameters.direction.x / direction_length, 0.0f, parameters.direction.z / direction_length};
    const Float3 right{forward.z, 0.0f, -forward.x};
    VfxPreparedLegacyEvent candidate;
    candidate.event = &event; candidate.definition = definition;
    candidate.parameters = parameters; candidate.forward = forward; candidate.right = right;
    candidate.decorative_fraction = decorative_fraction; candidate.decorative = decorative;
    candidate.particles_replaced = particles_replaced; candidate.mesh_replaced = mesh_replaced;
    candidate.first_index = old_index_size;
    const auto typed = std::lower_bound(image_.effect_lookup_by_id.begin(), image_.effect_lookup_by_id.end(),
        event.asset.value, [](const auto &effect, std::uint64_t id) { return effect.effect_id < id; });
    if (typed != image_.effect_lookup_by_id.end() && typed->effect_id == event.asset.value &&
        typed->handle && typed->handle <= image_.program.effects.size())
        candidate.importance = image_.program.effects[typed->handle - 1].importance;
    if (definition->kind != VfxDefinitionKind::Line && ground_suppression.all_particles)
        return Result::Success();
    if (definition->kind == VfxDefinitionKind::Line)
    {
        if ((parameters.flags & static_cast<std::uint32_t>(VfxEventFlag::HasTarget)) == 0)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX line has no target.");
        candidate.required_count = candidate.maximum_required_count = 1;
        candidate.replacement_counts_ready = true;
        prepared = candidate;
        return Result::Success();
    }
    // The legacy release ring expands across safe gaps and disagrees with the
    // authoritative wavefront. Keep its other accents while the typed snapshot
    // owns this boundary for the area's entire lifetime.
    const bool typed_wavefront = event.asset.value == kShockwaveRelease.value &&
        event.geometry.kind == PresentationGeometryKind::RingGaps && image_.has_typed_wavefront;
    try
    {
        for (std::uint32_t index = 0; index < definition->emitter_count; ++index)
        {
            const auto emitter_index = definition->first_emitter + index;
            const auto &emitter = image_.emitters[emitter_index];
            if ((ground_suppression.ground_ring && emitter.renderer == VfxRenderer::Ground &&
                 emitter.primitive == VfxPrimitive::Ring) ||
                (ground_suppression.ground_disc && emitter.renderer == VfxRenderer::Ground &&
                 emitter.primitive == VfxPrimitive::Disc) ||
                (ground_suppression.segment_solid_trail && emitter.renderer == VfxRenderer::Segment &&
                 emitter.primitive == VfxPrimitive::SolidTrail))
                continue;
            if (typed_wavefront && emitter.renderer == VfxRenderer::Ground && emitter.primitive == VfxPrimitive::Ring)
                continue;
            emitter_indices.push_back(emitter_index);
        }
    }
    catch (...)
    {
        emitter_indices.resize(old_index_size);
        prepared = {};
        return Result::Failure(ErrorCode::TaskFailed, "hs_vfx", "Preparing legacy VFX emitter indices failed.");
    }
    candidate.index_count = emitter_indices.size() - candidate.first_index;
    if (auto result = SetPreparedReplacements(candidate, emitter_indices,
            particles_replaced, mesh_replaced); !result)
    {
        emitter_indices.resize(old_index_size);
        prepared = {};
        return result;
    }
    prepared = candidate;
    return Result::Success();
}

Result VfxCatalog::SetPreparedReplacements(VfxPreparedLegacyEvent &prepared,
    std::span<const std::uint32_t> emitter_indices,
    bool particles_replaced, bool mesh_replaced) const
{
    if (!prepared.definition)
        return prepared.event ? Result::Failure(ErrorCode::InvalidArgument, "hs_vfx",
            "Prepared legacy VFX definition is missing.") : Result::Success();
    if (!prepared.event)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Prepared legacy VFX event is missing.");
    if (prepared.definition->kind == VfxDefinitionKind::Line)
    {
        prepared.particles_replaced = particles_replaced;
        prepared.mesh_replaced = mesh_replaced;
        prepared.maximum_required_count = prepared.required_count = 1;
        prepared.count = prepared.admitted_count = 0;
        prepared.replacement_counts_ready = true;
        return Result::Success();
    }
    if (prepared.first_index > emitter_indices.size() ||
        prepared.index_count > emitter_indices.size() - prepared.first_index)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Prepared legacy VFX range is invalid.");
    if (prepared.replacement_counts_ready && prepared.particles_replaced == particles_replaced &&
        prepared.mesh_replaced == mesh_replaced)
        return Result::Success();

    for (const auto emitter_index : emitter_indices.subspan(prepared.first_index, prepared.index_count))
        if (emitter_index < prepared.definition->first_emitter ||
            emitter_index - prepared.definition->first_emitter >= prepared.definition->emitter_count)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Prepared legacy VFX emitter index is invalid.");

    prepared.particles_replaced = particles_replaced;
    prepared.mesh_replaced = mesh_replaced;
    prepared.maximum_required_count = 0;
    prepared.required_count = 0;
    prepared.count = 0;
    prepared.admitted_count = 0;
    for (const auto emitter_index : emitter_indices.subspan(prepared.first_index, prepared.index_count))
    {
        const auto &emitter = image_.emitters[emitter_index];
        const bool optional = prepared.decorative && emitter.renderer != VfxRenderer::Ground;
        if (!optional) ++prepared.maximum_required_count;
        if (particles_replaced ||
            (mesh_replaced && emitter.renderer == VfxRenderer::Mesh &&
             (emitter.primitive != VfxPrimitive::ShockShell ||
              prepared.event->asset.value == kExplosiveArrowMain.value)))
            continue;
        if (optional) ++prepared.count;
        else ++prepared.required_count;
    }
    prepared.admitted_count = prepared.count;
    prepared.replacement_counts_ready = true;
    return Result::Success();
}

Result VfxCatalog::AppendPreparedEvent(const VfxPreparedLegacyEvent &prepared,
    std::span<const std::uint32_t> emitter_indices,
    std::vector<ParticleSpawnCommand> &particles, std::vector<VfxLineSpawnCommand> &lines) const
{
    if (!prepared.definition) return Result::Success();
    if (!prepared.event || !prepared.replacement_counts_ready || prepared.admitted_count > prepared.count ||
        prepared.first_index > emitter_indices.size() ||
        prepared.index_count > emitter_indices.size() - prepared.first_index)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Prepared legacy VFX range or admission is invalid.");
    const auto &event = *prepared.event;
    const auto *definition = prepared.definition;
    const auto &parameters = prepared.parameters;
    const auto forward = prepared.forward, right = prepared.right;
    const auto decorative = prepared.decorative;
    const auto decorative_fraction = prepared.decorative_fraction;
    if (definition->kind == VfxDefinitionKind::Line)
    {
        if (prepared.maximum_required_count != 1 || prepared.required_count != 1 ||
            prepared.count != 0 || prepared.admitted_count != 0 || prepared.index_count != 0)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Prepared VFX line admission is invalid.");
        if (prepared.required_count)
            lines.push_back({event.sequence, event.tick, event.position, parameters.target,
                definition->line_color, definition->line_width * parameters.scale,
                definition->line_lifetime, definition->line_sprite,
                definition->line_frame_columns, definition->line_frame_rows,
                definition->line_uv_repeat, definition->line_scroll_speed,
                definition->line_primitive});
        return Result::Success();
    }
    for (const auto emitter_index : emitter_indices.subspan(prepared.first_index, prepared.index_count))
        if (emitter_index < definition->first_emitter ||
            emitter_index - definition->first_emitter >= definition->emitter_count)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Prepared legacy VFX emitter index is invalid.");
    if (prepared.maximum_required_count > prepared.index_count ||
        prepared.count > prepared.index_count - prepared.maximum_required_count ||
        prepared.required_count > prepared.index_count ||
        prepared.admitted_count > prepared.index_count - prepared.required_count)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Prepared legacy VFX counts exceed the emitter range.");
    if (prepared.required_count > particles.max_size() - particles.size() ||
        prepared.admitted_count > particles.max_size() - particles.size() - prepared.required_count)
        throw std::length_error("Prepared legacy VFX output exceeds vector capacity");
    particles.reserve(particles.size() + prepared.required_count + prepared.admitted_count);
    auto remaining = prepared.admitted_count;
    for (const auto emitter_index : emitter_indices.subspan(prepared.first_index, prepared.index_count))
    {
        const auto &emitter = image_.emitters[emitter_index];
        if (prepared.particles_replaced ||
            (prepared.mesh_replaced && emitter.renderer == VfxRenderer::Mesh &&
             (emitter.primitive != VfxPrimitive::ShockShell ||
              event.asset.value == kExplosiveArrowMain.value)))
            continue;
        const auto index = emitter_index - definition->first_emitter;
        if (decorative && emitter.renderer != VfxRenderer::Ground)
        {
            if (!remaining) continue;
            --remaining;
        }
        const auto scaled = parameters.scale;
        ParticleSpawnCommand command;
        command.sequence = event.sequence;
        command.tick = event.tick;
        command.position = {event.position.x + scaled * (right.x * emitter.local_offset.x + forward.x * emitter.local_offset.z),
                            event.position.y + scaled * emitter.local_offset.y,
                            event.position.z + scaled * (right.z * emitter.local_offset.x + forward.z * emitter.local_offset.z)};
        command.shape = emitter.shape;
        command.velocity_mode = emitter.velocity;
        command.facing = emitter.facing;
        command.renderer = emitter.renderer;
        if (emitter.renderer == VfxRenderer::Mesh)
            command.mesh_index = image_.authored_mesh_indices[emitter_index];
        command.primitive = emitter.primitive;
        command.sprite = emitter.sprite;
        command.frame_columns = emitter.frame_columns;
        command.frame_rows = emitter.frame_rows;
        command.shape_extent = {emitter.shape_extent.x * scaled, emitter.shape_extent.y * scaled,
                                emitter.shape_extent.z * scaled};
        command.direction = {right.x * emitter.local_direction.x + emitter.local_direction.y * 0.0f + forward.x * emitter.local_direction.z,
                             emitter.local_direction.y,
                             right.z * emitter.local_direction.x + forward.z * emitter.local_direction.z};
        command.speed_min = emitter.speed_min;
        command.speed_max = emitter.speed_max;
        command.cone_radians = emitter.cone_degrees * 0.01745329251994329577f;
        command.lifetime_min = emitter.lifetime_min;
        command.lifetime_max = emitter.lifetime_max;
        command.start_color = emitter.start_color;
        command.end_color = emitter.end_color;
        command.start_size_min = emitter.start_size_min * scaled;
        command.start_size_max = emitter.start_size_max * scaled;
        command.end_size_min = emitter.end_size_min * scaled;
        command.end_size_max = emitter.end_size_max * scaled;
        command.gravity = emitter.gravity;
        command.rotation_min = emitter.rotation_min;
        command.rotation_max = emitter.rotation_max;
        command.angular_velocity_min = emitter.angular_velocity_min;
        command.angular_velocity_max = emitter.angular_velocity_max;
        command.stretch = emitter.stretch;
        command.uv_repeat = 1.0f;
        command.scroll_speed = 0.0f;
        command.count = decorative && emitter.renderer != VfxRenderer::Ground
            ? static_cast<std::uint32_t>(std::ceil(double(emitter.count) * decorative_fraction)) : emitter.count;
        command.seed = SpawnSeed(event.sequence, index);
        particles.push_back(command);
    }
    return Result::Success();
}

} // namespace hs
