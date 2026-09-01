#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "content_cooker.hpp"

#include <DirectXTex.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace hs::content
{

template <typename T, std::size_t N>
T ParseEnum(std::string_view value, const std::array<std::pair<std::string_view, T>, N> &values, std::string_view path)
{
    const auto found = std::ranges::find(values, value, &std::pair<std::string_view, T>::first);
    if (found == values.end())
        ThrowValidationError("particles", path, "unknown enum value");
    return found->second;
}

bool WriteCookedParticleEffects(const std::filesystem::path &path, const Json &document,
                                std::vector<ParticleSpriteSource> &sprite_sources, std::string &error_message)
{
    constexpr std::array<std::pair<std::string_view, hs::ParticleShape>, 5> shapes{
        {std::pair{std::string_view{"point"}, hs::ParticleShape::Point},
         {std::string_view{"sphere"}, hs::ParticleShape::Sphere},
         {std::string_view{"disc"}, hs::ParticleShape::Disc},
         {std::string_view{"ring"}, hs::ParticleShape::Ring},
         {std::string_view{"line"}, hs::ParticleShape::Line}}};
    constexpr std::array<std::pair<std::string_view, hs::ParticleVelocity>, 5> velocities{
        {std::pair{std::string_view{"direction"}, hs::ParticleVelocity::Direction},
         {std::string_view{"cone"}, hs::ParticleVelocity::Cone},
         {std::string_view{"radial"}, hs::ParticleVelocity::Radial},
         {std::string_view{"inward"}, hs::ParticleVelocity::Inward},
         {std::string_view{"upward"}, hs::ParticleVelocity::Upward}}};
    constexpr std::array<std::pair<std::string_view, hs::ParticleFacing>, 3> facings{
        {std::pair{std::string_view{"camera"}, hs::ParticleFacing::Camera},
         {std::string_view{"velocity"}, hs::ParticleFacing::Velocity},
         {std::string_view{"ground"}, hs::ParticleFacing::Ground}}};
    constexpr std::array<std::pair<std::string_view, hs::VfxRenderer>, 4> renderers{
        {std::pair{std::string_view{"sprite"}, hs::VfxRenderer::Sprite},
         {std::string_view{"ground"}, hs::VfxRenderer::Ground},
         {std::string_view{"segment"}, hs::VfxRenderer::Segment},
         {std::string_view{"mesh"}, hs::VfxRenderer::Mesh}}};
    constexpr std::array<std::pair<std::string_view, hs::VfxPrimitive>, 18> primitives{
        {std::pair{std::string_view{"soft"}, hs::VfxPrimitive::Soft},
         {std::string_view{"disc"}, hs::VfxPrimitive::Disc},
         {std::string_view{"ring"}, hs::VfxPrimitive::Ring},
         {std::string_view{"sector"}, hs::VfxPrimitive::Sector},
         {std::string_view{"chevron"}, hs::VfxPrimitive::Chevron},
         {std::string_view{"rune"}, hs::VfxPrimitive::Rune},
         {std::string_view{"cracks"}, hs::VfxPrimitive::Cracks},
         {std::string_view{"arrow"}, hs::VfxPrimitive::Arrow},
         {std::string_view{"shard"}, hs::VfxPrimitive::Shard},
         {std::string_view{"ember"}, hs::VfxPrimitive::Ember},
         {std::string_view{"spike"}, hs::VfxPrimitive::Spike},
         {std::string_view{"shock_shell"}, hs::VfxPrimitive::ShockShell},
         {std::string_view{"solid_trail"}, hs::VfxPrimitive::SolidTrail},
         {std::string_view{"dashed_ricochet"}, hs::VfxPrimitive::DashedRicochet},
         {std::string_view{"fire_transfer"}, hs::VfxPrimitive::FireTransfer},
         {std::string_view{"relic_chain"}, hs::VfxPrimitive::RelicChain},
          {std::string_view{"dash_wake"}, hs::VfxPrimitive::DashWake},
          {std::string_view{"flame"}, hs::VfxPrimitive::Flame}}};
    const auto read_values = [](const Json &object, std::string_view key, std::size_t count, std::string_view path) {
        const auto &array = RequireArray(object, "particles", path, key);
        if (array.size() != count)
            ThrowValidationError("particles", std::string(path) + "/" + std::string(key), "wrong vector size");
        std::array<float, 4> values{};
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!array[i].is_number())
                ThrowValidationError("particles", path, "expected number");
            values[i] = array[i].get<float>();
            if (!std::isfinite(values[i]))
                ThrowValidationError("particles", path, "non-finite number");
        }
        return values;
    };
    std::vector<hs::CookedVfxDefinition> definitions;
    std::vector<hs::CookedParticleEmitter> emitters;
    std::vector<hs::CookedParticleSprite> cooked_sprites;
    std::unordered_map<std::string, hs::ParticleSprite> sprite_indices;
    std::unordered_map<std::uint64_t, std::string> sprite_hashes;
    sprite_sources.clear();
    const auto &sprites = RequireArray(document, "particles", "$", "sprites");
    if (sprites.size() > std::numeric_limits<hs::ParticleSprite>::max())
        ThrowValidationError("particles", "$/sprites", "too many sprites");
    for (std::size_t index = 0; index < sprites.size(); ++index)
    {
        const auto sprite_path = "$/sprites/" + std::to_string(index);
        RequireExactKeys(sprites[index], "particles", {"id", "file", "frames"});
        ParticleSpriteSource sprite;
        sprite.id = RequireString(sprites[index], "particles", sprite_path, "id");
        sprite.file = RequireString(sprites[index], "particles", sprite_path, "file");
        const auto frames = read_values(sprites[index], "frames", 2, sprite_path);
        sprite.frame_columns = static_cast<std::uint8_t>(frames[0]);
        sprite.frame_rows = static_cast<std::uint8_t>(frames[1]);
        if (frames[0] != sprite.frame_columns || frames[1] != sprite.frame_rows ||
            !sprite_indices.emplace(sprite.id, static_cast<hs::ParticleSprite>(index)).second)
            ThrowValidationError("particles", sprite_path, "invalid or duplicate sprite");
        const auto sprite_hash = hs::MakeAssetId("particle_sprite." + sprite.id).value;
        if (!sprite_hashes.emplace(sprite_hash, sprite.id).second)
            ThrowValidationError("particles", sprite_path, "sprite hash collision");
        cooked_sprites.push_back(
            {sprite_hash, static_cast<hs::ParticleSprite>(index), sprite.frame_columns, sprite.frame_rows});
        sprite_sources.push_back(std::move(sprite));
    }
    std::unordered_map<std::uint64_t, std::string> hashes;
    const auto &effects = RequireArray(document, "particles", "$", "effects");
    for (std::size_t effect_index = 0; effect_index < effects.size(); ++effect_index)
    {
        const auto base = "$/effects/" + std::to_string(effect_index);
        const auto id = RequireString(effects[effect_index], "particles", base, "id");
        if (!id.starts_with("particle."))
            ThrowValidationError("particles", base + "/id", "effect ID must start with particle.");
        const auto hash = hs::MakeAssetId(id).value;
        if (!hashes.emplace(hash, id).second)
            ThrowValidationError("particles", base + "/id", "duplicate ID or hash collision");
        hs::CookedVfxDefinition definition;
        definition.effect_id = hash;
        const auto kind = RequireString(effects[effect_index], "particles", base, "kind");
        if (kind == "line")
        {
            RequireExactKeys(
                effects[effect_index], "particles",
                {"id", "kind", "color", "width", "lifetime", "sprite", "uv_repeat", "scroll_speed", "primitive"});
            definition.kind = hs::VfxDefinitionKind::Line;
            const auto color = read_values(effects[effect_index], "color", 4, base);
            definition.line_color = {color[0], color[1], color[2], color[3]};
            definition.line_width =
                static_cast<float>(RequireNumber(effects[effect_index], "particles", base, "width"));
            definition.line_lifetime =
                static_cast<float>(RequireNumber(effects[effect_index], "particles", base, "lifetime"));
            const auto sprite_name = RequireString(effects[effect_index], "particles", base, "sprite");
            const auto sprite = sprite_indices.find(sprite_name);
            if (sprite == sprite_indices.end())
                ThrowValidationError("particles", base + "/sprite", "unknown sprite");
            definition.line_sprite = sprite->second;
            definition.line_frame_columns = sprite_sources[sprite->second].frame_columns;
            definition.line_frame_rows = sprite_sources[sprite->second].frame_rows;
            definition.line_uv_repeat =
                static_cast<float>(RequireNumber(effects[effect_index], "particles", base, "uv_repeat"));
            definition.line_scroll_speed =
                static_cast<float>(RequireNumber(effects[effect_index], "particles", base, "scroll_speed"));
            definition.line_primitive = ParseEnum(RequireString(effects[effect_index], "particles", base, "primitive"),
                                                  primitives, base + "/primitive");
            if (definition.line_primitive < hs::VfxPrimitive::SolidTrail ||
                definition.line_primitive > hs::VfxPrimitive::DashWake)
                ThrowValidationError("particles", base + "/primitive", "line requires a segment primitive");
            if (definition.line_width <= 0 || definition.line_lifetime <= 0 || definition.line_uv_repeat <= 0)
                ThrowValidationError("particles", base, "line range invalid");
        }
        else if (kind == "particles")
        {
            RequireExactKeys(effects[effect_index], "particles", {"id", "kind", "emitters"});
            definition.kind = hs::VfxDefinitionKind::Particles;
            definition.first_emitter = static_cast<std::uint32_t>(emitters.size());
            const auto &source_emitters = RequireArray(effects[effect_index], "particles", base, "emitters");
            if (source_emitters.empty() || source_emitters.size() > std::numeric_limits<std::uint16_t>::max())
                ThrowValidationError("particles", base + "/emitters", "invalid emitter count");
            definition.emitter_count = static_cast<std::uint16_t>(source_emitters.size());
            for (std::size_t emitter_index = 0; emitter_index < source_emitters.size(); ++emitter_index)
            {
                const auto emitter_path = base + "/emitters/" + std::to_string(emitter_index);
                const auto &source = source_emitters[emitter_index];
                RequireExactKeys(source, "particles", {"sprite",
                                                       "renderer",
                                                       "primitive",
                                                       "shape",
                                                       "velocity",
                                                       "facing",
                                                       "local_offset",
                                                       "shape_extent",
                                                       "local_direction",
                                                       "start_color",
                                                       "end_color",
                                                       "lifetime",
                                                       "speed",
                                                       "cone_degrees",
                                                       "start_size",
                                                       "end_size",
                                                       "gravity",
                                                       "rotation",
                                                       "angular_velocity",
                                                       "stretch",
                                                       "count"});
                hs::CookedParticleEmitter emitter;
                const auto sprite_name = RequireString(source, "particles", emitter_path, "sprite");
                const auto sprite = sprite_indices.find(sprite_name);
                if (sprite == sprite_indices.end())
                    ThrowValidationError("particles", emitter_path + "/sprite", "unknown sprite");
                emitter.sprite = sprite->second;
                emitter.frame_columns = sprite_sources[sprite->second].frame_columns;
                emitter.frame_rows = sprite_sources[sprite->second].frame_rows;
                emitter.shape =
                    ParseEnum(RequireString(source, "particles", emitter_path, "shape"), shapes, emitter_path);
                emitter.velocity =
                    ParseEnum(RequireString(source, "particles", emitter_path, "velocity"), velocities, emitter_path);
                emitter.facing =
                    ParseEnum(RequireString(source, "particles", emitter_path, "facing"), facings, emitter_path);
                emitter.renderer = ParseEnum(RequireString(source, "particles", emitter_path, "renderer"), renderers,
                                             emitter_path + "/renderer");
                emitter.primitive = ParseEnum(RequireString(source, "particles", emitter_path, "primitive"), primitives,
                                              emitter_path + "/primitive");
                const auto offset = read_values(source, "local_offset", 3, emitter_path),
                           extent = read_values(source, "shape_extent", 3, emitter_path),
                           direction = read_values(source, "local_direction", 3, emitter_path);
                const auto start_color = read_values(source, "start_color", 4, emitter_path),
                           end_color = read_values(source, "end_color", 4, emitter_path);
                const auto lifetime = read_values(source, "lifetime", 2, emitter_path),
                           speed = read_values(source, "speed", 2, emitter_path),
                           start_size = read_values(source, "start_size", 2, emitter_path),
                           end_size = read_values(source, "end_size", 2, emitter_path),
                           rotation = read_values(source, "rotation", 2, emitter_path),
                           angular = read_values(source, "angular_velocity", 2, emitter_path);
                emitter.local_offset = {offset[0], offset[1], offset[2]};
                emitter.shape_extent = {extent[0], extent[1], extent[2]};
                emitter.local_direction = {direction[0], direction[1], direction[2]};
                emitter.start_color = {start_color[0], start_color[1], start_color[2], start_color[3]};
                emitter.end_color = {end_color[0], end_color[1], end_color[2], end_color[3]};
                emitter.lifetime_min = lifetime[0];
                emitter.lifetime_max = lifetime[1];
                emitter.speed_min = speed[0];
                emitter.speed_max = speed[1];
                emitter.start_size_min = start_size[0];
                emitter.start_size_max = start_size[1];
                emitter.end_size_min = end_size[0];
                emitter.end_size_max = end_size[1];
                emitter.rotation_min = rotation[0];
                emitter.rotation_max = rotation[1];
                emitter.angular_velocity_min = angular[0];
                emitter.angular_velocity_max = angular[1];
                emitter.cone_degrees =
                    static_cast<float>(RequireNumber(source, "particles", emitter_path, "cone_degrees"));
                emitter.gravity = static_cast<float>(RequireNumber(source, "particles", emitter_path, "gravity"));
                emitter.stretch = static_cast<float>(RequireNumber(source, "particles", emitter_path, "stretch"));
                emitter.count = static_cast<std::uint32_t>(RequireInteger(source, "particles", emitter_path, "count"));
                const auto direction_length =
                    std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
                if (lifetime[0] <= 0 || lifetime[0] > lifetime[1] || speed[0] < 0 || speed[0] > speed[1] ||
                    start_size[0] < 0 || start_size[0] > start_size[1] || end_size[0] < 0 ||
                    end_size[0] > end_size[1] || rotation[0] > rotation[1] || angular[0] > angular[1] ||
                    emitter.count == 0 || emitter.cone_degrees < 0 || emitter.cone_degrees > 180 ||
                    emitter.stretch < 1 ||
                    ((emitter.velocity == hs::ParticleVelocity::Direction ||
                      emitter.velocity == hs::ParticleVelocity::Cone) &&
                     direction_length <= 0.0001f) ||
                    (emitter.velocity == hs::ParticleVelocity::Inward && emitter.shape == hs::ParticleShape::Point) ||
                    (emitter.renderer == hs::VfxRenderer::Sprite &&
                     emitter.primitive != hs::VfxPrimitive::Soft && emitter.primitive != hs::VfxPrimitive::Flame) ||
                    (emitter.renderer == hs::VfxRenderer::Ground &&
                     ((emitter.primitive < hs::VfxPrimitive::Disc || emitter.primitive > hs::VfxPrimitive::Cracks) &&
                      emitter.primitive != hs::VfxPrimitive::Flame)) ||
                    (emitter.renderer == hs::VfxRenderer::Segment &&
                     (emitter.primitive < hs::VfxPrimitive::SolidTrail ||
                      emitter.primitive > hs::VfxPrimitive::DashWake)) ||
                    (emitter.renderer == hs::VfxRenderer::Mesh &&
                     (emitter.primitive < hs::VfxPrimitive::Arrow || emitter.primitive > hs::VfxPrimitive::ShockShell)))
                    ThrowValidationError("particles", emitter_path, "emitter contract violation");
                emitters.push_back(emitter);
            }
        }
        else
            ThrowValidationError("particles", base + "/kind", "unknown effect kind");
        definitions.push_back(definition);
    }
    std::ranges::sort(definitions, {}, &hs::CookedVfxDefinition::effect_id);
    std::ranges::sort(cooked_sprites, {}, &hs::CookedParticleSprite::sprite_id);
    std::vector<std::byte> payload(definitions.size() * sizeof(definitions.front()) +
                                   emitters.size() * sizeof(emitters.front()) +
                                   cooked_sprites.size() * sizeof(cooked_sprites.front()));
    std::memcpy(payload.data(), definitions.data(), definitions.size() * sizeof(definitions.front()));
    std::memcpy(payload.data() + definitions.size() * sizeof(definitions.front()), emitters.data(),
                emitters.size() * sizeof(emitters.front()));
    std::memcpy(payload.data() + definitions.size() * sizeof(definitions.front()) +
                    emitters.size() * sizeof(emitters.front()),
                cooked_sprites.data(), cooked_sprites.size() * sizeof(cooked_sprites.front()));
    hs::CookedParticleEffectsHeader header;
    header.effect_count = static_cast<std::uint32_t>(definitions.size());
    header.emitter_count = static_cast<std::uint32_t>(emitters.size());
    header.sprite_count = static_cast<std::uint32_t>(sprite_sources.size());
    header.schema_hash = hs::kParticleEffectsSchemaHash;
    header.payload_hash = hs::Fnv1a64(payload);
    std::vector<std::byte> file(sizeof(header) + payload.size());
    std::memcpy(file.data(), &header, sizeof(header));
    std::memcpy(file.data() + sizeof(header), payload.data(), payload.size());
    return AtomicWrite(path, file, error_message);
}

