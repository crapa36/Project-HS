#include "vfx_catalog.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void TestLegacyMeshReplacement()
{
    std::array<hs::CookedParticleEmitter, 4> emitters{};
    for (auto &emitter : emitters) emitter.count = 3;
    emitters[0].renderer = hs::VfxRenderer::Mesh;
    emitters[0].primitive = hs::VfxPrimitive::Arrow;
    emitters[1].renderer = hs::VfxRenderer::Mesh;
    emitters[1].primitive = hs::VfxPrimitive::ShockShell;
    emitters[2].renderer = hs::VfxRenderer::Sprite;
    emitters[3].renderer = hs::VfxRenderer::Ground;
    emitters[3].primitive = hs::VfxPrimitive::Ring;
    const std::array<std::uint8_t, 4> meshes{1, 2, 0, 0};
    const std::array<std::uint8_t, 1> decorative{0};
    for (const auto name : {"particle.fixture.mesh", "particle.skill.explosive_arrow.main"})
    {
        const auto id = hs::MakeAssetId(name);
        const std::array definitions{hs::CookedVfxDefinition{id.value, 0, 4}};
        hs::VfxImage image;
        image.definitions = definitions;
        image.emitters = emitters;
        image.authored_mesh_indices = meshes;
        image.decorative_definitions = decorative;
        hs::VfxCatalog catalog;
        catalog.Bind(image);
        hs::PresentationEvent event;
        event.kind = hs::PresentationKind::Vfx;
        event.asset = id;
        event.sequence = 27;
        event.parameters = hs::EncodeVfxParameters({{0.0f, 0.0f, 1.0f}, 1.0f, {}, {}});
        std::vector<hs::ParticleSpawnCommand> original, actual(1);
        actual.front().sequence = 99;
        std::vector<hs::VfxLineSpawnCommand> lines;
        Check(catalog.ExpandEvent(event, 1.0f, original, lines).Succeeded() &&
                  catalog.ExpandEvent(event, 1.0f, actual, lines, false, true).Succeeded(),
              "legacy mesh replacement expansion failed");
        const bool replaces_shell = id.value == hs::MakeAssetId("particle.skill.explosive_arrow.main").value;
        std::size_t kept = 1;
        for (const auto &command : original)
        {
            if (command.renderer == hs::VfxRenderer::Mesh &&
                (command.primitive != hs::VfxPrimitive::ShockShell || replaces_shell)) continue;
            Check(kept < actual.size() && actual[kept].seed == command.seed &&
                      actual[kept].count == command.count && actual[kept].renderer == command.renderer &&
                      actual[kept].primitive == command.primitive,
                  "mesh replacement changed retained accents, emitter order or seeds");
            ++kept;
        }
        Check(original.size() == 4 && actual.size() == kept && actual.front().sequence == 99 &&
                  kept == (replaces_shell ? 3 : 4),
              "mesh replacement removed unrelated outputs or retained replaced mesh");

        hs::VfxPreparedLegacyEvent prepared;
        std::vector<std::uint32_t> emitter_indices{99};
        Check(catalog.PrepareEvent(event, 1.0f, prepared, emitter_indices, false, true).Succeeded() &&
                  prepared.maximum_required_count == 4 && prepared.required_count == kept - 1 &&
                  emitter_indices.front() == 99,
              "prepared mesh replacement lost fallback capacity or shared scratch prefix");
        std::vector<hs::ParticleSpawnCommand> fallback;
        Check(catalog.SetPreparedReplacements(prepared, emitter_indices, false, false).Succeeded() &&
                  prepared.required_count == 4 &&
                  catalog.AppendPreparedEvent(prepared, emitter_indices, fallback, lines).Succeeded() &&
                  fallback.size() == original.size(),
              "prepared mesh replacement could not restore the compatibility fallback");
        for (std::size_t index = 0; index < original.size(); ++index)
            Check(fallback[index].seed == original[index].seed &&
                      fallback[index].renderer == original[index].renderer &&
                      fallback[index].primitive == original[index].primitive,
                  "prepared fallback changed legacy emitter ordering or identity");
        fallback.clear();
        Check(catalog.SetPreparedReplacements(prepared, emitter_indices, true, true).Succeeded() &&
                  prepared.maximum_required_count == 4 && prepared.required_count == 0 &&
                  catalog.AppendPreparedEvent(prepared, emitter_indices, fallback, lines).Succeeded() &&
                  fallback.empty(),
              "prepared particle replacement emitted rows after typed replacement succeeded");
    }
}

