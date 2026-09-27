#include "../Source/Runtime/Private/vfx_typed_fresnel_commands.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr std::uint32_t Hash32(std::string_view value)
{
    std::uint32_t hash = 2166136261u;
    for (const unsigned char character : value)
        hash = (hash ^ character) * 16777619u;
    return hash;
}

constexpr std::uint64_t Hash64(std::string_view value)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : value)
        hash = (hash ^ character) * 1099511628211ull;
    return hash;
}

void Require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

bool Near(float actual, float expected)
{
    return std::abs(actual - expected) < .00001f;
}

hs::VfxProgramData Program(bool transition)
{
    hs::VfxProgramData program;
    hs::VfxEffectRecord effect;
    effect.handle = 1;
    effect.input_mode = 1;
    effect.timing_kind = 1;
    effect.payload_kind = Hash32("EntityAttachmentPayload");
    effect.sources = {0, 1};
    program.effects.push_back(effect);
    program.effect_lookup.push_back({Hash64(transition
        ? "persistent.boss.phase_transition_invulnerable"
        : "persistent.boss.phase2_aura"), 1});
    hs::VfxSourceRecord source;
    source.effect = 1;
    source.type = hs::VfxSourceType::Direct;
    source.knot_count = 2;
    source.knots = {.2f, .8f, 0, 0};
    source.outputs = {0, 1};
    program.sources.push_back(source);
    hs::VfxOutputRecord output;
    output.source = 0;
    output.profile = hs::VfxOutputProfile::FresnelShellOit;
    output.shape = Hash32("boss_shell");
    output.shape_domain = Hash32("mesh_shell");
    output.shape_scale_rule = Hash32("owner_bounds");
    output.shape_component_kind = Hash32("particle");
    output.coverage_type = Hash32("analytic");
    output.coverage_ref = Hash32("boss_shell");
    output.motion = transition ? hs::VfxMotionKind::FresnelShellContracts
                               : hs::VfxMotionKind::SlowCounterRotationContraction;
    output.rgba = {.2f, .4f, .6f, transition ? .52f : .36f};
    output.hdr = transition ? 3.4f : 2.2f;
    output.gradient_row = 13;
    output.motion_rate_hz = .25f;
    output.motion_amplitude = .2f;
    output.motion_inset_fraction = .1f;
    output.parameters = {0, 2};
    output.textures = {0, 2};
    program.outputs.push_back(output);
    const auto add_texture = [&](std::uint32_t slot, std::string_view role,
                                 std::string_view path) {
        hs::VfxTextureResourceRecord resource;
        resource.slot = slot;
        resource.asset_path_bytes = {
            static_cast<std::uint32_t>(program.strings.size()),
            static_cast<std::uint32_t>(path.size())};
        for (const unsigned char character : path)
            program.strings.push_back(static_cast<std::byte>(character));
        program.texture_resources.push_back(resource);
        hs::VfxTextureBindingRecord binding;
        binding.role = Hash32(role);
        binding.catalog_slot = slot;
        program.texture_bindings.push_back(binding);
    };
    add_texture(1, "profile.global", "Content/Textures/VFX/vfx_gradient_lut.dds");
    add_texture(2, "profile.authored", "Content/Textures/VFX/vfx_noise_volume_64.dds");
    if (transition)
        program.parameters = {
            {Hash32("fresnel_power"), hs::VfxParameterType::Float,
             std::bit_cast<std::uint32_t>(2.3f)},
            {Hash32("end_crack"), hs::VfxParameterType::Int, 1}};
    else
        program.parameters = {
            {Hash32("sector_count"), hs::VfxParameterType::Int, 6},
            {Hash32("rotation_hz"), hs::VfxParameterType::Float,
             std::bit_cast<std::uint32_t>(-.09f)}};
    return program;
}

