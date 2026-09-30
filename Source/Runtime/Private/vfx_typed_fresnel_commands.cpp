#include "vfx_typed_fresnel_commands.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string_view>

namespace hs::runtime_detail
{
namespace
{
constexpr std::uint32_t Hash32(std::string_view value) noexcept
{
    std::uint32_t hash = 2166136261u;
    for (const unsigned char character : value)
        hash = (hash ^ character) * 16777619u;
    return hash;
}

constexpr std::uint64_t Hash64(std::string_view value) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : value)
        hash = (hash ^ character) * 1099511628211ull;
    return hash;
}

constexpr auto kPhase2Aura = Hash64("persistent.boss.phase2_aura");
constexpr auto kPhaseTransition = Hash64("persistent.boss.phase_transition_invulnerable");
constexpr auto kPlayerInvulnerable = Hash64("particle.player.invulnerable_loop");
constexpr auto kEntityPayload = Hash32("EntityAttachmentPayload");
constexpr auto kBossShell = Hash32("boss_shell");
constexpr auto kEntityShell = Hash32("entity_shell");
constexpr auto kBodyShell = Hash32("body_shell");
constexpr auto kMeshShell = Hash32("mesh_shell");
constexpr auto kOwnerBounds = Hash32("owner_bounds");
constexpr auto kParticle = Hash32("particle");
constexpr auto kAnalytic = Hash32("analytic");
constexpr auto kSectorCount = Hash32("sector_count");
constexpr auto kRotationHz = Hash32("rotation_hz");
constexpr auto kFresnelPower = Hash32("fresnel_power");
constexpr auto kNoise = Hash32("noise");
constexpr auto kEndCrack = Hash32("end_crack");
constexpr auto kProfileGlobal = Hash32("profile.global");
constexpr auto kProfileAuthored = Hash32("profile.authored");

template<class T>
bool InRange(VfxRange range, const std::vector<T> &values) noexcept
{
    return range.first <= values.size() &&
           range.count <= values.size() - range.first;
}

bool FiniteTransform(const VfxTransform &transform) noexcept
{
    return std::all_of(transform.begin(), transform.end(),
                       [](float value) { return std::isfinite(value); });
}

bool ExactTextures(const VfxProgramData &program,
                   const VfxOutputRecord &output) noexcept
{
    if (!InRange(output.textures, program.texture_bindings) ||
        output.textures.count != 2) return false;
    bool gradient = false, noise = false;
    for (std::size_t index = output.textures.first;
         index < static_cast<std::size_t>(output.textures.first) + output.textures.count;
         ++index)
    {
        const auto &binding = program.texture_bindings[index];
        const auto resource = std::find_if(program.texture_resources.begin(),
            program.texture_resources.end(), [&](const auto &candidate) {
                return candidate.slot == binding.catalog_slot;
            });
        if (resource == program.texture_resources.end() ||
            resource->asset_path_bytes.count == 0 ||
            !InRange(resource->asset_path_bytes, program.strings)) return false;
        const std::string_view path(
            reinterpret_cast<const char *>(program.strings.data()) +
                resource->asset_path_bytes.first,
            resource->asset_path_bytes.count);
        if (binding.role == kProfileGlobal &&
            path == "Content/Textures/VFX/vfx_gradient_lut.dds" && !gradient)
            gradient = true;
        else if (binding.role == kProfileAuthored &&
                 path == "Content/Textures/VFX/vfx_noise_volume_64.dds" && !noise)
            noise = true;
        else return false;
    }
    return gradient && noise;
}

bool AuthoredEffect(const VfxProgramData &program, VfxEffectHandle handle,
                    VfxFresnelShellKind &kind) noexcept
{
    std::uint64_t identity{};
    for (const auto &binding : program.effect_lookup)
    {
        if (binding.handle != handle) continue;
        if (identity != 0) return false;
        identity = binding.effect_id;
    }
    if (identity == kPhase2Aura)
        kind = VfxFresnelShellKind::BossCrown;
    else if (identity == kPhaseTransition)
        kind = VfxFresnelShellKind::BossTransition;
    else if (identity == kPlayerInvulnerable)
        kind = VfxFresnelShellKind::PlayerInvulnerable;
    else
        return false;
    return true;
}