void TestLegacyGroundSuppression()
{
    std::array<hs::CookedParticleEmitter, 5> emitters{};
    for (auto &emitter : emitters) emitter.count = 2;
    emitters[0].renderer = hs::VfxRenderer::Ground;
    emitters[0].primitive = hs::VfxPrimitive::Ring;
    emitters[1].renderer = hs::VfxRenderer::Ground;
    emitters[1].primitive = hs::VfxPrimitive::Disc;
    emitters[2].renderer = hs::VfxRenderer::Segment;
    emitters[2].primitive = hs::VfxPrimitive::SolidTrail;
    emitters[3].renderer = hs::VfxRenderer::Sprite;
    emitters[4].renderer = hs::VfxRenderer::Ground;
    emitters[4].primitive = hs::VfxPrimitive::ShockShell;
    const std::array<std::uint8_t, 1> decorative{0};
    const auto id = hs::MakeAssetId("particle.fixture.ground_suppression");
    const std::array definitions{hs::CookedVfxDefinition{id.value, 0, 5}};
    hs::VfxImage image;
    image.definitions = definitions;
    image.emitters = emitters;
    image.decorative_definitions = decorative;
    hs::VfxCatalog catalog;
    catalog.Bind(image);

    hs::PresentationEvent event;
    event.kind = hs::PresentationKind::Vfx;
    event.asset = id;
    event.sequence = 27;
    event.parameters = hs::EncodeVfxParameters({{0.0f, 0.0f, 1.0f}, 1.0f, {}, {}});
    std::vector<hs::ParticleSpawnCommand> actual;
    std::vector<hs::VfxLineSpawnCommand> lines;
    const hs::VfxLegacyGroundSuppression matching{false, true, true, true};
    hs::VfxPreparedLegacyEvent prepared;
    std::vector<std::uint32_t> emitter_indices{99};
    Check(catalog.PrepareEvent(event, 1.0f, prepared, emitter_indices, true, false, matching).Succeeded() &&
              prepared.index_count == 2 && prepared.maximum_required_count == 2 &&
              prepared.required_count == 0 && emitter_indices.front() == 99,
          "prepared ground suppression lost shared scratch prefix or fallback count");
    Check(catalog.SetPreparedReplacements(prepared, emitter_indices, false, false).Succeeded() &&
              prepared.required_count == 2,
          "prepared ground plan did not reselect fallback emitters");
    std::vector<hs::ParticleSpawnCommand> prepared_ground;
    Check(catalog.AppendPreparedEvent(prepared, emitter_indices, prepared_ground, lines).Succeeded() &&
              prepared_ground.size() == 2 && prepared_ground[0].renderer == hs::VfxRenderer::Sprite &&
              prepared_ground[1].primitive == hs::VfxPrimitive::ShockShell,
          "prepared ground fallback changed retained accent order");
    Check(catalog.ExpandEvent(event, 1.0f, actual, lines, false, false, matching).Succeeded() &&
              actual.size() == 2 && actual[0].renderer == hs::VfxRenderer::Sprite &&
              actual[1].renderer == hs::VfxRenderer::Ground &&
              actual[1].primitive == hs::VfxPrimitive::ShockShell,
          "ground suppression changed unrelated accents or ShockShell ordering");
    actual.clear();
    Check(catalog.ExpandEvent(event, 1.0f, actual, lines, false, false,
              hs::VfxLegacyGroundSuppression{true, false, false, false}).Succeeded() &&
              actual.empty(),
          "full typed replacement retained legacy particle rows");

    std::array<hs::CookedParticleEmitter, 2> optional_emitters{};
    optional_emitters[0].renderer = hs::VfxRenderer::Sprite;
    optional_emitters[0].count = 4;
    optional_emitters[1].renderer = hs::VfxRenderer::Ground;
    optional_emitters[1].count = 5;
    const std::array<std::uint8_t, 1> optional_definition{1};
    const auto optional_id = hs::MakeAssetId("particle.fixture.prepared_optional");
    const std::array optional_definitions{hs::CookedVfxDefinition{optional_id.value, 0, 2}};
    hs::VfxImage optional_image;
    optional_image.definitions = optional_definitions;
    optional_image.emitters = optional_emitters;
    optional_image.decorative_definitions = optional_definition;
    hs::VfxCatalog optional_catalog;
    optional_catalog.Bind(optional_image);
    auto optional_event = event;
    optional_event.asset = optional_id;
    hs::VfxPreparedLegacyEvent optional_plan;
    std::vector<std::uint32_t> optional_indices;
    Check(optional_catalog.PrepareEvent(optional_event, .5f, optional_plan, optional_indices).Succeeded() &&
              optional_plan.maximum_required_count == 1 && optional_plan.required_count == 1 &&
              optional_plan.count == 1 && optional_plan.admitted_count == 1,
          "prepared decorative rows were not counted separately from required ground output");
    optional_plan.admitted_count = 0;
    std::vector<hs::ParticleSpawnCommand> optional_output;
    Check(optional_catalog.AppendPreparedEvent(optional_plan, optional_indices, optional_output, lines).Succeeded() &&
              optional_output.size() == 1 && optional_output.front().renderer == hs::VfxRenderer::Ground,
          "zero optional admission removed protected ground output");
}
}

