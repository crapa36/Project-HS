#include "vfx_catalog.hpp"

#include <hs/core/cooked_format.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace hs
{
namespace
{
std::uint32_t SpawnSeed(Sequence sequence, std::uint32_t emitter) noexcept
{
    auto value = sequence ^ (0x9e3779b97f4a7c15ull + emitter);
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27;
    return static_cast<std::uint32_t>(value ^ (value >> 31));
}
}

Result VfxCatalog::Load(const std::filesystem::path &path, VfxCatalog &catalog)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle catalog is missing.");
    const auto size = stream.tellg();
    if (size < static_cast<std::streamoff>(sizeof(CookedParticleEffectsHeader)))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle catalog header is missing.");
    stream.seekg(0);
    CookedParticleEffectsHeader header;
    stream.read(reinterpret_cast<char *>(&header), sizeof(header));
    const char magic[8]{'H', 'S', 'P', 'F', 'X', '\0', '\0', '\0'};
    if (std::memcmp(header.magic, magic, sizeof(magic)) != 0 ||
        header.format_version != 3 || header.sprite_count == 0 ||
        header.schema_hash != kParticleEffectsSchemaHash)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle catalog header is incompatible.");
    const auto payload_size = static_cast<std::uint64_t>(header.effect_count) * sizeof(CookedVfxDefinition) +
                              static_cast<std::uint64_t>(header.emitter_count) * sizeof(CookedParticleEmitter) +
                              static_cast<std::uint64_t>(header.sprite_count) * sizeof(CookedParticleSprite);
    if (payload_size > std::numeric_limits<std::uint32_t>::max() ||
        static_cast<std::uint64_t>(size) != sizeof(header) + payload_size)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle catalog size is invalid.");
    std::vector<std::byte> payload(static_cast<std::size_t>(payload_size));
    if (!stream.read(reinterpret_cast<char *>(payload.data()), static_cast<std::streamsize>(payload.size())) ||
        Fnv1a64(payload) != header.payload_hash)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle catalog payload is corrupt.");

    VfxCatalog loaded;
    loaded.definitions_.resize(header.effect_count);
    loaded.emitters_.resize(header.emitter_count);
    loaded.sprites_.resize(header.sprite_count);
    std::memcpy(loaded.definitions_.data(), payload.data(), loaded.definitions_.size() * sizeof(CookedVfxDefinition));
    std::memcpy(loaded.emitters_.data(), payload.data() + loaded.definitions_.size() * sizeof(CookedVfxDefinition),
                loaded.emitters_.size() * sizeof(CookedParticleEmitter));
    std::memcpy(loaded.sprites_.data(), payload.data() +
                    loaded.definitions_.size() * sizeof(CookedVfxDefinition) +
                    loaded.emitters_.size() * sizeof(CookedParticleEmitter),
                loaded.sprites_.size() * sizeof(CookedParticleSprite));
    std::uint64_t previous{};
    for (std::size_t index = 0; index < loaded.definitions_.size(); ++index)
    {
        const auto &definition = loaded.definitions_[index];
        if (definition.effect_id == 0 || (index && definition.effect_id <= previous) || definition.reserved != 0 ||
            static_cast<std::uint64_t>(definition.first_emitter) + definition.emitter_count > loaded.emitters_.size())
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle definition table is invalid.");
        if ((definition.kind == VfxDefinitionKind::Particles) != (definition.emitter_count != 0))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle definition kind is invalid.");
        if (definition.kind == VfxDefinitionKind::Line &&
            definition.line_primitive < VfxPrimitive::SolidTrail)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX line primitive is invalid.");
        if (definition.line_sprite >= header.sprite_count || definition.line_frame_columns == 0 ||
            definition.line_frame_rows == 0)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle definition sprite is invalid.");
        previous = definition.effect_id;
    }
    for (const auto &emitter : loaded.emitters_)
    {
        const auto valid_primitive =
            (emitter.renderer == VfxRenderer::Sprite && emitter.primitive == VfxPrimitive::Soft) ||
            (emitter.renderer == VfxRenderer::Ground && emitter.primitive >= VfxPrimitive::Disc &&
             emitter.primitive <= VfxPrimitive::Cracks) ||
            (emitter.renderer == VfxRenderer::Segment &&
             emitter.primitive >= VfxPrimitive::SolidTrail) ||
            (emitter.renderer == VfxRenderer::Mesh && emitter.primitive >= VfxPrimitive::Arrow &&
             emitter.primitive <= VfxPrimitive::ShockShell);
        if (emitter.sprite >= header.sprite_count || emitter.frame_columns == 0 ||
            emitter.frame_rows == 0 || !valid_primitive)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle emitter contract is invalid.");
    }
    previous = 0;
    for (std::size_t index = 0; index < loaded.sprites_.size(); ++index)
    {
        const auto &sprite = loaded.sprites_[index];
        if (sprite.sprite_id == 0 || (index && sprite.sprite_id <= previous) ||
            sprite.index >= header.sprite_count || sprite.frame_columns == 0 ||
            sprite.frame_rows == 0)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle sprite table is invalid.");
        previous = sprite.sprite_id;
    }
    loaded.sprite_count_ = header.sprite_count;
    loaded.payload_hash_ = header.payload_hash;
    catalog = std::move(loaded);
    return Result::Success();
}