bool SourceEnvelope(const VfxSourceRecord &source, float age,
                    float &envelope, float &progress) noexcept
{
    if (source.knot_count != 2 || !std::isfinite(source.knots[0]) ||
        !std::isfinite(source.knots[1]) || source.knots[0] < 0.0f ||
        source.knots[1] > 1.0f || source.knots[1] <= source.knots[0] ||
        age < source.knots[0] || age >= source.knots[1]) return false;
    progress = (age - source.knots[0]) / (source.knots[1] - source.knots[0]);
    envelope = 1.0f;
    return std::isfinite(progress) && progress >= 0.0f && progress < 1.0f;
}

bool AuthoredParameters(const VfxProgramData &program,
                        const VfxOutputRecord &output,
                        VfxFresnelShellKind kind,
                        VfxFresnelInput &input) noexcept
{
    if (!InRange(output.parameters, program.parameters) ||
        output.parameters.count != 2) return false;
    bool first_found = false, second_found = false;
    for (std::size_t index = output.parameters.first;
         index < static_cast<std::size_t>(output.parameters.first) + output.parameters.count;
         ++index)
    {
        const auto &parameter = program.parameters[index];
        if (kind == VfxFresnelShellKind::BossCrown)
        {
            if (parameter.key == kSectorCount && parameter.type == VfxParameterType::Int &&
                !first_found)
            {
                input.sector_count = parameter.bits;
                first_found = true;
            }
            else if (parameter.key == kRotationHz && parameter.type == VfxParameterType::Float &&
                     !second_found)
            {
                input.rotation_hz = std::bit_cast<float>(parameter.bits);
                second_found = true;
            }
            else return false;
        }
        else if (kind == VfxFresnelShellKind::BossTransition)
        {
            if (parameter.key == kFresnelPower && parameter.type == VfxParameterType::Float &&
                !first_found)
            {
                input.fresnel_power = std::bit_cast<float>(parameter.bits);
                first_found = true;
            }
            else if (parameter.key == kEndCrack && parameter.type == VfxParameterType::Int &&
                     !second_found)
            {
                input.end_crack = parameter.bits;
                second_found = true;
            }
            else return false;
        }
        else
        {
            if (parameter.key == kFresnelPower && parameter.type == VfxParameterType::Float &&
                !first_found)
            {
                input.fresnel_power = std::bit_cast<float>(parameter.bits);
                first_found = true;
            }
            else if (parameter.key == kNoise && parameter.type == VfxParameterType::Float &&
                     !second_found)
            {
                input.noise_amount = std::bit_cast<float>(parameter.bits);
                second_found = true;
            }
            else return false;
        }
    }
    return first_found && second_found &&
        (kind == VfxFresnelShellKind::BossCrown
            ? input.sector_count > 0 && input.sector_count <= 64 &&
              std::isfinite(input.rotation_hz)
            : kind == VfxFresnelShellKind::BossTransition
                ? std::isfinite(input.fresnel_power) && input.fresnel_power > 0.0f &&
                  input.end_crack <= 1
                : std::isfinite(input.fresnel_power) && input.fresnel_power > 0.0f &&
                  std::isfinite(input.noise_amount) && input.noise_amount >= 0.0f &&
                  input.noise_amount <= 1.0f);
}
}

