#pragma once

#include "../AssetPipeline/action.hpp"
#include "../Content/character_cooker.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hs::content
{

inline void RequireCharacterSourceInput(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error("hs_character_content: " + message);
}

inline std::string RelativeCharacterInputId(const std::filesystem::path &source_root,
                                            const std::filesystem::path &path)
{
    const auto relative = path.lexically_relative(source_root);
    RequireCharacterSourceInput(!relative.empty() && !relative.is_absolute() &&
            relative.begin() != relative.end() && *relative.begin() != "..",
            "input escapes source root: " + path.string());
    return relative.generic_string();
}

inline void AddCharacterInput(asset_pipeline::ActionSpec &spec,
                              const std::filesystem::path &source_root,
                              const std::filesystem::path &path)
{
    spec.inputs.push_back(asset_pipeline::HashInput(RelativeCharacterInputId(source_root, path), path));
}

inline void AddCharacterModelInputs(asset_pipeline::ActionSpec &spec,
                                    const std::filesystem::path &source_root,
                                    const CharacterAssetSource &source)
{
    namespace fs = std::filesystem;
    AddCharacterInput(spec, source_root, source.model);
    const auto fbm = source.model.parent_path() /
        (source.model.stem().string() + ".fbm");
    if (fs::exists(fbm))
    {
        asset_pipeline::RequireDirectory(fbm);
        std::vector<fs::path> textures;
        for (const auto &entry : fs::directory_iterator(fbm))
        {
            asset_pipeline::RequireRegularFile(entry.path());
            textures.push_back(entry.path());
        }
        std::ranges::sort(textures);
        for (const auto &texture : textures)
            AddCharacterInput(spec, source_root, texture);
    }
}

inline void AddCharacterInputs(asset_pipeline::ActionSpec &spec,
                               const std::filesystem::path &source_root,
                               const CharacterAssetSource &source)
{
    AddCharacterModelInputs(spec, source_root, source);
    for (const auto &clip : source.animations)
        AddCharacterInput(spec, source_root, clip);
    for (const auto &clip : source.named_animations)
        if (std::ranges::find(source.animations, clip.file) == source.animations.end())
            AddCharacterInput(spec, source_root, clip.file);
}

struct CharacterInputSnapshot
{
    std::filesystem::path root;
    std::unordered_set<std::string> logical_ids;

    std::filesystem::path Remap(const std::filesystem::path &source_root,
                                const std::filesystem::path &path) const
    {
        const auto logical_id = RelativeCharacterInputId(source_root, path);
        RequireCharacterSourceInput(logical_ids.contains(logical_id),
            "character source file was not included in its authenticated inputs: " + logical_id);
        return root / std::filesystem::path(logical_id);
    }

    std::filesystem::path RemapDirectory(const std::filesystem::path &source_root,
                                         const std::filesystem::path &path) const
    {
        auto logical_id = RelativeCharacterInputId(source_root, path);
        logical_id += "/";
        RequireCharacterSourceInput(std::ranges::any_of(logical_ids, [&](const std::string &input) {
                    return input.starts_with(logical_id);
                }), "character input snapshot does not contain the requested source directory: " +
                path.string());
        logical_id.pop_back();
        return root / std::filesystem::path(logical_id);
    }
};

inline CharacterInputSnapshot SnapshotActionInputs(
    const std::filesystem::path &stage,
    const std::vector<asset_pipeline::Input> &inputs)
{
    namespace fs = std::filesystem;
    namespace ap = asset_pipeline;
    CharacterInputSnapshot snapshot{stage / "source-snapshot", {}};
    std::unordered_map<std::string, const ap::Input *> staged_inputs;
    for (const auto &input : inputs)
    {
        const fs::path relative(input.logical_id);
        RequireCharacterSourceInput(!relative.empty() && !relative.is_absolute() &&
                relative.has_relative_path() &&
                std::ranges::none_of(relative, [](const fs::path &part) {
                    return part == "." || part == "..";
                }), "character input has an unsafe snapshot identity: " + input.logical_id);
        const auto [found, inserted] = staged_inputs.emplace(input.logical_id, &input);
        if (!inserted)
        {
            RequireCharacterSourceInput(found->second->digest == input.digest &&
                    found->second->path.lexically_normal() == input.path.lexically_normal(),
                    "character inputs contain a conflicting duplicate identity: " + input.logical_id);
            continue;
        }
        const auto target = snapshot.root / relative;
        ap::MakeDirectory(target.parent_path());
        const auto copied = ap::CopyAndHashInput(input.logical_id, input.path, target);
        RequireCharacterSourceInput(copied.digest == input.digest,
            "character input changed after action identity was computed: " + input.logical_id);
    }
    for (const auto &[logical_id, input] : staged_inputs)
    {
        static_cast<void>(input);
        snapshot.logical_ids.insert(logical_id);
    }
    return snapshot;
}

inline CharacterAssetSource SnapshotCharacterSource(
    const std::filesystem::path &source_root, const CharacterInputSnapshot &snapshot,
    const CharacterAssetSource &source)
{
    auto result = source;
    result.model = snapshot.Remap(source_root, source.model);
    for (auto &animation : result.animations) animation = snapshot.Remap(source_root, animation);
    for (auto &animation : result.named_animations)
        animation.file = snapshot.Remap(source_root, animation.file);
    return result;
}

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

} // namespace hs::content
