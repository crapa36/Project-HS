#include "gameplay_test_support.hpp"
#include "test_generation_paths.hpp"

#include <hs/core/cooked_format.hpp>
#include <hs/core/dds_format.hpp>
#include <hs/render_data/character_package.hpp>
#include <hs/render_data/geometry_package.hpp>
#include <hs/render_data/dds_texture.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gameplay_test
{

void TestRelicDataIsCookedFromJson()
{
    const auto &content = DefaultContent();
    const auto &data = content.simulation_rules;
    const auto &blood_and_fire = data.relics.bleed_burn_explosion;
    Check(std::abs(blood_and_fire.radius - 5.0f) < 0.0001f &&
              std::abs(blood_and_fire.damage_multiplier - 2.0f) < 0.0001f &&
              blood_and_fire.per_target_cooldown_ticks == 120,
          "blood and fire uses cooked JSON values");

    Check(std::abs(data.relics.bleed_kill_heal.maximum_hp_heal_fraction - 0.05f) <
                  0.0001f &&
              std::abs(data.relics.burn_propagation.copied_burn_strength - 0.50f) <
                  0.0001f &&
              data.relics.kill_cooldown_surge.cooldown_reduction_ticks == 120 &&
              data.relics.radial_basic_attack.cadence_interval == 6 &&
              std::abs(data.relics.radial_basic_attack.damage_multiplier - 0.15f) <
                  0.0001f &&
              std::abs(data.relics.basic_kill_tracker.damage_multiplier - 0.20f) <
                  0.0001f &&
              std::abs(data.relics.alternating_skills.cooldown_refund_fraction - 0.20f) <
                  0.0001f,
          "tuned original relic values are cooked from JSON");

    const auto &counter = data.relics.damage_knockback;
    Check(std::abs(counter.radius - 4.0f) < 0.0001f &&
              std::abs(counter.push_distance - 0.25f) < 0.0001f &&
              std::abs(counter.slow_fraction - 0.5f) < 0.0001f &&
              counter.slow_duration_ticks == 120 && counter.cooldown_ticks == 60,
          "damage counter uses cooked JSON values");
    Check(std::string_view(content.presentation.relic_names[3].data()) == "피와 불" &&
              std::string_view(content.presentation.relic_names[9].data()) == "충격 반격" &&
              !std::string_view(content.presentation.relic_rules[3].data()).empty() &&
              !std::string_view(content.presentation.relic_rules[9].data()).empty(),
          "relic UI text is cooked from JSON");
    Check(data.relics.projectile_cadence_reward.hits_per_trigger == 8 &&
              data.relics.projectile_cadence_reward.cooldown_reduction_ticks == 45 &&
              std::abs(data.relics.pre_damage_guard.damage_reduction_fraction - 0.20f) < 0.0001f &&
              data.relics.pre_damage_guard.cooldown_ticks == 360 &&
              std::abs(data.relics.slow_synergy.damage_multiplier - 0.25f) < 0.0001f &&
              data.relics.slow_synergy.per_target_cooldown_ticks == 30 &&
              std::abs(data.relics.area_resonance.damage_multiplier - 0.60f) < 0.0001f &&
              data.relics.area_resonance.cooldown_ticks == 60 &&
              std::abs(data.relics.boss_pressure.damage_multiplier - 0.25f) < 0.0001f &&
              data.relics.boss_pressure.per_target_cooldown_ticks == 60 &&
              data.relics.hit_streak_reward.direct_hits_per_trigger == 10 &&
              std::abs(data.relics.hit_streak_reward.damage_multiplier - 1.50f) < 0.0001f &&
              std::abs(data.relics.pickup_reward.attack_power_fraction - 0.10f) < 0.0001f &&
              data.relics.pickup_reward.duration_ticks == 90 &&
              std::abs(data.relics.low_health_survival.health_threshold_fraction - 0.35f) < 0.0001f &&
              std::abs(data.relics.low_health_survival.damage_reduction_fraction - 0.40f) < 0.0001f &&
              data.relics.low_health_survival.cooldown_ticks == 480,
          "eight appended relic contracts are cooked from JSON");

    std::vector<std::uint16_t> shared_parents;
    std::vector<std::array<float, 16>> shared_bind;
    for (const auto asset : {"enemy_melee", "enemy_ranged", "enemy_suicide", "enemy_gel_projectile"})
    {
    const auto read_product = [](const std::string &id) {
        std::ifstream input(test_generation::ProductPath(id), std::ios::binary | std::ios::ate);
        Check(input.good(), "Slime render product opens from selected Generation");
        const auto size = input.tellg();
        Check(size >= 0, "Slime render product extent is readable");
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        input.seekg(0).read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        Check(input.good(), "Slime render product is complete");
        return bytes;
    };
    const auto metadata_id = "render/character/" + std::string(asset) + ".rpkg";
    const auto page_id = "render/geometry/character/" + std::string(asset) + ".rpage";
    // Monsters use whole-body animation; only the archer requires two mask regions.
    const auto character = hs::render_data::DecodeCharacterMetadataPackage(
        read_product(metadata_id), hs::MakeAssetId(metadata_id), false);
    const auto geometry = hs::render_data::DecodeGeometryPage(
        read_product(page_id), hs::MakeAssetId(page_id), character.geometry);
    hs::render_data::ValidateCharacterGeometry(character, geometry);
    const bool is_projectile = std::string_view(asset) == "enemy_gel_projectile";
    Check(character.bone_count == (is_projectile ? 1u : 14u) && character.clips.size() == 5 &&
              character.material_count == 1,
          "Slime family keeps the required rig controls, five animation slots, and one material");
    const auto &slime_vertices = geometry.vertices;
    const auto &slime_clips = character.clips;
    Check(std::ranges::all_of(geometry.bounds.minimum, [](float value) { return std::isfinite(value); }) &&
              std::ranges::all_of(geometry.bounds.maximum, [](float value) { return std::isfinite(value); }) &&
              geometry.bounds.maximum[0] > geometry.bounds.minimum[0] &&
              geometry.bounds.maximum[1] > geometry.bounds.minimum[1] &&
              geometry.bounds.maximum[2] > geometry.bounds.minimum[2],
          "Slime family cooked bounds are finite and nonempty");
    const auto &parents = character.parents;
    const auto &bind = character.inverse_bind_matrices;
    if (!is_projectile)
    {
        if (shared_parents.empty()) { shared_parents = parents; shared_bind = bind; }
        Check(parents == shared_parents && bind == shared_bind,
              "Slime family shares bone hierarchy and bind positions across roles");
    }
    Check(std::ranges::all_of(slime_vertices, [](const hs::render_data::ExpandedVertex &vertex) {
              return vertex.material_index == 0;
          }),
          "Slime family vertices use the single cooked material slot");
    if (std::string_view(asset) == "enemy_gel_projectile")
        Check(std::ranges::all_of(slime_vertices, [](const hs::render_data::ExpandedVertex &vertex) {
                  return vertex.bone_indices[0] == 0 &&
                         std::abs(vertex.bone_weights[0] - 1.0f) < 0.0001f &&
                         std::ranges::all_of(std::span(vertex.bone_weights).subspan(1),
                                             [](float weight) { return std::abs(weight) < 0.0001f; });
              }),
              "Slime gel projectile vertices are root-only weighted");
    Check(std::ranges::all_of(slime_clips, [](const hs::CharacterClipHeader &clip) {
              return clip.frame_count >= 2 && clip.duration_seconds > 0.0f;
          }),
          "Slime family cooks every animation slot with playable frames");

    Check(character.monster.has_value() && character.transforms.empty(),
          "Slime products contain cooked final palettes without duplicate runtime TRS reconstruction");
    const auto &deformation = *character.monster;
    Check(deformation.bone_count == character.bone_count && deformation.lods.size() == 1 &&
              deformation.lods.front().lod == 0 && !deformation.interval_bounds.empty(),
          "Slime final palettes retain their actual LOD0 and interval directory");
    Check(parents.front() == std::numeric_limits<std::uint16_t>::max(),
          "Slime root remains the hierarchy root");
    // Uploaded skin = transpose(global) * inverse_bind. Solve the bind origin
    // independently so the old root-translation oracle still tests final rows.
    std::array<std::array<double, 5>, 4> root_system{};
    for (std::size_t row = 0; row < 4; ++row)
    {
        for (std::size_t column = 0; column < 4; ++column)
            root_system[row][column] = bind.front()[row * 4 + column];
        root_system[row][4] = row == 3 ? 1.0 : 0.0;
    }
    for (std::size_t column = 0; column < 4; ++column)
    {
        auto pivot = column;
        for (std::size_t row = column + 1; row < 4; ++row)
            if (std::abs(root_system[row][column]) > std::abs(root_system[pivot][column])) pivot = row;
        Check(std::isfinite(root_system[pivot][column]) && std::abs(root_system[pivot][column]) > 1e-12,
              "Slime root bind remains invertible");
        std::swap(root_system[column], root_system[pivot]);
        const auto divisor = root_system[column][column];
        for (auto &value : root_system[column]) value /= divisor;
        for (std::size_t row = 0; row < 4; ++row)
            if (row != column)
            {
                const auto factor = root_system[row][column];
                for (std::size_t entry = 0; entry < 5; ++entry)
                    root_system[row][entry] -= factor * root_system[column][entry];
            }
    }
    bool root_motion_valid = true;
    float draw_hop_height = 0.0f;
    for (const auto &clip : slime_clips)
        for (std::uint32_t frame = 0; frame < clip.frame_count && root_motion_valid; ++frame) {
            const auto index = static_cast<std::size_t>(clip.first_transform) +
                               static_cast<std::size_t>(frame) * character.bone_count;
            if (index >= deformation.final_matrices.size()) {
                root_motion_valid = false;
                break;
            }
            const auto &matrix = deformation.final_matrices[index];
            std::array<double, 3> translation{};
            for (std::size_t row = 0; row < 3; ++row)
                for (std::size_t column = 0; column < 4; ++column)
                    translation[row] += double(matrix[row * 4 + column]) * root_system[column][4];
            root_motion_valid = std::ranges::all_of(translation,
                                                    [](double value) { return std::isfinite(value); }) &&
                                std::abs(translation[0]) < 0.0001f &&
                                std::abs(translation[2]) < 0.0001f &&
                                translation[1] >= -0.0001f && translation[1] <= 0.2f;
            if (clip.clip == hs::CharacterAnimationClip::Draw)
                draw_hop_height = std::max(draw_hop_height, static_cast<float>(translation[1]));
        }
    Check(root_motion_valid,
          "Slime family root motion stays horizontally stationary with finite bounded vertical hops");
    if (std::string_view(asset) == "enemy_melee")
        Check(draw_hop_height > 0.1f, "melee attack retains its authored jump above the ground");
    for (const auto suffix : {"_diffuse_0.dds", "_normal_0.dds"})
    {
        const auto path = test_generation::ProductPath(
            "character/" + std::string(asset) + suffix);
        std::ifstream texture(path, std::ios::binary);
        const auto length = std::filesystem::file_size(path);
        std::vector<std::byte> data(static_cast<std::size_t>(length));
        Check(bool(texture.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()))),
              "Slime family DDS source bytes can be read");
        const auto decoded = hs::render_data::DecodeDdsTexture(data);
        const auto format = std::string_view(suffix).starts_with("_normal")
            ? hs::render_data::kTextureFormatBc7 : hs::render_data::kTextureFormatBc7Srgb;
        hs::render_data::ValidateDdsTextureRole(decoded, hs::render_data::DdsTextureRole::CharacterFinalBc7,
            {1024, 1024, 1, 11, format});
        Check(decoded.subresources.size() == 11 && decoded.subresources.back().width == 1 &&
              decoded.subresources.back().height == 1 && decoded.subresources.back().bytes.size() == 16,
              "Slime family color/data BC7 textures retain all channels and complete 1024-to-1 block mip chains");
    }
    }

}

