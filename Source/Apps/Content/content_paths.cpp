#include "content_paths.hpp"

namespace hs::content::paths
{

std::filesystem::path SourceRoot() { return HS_CONTENT_SOURCE_ROOT; }

std::filesystem::path GameData() { return SourceRoot() / "ContentSource/GameData"; }
std::filesystem::path Audio() { return SourceRoot() / "ContentSource/Audio"; }
std::filesystem::path Schema() { return SourceRoot() / "Schemas/game.schema.json"; }
std::filesystem::path ArcherModel()
{
    return SourceRoot() / "ContentSource/Models/Characters/Archer/ErikaArcher.fbx";
}
std::filesystem::path ArcherAnimations()
{
    return SourceRoot() / "ContentSource/Animations/Characters/Archer";
}
std::filesystem::path MonsterModels()
{
    return SourceRoot() / "ContentSource/Models/Monsters";
}
std::filesystem::path MonsterAnimations()
{
    return SourceRoot() / "ContentSource/Animations/Monsters";
}
std::filesystem::path MonsterTextures()
{
    return SourceRoot() / "ContentSource/Textures/Monsters/PBR";
}

} // namespace hs::content::paths
