#include "vfx_typed_mesh_commands.hpp"
#include <bit>
#include <cmath>
#include <string_view>

namespace hs::runtime_detail
{
namespace
{
constexpr std::uint32_t Hash(std::string_view s)
{
    std::uint32_t h = 2166136261u;
    for (const unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}
bool Finite(Float3 v)
{ return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool Range(VfxRange r, std::size_t size)
{ return r.first <= size && r.count <= size - r.first; }
}

std::vector<VfxOwnedMeshInput> BuildVfxTypedMeshCommands(
    const VfxProgramView &program, std::span<const VfxPersistentInput> inputs)
{
    std::vector<VfxOwnedMeshInput> result;
    for (const auto &input : inputs)
    {
        const auto *owner = std::get_if<VfxProjectilePayload>(&input.payload);
        if (!owner || input.effect_handle == 0 || input.effect_handle > program.effects.size()) continue;
        const auto &effect = program.effects[input.effect_handle - 1];
        if (effect.handle != input.effect_handle || effect.input_mode != 1 ||
            effect.payload_kind != Hash("ProjectilePayload") || !Range(effect.sources, program.sources.size())) continue;
        const Float3 position{input.current_transform[12], input.current_transform[13], input.current_transform[14]};
        const Float3 previous{input.previous_transform[12], input.previous_transform[13], input.previous_transform[14]};
        if (!Finite(position) || !Finite(previous) || !Finite(owner->velocity) ||
            !std::isfinite(owner->hitbox_radius) || owner->hitbox_radius <= 0) continue;
        for (std::size_t si = effect.sources.first; si < effect.sources.first + effect.sources.count; ++si)
        {
            const auto &source = program.sources[si];
            if (source.effect != effect.handle || !Range(source.outputs, program.outputs.size())) continue;
            // Gameplay owns even the boss ballistic trajectory. Never integrate
            // or collide that owner again in a visual particle simulation.
            if (source.type != VfxSourceType::ProjectileFollow &&
                source.type != VfxSourceType::BallisticCollision) continue;
            for (std::size_t oi = source.outputs.first; oi < source.outputs.first + source.outputs.count; ++oi)
            {
                const auto &output = program.outputs[oi];
                if (output.source != si || output.profile != VfxOutputProfile::MeshEmissiveOit ||
                    output.min_quality > static_cast<std::uint32_t>(input.quality)) continue;
                std::uint32_t mesh{};
                if (output.shape == Hash("arrowhead_mesh") && output.motion == VfxMotionKind::VelocityAlignedSharpHead) mesh = 1;
                if (output.shape == Hash("enemy_thorn_mesh") && output.motion == VfxMotionKind::VelocityAligned) mesh = 5;
                if (output.shape == Hash("boss_crest_lance_mesh") && output.motion == VfxMotionKind::VelocityAlignedHeavyLance) mesh = 6;
                if (mesh == 0 || !std::isfinite(output.hdr) || output.hdr <= 0 ||
                    !Range(output.parameters, program.parameters.size())) continue;
                float fresnel{}, head_to_tail_ratio{}, shell_thickness{};
                std::uint32_t seen{};
                bool valid = true;
                for (std::size_t pi = output.parameters.first; pi < output.parameters.first + output.parameters.count; ++pi)
                {
                    const auto &p = program.parameters[pi];
                    std::uint32_t bit{};
                    if (p.type != VfxParameterType::Float) { valid = false; break; }
                    const auto value = std::bit_cast<float>(p.bits);
                    if (p.key == Hash("fresnel")) { bit = 1; fresnel = value; }
                    else if (p.key == Hash("head_to_tail_ratio") && mesh != 6) { bit = 2; head_to_tail_ratio = value; }
                    else if (p.key == Hash("shell_thickness") && mesh == 6) { bit = 4; shell_thickness = value; }
                    if (bit == 0 || (seen & bit)) { valid = false; break; }
                    seen |= bit;
                }
                if (!valid || (seen & (mesh == 6 ? 4u : 2u)) == 0 ||
                    !std::isfinite(head_to_tail_ratio) || !std::isfinite(shell_thickness) ||
                    (mesh == 6 ? shell_thickness != .22f : head_to_tail_ratio != (mesh == 5 ? .5f : .42f))) continue;
                if (!std::isfinite(fresnel) || fresnel < 0 || fresnel > 1) continue;
                const Float4 color{output.rgba[0], output.rgba[1], output.rgba[2], output.rgba[3]};
                if (!Finite({color.x,color.y,color.z}) || !std::isfinite(color.w)) continue;
                result.push_back({input.stable_id, position, previous, owner->velocity, color,
                    owner->hitbox_radius, output.hdr, fresnel, mesh, output.gradient_row, head_to_tail_ratio, shell_thickness});
            }
        }
    }
    return result;
}
}
