#include "content_cooker.hpp"

#define CookCharacterAsset CookCharacterAssetBase
#include "character_asset_cooker_base.inl"
#undef CookCharacterAsset

namespace hs::content
{
namespace
{

void AppendClipAlias(CharacterCookResult &character,
                     hs::CharacterAnimationClip destination,
                     hs::CharacterAnimationClip source)
{
    const auto found = std::ranges::find(character.clips, source,
                                         &hs::CharacterClipHeader::clip);
    if (found == character.clips.end())
        throw std::runtime_error("Character compatibility clip source is missing");
    const auto transform_count = static_cast<std::size_t>(found->frame_count) *
                                 character.bone_count;
    const auto first = static_cast<std::size_t>(found->first_transform);
    if (first + transform_count > character.transforms.size())
        throw std::runtime_error("Character compatibility clip range is invalid");

    auto header = *found;
    header.clip = destination;
    header.looping = false;
    character.clips.push_back(header);
}

} // namespace

bool CookCharacterAsset(const std::filesystem::path &output,
                        CharacterCookResult &character,
                        std::string &error_message,
                        const CharacterAssetSource &source)
{
    if (!CookCharacterAssetBase(output, character, error_message, source))
        return false;

    try
    {
        if (source.output_name != "archer")
        {
            // The renderer keeps one fixed clip table for all skinned assets.
            // Monsters never select these player-only ids, so preserve that compact
            // contract with shared-range aliases instead of expanding their source set.
            AppendClipAlias(character, hs::CharacterAnimationClip::Dive,
                            hs::CharacterAnimationClip::Run);
            AppendClipAlias(character, hs::CharacterAnimationClip::Stop,
                            hs::CharacterAnimationClip::Idle);
            AppendClipAlias(character, hs::CharacterAnimationClip::Hit,
                            hs::CharacterAnimationClip::Recoil);
            return WriteCharacterAsset(output / (source.output_name + ".meshbin"),
                                       character, error_message);
        }

        auto *manager = FbxManager::Create();
        if (!manager)
        {
            error_message = "FBX manager creation failed";
            return false;
        }
        manager->SetIOSettings(FbxIOSettings::Create(manager, IOSROOT));
        try
        {
            auto *model = LoadFbx(*manager, source.model, source.diagnostic_name);
            CharacterCookResult scratch;
            std::vector<BoneSource> bones;
            GatherModel(*model, scratch, bones, source.model,
                        source.allow_missing_material_textures);
            model->Destroy();

            const auto directory = std::filesystem::path(HS_CHARACTER_ANIMATION_DIRECTORY);
            const std::array extra_animations{
                std::tuple{directory / "DiveForward.fbx",
                           hs::CharacterAnimationClip::Dive, false},
                std::tuple{directory / "RunForwardStop.fbx",
                           hs::CharacterAnimationClip::Stop, false},
                std::tuple{directory / "ReactBack.fbx",
                           hs::CharacterAnimationClip::Hit, false},
            };
            for (const auto &[animation_path, clip, looping] : extra_animations)
            {
                auto *animation = LoadFbx(*manager, animation_path,
                                          source.diagnostic_name);
                GatherAnimation(*animation, bones, clip, looping, character,
                                animation_path);
                animation->Destroy();
            }
            if (!WriteCharacterAsset(output / (source.output_name + ".meshbin"),
                                     character, error_message))
            {
                manager->Destroy();
                return false;
            }
        }
        catch (...)
        {
            manager->Destroy();
            throw;
        }
        manager->Destroy();
        return true;
    }
    catch (const std::exception &exception)
    {
        error_message = exception.what();
        return false;
    }
}

bool CookCharacterAsset(const std::filesystem::path &output,
                        CharacterCookResult &character,
                        std::string &error_message)
{
    const CharacterAssetSource archer{
        HS_CHARACTER_MODEL,
        {std::filesystem::path(HS_CHARACTER_ANIMATION_DIRECTORY) / "Idle.fbx",
         std::filesystem::path(HS_CHARACTER_ANIMATION_DIRECTORY) / "RunForward.fbx",
         std::filesystem::path(HS_CHARACTER_ANIMATION_DIRECTORY) / "DrawArrow.fbx",
         std::filesystem::path(HS_CHARACTER_ANIMATION_DIRECTORY) / "AimRecoil.fbx",
         std::filesystem::path(HS_CHARACTER_ANIMATION_DIRECTORY) / "DeathBackward.fbx"},
        "archer", "Archer", false};
    return CookCharacterAsset(output, character, error_message, archer);
}

} // namespace hs::content
