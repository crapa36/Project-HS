#include <hs/game_domain/game_read_model.hpp>

namespace hs
{

GameReadModelStorage::GameReadModelStorage(std::size_t enemy_capacity,
                                           std::size_t projectile_capacity,
                                           std::size_t area_capacity,
                                           std::size_t pickup_capacity,
                                           std::size_t boss_action_capacity)
{
    enemies_.reserve(enemy_capacity);
    projectiles_.reserve(projectile_capacity);
    areas_.reserve(area_capacity);
    pickups_.reserve(pickup_capacity);
    boss_actions_.reserve(boss_action_capacity);
}

void GameReadModelStorage::Clear() noexcept
{
    enemies_.clear();
    projectiles_.clear();
    areas_.clear();
    pickups_.clear();
    boss_actions_.clear();
}

void GameReadModelStorage::AddEnemy(const EnemyView &value) { enemies_.push_back(value); }
void GameReadModelStorage::AddProjectile(const ProjectileView &value)
{
    projectiles_.push_back(value);
}
void GameReadModelStorage::AddArea(const AreaView &value) { areas_.push_back(value); }
void GameReadModelStorage::AddPickup(const PickupView &value) { pickups_.push_back(value); }
void GameReadModelStorage::AddBossAction(const BossActionView &value)
{
    boss_actions_.push_back(value);
}

GameReadModel GameReadModelStorage::View() const noexcept
{
    return {tick,
            checksum,
            seed,
            session,
            player,
            enemies_,
            projectiles_,
            areas_,
            pickups_,
            boss_actions_,
            effective_attack,
            effective_attack_speed,
            effective_move_speed,
            effective_magnet_radius,
            collection_skill_index,
            character_skill_index,
            character_slot_source};
}

} // namespace hs