hs::VfxProgramData PlayerProgram()
{
    auto program = Program(false);
    program.effect_lookup[0].effect_id = Hash64("particle.player.invulnerable_loop");
    program.sources[0].stable_id = Hash32("body_shell");
    program.sources[0].knots = {0.0f, 1.0f, 0.0f, 0.0f};
    auto &output = program.outputs[0];
    output.profile = hs::VfxOutputProfile::FresnelShellEmissive;
    output.shape = Hash32("entity_shell");
    output.coverage_ref = Hash32("entity_shell");
    output.motion = hs::VfxMotionKind::SubtleFresnelPulse;
    output.rgba = {.25f, .6f, .9f, .28f};
    output.hdr = 1.6f;
    output.gradient_row = 5;
    program.parameters = {
        {Hash32("fresnel_power"), hs::VfxParameterType::Float,
         std::bit_cast<std::uint32_t>(3.5f)},
        {Hash32("noise"), hs::VfxParameterType::Float,
         std::bit_cast<std::uint32_t>(.08f)}};
    return program;
}

hs::VfxPersistentInput Input()
{
    hs::VfxPersistentInput input;
    input.effect_handle = 1;
    input.stable_id = 0x1234;
    input.current_transform[12] = 4.0f;
    input.current_transform[13] = .025f;
    input.current_transform[14] = -2.0f;
    input.previous_transform[12] = 3.0f;
    input.previous_transform[14] = -2.5f;
    input.normalized_age = .5f;
    input.elapsed_seconds = 2.0f;
    input.payload = hs::VfxEntityPayload{1.4f, .5f, .75f, 2, 0xabcdef};
    return input;
}

void PreservesBossOwnerAndAuthoredShells()
{
    for (bool transition : {false, true})
    {
        const auto program = Program(transition);
        const auto input = Input();
        const auto commands = hs::runtime_detail::BuildVfxTypedFresnelCommands(
            program, std::span(&input, 1));
        Require(commands.size() == 1, "boss Fresnel output missing");
        const auto &command = commands[0];
        Require(command.kind == (transition ? hs::VfxFresnelShellKind::BossTransition
                                             : hs::VfxFresnelShellKind::BossCrown) &&
                command.render_instance_id == 0xabcdef &&
                command.stable_id == input.stable_id &&
                command.effect_handle == 1 &&
                command.current_transform == input.current_transform &&
                command.previous_transform == input.previous_transform &&
                Near(command.footprint_radius, 1.4f) &&
                Near(command.source_progress, .5f) &&
                Near(command.elapsed_seconds, 2.0f),
                "Fresnel shell lost its gameplay owner or source-local time");
        Require(Near(command.color.x, .2f) && Near(command.color.y, .4f) &&
                Near(command.color.z, .6f) &&
                Near(command.color.w, transition ? .52f : .36f) &&
                Near(command.hdr, transition ? 3.4f : 2.2f) &&
                command.gradient_row == 13 &&
                Near(command.motion_rate_hz, .25f) &&
                Near(command.motion_amplitude, .2f) &&
                Near(command.motion_inset_fraction, .1f),
                "Fresnel shell lost cooked material or motion");
        if (transition)
            Require(Near(command.fresnel_power, 2.3f) && command.end_crack == 1 &&
                    command.sector_count == 0,
                    "transition shell did not decode fresnel/end crack params");
        else
            Require(command.sector_count == 6 && Near(command.rotation_hz, -.09f) &&
                    Near(command.fresnel_power, 0),
                    "crown shell did not decode sector/rotation params");
    }
}

