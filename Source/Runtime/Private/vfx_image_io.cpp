#include <hs/runtime/vfx_image_io.hpp>
#include <hs/core/cooked_format.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <iostream>
#include <string_view>
#include <utility>
#include <cmath>
#include <bit>
namespace hs {
namespace {
constexpr std::uint32_t ShapeId(std::string_view name) noexcept
{
    std::uint32_t hash = 2166136261u;
    for (const unsigned char character : name) { hash ^= character; hash *= 16777619u; }
    return hash;
}
std::uint8_t MeshIndex(std::uint32_t shape) noexcept
{
    constexpr std::string_view names[]{"arrowhead_mesh", "shard_mesh", "debris_shard_mesh",
        "small_shard_mesh", "enemy_thorn_mesh", "boss_crest_lance_mesh", "needle_shard_mesh"};
    for (std::uint8_t index = 0; index < 7; ++index)
        if (shape == ShapeId(names[index])) return index + 1;
    return 0;
}
}

void PrepareVfxLightRecipes(VfxProgramView p, std::vector<VfxRange> &ranges,
                            std::vector<VfxLightRecipe> &recipes)
{
    ranges.assign(p.effects.size(), {}); recipes.clear();
    const auto valid_range=[](VfxRange r,std::size_t n){return r.first<=n && r.count<=n-r.first;};
    const auto mix=[](std::uint64_t v){v^=v>>30;v*=0xbf58476d1ce4e5b9ull;v^=v>>27;v*=0x94d049bb133111ebull;return v^(v>>31);};
    for(std::size_t ei=0;ei<p.effects.size();++ei)
    {
        auto &range=ranges[ei]; range.first=static_cast<std::uint32_t>(recipes.size());
        const auto &e=p.effects[ei];
        if(e.handle!=ei+1 || e.input_mode!=0 || e.timing_kind!=0 || !std::isfinite(e.seconds) || e.seconds<=0 || !valid_range(e.sources,p.sources.size()))continue;
        const auto id=std::find_if(p.effect_lookup.begin(),p.effect_lookup.end(),[&](const auto &v){return v.handle==e.handle;});
        if(id==p.effect_lookup.end() || !id->effect_id)continue;
        for(std::uint32_t si=e.sources.first;si<e.sources.first+e.sources.count;++si)
        {
            const auto &source=p.sources[si];
            if(source.effect!=e.handle || source.type!=VfxSourceType::Direct || source.parameters.count ||
                !valid_range(source.parameters,p.parameters.size()) || !valid_range(source.outputs,p.outputs.size()) ||
                (source.knot_count!=2 && source.knot_count!=4))continue;
            bool valid=true;
            for(std::uint32_t k=0;k<source.knot_count;++k)
                if(!std::isfinite(source.knots[k]) || source.knots[k]<0 || source.knots[k]>1 || (k && source.knots[k]<source.knots[k-1]))valid=false;
            if(!valid || source.knots[source.knot_count-1]<=source.knots[0])continue;
            for(const auto &o:p.outputs.subspan(source.outputs.first,source.outputs.count))
            {
                const bool hero=o.motion==VfxMotionKind::SingleHighPriorityLightPulse;
                if(o.source!=si || o.profile!=VfxOutputProfile::Light || (!hero && o.motion!=VfxMotionKind::SingleCappedLightPulse && o.motion!=VfxMotionKind::GentleGreenPulse) ||
                    !valid_range(o.parameters,p.parameters.size()) || o.parameters.count!=(hero?1u:2u))continue;
                const VfxParameterRecord *radius=nullptr,*curve=nullptr; valid=true;
                for(const auto &a:p.parameters.subspan(o.parameters.first,o.parameters.count))
                    if(a.key==ShapeId("radius") && !radius)radius=&a;
                    else if(a.key==ShapeId("intensity_curve_ref") && !curve)curve=&a;
                    else valid=false;
                if(!valid || !radius || radius->type!=VfxParameterType::Float)continue;
                const float metres=std::bit_cast<float>(radius->bits);
                if(!std::isfinite(metres) || metres<=0)continue;
                VfxRange keys{};std::uint32_t interpolation{};
                if(!hero)
                {
                    if(!curve || curve->type!=VfxParameterType::CurveRow || curve->bits>=p.curves.size())continue;
                    const auto &c=p.curves[curve->bits];
                    if(c.interpolation>1 || c.domain!=ShapeId("normalized_age") || !valid_range(c.keys,p.curve_keys.size()) || c.keys.count<2)continue;
                    auto values=p.curve_keys.subspan(c.keys.first,c.keys.count);
                    if(values.front().time!=0 || values.back().time!=1)continue;
                    valid=true;
                    for(std::size_t k=0;k<values.size();++k)
                        if(!std::isfinite(values[k].time)||!std::isfinite(values[k].value)||values[k].value<0||(k&&values[k].time<=values[k-1].time))valid=false;
                    if(!valid)continue;keys=c.keys;interpolation=c.interpolation;
                }
                if(!std::isfinite(o.rgba[0])||!std::isfinite(o.rgba[1])||!std::isfinite(o.rgba[2])||o.rgba[0]<0||o.rgba[1]<0||o.rgba[2]<0||
                    !std::isfinite(o.hdr)||o.hdr<=0||!std::isfinite(o.rgba[3])||o.rgba[3]<0||o.rgba[3]>1)continue;
                const auto output_id=(static_cast<std::uint64_t>(o.motion)<<32)|o.gradient_row;
                recipes.push_back({mix(id->effect_id)^mix(source.stable_id)^mix(output_id),e.payload_kind,e.importance,o.min_quality,source.knot_count,
                    e.seconds,metres,source.knots,{o.rgba[0],o.rgba[1],o.rgba[2]},o.hdr,o.rgba[3],keys,interpolation,hero});
            }
        }
        range.count=static_cast<std::uint32_t>(recipes.size())-range.first;
    }
}
Result DecodeVfxImage(std::span<const std::byte> legacy, std::span<const std::byte> program_bytes,
                      VfxImageData &output)
{
    if (legacy.size() < sizeof(CookedParticleEffectsHeader))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle catalog header is missing.");
    CookedParticleEffectsHeader header;
    std::memcpy(&header, legacy.data(), sizeof(header));
    const char magic[8]{'H', 'S', 'P', 'F', 'X', '\0', '\0', '\0'};
    if (std::memcmp(header.magic, magic, sizeof(magic)) != 0 ||
        header.format_version != 4 || header.sprite_count == 0 || header.schema_hash != kParticleEffectsSchemaHash)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle catalog header is incompatible.");
    const auto payload_size = static_cast<std::uint64_t>(header.effect_count) * sizeof(CookedVfxDefinition) +
        static_cast<std::uint64_t>(header.emitter_count) * sizeof(CookedParticleEmitter) +
        static_cast<std::uint64_t>(header.sprite_count) * sizeof(CookedParticleSprite);
    if (payload_size > std::numeric_limits<std::uint32_t>::max() || legacy.size() != sizeof(header) + payload_size)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle catalog size is invalid.");
    const auto payload = legacy.subspan(sizeof(header));
    if (Fnv1a64(payload) != header.payload_hash)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle catalog payload is corrupt.");
    VfxImageData loaded;
    loaded.definitions.resize(header.effect_count);
    loaded.emitters.resize(header.emitter_count);
    loaded.sprites.resize(header.sprite_count);
    std::memcpy(loaded.definitions.data(), payload.data(), loaded.definitions.size() * sizeof(CookedVfxDefinition));
    std::memcpy(loaded.emitters.data(), payload.data() + loaded.definitions.size() * sizeof(CookedVfxDefinition),
                loaded.emitters.size() * sizeof(CookedParticleEmitter));
    std::memcpy(loaded.sprites.data(), payload.data() +
                    loaded.definitions.size() * sizeof(CookedVfxDefinition) +
                    loaded.emitters.size() * sizeof(CookedParticleEmitter),
                loaded.sprites.size() * sizeof(CookedParticleSprite));
    std::uint64_t previous{};
    for (std::size_t index = 0; index < loaded.definitions.size(); ++index)
    {
        const auto &definition = loaded.definitions[index];
        if (definition.effect_id == 0 || (index && definition.effect_id <= previous) || definition.reserved != 0 ||
            static_cast<std::uint64_t>(definition.first_emitter) + definition.emitter_count > loaded.emitters.size())
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle definition table is invalid.");
        if ((definition.kind == VfxDefinitionKind::Particles) != (definition.emitter_count != 0))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle definition kind is invalid.");
        if (definition.kind == VfxDefinitionKind::Line &&
            (definition.line_primitive < VfxPrimitive::SolidTrail ||
             definition.line_primitive > VfxPrimitive::DashWake))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX line primitive is invalid.");
        if (definition.line_sprite >= header.sprite_count || definition.line_frame_columns == 0 ||
            definition.line_frame_rows == 0)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle definition sprite is invalid.");
        previous = definition.effect_id;
    }
    for (const auto &emitter : loaded.emitters)
    {
        const auto valid_primitive =
            (emitter.renderer == VfxRenderer::Sprite &&
             (emitter.primitive == VfxPrimitive::Soft || emitter.primitive == VfxPrimitive::Flame)) ||
            (emitter.renderer == VfxRenderer::Ground &&
             ((emitter.primitive >= VfxPrimitive::Disc && emitter.primitive <= VfxPrimitive::Cracks) ||
              emitter.primitive == VfxPrimitive::Flame)) ||
            (emitter.renderer == VfxRenderer::Segment &&
             emitter.primitive >= VfxPrimitive::SolidTrail && emitter.primitive <= VfxPrimitive::DashWake) ||
            (emitter.renderer == VfxRenderer::Mesh && emitter.primitive >= VfxPrimitive::Arrow &&
             emitter.primitive <= VfxPrimitive::ShockShell);
        if (emitter.sprite >= header.sprite_count || emitter.frame_columns == 0 ||
            emitter.frame_rows == 0 || !valid_primitive)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle emitter contract is invalid.");
    }
    previous = 0;
    for (std::size_t index = 0; index < loaded.sprites.size(); ++index)
    {
        const auto &sprite = loaded.sprites[index];
        if (sprite.sprite_id == 0 || (index && sprite.sprite_id <= previous) ||
            sprite.index >= header.sprite_count || sprite.frame_columns == 0 ||
            sprite.frame_rows == 0)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "Particle sprite table is invalid.");
        previous = sprite.sprite_id;
    }
    if (program_bytes.empty() || program_bytes.size() > std::numeric_limits<std::uint32_t>::max())
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX program size is invalid.");
    VfxProgramData program;
    std::string program_error;
    if (!LoadVfxProgram(program_bytes, program, program_error))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX v4 program: " + program_error);
    loaded.program = std::move(program);
    const auto &cooked_program = loaded.program;
    loaded.authored_mesh_indices.resize(loaded.emitters.size());
    loaded.decorative_definitions.reserve(loaded.definitions.size());
    loaded.has_typed_wavefront = std::ranges::any_of(cooked_program.effect_lookup, [](const auto &effect) {
        return effect.effect_id == MakeAssetId("particle.boss.shockwave.wavefront").value;
    });
    std::uint32_t mapped{}, unmapped{};
    for (const auto &definition : loaded.definitions)
    {
        const auto lookup = std::find_if(cooked_program.effect_lookup.begin(), cooked_program.effect_lookup.end(),
            [&](const auto &entry) { return entry.effect_id == definition.effect_id; });
        const auto *policy = lookup != cooked_program.effect_lookup.end()
            ? &cooked_program.effects[lookup->handle - 1] : nullptr;
        loaded.decorative_definitions.push_back(policy && !policy->must_match_logic && std::ranges::none_of(
            std::span(cooked_program.sources).subspan(policy->sources.first, policy->sources.count), [](const auto &source) {
                return source.authoritative || source.type == VfxSourceType::ProjectileFollow;
            }));
        for (std::uint32_t ei = definition.first_emitter; ei < definition.first_emitter + definition.emitter_count; ++ei)
        {
            const auto &emitter = loaded.emitters[ei];
            if (emitter.renderer != VfxRenderer::Mesh) continue;
            std::uint8_t selected{};
            bool ambiguous = false;
            if (lookup != cooked_program.effect_lookup.end())
            {
                const auto &effect = cooked_program.effects[lookup->handle - 1];
                for (std::uint32_t si = effect.sources.first; si < effect.sources.first + effect.sources.count; ++si)
                {
                    const auto &source = cooked_program.sources[si];
                    for (std::uint32_t oi = source.outputs.first; oi < source.outputs.first + source.outputs.count; ++oi)
                    {
                        const auto &output = cooked_program.outputs[oi];
                        if (output.profile != VfxOutputProfile::MeshEmissiveOit) continue;
                        const auto mesh = MeshIndex(output.shape);
                        if (!mesh) continue;
                        const bool compatible =
                            (emitter.primitive == VfxPrimitive::Arrow && mesh == 1) ||
                            (emitter.primitive == VfxPrimitive::Shard && (mesh == 2 || mesh == 3 || mesh == 4)) ||
                            (emitter.primitive == VfxPrimitive::Ember && (mesh == 2 || mesh == 3 || mesh == 4)) ||
                            (emitter.primitive == VfxPrimitive::Spike && (mesh == 5 || mesh == 6 || mesh == 7));
                        if (!compatible) continue;
                        if (selected && selected != mesh) ambiguous = true;
                        selected = mesh;
                    }
                }
            }
            if (selected && !ambiguous) { loaded.authored_mesh_indices[ei] = selected; ++mapped; }
            else ++unmapped;
        }
    }
    std::clog << "vfx.authored_mesh mapped_emitters=" << mapped << " unmapped_emitters=" << unmapped
              << " policy=primitive_compatible_unique_shape motion=legacy\n";
    loaded.sprite_count = header.sprite_count;
    loaded.payload_hash = header.payload_hash;
    loaded.has_required_sprite_bindings = true;
    for (const auto &[id, target] : {
             std::pair<std::string_view, ParticleSpriteBinding *>("particle_sprite.blood", &loaded.sprite_bindings.bleed_status_sprite),
             {"particle_sprite.fire", &loaded.sprite_bindings.burn_status_sprite},
             {"particle_sprite.slow_rune", &loaded.sprite_bindings.slow_status_sprite},
             {"particle_sprite.mark_target", &loaded.sprite_bindings.mark_status_sprite},
             {"particle_sprite.trap_rune", &loaded.sprite_bindings.trap_sprite},
             {"particle_sprite.fire_ground", &loaded.sprite_bindings.fire_area_sprite}})
    {
        const auto identity = MakeAssetId(id).value;
        const auto found = std::lower_bound(loaded.sprites.begin(), loaded.sprites.end(), identity,
            [](const CookedParticleSprite &sprite, std::uint64_t key) { return sprite.sprite_id < key; });
        if (found == loaded.sprites.end() || found->sprite_id != identity)
            loaded.has_required_sprite_bindings = false;
        else *target = {found->index, found->frame_columns, found->frame_rows};
    }
    PrepareVfxLightRecipes(loaded.program.View(), loaded.light_effect_ranges, loaded.light_recipes);
    output = std::move(loaded);
    return Result::Success();
}
Result LoadVfxImage(const std::filesystem::path &legacy_path,
                    const std::filesystem::path &program_path, VfxImageData &output)
{
    const auto read = [](const std::filesystem::path &path, std::vector<std::byte> &bytes) -> Result {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX image file is missing.");
        const auto length = file.tellg();
        if (length < 0 || static_cast<std::uint64_t>(length) > std::numeric_limits<std::uint32_t>::max() + std::uint64_t(sizeof(CookedParticleEffectsHeader)))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX image file size is invalid.");
        bytes.resize(static_cast<std::size_t>(length));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char *>(bytes.data()), length))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_vfx", "VFX image file is truncated.");
        return Result::Success();
    };
    std::vector<std::byte> legacy, program;
    if (auto result = read(legacy_path, legacy); !result) return result;
    if (auto result = read(program_path, program); !result) return result;
    return DecodeVfxImage(legacy, program, output);
}
}