const CookedVfxDefinition *VfxCatalog::Find(EffectId id) const noexcept
{
    const auto found = std::lower_bound(definitions_.begin(), definitions_.end(), id.value,
        [](const CookedVfxDefinition &definition, std::uint64_t value) { return definition.effect_id < value; });
    return found != definitions_.end() && found->effect_id == id.value ? &*found : nullptr;
}

const CookedParticleSprite *VfxCatalog::FindSprite(AssetId id) const noexcept
{
    const auto found = std::lower_bound(sprites_.begin(), sprites_.end(), id.value,
        [](const CookedParticleSprite &sprite, std::uint64_t value) {
            return sprite.sprite_id < value;
        });
    return found != sprites_.end() && found->sprite_id == id.value ? &*found : nullptr;
}

Result VfxCatalog::Expand(const PresentationEvent &event, std::uint32_t quality_percent,
                          std::vector<ParticleSpawnCommand> &particles,
                          std::vector<EffectLineSpawnCommand> &lines) const
{
    if (event.kind != PresentationKind::Vfx)
        return Result::Success();
    const auto *definition = Find(event.asset);
    if (!definition)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX effect ID is missing.");
    const auto parameters = DecodeVfxParameters(event.parameters);
    const auto direction_length = std::hypot(parameters.direction.x, parameters.direction.z);
    if (!std::isfinite(parameters.scale) || parameters.scale <= 0.0f || direction_length <= 0.0001f)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX event parameters are invalid.");
    const Float3 forward{parameters.direction.x / direction_length, 0.0f, parameters.direction.z / direction_length};
    const Float3 right{forward.z, 0.0f, -forward.x};
    if (definition->kind == VfxDefinitionKind::Line)
    {
        if ((parameters.flags & static_cast<std::uint32_t>(VfxEventFlag::HasTarget)) == 0)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX line has no target.");
        lines.push_back({event.sequence, event.tick, event.position, parameters.target,
                         definition->line_color, definition->line_width * parameters.scale,
                         definition->line_lifetime, definition->line_sprite,
                         definition->line_frame_columns, definition->line_frame_rows,
                         definition->line_uv_repeat, definition->line_scroll_speed,
                         definition->line_primitive});
        return Result::Success();
    }
    for (std::uint32_t index = 0; index < definition->emitter_count; ++index)
    {
        const auto &emitter = emitters_[definition->first_emitter + index];
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
        command.count = quality_percent <= 50 ? (emitter.count + 1) / 2 : emitter.count;
        command.seed = SpawnSeed(event.sequence, index);
        particles.push_back(command);
    }
    return Result::Success();
}

} // namespace hs