void RejectsMissingOwnerAndWrongAuthoredContract()
{
    auto program = Program(false);
    auto input = Input();
    const auto run = [&] {
        return hs::runtime_detail::BuildVfxTypedFresnelCommands(
            program, std::span(&input, 1));
    };
    std::get<hs::VfxEntityPayload>(input.payload).render_instance_id = 0;
    Require(run().empty(), "shell without target mesh accepted");
    input = Input();
    program.effect_lookup[0].effect_id = Hash64("persistent.status.bleed");
    Require(run().empty(), "unrelated entity effect accepted as boss shell");
    program = Program(false);
    program.outputs[0].profile = hs::VfxOutputProfile::FresnelShellEmissive;
    Require(run().empty(), "wrong shell profile accepted");
    program.outputs[0].profile = hs::VfxOutputProfile::FresnelShellOit;
    program.outputs[0].shape = Hash32("entity_shell");
    Require(run().empty(), "wrong shell shape accepted");
    program.outputs[0].shape = Hash32("boss_shell");
    program.outputs[0].motion = hs::VfxMotionKind::FresnelShellContracts;
    Require(run().empty(), "wrong boss shell motion accepted");
    program = Program(false);
    program.parameters[0].bits = 0;
    Require(run().empty(), "zero crown sector count accepted");
    program = Program(false);
    program.parameters[1].bits = std::bit_cast<std::uint32_t>(
        std::numeric_limits<float>::quiet_NaN());
    Require(run().empty(), "nonfinite crown rotation accepted");
    program = Program(false);
    input.normalized_age = .8f;
    std::get<hs::VfxEntityPayload>(input.payload).lifetime01 = .8f;
    Require(run().empty(), "shell survived exclusive source end");
    program = Program(true);
    input = Input();
    program.parameters[1].type = hs::VfxParameterType::Bool;
    Require(run().empty(), "end_crack accepted wrong cooked parameter type");
    program = Program(true);
    program.parameters[1].bits = 2;
    Require(run().empty(), "end_crack accepted non-flag value");
}

void PreservesPlayerInvulnerableOwnerAndCookedShell()
{
    auto program = PlayerProgram();
    auto input = Input();
    const auto run = [&] {
        return hs::runtime_detail::BuildVfxTypedFresnelCommands(
            program, std::span(&input, 1));
    };
    const auto commands = run();
    Require(commands.size() == 1, "player invulnerability Fresnel output missing");
    const auto &shell = commands.front();
    Require(shell.kind == hs::VfxFresnelShellKind::PlayerInvulnerable &&
            shell.render_instance_id == 0xabcdef &&
            shell.stable_id == input.stable_id &&
            shell.current_transform == input.current_transform &&
            shell.previous_transform == input.previous_transform &&
            Near(shell.footprint_radius, 1.4f) &&
            Near(shell.source_progress, .5f) && Near(shell.elapsed_seconds, 2.0f),
            "player shell lost actual owner mesh, footprint, or lifecycle clock");
    Require(Near(shell.fresnel_power, 3.5f) && Near(shell.noise_amount, .08f) &&
            Near(shell.color.x, .25f) && Near(shell.color.w, .28f) &&
            Near(shell.hdr, 1.6f) && shell.gradient_row == 5,
            "player shell lost cooked parameters or material");
    program.sources[0].stable_id = Hash32("wrong_shell");
    Require(run().empty(), "wrong player shell source accepted");
    program = PlayerProgram();
    program.outputs[0].profile = hs::VfxOutputProfile::FresnelShellOit;
    Require(run().empty(), "player shell accepted boss OIT profile");
    program = PlayerProgram();
    program.outputs[0].motion = hs::VfxMotionKind::FresnelShellContracts;
    Require(run().empty(), "player shell accepted boss contraction motion");
    program = PlayerProgram();
    program.parameters[1].bits = std::bit_cast<std::uint32_t>(1.1f);
    Require(run().empty(), "player shell accepted out-of-range noise amount");
    program = PlayerProgram();
    std::get<hs::VfxEntityPayload>(input.payload).render_instance_id = 0;
    Require(run().empty(), "player shell without Archer render target accepted");
}