std::vector<VfxFresnelInput> BuildVfxTypedFresnelCommands(
    const VfxProgramData &program,
    std::span<const VfxPersistentInput> inputs)
{
    std::vector<VfxFresnelInput> result;
    for (const auto &owner : inputs)
    {
        const auto *entity = std::get_if<VfxEntityPayload>(&owner.payload);
        if (!entity || owner.effect_handle == 0 ||
            owner.effect_handle > program.effects.size() || owner.stable_id == 0 ||
            entity->render_instance_id == 0 ||
            !FiniteTransform(owner.current_transform) ||
            !FiniteTransform(owner.previous_transform) ||
            !std::isfinite(entity->footprint_radius) || entity->footprint_radius <= 0.0f ||
            !std::isfinite(entity->lifetime01) || entity->lifetime01 < 0.0f ||
            entity->lifetime01 >= 1.0f ||
            !std::isfinite(entity->health_fraction) || entity->health_fraction < 0.0f ||
            entity->health_fraction > 1.0f ||
            !std::isfinite(owner.normalized_age) ||
            owner.normalized_age != entity->lifetime01 ||
            !std::isfinite(owner.elapsed_seconds) || owner.elapsed_seconds < 0.0f ||
            static_cast<std::uint32_t>(owner.quality) > 2) continue;
        const auto &effect = program.effects[owner.effect_handle - 1];
        VfxFresnelShellKind kind{};
        if (effect.handle != owner.effect_handle || effect.input_mode != 1 ||
            effect.timing_kind != 1 || effect.payload_kind != kEntityPayload ||
            !AuthoredEffect(program, effect.handle, kind) ||
            !InRange(effect.sources, program.sources)) continue;
        for (std::size_t source_index = effect.sources.first;
             source_index < static_cast<std::size_t>(effect.sources.first) + effect.sources.count;
             ++source_index)
        {
            const auto &source = program.sources[source_index];
            float envelope{}, progress{};
            if (source.effect != effect.handle || source.type != VfxSourceType::Direct ||
                (kind == VfxFresnelShellKind::PlayerInvulnerable &&
                 source.stable_id != kBodyShell) ||
                !InRange(source.parameters, program.parameters) ||
                source.parameters.count != 0 ||
                !InRange(source.outputs, program.outputs) ||
                !SourceEnvelope(source, owner.normalized_age, envelope, progress)) continue;
            for (std::size_t output_index = source.outputs.first;
                 output_index < static_cast<std::size_t>(source.outputs.first) + source.outputs.count;
                 ++output_index)
            {
                const auto &output = program.outputs[output_index];
                const auto motion = kind == VfxFresnelShellKind::BossCrown
                    ? VfxMotionKind::SlowCounterRotationContraction
                    : kind == VfxFresnelShellKind::BossTransition
                        ? VfxMotionKind::FresnelShellContracts
                        : VfxMotionKind::SubtleFresnelPulse;
                const bool player = kind == VfxFresnelShellKind::PlayerInvulnerable;
                const auto shell = player ? kEntityShell : kBossShell;
                if (output.source != source_index ||
                    output.profile != (player ? VfxOutputProfile::FresnelShellEmissive
                                              : VfxOutputProfile::FresnelShellOit) ||
                    output.shape != shell || output.shape_domain != kMeshShell ||
                    output.shape_scale_rule != kOwnerBounds ||
                    output.shape_component_kind != kParticle ||
                    output.coverage_type != kAnalytic || output.coverage_ref != shell ||
                    output.motion != motion ||
                    output.min_quality > static_cast<std::uint32_t>(owner.quality) ||
                    !ExactTextures(program, output) ||
                    !std::isfinite(output.hdr) || output.hdr <= 0.0f ||
                    !std::isfinite(output.motion_rate_hz) || output.motion_rate_hz < 0.0f ||
                    !std::isfinite(output.motion_amplitude) ||
                    output.motion_amplitude < 0.0f || output.motion_amplitude > 1.0f ||
                    !std::isfinite(output.motion_inset_fraction) ||
                    output.motion_inset_fraction < 0.0f ||
                    output.motion_inset_fraction > 1.0f ||
                    std::any_of(output.rgba.begin(), output.rgba.end(),
                                [](float value) { return !std::isfinite(value); }) ||
                    output.rgba[3] < 0.0f || output.rgba[3] > 1.0f) continue;
                VfxFresnelInput command;
                if (!AuthoredParameters(program, output, kind, command)) continue;
                command.render_instance_id = entity->render_instance_id;
                command.stable_id = owner.stable_id;
                command.effect_handle = effect.handle;
                command.kind = kind;
                command.current_transform = owner.current_transform;
                command.previous_transform = owner.previous_transform;
                command.footprint_radius = entity->footprint_radius;
                command.source_progress = progress;
                command.elapsed_seconds = owner.elapsed_seconds;
                command.color = {output.rgba[0], output.rgba[1], output.rgba[2],
                                 output.rgba[3] * envelope};
                command.hdr = output.hdr;
                command.gradient_row = output.gradient_row;
                command.motion_rate_hz = output.motion_rate_hz;
                command.motion_amplitude = output.motion_amplitude;
                command.motion_inset_fraction = output.motion_inset_fraction;
                result.push_back(command);
            }
        }
    }
    return result;
}
}