int RunVfxLegacyTests()
{
    try
    {
        TestLegacyMeshReplacement();
        TestLegacyGroundSuppression();
        {
            hs::CookedVfxDefinition definition;
            definition.effect_id = hs::MakeAssetId("fixture.prepared_line").value;
            definition.kind = hs::VfxDefinitionKind::Line;
            definition.line_width = .2f;
            definition.line_lifetime = .5f;
            const std::array<std::uint8_t, 1> decorative{1};
            hs::VfxImage image;
            image.definitions = std::span(&definition, 1);
            image.decorative_definitions = decorative;
            hs::VfxCatalog catalog;
            catalog.Bind(image);
            hs::PresentationEvent event;
            event.kind = hs::PresentationKind::Vfx;
            event.asset = hs::AssetId{definition.effect_id};
            event.sequence = 42;
            hs::VfxEventParameters parameters;
            parameters.direction = {0, 0, 1}; parameters.scale = 2;
            event.parameters = hs::EncodeVfxParameters(parameters);
            hs::VfxPreparedLegacyEvent plan;
            std::vector<std::uint32_t> indices{99};
            Check(!catalog.PrepareEvent(event, 1, plan, indices), "missing line target was accepted");
            parameters.flags = static_cast<std::uint32_t>(hs::VfxEventFlag::HasTarget);
            parameters.target = {2, 3, 4};
            event.parameters = hs::EncodeVfxParameters(parameters);
            Check(catalog.PrepareEvent(event, 1, plan, indices, true, true).Succeeded() &&
                plan.maximum_required_count == 1 && plan.required_count == 1 && !plan.count &&
                indices == std::vector<std::uint32_t>{99}, "required line used particle suppression or emitter scratch");
            std::vector<hs::ParticleSpawnCommand> particles;
            std::vector<hs::VfxLineSpawnCommand> lines;
            Check(catalog.AppendPreparedEvent(plan, indices, particles, lines).Succeeded() &&
                particles.empty() && lines.size() == 1 && lines[0].width == .4f &&
                lines[0].sequence == event.sequence, "required line output changed during preparation");
        }
        std::cout << "Prepared legacy VFX commands passed\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
