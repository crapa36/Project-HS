#include "gameplay_test_support.hpp"

#include <hs/core/cooked_format.hpp>
#include <hs/core/dds_format.hpp>

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
    std::ifstream slime(std::filesystem::current_path() / "Cooked" /
                            (std::string(asset) + ".meshbin"),
                        std::ios::binary);
    hs::CharacterAssetHeader slime_header;
    slime.read(reinterpret_cast<char *>(&slime_header), sizeof(slime_header));
    const auto valid_magic = slime_header.magic ==
                             std::array<char, 8>{'H', 'S', 'C', 'H', 'A', 'R', '1', '\0'};
    Check(slime && valid_magic && slime_header.version == hs::kCharacterAssetVersion,
          "Slime family cooked mesh has a valid character header");
    const bool is_projectile = std::string_view(asset) == "enemy_gel_projectile";
    Check(slime_header.bone_count == (is_projectile ? 1u : 14u) && slime_header.clip_count == 5 &&
              slime_header.material_count == 1,
          "Slime family keeps the required rig controls, five animation slots, and one material");

    std::vector<hs::SkinnedVertex> slime_vertices(slime_header.vertex_count);
    std::vector<hs::CharacterClipHeader> slime_clips(slime_header.clip_count);
    if (!slime_vertices.empty())
        slime.seekg(slime_header.vertices_offset).read(
            reinterpret_cast<char *>(slime_vertices.data()),
            static_cast<std::streamsize>(slime_vertices.size() * sizeof(slime_vertices.front())));
    if (!slime_clips.empty())
        slime.seekg(slime_header.clips_offset).read(
            reinterpret_cast<char *>(slime_clips.data()),
            static_cast<std::streamsize>(slime_clips.size() * sizeof(slime_clips.front())));
    Check(slime && std::ranges::all_of(slime_header.bounds_min, [](float value) { return std::isfinite(value); }) &&
              std::ranges::all_of(slime_header.bounds_max, [](float value) { return std::isfinite(value); }) &&
              slime_header.bounds_max[0] > slime_header.bounds_min[0] &&
              slime_header.bounds_max[1] > slime_header.bounds_min[1] &&
              slime_header.bounds_max[2] > slime_header.bounds_min[2],
          "Slime family cooked bounds are finite and nonempty");
    std::vector<std::uint16_t> parents(slime_header.bone_count);
    std::vector<std::array<float, 16>> bind(slime_header.bone_count);
    slime.seekg(slime_header.parents_offset).read(reinterpret_cast<char *>(parents.data()),
        static_cast<std::streamsize>(parents.size() * sizeof(parents.front())));
    slime.seekg(slime_header.inverse_bind_matrices_offset).read(reinterpret_cast<char *>(bind.data()),
        static_cast<std::streamsize>(bind.size() * sizeof(bind.front())));
    if (!is_projectile)
    {
        if (shared_parents.empty()) { shared_parents = parents; shared_bind = bind; }
        Check(slime && parents == shared_parents && bind == shared_bind,
              "Slime family shares bone hierarchy and bind positions across roles");
    }
    Check(slime && std::ranges::all_of(slime_vertices, [](const hs::SkinnedVertex &vertex) {
              return vertex.material_index == 0;
          }),
          "Slime family vertices use the single cooked material slot");
    if (std::string_view(asset) == "enemy_gel_projectile")
        Check(slime && std::ranges::all_of(slime_vertices, [](const hs::SkinnedVertex &vertex) {
                  return vertex.bone_indices[0] == 0 &&
                         std::abs(vertex.bone_weights[0] - 1.0f) < 0.0001f &&
                         std::ranges::all_of(std::span(vertex.bone_weights).subspan(1),
                                             [](float weight) { return std::abs(weight) < 0.0001f; });
              }),
              "Slime gel projectile vertices are root-only weighted");
    Check(slime && std::ranges::all_of(slime_clips, [](const hs::CharacterClipHeader &clip) {
              return clip.frame_count >= 2 && clip.duration_seconds > 0.0f;
          }),
          "Slime family cooks every animation slot with playable frames");

    const auto transform_prefix = slime_header.transforms_offset >= sizeof(slime_header)
                                      ? slime_header.transforms_offset - sizeof(slime_header)
                                      : std::numeric_limits<std::uint32_t>::max();
    const auto layout_valid = transform_prefix <= slime_header.payload_size &&
                              (slime_header.payload_size - transform_prefix) %
                                      sizeof(hs::CharacterLocalTransform) ==
                                  0;
    const auto transform_bytes = layout_valid ? slime_header.payload_size - transform_prefix : 0;
    std::vector<hs::CharacterLocalTransform> slime_transforms(
        transform_bytes / sizeof(hs::CharacterLocalTransform));
    if (layout_valid)
        slime.seekg(slime_header.transforms_offset).read(
            reinterpret_cast<char *>(slime_transforms.data()),
            static_cast<std::streamsize>(slime_transforms.size() * sizeof(slime_transforms.front())));
    bool root_motion_valid = layout_valid && slime.good();
    float draw_hop_height = 0.0f;
    for (const auto &clip : slime_clips)
        for (std::uint32_t frame = 0; frame < clip.frame_count && root_motion_valid; ++frame) {
            const auto index = static_cast<std::size_t>(clip.first_transform) +
                               static_cast<std::size_t>(frame) * slime_header.bone_count;
            if (index >= slime_transforms.size()) {
                root_motion_valid = false;
                break;
            }
            const auto &translation = slime_transforms[index].translation;
            root_motion_valid = std::ranges::all_of(translation,
                                                    [](float value) { return std::isfinite(value); }) &&
                                std::abs(translation[0]) < 0.0001f &&
                                std::abs(translation[2]) < 0.0001f &&
                                translation[1] >= -0.0001f && translation[1] <= 0.2f;
            if (clip.clip == hs::CharacterAnimationClip::Draw)
                draw_hop_height = std::max(draw_hop_height, translation[1]);
        }
    Check(root_motion_valid,
          "Slime family root motion stays horizontally stationary with finite bounded vertical hops");
    if (std::string_view(asset) == "enemy_melee")
        Check(draw_hop_height > 0.1f, "melee attack retains its authored jump above the ground");
    for (const auto suffix : {"_diffuse_0.dds", "_normal_0.dds"})
    {
        const auto path = std::filesystem::current_path() / "Cooked" / (std::string(asset) + suffix);
        std::ifstream texture(path, std::ios::binary);
        std::uint32_t magic{};
        hs::DdsHeader header;
        texture.read(reinterpret_cast<char *>(&magic), sizeof(magic));
        texture.read(reinterpret_cast<char *>(&header), sizeof(header));
        std::uintmax_t expected_size = sizeof(magic) + sizeof(header);
        for (std::uint32_t side = 1024; side; side /= 2) expected_size += side * side * 4;
        Check(texture && magic == hs::kDdsMagic && header.width == 1024 && header.height == 1024 &&
                  header.mip_count == 11 && (header.flags & 0x20000) != 0 &&
                  (header.caps & 0x400008) == 0x400008 && header.pixel_format.alpha_mask == 0xff000000 &&
                  std::filesystem::file_size(path) == expected_size,
              "Slime family two-map RGBA textures contain complete 1024-to-1 mip chains");
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