bool WriteVfxMaskArray(const std::filesystem::path &path, std::span<const ParticleSpriteSource> sprites,
                       std::string &error_message)
{
    DirectX::ScratchImage masks;
    auto result = masks.Initialize2D(DXGI_FORMAT_R8_UNORM, 512, 512, sprites.size(), 1);
    if (FAILED(result))
    {
        error_message = "cannot allocate VFX mask array";
        return false;
    }
    for (std::size_t slice = 0; slice < sprites.size(); ++slice)
    {
        DirectX::TexMetadata metadata;
        DirectX::ScratchImage source;
        const auto source_path = std::filesystem::path(HS_VFX_TEXTURE_DIRECTORY) / sprites[slice].file;
        result = DirectX::LoadFromWICFile(source_path.c_str(), DirectX::WIC_FLAGS_NONE, &metadata, source);
        DirectX::ScratchImage rgba;
        if (SUCCEEDED(result))
            result = DirectX::Convert(source.GetImages(), source.GetImageCount(), source.GetMetadata(),
                                      DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0.0f, rgba);
        const auto *input = rgba.GetImage(0, 0, 0);
        auto *output = masks.GetImage(0, slice, 0);
        if (FAILED(result) || !input || !output || metadata.width != 512 || metadata.height != 512)
        {
            error_message = "invalid VFX mask: " + source_path.string();
            return false;
        }
        for (std::size_t row = 0; row < 512; ++row)
            for (std::size_t column = 0; column < 512; ++column)
                output->pixels[row * output->rowPitch + column] = input->pixels[row * input->rowPitch + column * 4 + 3];
    }
    DirectX::ScratchImage mipmaps;
    result = DirectX::GenerateMipMaps(masks.GetImages(), masks.GetImageCount(), masks.GetMetadata(),
                                      DirectX::TEX_FILTER_DEFAULT, 0, mipmaps);
    DirectX::ScratchImage compressed;
    if (SUCCEEDED(result))
        result = DirectX::Compress(mipmaps.GetImages(), mipmaps.GetImageCount(), mipmaps.GetMetadata(),
                                   DXGI_FORMAT_BC4_UNORM, DirectX::TEX_COMPRESS_DEFAULT, 0.5f, compressed);
    if (SUCCEEDED(result))
        result = DirectX::SaveToDDSFile(compressed.GetImages(), compressed.GetImageCount(), compressed.GetMetadata(),
                                        DirectX::DDS_FLAGS_NONE, path.c_str());
    if (FAILED(result))
    {
        error_message = "cannot cook BC4 VFX mask array";
        return false;
    }
    return true;
}

} // namespace hs::content