void TestTypedSimulationRulesAreCookedFromJson()
{
    const auto &content = DefaultContent();
    const auto &data = content.simulation_rules;
    const auto &split = data.upgrades.charged_shot.boss_or_fifth_pierce_split;
    constexpr std::array expected_angles{-35.0f, -25.0f, -15.0f, -5.0f,
                                          5.0f,  15.0f,  25.0f, 35.0f};

    Check(std::abs(data.upgrades.piercing_shot.align_hit_normal_enemy.move_distance -
                   3.0f) < 0.0001f,
          "upgrade scalar is cooked from JSON");
    Check(data.upgrades.charged_shot.extended_full_charge_explosion
              .maximum_charge_time_ticks == 84,
          "upgrade seconds are cooked to ticks");
    Check(split.angles_degrees == expected_angles && split.projectile_count == 8,
          "upgrade fixed array is cooked from JSON");
    Check(std::abs(data.enemies[1].ranged_projectile_radius - 0.25f) < 0.0001f,
          "enemy parameter is cooked from JSON");
    Check(std::abs(data.enemies[0].collision_radius - 0.63f) < 0.0001f &&
              std::abs(data.enemies[1].collision_radius - 0.71f) < 0.0001f &&
              std::abs(data.enemies[2].collision_radius - 0.79f) < 0.0001f,
          "role-specific enemy collision radii use the authored meter scale");
    Check(std::abs(data.stats.allocations[1].amount_per_point - 0.1f) < 0.0001f &&
              data.progression.required_xp_base == 12.0f &&
              data.progression.boss_spawn_ticks[2] == 54'000,
          "stat and progression parameters are cooked from JSON");
    Check(data.relic_drop.maximum_choices_per_box == 3 &&
              std::abs(data.relic_drop.healing_pickup_probability - 0.01f) <
                  0.0001f,
          "relic top-level parameters are cooked from JSON");
    const auto &spawn = data.spawn_placement;
    Check(spawn.require_outside_max_zoom_view && spawn.fallback_warning_ticks == 30 &&
              spawn.max_zoom_view_min_forward_m < 0.0f &&
              spawn.max_zoom_view_max_forward_m > 0.0f &&
              spawn.max_zoom_view_half_right_m > 0.0f &&
              std::abs(spawn.max_zoom_view_forward_x - 0.70710678f) < 0.0001f &&
              std::abs(spawn.max_zoom_view_forward_z - 0.70710678f) < 0.0001f,
          "spawn visibility bounds are derived from the authored maximum camera view");
    Check(content.presentation.camera.yaw_degrees == 45.0f &&
              content.presentation.camera.pitch_degrees == 55.0f &&
              content.presentation.camera.vertical_fov_degrees == 45.0f &&
              content.presentation.camera.distance_m == 28.0f &&
              content.presentation.camera.minimum_distance_percent == 80 &&
              content.presentation.camera.maximum_distance_percent == 120,
          "runtime camera settings are cooked from the same character authoring source");
}

void TestGameplayDataHotReloadBoundary()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({17, false, true}, QuietGameData()).Succeeded(),
          "Hot Reload initialize");
    auto replacement = QuietGameData();
    replacement.stats.base_maximum_hp = 177.0f;
    replacement.stats.base_current_hp = 177.0f;
    Check(simulation.ApplySimulationRules(replacement).Succeeded(),
          "Hot Reload applies at main menu boundary");

    hs::HeldInputState held;
    SetCursor(held, 960, 316);
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::Playing &&
              simulation.GetObservation().health == 177,
          "next session uses immutable replacement data");
    Check(!simulation.ApplySimulationRules(QuietGameData()),
          "Hot Reload rejects active gameplay");
    Check(simulation.Shutdown().Succeeded(), "Hot Reload shutdown");
}

void RunGameplayContentTests()
{
    TestRelicDataIsCookedFromJson();
    TestTypedSimulationRulesAreCookedFromJson();
    TestGameplayDataHotReloadBoundary();
}

} // namespace gameplay_test
