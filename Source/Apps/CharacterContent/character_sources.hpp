#pragma once

#include "../Content/character_cooker.hpp"

#include <array>
#include <filesystem>

namespace hs::content
{

// Native importer specifications for the output-driven character actions.
inline std::array<CharacterAssetSource, 8> CharacterAssetSources(
    const std::filesystem::path &source_root)
{
    const auto archer_model = source_root /
        "ContentSource/Models/Characters/Archer/ErikaArcher.fbx";
    const auto archer_animations = source_root /
        "ContentSource/Animations/Characters/Archer";
    const auto monster_models = source_root / "ContentSource/Models/Monsters";
    const auto monster_animations = source_root / "ContentSource/Animations/Monsters";
    return {{
        {archer_model,
         {archer_animations / "Idle.fbx", archer_animations / "RunForward.fbx",
          archer_animations / "DrawArrow.fbx", archer_animations / "AimRecoil.fbx",
          archer_animations / "DeathBackward.fbx"},
         "archer", "Archer", false, false,
         {{archer_animations / "Idle.fbx", "Idle", true},
          {archer_animations / "RunForward.fbx", "RunForward", true},
          {archer_animations / "DrawArrow.fbx", "DrawArrow", false},
          {archer_animations / "AimRecoil.fbx", "AimRecoil", false},
          {archer_animations / "DeathBackward.fbx", "DeathBackward", false},
          {archer_animations / "DiveForward.fbx", "DiveForward", false},
          {archer_animations / "ReactBack.fbx", "ReactBack", false},
          {archer_animations / "ReactFront.fbx", "ReactFront", false},
          {archer_animations / "ReactRight.fbx", "ReactRight", false},
          {archer_animations / "RunForwardStop.fbx", "RunForwardStop", false},
          {archer_animations / "WalkForward.fbx", "WalkForward", true}}},
        {monster_models / "SlimeFamily/Melee/SlimeMelee.fbx",
         {monster_animations / "SlimeFamily/Melee/Idle.fbx",
          monster_animations / "SlimeFamily/Melee/Run.fbx",
          monster_animations / "SlimeFamily/Melee/Draw.fbx",
          monster_animations / "SlimeFamily/Melee/Recoil.fbx",
          monster_animations / "SlimeFamily/Melee/Death.fbx"},
         "enemy_melee", "SlimeMelee", false, false},
        {monster_models / "SlimeFamily/Ranged/SlimeRanged.fbx",
         {monster_animations / "SlimeFamily/Ranged/Idle.fbx",
          monster_animations / "SlimeFamily/Ranged/Run.fbx",
          monster_animations / "SlimeFamily/Ranged/Draw.fbx",
          monster_animations / "SlimeFamily/Ranged/Recoil.fbx",
          monster_animations / "SlimeFamily/Ranged/Death.fbx"},
         "enemy_ranged", "SlimeRanged", false, false},
        {monster_models / "SlimeFamily/Suicide/SlimeSuicide.fbx",
         {monster_animations / "SlimeFamily/Suicide/Idle.fbx",
          monster_animations / "SlimeFamily/Suicide/Run.fbx",
          monster_animations / "SlimeFamily/Suicide/Draw.fbx",
          monster_animations / "SlimeFamily/Suicide/Recoil.fbx",
          monster_animations / "SlimeFamily/Suicide/Death.fbx"},
         "enemy_suicide", "SlimeSuicide", false, false},
        {monster_models / "SlimeFamily/Projectile/SlimeGelProjectile.fbx",
         {monster_animations / "SlimeFamily/Ranged/Idle.fbx",
          monster_animations / "SlimeFamily/Ranged/Run.fbx",
          monster_animations / "SlimeFamily/Ranged/Draw.fbx",
          monster_animations / "SlimeFamily/Ranged/Recoil.fbx",
          monster_animations / "SlimeFamily/Ranged/Death.fbx"},
         "enemy_gel_projectile", "SlimeGelProjectile", false, false},
        {monster_models / "TurtleShell_SK.fbx",
         {monster_animations / "5m/IdleBattle.fbx", monster_animations / "5m/Run.fbx",
          monster_animations / "5m/Attack01.fbx", monster_animations / "5m/Attack02.fbx",
          monster_animations / "5m/Die.fbx"},
         "boss_5m", "Boss5mTurtleShell", true},
        {monster_models / "ChestMonster_SK.fbx",
         {monster_animations / "10m/IdleBattle.fbx", monster_animations / "10m/Run.fbx",
          monster_animations / "10m/Attack01.fbx", monster_animations / "10m/Attack02.fbx",
          monster_animations / "10m/Die.fbx"},
         "boss_10m", "Boss10mChestMonster", true},
        {monster_models / "Beholder_SK.fbx",
         {monster_animations / "final/IdleBattle.fbx", monster_animations / "final/Run.fbx",
          monster_animations / "final/Attack01.fbx", monster_animations / "final/Attack03.fbx",
          monster_animations / "final/Die.fbx"},
         "boss_final", "BossFinalBeholder", true},
    }};
}

bool CookMonsterMaterialTextures(const std::filesystem::path &output,
                                 std::string &error_message,
                                 const std::filesystem::path &texture_source);

} // namespace hs::content