void CookedBossShellsConvertWhenAvailable()
{
    const auto path = std::filesystem::path("Cooked") / "vfx_program.hsbin";
    if (!std::filesystem::exists(path)) return;
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(stream),
                                  std::istreambuf_iterator<char>()};
    hs::VfxProgramData program;
    std::string error;
    Require(hs::LoadVfxProgram(std::as_bytes(std::span(bytes)), program, error),
            "cooked VFX program failed to load for Fresnel coverage");
    std::size_t authored{}, converted{};
    for (const auto &lookup : program.effect_lookup)
    {
        if (lookup.effect_id != Hash64("persistent.boss.phase2_aura") &&
            lookup.effect_id != Hash64("persistent.boss.phase_transition_invulnerable"))
            continue;
        Require(lookup.handle > 0 && lookup.handle <= program.effects.size(),
                "cooked boss shell lookup has invalid handle");
        const auto &effect = program.effects[lookup.handle - 1];
        for (std::size_t source_index = effect.sources.first;
             source_index < static_cast<std::size_t>(effect.sources.first) + effect.sources.count;
             ++source_index)
        {
            const auto &source = program.sources[source_index];
            for (std::size_t output_index = source.outputs.first;
                 output_index < static_cast<std::size_t>(source.outputs.first) + source.outputs.count;
                 ++output_index)
                authored += program.outputs[output_index].profile ==
                    hs::VfxOutputProfile::FresnelShellOit;
        }
        auto input = Input();
        input.effect_handle = lookup.handle;
        const auto commands = hs::runtime_detail::BuildVfxTypedFresnelCommands(
            program, std::span(&input, 1));
        converted += commands.size();
    }
    if (authored != 2 || converted != authored)
        std::cerr << "cooked Fresnel authored=" << authored
                  << " converted=" << converted << '\n';
    Require(authored == 2 && converted == authored,
            "both cooked persistent boss Fresnel outputs must convert");
}

void CookedPlayerShellConvertsWhenAvailable()
{
    const auto path = std::filesystem::path("Cooked") / "vfx_program.hsbin";
    if (!std::filesystem::exists(path)) return;
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(stream),
                                  std::istreambuf_iterator<char>()};
    hs::VfxProgramData program;
    std::string error;
    Require(hs::LoadVfxProgram(std::as_bytes(std::span(bytes)), program, error),
            "cooked VFX program failed to load for player shell");
    std::size_t authored{}, converted{};
    for (const auto &lookup : program.effect_lookup)
    {
        if (lookup.effect_id != Hash64("particle.player.invulnerable_loop")) continue;
        Require(lookup.handle > 0 && lookup.handle <= program.effects.size(),
                "cooked player shell lookup has invalid handle");
        const auto &effect = program.effects[lookup.handle - 1];
        for (std::size_t source_index = effect.sources.first;
             source_index < static_cast<std::size_t>(effect.sources.first) + effect.sources.count;
             ++source_index)
        {
            const auto &source = program.sources[source_index];
            for (std::size_t output_index = source.outputs.first;
                 output_index < static_cast<std::size_t>(source.outputs.first) + source.outputs.count;
                 ++output_index)
                authored += program.outputs[output_index].profile ==
                    hs::VfxOutputProfile::FresnelShellEmissive;
        }
        auto input = Input();
        input.effect_handle = lookup.handle;
        const auto commands = hs::runtime_detail::BuildVfxTypedFresnelCommands(
            program, std::span(&input, 1));
        converted += commands.size();
        if (!commands.empty())
            Require(commands[0].kind == hs::VfxFresnelShellKind::PlayerInvulnerable &&
                    Near(commands[0].fresnel_power, 3.5f) &&
                    Near(commands[0].noise_amount, .08f),
                    "cooked player shell parameter or kind changed");
    }
    Require(authored == 1 && converted == 1,
            "one cooked player invulnerability Fresnel output must convert");
}
}

int main()
{
    try
    {
        PreservesBossOwnerAndAuthoredShells();
        RejectsMissingOwnerAndWrongAuthoredContract();
        PreservesPlayerInvulnerableOwnerAndCookedShell();
        CookedBossShellsConvertWhenAvailable();
        CookedPlayerShellConvertsWhenAvailable();
        std::cout << "VFX typed boss Fresnel commands passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
