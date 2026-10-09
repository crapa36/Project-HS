#include "renderer_named_animation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace hs
{
namespace
{
Result Invalid(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_renderer_named_animation", std::move(message));
}

CharacterLocalTransform Blend(const CharacterLocalTransform &first,
                              const CharacterLocalTransform &second, float weight)
{
    CharacterLocalTransform output{};
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        output.translation[axis] = std::lerp(first.translation[axis], second.translation[axis], weight);
        output.scale[axis] = std::lerp(first.scale[axis], second.scale[axis], weight);
    }
    const auto first_rotation = DirectX::XMQuaternionNormalize(DirectX::XMVectorSet(
        first.rotation[0], first.rotation[1], first.rotation[2], first.rotation[3]));
    auto second_rotation = DirectX::XMQuaternionNormalize(DirectX::XMVectorSet(
        second.rotation[0], second.rotation[1], second.rotation[2], second.rotation[3]));
    if (DirectX::XMVectorGetX(DirectX::XMQuaternionDot(first_rotation, second_rotation)) < 0.0f)
        second_rotation = DirectX::XMVectorNegate(second_rotation);
    DirectX::XMFLOAT4 rotation{};
    DirectX::XMStoreFloat4(&rotation, DirectX::XMQuaternionNormalize(
        DirectX::XMQuaternionSlerp(first_rotation, second_rotation, weight)));
    output.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
    return output;
}
} // namespace

Result SampleNamedCharacterAnimation(
    const render_data::CharacterAnimationExtensionData &animation,
    const RenderInstance &instance,
    std::span<DirectX::XMFLOAT4X4> transposed_skin_matrices)
{
    if (instance.animation_clip_id.value == 0)
        return Invalid(ErrorCode::InvalidArgument, "Named playback requires an explicit clip identity.");
    if (instance.asset_id.value == 0 || animation.character_asset_id.value != instance.asset_id.value)
        return Invalid(ErrorCode::InvalidArgument, "Named animation does not belong to the selected character asset.");
    if (!std::isfinite(instance.animation_time_seconds) || !std::isfinite(instance.animation_playback_rate))
        return Invalid(ErrorCode::InvalidArgument, "Named animation time and playback rate must be finite.");
    const auto bones = animation.parents.size();
    if (bones == 0 || bones > kMaxCharacterBones || animation.bone_names.size() != bones ||
        animation.inverse_bind_matrices.size() != bones || transposed_skin_matrices.size() < bones)
        return Invalid(ErrorCode::InvalidState, "Named animation skeleton or destination matrix count is invalid.");
    const auto clip = std::ranges::find_if(animation.clips,
        [&](const auto &entry) { return entry.id.value == instance.animation_clip_id.value; });
    if (clip == animation.clips.end())
        return Invalid(ErrorCode::InvalidArgument, "Selected named clip " +
            std::to_string(instance.animation_clip_id.value) + " is absent from this character's immutable animation product.");
    if (clip->frame_count < 2 || !std::isfinite(clip->duration_seconds) || clip->duration_seconds <= 0.0f ||
        clip->first_transform > animation.transforms.size() ||
        clip->frame_count > (animation.transforms.size() - clip->first_transform) / bones)
        return Invalid(ErrorCode::InvalidState, "Selected named animation has an invalid frame range or duration.");

    const double duration = clip->duration_seconds;
    double time = instance.animation_time_seconds * static_cast<double>(instance.animation_playback_rate);
    if (!std::isfinite(time))
        return Invalid(ErrorCode::InvalidArgument, "Named animation playback time overflowed.");
    if (clip->looping)
    {
        time = std::fmod(time, duration);
        if (time < 0.0) time += duration;
    }
    else time = std::clamp(time, 0.0, duration);
    const double frame_position = std::clamp(time / duration, 0.0, 1.0) *
                                  static_cast<double>(clip->frame_count - 1);
    const auto first_frame = static_cast<std::size_t>(frame_position);
    const auto second_frame = std::min(first_frame + 1, static_cast<std::size_t>(clip->frame_count - 1));
    const auto weight = static_cast<float>(frame_position - static_cast<double>(first_frame));
    std::array<DirectX::XMFLOAT4X4, kMaxCharacterBones> global_transforms{};
    std::array<DirectX::XMFLOAT4X4, kMaxCharacterBones> staged_skin{};
    for (std::size_t bone = 0; bone < bones; ++bone)
    {
        const auto parent = animation.parents[bone];
        if (parent != std::numeric_limits<std::uint16_t>::max() && parent >= bone)
            return Invalid(ErrorCode::InvalidState, "Named animation parent does not precede its child.");
        const auto base = static_cast<std::size_t>(clip->first_transform);
        const auto transform = Blend(animation.transforms[base + first_frame * bones + bone],
                                     animation.transforms[base + second_frame * bones + bone], weight);
        const auto local = DirectX::XMMatrixScaling(transform.scale[0], transform.scale[1], transform.scale[2]) *
            DirectX::XMMatrixRotationQuaternion(DirectX::XMVectorSet(
                transform.rotation[0], transform.rotation[1], transform.rotation[2], transform.rotation[3])) *
            DirectX::XMMatrixTranslation(transform.translation[0], transform.translation[1], transform.translation[2]);
        const auto global = parent == std::numeric_limits<std::uint16_t>::max()
            ? local : local * DirectX::XMLoadFloat4x4(&global_transforms[parent]);
        DirectX::XMStoreFloat4x4(&global_transforms[bone], global);
        DirectX::XMFLOAT4X4 inverse_bind{};
        std::memcpy(&inverse_bind, animation.inverse_bind_matrices[bone].data(), sizeof(inverse_bind));
        const auto skin = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&inverse_bind)) * global;
        const auto determinant = DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(skin));
        if (!std::isfinite(determinant) || std::abs(determinant) <= 0.0001f ||
            DirectX::XMMatrixIsNaN(skin) || DirectX::XMMatrixIsInfinite(skin))
            return Invalid(ErrorCode::InvalidState, "Selected named animation produced an invalid skin pose.");
        DirectX::XMStoreFloat4x4(&staged_skin[bone], DirectX::XMMatrixTranspose(skin));
    }
    std::copy_n(staged_skin.begin(), bones, transposed_skin_matrices.begin());
    return Result::Success();
}
} // namespace hs
